/* HM-CFG-LAN emulation for HM-CFG-USB
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
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libusb-1.0/libusb.h>

#include "hexdump.h"
#include "hmcfgusb.h"

#define PID_FILE "/var/run/hmland.pid"

extern char *optarg;

static int impersonate_hmlanif = 0;
static int debug = 0;
static int verbose = 0;

struct queued_rx {
	char *rx;
	int len;
	struct queued_rx *next;
};

static struct queued_rx *qrx = NULL;
static int wait_for_h = 0;

#define	FLAG_LENGTH_BYTE	(1<<0)
#define	FLAG_FORMAT_HEX		(1<<1)
#define	FLAG_COMMA_BEFORE	(1<<2)
#define	FLAG_COMMA_AFTER	(1<<3)
#define	FLAG_NL			(1<<4)
#define	FLAG_IGNORE_COMMAS	(1<<5)

#define CHECK_SPACE(x)		if ((*outpos + x) > outend) { fprintf(stderr, "Not enough space!\n"); return 0; }
#define CHECK_AVAIL(x)		if ((*inpos + x) > inend) { fprintf(stderr, "Not enough input available!\n"); return 0; }

static void print_timestamp(FILE *f)
{
	struct timeval tv;
	struct tm *tmp;
	char ts[32];

	gettimeofday(&tv, NULL);
	tmp = localtime(&tv.tv_sec);
	memset(ts, 0, sizeof(ts));
	strftime(ts, sizeof(ts)-1, "%Y-%m-%d %H:%M:%S", tmp);
	fprintf(f, "%s.%06ld: ", ts, tv.tv_usec);
}

static int format_part_out(uint8_t **inpos, int inlen, uint8_t **outpos, int outlen, int len, int flags)
{
	const uint8_t nibble[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
		'A', 'B', 'C', 'D', 'E', 'F'};
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
		CHECK_AVAIL(len);
		CHECK_SPACE(len*2);
		for (i = 0; i < len; i++) {
			**outpos = nibble[((**inpos) & 0xf0) >> 4];
			*outpos += 1;
			**outpos = nibble[((**inpos) & 0xf)];
			*inpos += 1; *outpos += 1;
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

static uint8_t ascii_to_nibble(uint8_t a)
{
	uint8_t c = 0x00;

	if ((a >= '0') && (a <= '9')) {
		c = a - '0';
	} else if ((a >= 'A') && (a <= 'F')) {
		c = (a - 'A') + 10;
	} else if ((a >= 'a') && (a <= 'f')) {
		c = (a - 'a') + 10;
	}

	return c;
}

static int parse_part_in(uint8_t **inpos, int inlen, uint8_t **outpos, int outlen, int flags)
{
	uint8_t *buf_out = *outpos;
	uint8_t *outend = *outpos + outlen;
	uint8_t *inend = *inpos + inlen;

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

		**outpos = ascii_to_nibble(**inpos) << 4;
		*inpos += 1;
		**outpos |= ascii_to_nibble(**inpos);
		*inpos += 1; *outpos += 1;
	}

	return *outpos - buf_out;
}

static int hmlan_format_out(uint8_t *buf, int buf_len, void *data)
{
	uint8_t out[1024];
	uint8_t *outpos;
	uint8_t *inpos;
	int fd = *((int*)data);
	int w;

	if (buf_len < 1)
		return 1;

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

	/* Queue packet until first respone to 'K' is received */
	if (wait_for_h && buf[0] != 'H') {
		struct queued_rx **rxp = &qrx;

		while (*rxp)
			rxp = &((*rxp)->next);

		*rxp = malloc(sizeof(struct queued_rx));
		if (!*rxp) {
			perror("malloc");
			return 0;
		}

		memset(*rxp, 0, sizeof(struct queued_rx));
		(*rxp)->len = outpos-out;
		(*rxp)->rx = malloc((*rxp)->len);
		if (!(*rxp)->rx) {
			perror("malloc");
			return 0;
		}
		memset((*rxp)->rx, 0, (*rxp)->len);
		memcpy((*rxp)->rx, out, (*rxp)->len);

		return 1;
	}

	if (verbose) {
		int i;

		print_timestamp(stdout);
		printf("LAN < ");
		for (i = 0; i < outpos-out-2; i++)
			printf("%c", out[i]);
		printf("\n");
	}

	w = write(fd, out, outpos-out);
	if (w <= 0) {
		perror("write");
		return 0;
	}

	/* Send all queued packets */
	if (wait_for_h) {
		struct queued_rx *curr_rx = qrx;
		struct queued_rx *last_rx;

		while (curr_rx) {
			if (verbose) {
				int i;

				print_timestamp(stdout);
				printf("LAN < ");
				for (i = 0; i < curr_rx->len-2; i++)
					printf("%c", curr_rx->rx[i]);
				printf("\n");
			}

			w = write(fd, curr_rx->rx, curr_rx->len);
			if (w <= 0) {
				perror("write");
			}
			last_rx = curr_rx;
			curr_rx = curr_rx->next;

			free(last_rx->rx);
			free(last_rx);
		}

		qrx = NULL;

		wait_for_h = 0;
	}

	return 1;
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

			if (verbose) {
				print_timestamp(stdout);
				printf("LAN > ");
				for (i = 0; i < last; i++)
					printf("%c", instart[i]);
				printf("\n");
			}

			memset(out, 0, sizeof(out));
			*outpos++ = *inpos++;

			switch(*instart) {
				case 'S':
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), 0);
					parse_part_in(&inpos, (last-(inpos-instart)), &outpos, (sizeof(out)-(outpos-out)), FLAG_LENGTH_BYTE);
					break;
				case 'Y':
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
		if (errno != ECONNRESET)
			perror("read");
		return r;
	} else {
		return 0;
	}

	return 1;
}

static int comm(int fd_in, int fd_out, int master_socket, int flags)
{
	struct hmcfgusb_dev *dev;
	uint8_t out[0x40]; //FIXME!!!
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

	memset(out, 0, sizeof(out));
	out[0] = 'K';
	wait_for_h = 1;
	hmcfgusb_send_null_frame(dev, 1);
	hmcfgusb_send(dev, out, sizeof(out), 1);

	while(!quit) {
		int fd;

		fd = hmcfgusb_poll(dev, 1);	/* Wakeup device/bus at least once a second */
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
			} else {
				/* periodically wakeup the device */
				hmcfgusb_send_null_frame(dev, 1);
			}
		}
	}

	hmcfgusb_close(dev);
	return 1;
}

void sigterm_handler(int sig)
{
	if (unlink(PID_FILE) == -1)
		perror("Can't remove PID file");

	exit(EXIT_SUCCESS);
}

#define FLAG_DAEMON	(1 << 0)
#define FLAG_PID_FILE	(1 << 1)

static int socket_server(char *iface, int port, int flags)
{
	struct sigaction sact;
	struct sockaddr_in sin;
	int sock;
	int n;
	pid_t pid;

	if (flags & FLAG_DAEMON) {
		FILE *pidfile = NULL;

		if (flags & FLAG_PID_FILE) {
			int fd;

			fd = open(PID_FILE, O_CREAT | O_EXCL | O_WRONLY, 0644);
			if (fd == -1) {
				if (errno == EEXIST) {
					pid_t old_pid;
					pidfile = fopen(PID_FILE, "r");
					if (!pidfile) {
						perror("PID file " PID_FILE " already exists, already running?");
						exit(EXIT_FAILURE);
					}

					if (fscanf(pidfile, "%u", &old_pid) != 1) {
						fclose(pidfile);
						fprintf(stderr, "Can't read old PID from " PID_FILE ", already running?\n");
						exit(EXIT_FAILURE);
					}

					fclose(pidfile);

					fprintf(stderr, "Already running with PID %u according to " PID_FILE "!\n", old_pid);
					exit(EXIT_FAILURE);
				}
				perror("Can't create PID file " PID_FILE);
				exit(EXIT_FAILURE);
			}

			pidfile = fdopen(fd, "w");
			if (!pidfile) {
				perror("Can't reopen PID file fd");
				exit(EXIT_FAILURE);
			}

			memset(&sact, 0, sizeof(sact));
			sact.sa_handler = sigterm_handler;

			if (sigaction(SIGTERM, &sact, NULL) == -1) {
				perror("sigaction(SIGTERM)");
				exit(EXIT_FAILURE);
			}
		}

		pid = fork();
		if (pid > 0) {
			if (pidfile) {
				fprintf(pidfile, "%u\n", pid);
				fclose(pidfile);
			}

			printf("Daemon with PID %u started!\n", pid);
			exit(EXIT_SUCCESS);
		} else if (pid < 0) {
			perror("fork");
			exit(EXIT_FAILURE);
		}

		if (pidfile)
			fclose(pidfile);
	}

	memset(&sact, 0, sizeof(sact));
	sact.sa_handler = SIG_IGN;

	if (sigaction(SIGPIPE, &sact, NULL) == -1) {
		perror("sigaction(SIGPIPE)");
		exit(EXIT_FAILURE);
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
	if (!iface) {
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if (inet_pton(AF_INET, iface, &(sin.sin_addr.s_addr)) != 1) {
			perror("inet_ntop");
			return EXIT_FAILURE;
		}
	}

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
			print_timestamp(stdout);
			printf("Client %d.%d.%d.%d connected!\n",
					(client_addr & 0xff000000) >> 24,
					(client_addr & 0x00ff0000) >> 16,
					(client_addr & 0x0000ff00) >> 8,
					(client_addr & 0x000000ff));
		}

		comm(client, client, sock, flags);

		shutdown(client, SHUT_RDWR);
		close(client);

		if (verbose) {
			print_timestamp(stdout);
			printf("Connection to %d.%d.%d.%d closed!\n",
					(client_addr & 0xff000000) >> 24,
					(client_addr & 0x00ff0000) >> 16,
					(client_addr & 0x0000ff00) >> 8,
					(client_addr & 0x000000ff));
		}
		sleep(1);
	}

	return EXIT_SUCCESS;
}

static int interactive_server(int flags)
{
	if (!comm(STDIN_FILENO, STDOUT_FILENO, -1, flags))
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
	fprintf(stderr, "\t-l ip\tlisten on given IP address only (for example 127.0.0.1)\n");
	fprintf(stderr, "\t-P\tcreate PID file " PID_FILE " in daemon mode\n");
	fprintf(stderr, "\t-p n\tlisten on port n (default 1000)\n");
	fprintf(stderr, "\t-v\tverbose mode\n");

}

int main(int argc, char **argv)
{
	int port = 1000;
	char *iface = NULL;
	int interactive = 0;
	int flags = 0;
	char *ep;
	int opt;

	while((opt = getopt(argc, argv, "DdhiPp:Rl:v")) != -1) {
		switch (opt) {
			case 'D':
				debug = 1;
				verbose = 1;
				break;
			case 'd':
				flags |= FLAG_DAEMON;
				break;
			case 'i':
				interactive = 1;
				break;
			case 'P':
				flags |= FLAG_PID_FILE;
				break;
			case 'p':
				port = strtoul(optarg, &ep, 10);
				if (*ep != '\0') {
					fprintf(stderr, "Can't parse port!\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'R':
				fprintf(stderr, "-R is no longer needed (1s wakeup is default)\n");
				break;
			case 'l':
				iface = optarg;
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
		return interactive_server(flags);
	} else {
		return socket_server(iface, port, flags);
	}
}
