/* (not working) flasher for HM-CFG-USB
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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <libusb-1.0/libusb.h>

#include "hexdump.h"
#include "hmcfgusb.h"

struct recv_data {
};

static int parse_hmcfgusb(uint8_t *buf, int buf_len, void *data)
{
	if (buf_len < 1)
		return 1;

	switch(buf[0]) {
		case 'E':
		case 'H':
		case 'R':
		case 'I':
			break;
		default:
			hexdump(buf, buf_len, "Unknown> ");
			break;
	}

	return 1;
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

int main(int argc, char **argv)
{
	struct hmcfgusb_dev *dev;
	struct recv_data rdata;
	uint8_t out[4096];
	uint8_t buf[4096];
	uint8_t *outp;
	int fd;
	int r;
	int i;
	int cnt;
	int pkt;

	hmcfgusb_set_debug(0);

	memset(&rdata, 0, sizeof(rdata));

	dev = hmcfgusb_init(parse_hmcfgusb, &rdata);
	if (!dev) {
		fprintf(stderr, "Can't initialize HM-CFG-USB\n");
		exit(EXIT_FAILURE);
	}
	printf("HM-CFG-USB opened!\n");

	fd = open("hmusbif.enc", O_RDONLY);
	if (fd < 0) {
		perror("Can't open hmusbif.enc");
		exit(EXIT_FAILURE);
	}

	cnt = 0;
	pkt = 0;
	do {
		int len;

		memset(buf, 0, sizeof(buf));
		r = read(fd, buf, 4);
		if (r < 0) {
			perror("read");
			exit(EXIT_FAILURE);
		} else if (r == 0) {
			break;
		} else if (r != 4) {
			printf("can't get length information!\n");
			exit(EXIT_FAILURE);
		}

		len = (ascii_to_nibble(buf[0]) & 0xf)<< 4;
		len |= ascii_to_nibble(buf[1]) & 0xf;
		len <<= 8;
		len |= (ascii_to_nibble(buf[2]) & 0xf)<< 4;
		len |= ascii_to_nibble(buf[3]) & 0xf;

		printf("packet length: %x\n", len);

		r = read(fd, buf, len * 2);
		if (r < 0) {
			perror("read");
			exit(EXIT_FAILURE);
		} else if (r < len * 2) {
			printf("short read, aborting (%d < %d)\n", r, len * 2);
			break;
		}

		memset(out, 0, sizeof(out));
		outp = out;
		*outp++ = 'W';
		*outp++ = (pkt >> 8) & 0xff;
		*outp++ = pkt & 0xff;
		*outp++ = (len >> 8) & 0xff;
		*outp++ = len & 0xff;
		for (i = 0; i < r; i+=2) {
			*outp = (ascii_to_nibble(buf[i]) & 0xf)<< 4;
			*outp |= ascii_to_nibble(buf[i+1]) & 0xf;
			outp++;
		}
		cnt += r/2;
		printf("Flashing %d bytes...\n", cnt);
		hexdump(out, outp-out, "F> ");
		//hmcfgusb_send(dev, out, r/2, 1);
		pkt++;
	} while (r > 0);

	hmcfgusb_close(dev);

	return EXIT_SUCCESS;
}
