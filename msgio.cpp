/*

Copyright 2018 Intel Corporation

This software and the related documents are Intel copyrighted materials,
and your use of them is governed by the express license under which they
were provided to you (License). Unless the License provides otherwise,
you may not use, modify, copy, publish, distribute, disclose or transmit
this software or the related documents without Intel's prior written
permission.

This software and the related documents are provided as is, with no
express or implied warranties, other than those that are expressly stated
in the License.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sgx_urts.h>
#include <arpa/inet.h>
#include <sys/select.h>
#ifdef _WIN32
# include <Windows.h>
# include <winsock2.h>
# include <ws2tcpip.h>

# pragma comment(lib, "Ws2_32.lib")
# pragma comment(lib, "Mswsock.lib")
# pragma comment(lib, "AdvApi32.lib")
#else
# include <sys/socket.h>
# include <netdb.h>
# include <unistd.h>
#endif
#include <exception>
#include <stdexcept>
#include <string>
#include "hexutil.h"
#include "msgio.h"
#include "common.h"

using namespace std;

static char *buffer = NULL;
static uint32_t buffer_size = MSGIO_BUFFER_SZ;

/* With no arguments, we read/write to stdin/stdout using stdio */

MsgIO::MsgIO()
{
	use_stdio = true;
}

/* Connect to a remote server and port, and use socket IO */

MsgIO::MsgIO(const char *peer, const char *port)
{
#ifdef _WIN32
	WSADATA wsa;
#endif
	int rv, proto;
	struct addrinfo *addrs, *addr, hints;
	SOCKET ts;

	use_stdio= false;
#ifdef _WIN32
	rv = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (rv != 0) {
		eprintf("WSAStartup: %d\n", rv);
		throw std::runtime_error("WSAStartup failed");
	}
#endif

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = IPPROTO_TCP;

	rv= getaddrinfo(peer, port, &hints, &addrs);
	if (rv != 0) {		
		eprintf("getaddrinfo: %s\n", gai_strerror(rv));
		throw std::runtime_error("getaddrinfo failed");
	}

	for (addr= addrs; addr != NULL; addr= addr->ai_next)
	{
		proto= addr->ai_family;
		ts= socket(addr->ai_family, addr->ai_socktype,
			addr->ai_protocol);
		if ( ts == -1 ) {
			perror("socket");
			continue;
		}

		if ( peer == NULL ) { 	// We're the server
			if ( bind(ts, addr->ai_addr, addr->ai_addrlen) == 0 ) break;
		} else {
			if ( connect(ts, addr->ai_addr, addr->ai_addrlen) == 0 ) break;
		}

		perror("bind");
		close(ts);
	}

	freeaddrinfo(addrs);

	if ( ts == -1 ) {
		throw std::runtime_error("could not establish socket");
	}

	if ( peer == NULL ) {	// Server here. Create our listening socket.
		int enable= 1;
		sockaddr cliaddr;
		socklen_t slen= sizeof(sockaddr);

		setsockopt(ts, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
		setsockopt(ts, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
		if ( listen(ts, 5) == -1 ) { // The "traditional" backlog value in UNIX
			perror("listen");
			throw std::runtime_error("could not listen on socket");
		}
		
		// We have a very simple server: it will block until we get a 
		// connection.

		eprintf("Listening for connections on port %s\n", port);

		s= accept(ts, &cliaddr, &slen);
		if ( s == -1 ) {
			perror("accept");
			throw std::runtime_error("could not listen on socket");
		}

		eprintf("Connected: ");

		// A client has connected.

		if ( proto == AF_INET ) {
			char clihost[INET_ADDRSTRLEN];
			struct sockaddr_in *saddr= (struct sockaddr_in *) &cliaddr;

			if ( inet_ntop(proto, &(saddr->sin_addr), clihost,
				sizeof(clihost)) != NULL ) {

				eprintf("%s", clihost);
			} else eprintf("(could not translate network address)\n");
		} else if ( proto == AF_INET6 ) {
			char clihost[INET6_ADDRSTRLEN];

			if ( inet_ntop(proto, (struct sockaddr_in6 *) &cliaddr, clihost,
				sizeof(clihost)) != NULL ) {

				eprintf("%s", clihost);
			} else eprintf("(could not translate network address)\n");
		}
		eprintf("\n");

	}
}

MsgIO::~MsgIO()
{
}

int MsgIO::read(void **dest, size_t *sz)
{
	ssize_t bread= 0;
	bool repeat= true;
	int ws;

	if (use_stdio) return read_msg(dest, sz);

	/* 
	 * We don't know how many bytes are coming, so read until we find a
	 * newline.
	 */

	while (repeat) {
again:
		bread= recv(s, lbuffer, sizeof(lbuffer), 0);
		if ( bread == -1 ) {
			if ( errno == EINTR ) goto again;
			perror("recv");
			return -1;
		}
		if ( bread > 0 ) {
			size_t idx;

			rbuffer.append(lbuffer, bread);
			idx= rbuffer.find("\r\n");
			if ( idx == string::npos ) {
				idx= rbuffer.find('\n');
				if ( idx == string::npos ) continue;
				ws= 1;
			} else ws= 2;

			if ( idx == 0 ) return 1;
			else if ( idx %2 ) {
				eprintf("read odd byte count %zu\n", idx);
				return 0;
			}
			if ( sz != NULL ) *sz= idx/2;

			*dest= (char *) malloc(idx/2);
			if ( *dest == NULL ) {
				perror("malloc");
				return -1;
			}

			if (debug) {
				edividerWithText("read buffer");
				fwrite(rbuffer.c_str(), 1, idx, stdout);
				printf("\n");
				edivider();
			}

			from_hexstring((unsigned char *) *dest, rbuffer.c_str(), idx/2);
			rbuffer.erase(0, idx+ws);

			return 1;
		} else return 0;
	}
	
	return -1;
}

void MsgIO::send(void *src, size_t sz)
{
	ssize_t bsent;
	size_t len;

	if (use_stdio) {
		send_msg(src, sz);
		return;
	}

	wbuffer.append(hexstring(src, sz));
	wbuffer.append("\n");

	while ( len= wbuffer.length() ) {
again:
		bsent= ::send(s, wbuffer.c_str(), len, 0);
		if ( bsent == -1 ) {
			if (errno == EINTR) goto again;
			perror("send");
			return;
		}
		fwrite(wbuffer.c_str(), 1, bsent, stdout);

		if ( bsent == len ) {
			wbuffer.clear();
			return;
		}
		
		wbuffer.erase(0, bsent);
	}
}

void MsgIO::send_partial(void *src, size_t sz)
{
	if (use_stdio) {
		send_msg_partial(src, sz);
		return;
	}

	wbuffer.append(hexstring(src, sz));
}

/*
* Read a msg from stdin. There's a fixed portion and a variable-length
* payload.
*
* Our destination buffer is the entire message plus the payload,
* returned to the caller. This lets them cast it as an sgx_ra_msgN_t
* structure, and take advantage of the flexible array member at the
* end (if it has one, as sgx_ra_msg2_t and sgx_ra_msg3_t do).
*
* All messages are base16 encoded (ASCII hex strings) for simplicity
* and readability. We decode the message into the destination.
*
* We do not allow any whitespace in the incoming message, save for the
* terminating newline.
*
*/


/* Note that these were written back when this was pure C */

int read_msg(void **dest, size_t *sz)
{
	size_t bread;
	int repeat = 1;

	if (buffer == NULL) {
		buffer = (char *)malloc(buffer_size);
		if (buffer == NULL) {
			perror("malloc");
			return -1;
		}
	}

	bread = 0;
	while (repeat) {
		if (fgets(&buffer[bread], (int)(buffer_size - bread), stdin) == NULL) {
			if (ferror(stdin)) {
				perror("fgets");
				return -1;
			}
			else {
				fprintf(stderr, "EOF received\n");
				return 0;
			}
		}
		/* If the last char is not a newline, we have more reading to do */

		bread = strlen(buffer);
		if (bread == 0) {
			fprintf(stderr, "EOF received\n");
			return 0;
		}

		if (buffer[bread - 1] == '\n') {
			repeat = 0;
			--bread;	/* Discard the newline */
		}
		else {
			buffer_size += MSGIO_BUFFER_SZ;
			buffer = (char *) realloc(buffer, buffer_size);
			if (buffer == NULL) return -1;
		}
	}

	/* Make sure we didn't get \r\n */
	if (bread && buffer[bread - 1] == '\r') --bread;

	if (bread % 2) {
		fprintf(stderr, "read odd byte count %zu\n", bread);
		return 0;	/* base16 encoding = even number of bytes */
	}

	*dest = malloc(bread / 2);
	if (*dest == NULL) return -1;

	if (debug) {
		edividerWithText("read buffer");
		eputs(buffer);
		edivider();
	}

	from_hexstring((unsigned char *) *dest, buffer, bread / 2);

	if (sz != NULL) *sz = bread;

	return 1;
}

/* Send a partial message (no newline) */

void send_msg_partial(void *src, size_t sz) {
	if (sz) print_hexstring(stdout, src, sz);
#ifndef _WIN32
	/*
	* If were aren't writing to a tty, also print the message to stderr
	* so you can still see the output.
	*/
	if (!isatty(fileno(stdout))) print_hexstring(stderr, src, sz);
#endif
}

void send_msg(void *src, size_t sz)
{
	if (sz) print_hexstring(stdout, src, sz);
	printf("\n");
#ifndef _WIN32
	/* As above */
	if (!isatty(fileno(stdout))) {
		print_hexstring(stderr, src, sz);
		fprintf(stderr, "\n");
	}
#endif

	/*
	* Since we have both stdout and stderr, flush stdout to keep the
	* the output stream synchronized.
	*/

	fflush(stdout);
}


/* Send a partial message (no newline) */

void fsend_msg_partial(FILE *fp, void *src, size_t sz) {
	if (sz) print_hexstring(fp, src, sz);
}

void fsend_msg(FILE *fp, void *src, size_t sz)
{
	if (sz) print_hexstring(fp, src, sz);
	fprintf(fp, "\n");
	fflush(fp);
}

