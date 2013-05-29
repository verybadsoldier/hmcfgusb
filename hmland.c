/* HM-CFG-LAN emuldation for HM-CFG-USB
 *
 * Copyright (c) 2013 Michael Gernoth <michael@gernoth.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libusb-1.0/libusb.h>

#include "hexdump.h"
#include "hmcfgusb.h"

extern char *optarg;

static int impersonate_hmlanif = 0;
static int debug = 0;
static int verbose = 0;

#define	FLAG_LENGTH_BYTE	(1<<0)
#define	FLAG_FORMAT_HEX		(1<<1)
#define	FLAG_COMMA_BEFORE	(1<<2)
#define	FLAG_COMMA_AFTER	(1<<3)
#define	FLAG_NL			(1<<4)
#define	FLAG_IGNORE_COMMAS	(1<<5)

#define CHECK_SPACE(x)		if ((*outpos + x) > outend) { fprintf(stderr, "Not enough space!\n"); return 0; }
#define CHECK_AVAIL(x)		if ((*inpos + x) > inend) { fprintf(stderr, "Not enough input available!\n"); return 0; }

static int format_part_out(uint8_t **inpos, int inlen, uint8_t **outpos, int outlen, int len, int flags)
{
	uint8_t *buf_out = *outpos;
	uint8_t *outend = *outpos + outlen;
	uint8_t *inend = *inpos + inlen;
	int i;

	if (flags & FLAG_COMMA_BEFORE) {
		CHECK_SPACE(1);
		**outpos=',';
		*outpos += 1;
	}

	if (flags & FLAG_LENGTH_BYTE) {
		CHECK_AVAIL(1);
		len = **inpos;
		*inpos += 1;
	}

	if (flags & FLAG_FORMAT_HEX) {
		char hex[3];

		memset(hex, 0, sizeof(hex));

		CHECK_AVAIL(len);
		CHECK_SPACE(len*2);
		for (i = 0; i < len; i++) {
			if (snprintf(hex, sizeof(hex), "%02X", **inpos) != 2) {
				fprintf(stderr, "Can't format hex-string!\n");
				return 0;
			}
			*inpos += 1;
			memcpy(*outpos, hex, 2);
			*outpos += 2;
		}
	} else {
		CHECK_AVAIL(len);
		CHECK_SPACE(len);
		memcpy(*outpos, *inpos, len);
		*outpos += len;
		*inpos += len;
	}

	if (flags & FLAG_COMMA_AFTER) {
		CHECK_SPACE(1);
		**outpos=',';
		*outpos += 1;
	}

	if (flags & FLAG_NL) {
		CHECK_SPACE(2);
		**outpos='\r';
		*outpos += 1;
		**outpos='\n';
		*outpos += 1;
	}

	return *outpos - buf_out;
}

static int parse_part_in(uint8_t **inpos, int inlen, uint8_t **outpos, int outlen, int flags)
{
	uint8_t *buf_out = *outpos;
	uint8_t *outend = *outpos + outlen;
	uint8_t *inend = *inpos + inlen;
	char hex[3];

	memset(hex, 0, sizeof(hex));

	if (flags & FLAG_LENGTH_BYTE) {
		int len = 0;
		uint8_t *ip;

		ip = *inpos;
		while(ip < inend) {
			if (*ip == ',') {
				ip++;
				if (!(flags & FLAG_IGNORE_COMMAS))
					break;

				continue;
			}
			len++;
			ip++;
		}
		CHECK_SPACE(1);
		**outpos = (len / 2);
		*outpos += 1;
	}

	while(*inpos < inend) {
		if (**inpos == ',') {
			*inpos += 1;
			if (!(flags & FLAG_IGNORE_COMMAS))
				break;

			continue;
		}

		CHECK_SPACE(1);
		CHECK_AVAIL(2);
		memcpy(hex, *inpos, 2);
		*inpos += 2;

		**outpos = strtoul(hex, NULL, 16);
		*outpos += 1;
	}

	return *outpos - buf_out;
}

static void hmlan_format_out(uint8_t *buf, int buf_len, void *data)
{
	uint8_t out[1024];
	uint8_t *outpos;
	uint8_t *inpos;
	int fd = *((int*)data);

	if (buf_len < 1)
		return;

	memset(out, 0, sizeof(out));
	outpos = out;
	inpos = buf;

	format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, 0);
	switch(buf[0]) {
		case 'H':
			if (impersonate_hmlanif) {
				buf[5] = 'L';
				buf[6] = 'A';
				buf[7] = 'N';
			}
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 0, FLAG_LENGTH_BYTE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 0, FLAG_COMMA_BEFORE | FLAG_LENGTH_BYTE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 3, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 3, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 4, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE | FLAG_NL);

			break;
		case 'E':
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 3, FLAG_FORMAT_HEX);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 4, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 0, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE | FLAG_LENGTH_BYTE | FLAG_NL);

			break;
		case 'R':
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 4, FLAG_FORMAT_HEX);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 4, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 2, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 0, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE | FLAG_LENGTH_BYTE | FLAG_NL);

			break;
		case 'I':
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE);
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), 1, FLAG_FORMAT_HEX | FLAG_COMMA_BEFORE | FLAG_NL);

			break;
		default:
			format_part_out(&inpos, (buf_len-(inpos-buf)), &outpos, (sizeof(out)-(outpos-out)), buf_len-1, FLAG_FORMAT_HEX | FLAG_NL);
			hexdump(buf, buf_len, "Unknown> ");
			break;
	}
	write(fd, out, outpos-out);
	if (debug)
		fprintf(stderr, "LAN < %s\n", out);
}

static int hmlan_parse_in(int fd, void *data)
{
	struct hmcfgusb_dev *dev = data;
	uint8_t buf[1025];
	uint8_t out[0x40]; //FIXME!!!
	uint8_t *outpos;
	uint8_t *inpos;
	int i;
	int last;
	int r;

	memset(buf, 0, sizeof(buf));

	r = read(fd, buf, sizeof(buf)-1);
	if (r > 0) {
		uint8_t *inend = buf + r;

		inpos = buf;

		if (debug)
			fprintf(stderr, "\nLAN > %s", buf);

		while (inpos < inend) {
			uint8_t *instart = inpos;

			if ((*inpos == '\r') || (*inpos == '\n')) {
				inpos++;
				continue;
			}
			
			outpos = out;

			last = inend - inpos;

			for (i = 0; i < last; i++) {
				if ((inpos[i] == '\r') || (inpos[i] == '\n')) {
					last = i;
					break;
				}
			}

			if (last == 0)
				continue;

			memset(out, 0, sizeof(out));
			*outpos++ = *inpos++;

			switch(buf[0]) {
				case 'S':
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), FLAG_LENGTH_BYTE);
					break;
				default:
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), FLAG_IGNORE_COMMAS);
					break;
			}

			hmcfgusb_send(dev, out, sizeof(out), 1);
		}
	} else if (r < 0) {
		perror("read");
		return r;
	} else {
		return 0;
	}

	return 1;
}

static int comm(int fd_in, int fd_out, int master_socket)
{
	struct hmcfgusb_dev *dev;
	int quit = 0;

	hmcfgusb_set_debug(debug);

	dev = hmcfgusb_init(hmlan_format_out, &fd_out);
	if (!dev) {
		fprintf(stderr, "Can't initialize HM-CFG-USB!\n");
		return 0;
	}

	if (!hmcfgusb_add_pfd(dev, fd_in, POLLIN)) {
		fprintf(stderr, "Can't add client to pollfd!\n");
		hmcfgusb_close(dev);
		return 0;
	}

	if (master_socket >= 0) {
		if (!hmcfgusb_add_pfd(dev, master_socket, POLLIN)) {
			fprintf(stderr, "Can't add master_socket to pollfd!\n");
			hmcfgusb_close(dev);
			return 0;
		}
	}

	hmcfgusb_send(dev, (unsigned char*)"K", 1, 1);

	while(!quit) {
		int fd;

		fd = hmcfgusb_poll(dev, 3600);
		if (fd >= 0) {
			if (fd == master_socket) {
				int client;

				client = accept(master_socket, NULL, 0);
				if (client >= 0) {
					shutdown(client, SHUT_RDWR);
					close(client);
				}
			} else {
				if (hmlan_parse_in(fd, dev) <= 0) {
					quit = 1;
				}
			}
		} else if (fd == -1) {
			if (errno) {
				perror("hmcfgusb_poll");
				quit = 1;
			}
		}
	}

	hmcfgusb_close(dev);
	return 1;
}

static int socket_server(int port, int daemon)
{
	struct sockaddr_in sin;
	int sock;
	int n;
	pid_t pid;

	if (daemon) {
		pid = fork();
		if (pid > 0) {
			printf("Daemon with PID %u started!\n", pid);
			exit(EXIT_SUCCESS);
		} else if (pid < 0) {
			perror("fork");
			exit(EXIT_FAILURE);
		}
	}

	impersonate_hmlanif = 1;

	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == -1) {
		perror("Can't open socket");
		return EXIT_FAILURE;
	}

	n = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n)) == -1) {
		perror("Can't set socket options");
		return EXIT_FAILURE;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
		perror("Can't bind socket");
		return EXIT_FAILURE;
	}

	if (listen(sock, 1) == -1) {
		perror("Can't listen on socket");
		return EXIT_FAILURE;
	}

	while(1) {
		struct sockaddr_in csin;
		socklen_t csinlen;
		int client;
		in_addr_t client_addr;

		memset(&csin, 0, sizeof(csin));
		csinlen = sizeof(csin);
		client = accept(sock, (struct sockaddr*)&csin, &csinlen);
		if (client == -1) {
			perror("Couldn't accept client");
			continue;
		}

		/* FIXME: getnameinfo... */
		client_addr = ntohl(csin.sin_addr.s_addr);

		if (verbose) {
			printf("Client %d.%d.%d.%d connected!\n",
					(client_addr & 0xff000000) >> 24,
					(client_addr & 0x00ff0000) >> 16,
					(client_addr & 0x0000ff00) >> 8,
					(client_addr & 0x000000ff));
		}

		comm(client, client, sock);

		shutdown(client, SHUT_RDWR);
		close(client);

		if (verbose) {
			printf("Connection to %d.%d.%d.%d closed!\n",
					(client_addr & 0xff000000) >> 24,
					(client_addr & 0x00ff0000) >> 16,
					(client_addr & 0x0000ff00) >> 8,
					(client_addr & 0x000000ff));
		}

	}

	return EXIT_SUCCESS;
}

static int interactive_server(void)
{
	if (!comm(STDIN_FILENO, STDOUT_FILENO, -1))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

void hmlan_syntax(char *prog)
{
	fprintf(stderr, "Syntax: %s options\n\n", prog);
	fprintf(stderr, "Possible options:\n");
	fprintf(stderr, "\t-D\tdebug mode\n");
	fprintf(stderr, "\t-d\tdaemon mode\n");
	fprintf(stderr, "\t-h\tthis help\n");
	fprintf(stderr, "\t-i\tinteractive mode (connect HM-CFG-USB to terminal)\n");
	fprintf(stderr, "\t-p n\tlisten on port n (default 1000)\n");
	fprintf(stderr, "\t-v\tverbose mode\n");

}

int main(int argc, char **argv)
{
	int port = 1000;
	int interactive = 0;
	int daemon = 0;
	char *ep;
	int opt;

	while((opt = getopt(argc, argv, "Ddhip:v")) != -1) {
		switch (opt) {
			case 'D':
				debug = 1;
				verbose = 1;
				break;
			case 'd':
				daemon = 1;
				break;
			case 'i':
				interactive = 1;
				break;
			case 'p':
				port = strtoul(optarg, &ep, 10);
				if (*ep != '\0') {
					fprintf(stderr, "Can't parse port!\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'v':
				verbose = 1;
				break;
			case 'h':
			case ':':
			case '?':
			default:
				hmlan_syntax(argv[0]);
				exit(EXIT_FAILURE);
				break;
		}
	}
	
	if (interactive) {
		return interactive_server();
	} else {
		return socket_server(port, daemon);
	}
}
