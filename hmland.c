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

static int impersonate_hmlanif = 0;

static void hmlan_format_out(uint8_t *buf, int buf_len, void *data)
{
	char out[1024];
	char *pos;
	int fd = *((int*)data);
	int len;
	int i;

	if (buf_len < 1)
		return;

	memset(out, 0, sizeof(out));
	pos = out;

	*pos++ = buf[0];
	switch(buf[0]) {
		case 'H':
			if (impersonate_hmlanif) {
				buf[5] = 'L';
				buf[6] = 'A';
				buf[7] = 'N';
			}
			len = buf[1];
			for (i = 2; i < len + 2; i++) {
				*pos++=buf[i];
			}
			snprintf(pos, 7, ",%02X%02X,", buf[i], buf[i+1]);
			pos += 6;

			i += 2;
			len = buf[i]+i+1;
			i++;
			for (; i < len; i++) {
				*pos++=buf[i];
			}
			*pos++=',';
			len = i+12;
			for (; i < len; i++) {
				snprintf(pos, 3, "%02X", buf[i]);
				pos += 2;

				switch(len-i) {
					case 10:
					case 7:
					case 3:
						*pos++=',';
						break;
					default:
						break;
				}
			}

			break;
		case 'E':
			len = 13 + buf[13];
			for (i = 0; i < len; i++) {
				if (i != 12) {
					snprintf(pos, 3, "%02X", buf[i+1]);
					pos += 2;
				}
				switch(i) {
					case 2:
					case 4:
					case 8:
					case 9:
					case 11:
						*pos++=',';
						break;
					default:
						break;
				}
			}
			break;
		case 'R':
			len = 14 + buf[14];
			for (i = 0; i < len; i++) {
				if (i != 13) {
					snprintf(pos, 3, "%02X", buf[i+1]);
					pos += 2;
				}
				switch(i) {
					case 3:
					case 5:
					case 9:
					case 10:
					case 12:
						*pos++=',';
						break;
					default:
						break;
				}
			}
			break;
		case 'I':
			//HM> 0x0000: 49 00 00 00 00 55 53 42 2d 49 46 03 bc 0a 4a 45   I....USB-IF...JE
			//HM> 0x0010: 51 30 35 33 35 31 32 32 1d b1 55 68 ea 13 00 14   Q0535122..Uh....
			//HM> 0x0020: 9f a6 00 03 00 00 00 00 00 00 00 00 00 00 00 00   ................
			//HM> 0x0030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................ 
		default:
			for (i = 1; i < buf_len; i++) {
				snprintf(pos, 3, "%02X", buf[i]);
				pos += 2;
			}
			hexdump(buf, buf_len, "Unknown> ");
			break;
	}
	*pos++='\r';
	*pos++='\n';
	write(fd, out, pos-out);
}

static int hmlan_parse_in(int fd, void *data)
{
	struct hmcfgusb_dev *dev = data;
	unsigned char buf[1024];
	unsigned char send_buf[0x40]; //FIXME!!!
	char tmp[3];
	int i;
	int r;

	r = read(fd, buf, sizeof(buf));
	if (r > 0) {
		int cnt;

		memset(send_buf, 0, sizeof(send_buf));
		for (i = 0; i < r; i++) {
			if ((buf[i] == 0x0a) ||
					(buf[i] == 0x0d)) {
				r = i;
				break;
			}
		}

		send_buf[0] = buf[0];

		cnt = 0;
		for (i = 1; i < r; i++) {
			if (buf[i] == ',') {
				switch (buf[0]) {
					case 'S':
						if (cnt == 4) {
							/* Add msg length */
							memmove(buf+i+2, buf+i+1, r-(i+1));
							snprintf(tmp, 3, "%02X", (int)((r-(i+1))/2));
							memcpy(buf+i, tmp, 2);
							r++;
							break;
						}
					default:
						memmove(buf+i, buf+i+1, r-(i+1));
						r--;
						break;
				}
				cnt++;
			}
		}

		memset(tmp, 0, sizeof(tmp));
		for (i = 1; i < r; i+=2) {
			memcpy(tmp, buf + i, 2);
			send_buf[1+(i/2)] = strtoul(tmp, NULL, 16);
		}
		hmcfgusb_send(dev, send_buf, 1+(i/2), 1);
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

static int socket_server(int port)
{
	struct sockaddr_in sin;
	int sock;
	int n;

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

		memset(&csin, 0, sizeof(csin));
		csinlen = sizeof(csin);
		client = accept(sock, (struct sockaddr*)&csin, &csinlen);
		if (client == -1) {
			perror("Couldn't accept client");
			continue;
		}

		printf("Client accepted!\n");

		comm(client, client, sock);

		shutdown(client, SHUT_RDWR);
		close(client);

		printf("Connection closed!\n");

	}

	return EXIT_SUCCESS;
}

static int interactive_server(void)
{
	if (!comm(STDIN_FILENO, STDOUT_FILENO, -1))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	//return interactive_server();
	return socket_server(1234);
}
