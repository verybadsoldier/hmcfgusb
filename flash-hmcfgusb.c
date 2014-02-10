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
	int ack;
};

static int parse_hmcfgusb(uint8_t *buf, int buf_len, void *data)
{
	struct recv_data *rdata = data;

	if (buf_len != 1)
		return 1;

	rdata->ack = buf[0];

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
	int pfd;
	int r;
	int i;
	int cnt;
	int pkt;
	int debug = 0;

	hmcfgusb_set_debug(debug);

	memset(&rdata, 0, sizeof(rdata));

	fd = open("hmusbif.enc", O_RDONLY);
	if (fd < 0) {
		perror("Can't open hmusbif.enc");
		exit(EXIT_FAILURE);
	}

	dev = hmcfgusb_init(parse_hmcfgusb, &rdata);
	if (!dev) {
		fprintf(stderr, "Can't initialize HM-CFG-USB\n");
		exit(EXIT_FAILURE);
	}

	if (!dev->bootloader) {
		fprintf(stderr, "HM-CFG-USB not in bootloader mode, entering bootloader.\n");
		hmcfgusb_enter_bootloader(dev);
		fprintf(stderr, "\nWaiting for device to reappear...\n");

		do {
			sleep(1);
		} while ((dev = hmcfgusb_init(parse_hmcfgusb, &rdata)) == NULL);

		if (!dev->bootloader) {
			fprintf(stderr, "Can't enter bootloader, giving up!\n");
			exit(EXIT_FAILURE);
		}
	}

	printf("HM-CFG-USB opened!\n");

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
		*outp++ = (pkt >> 8) & 0xff;
		*outp++ = pkt & 0xff;
		*outp++ = (len >> 8) & 0xff;
		*outp++ = len & 0xff;
		for (i = 0; i < r; i+=2) {
			*outp = (ascii_to_nibble(buf[i]) & 0xf)<< 4;
			*outp |= ascii_to_nibble(buf[i+1]) & 0xf;
			outp++;
		}
		cnt = outp - out;
		printf("Flashing %d bytes...\n", cnt);
		if (debug)
			hexdump(out, cnt, "F> ");

		rdata.ack = 0;
		if (!hmcfgusb_send(dev, out, cnt, 0)) {
			perror("hmcfgusb_send");
			exit(EXIT_FAILURE);
		}

		printf("Waiting for ack...\n");
		do {
			errno = 0;
			pfd = hmcfgusb_poll(dev, 1);
			if ((pfd < 0) && errno) {
				perror("hmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
			if (rdata.ack) {
				break;
			}
		} while (pfd < 0);

		if (rdata.ack == 2) {
			printf("Firmware update successfull!\n");
			break;
		}
		pkt++;
	} while (r > 0);

	hmcfgusb_close(dev);

	return EXIT_SUCCESS;
}
