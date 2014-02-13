/* flasher for HM-CFG-USB
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
#include "version.h"
#include "hmcfgusb.h"

/* This might be wrong, but it works for current fw */
#define MAX_BLOCK_LENGTH	512

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

static int validate_nibble(uint8_t a)
{
	if (((a >= '0') && (a <= '9')) ||
	    ((a >= 'A') && (a <= 'F')) ||
	    ((a >= 'a') && (a <= 'f')))
	    	return 1;

	return 0;
}

int main(int argc, char **argv)
{
	const char twiddlie[] = { '-', '\\', '|', '/' };
	struct hmcfgusb_dev *dev;
	struct recv_data rdata;
	struct stat stat_buf;
	uint8_t buf[4096];
	uint16_t len;
	uint8_t **fw = NULL;
	int fw_blocks = 0;
	int block;
	int fd;
	int pfd;
	int r;
	int i;
	int debug = 0;

	printf("HM-CFG-USB flasher version " VERSION "\n\n");

	if (argc != 2) {
		if (argc == 1)
			fprintf(stderr, "Missing firmware filename!\n\n");

		fprintf(stderr, "Syntax: %s hmusbif.enc\n\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (stat(argv[1], &stat_buf) == -1) {
		fprintf(stderr, "Can't stat %s: %s\n", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Can't open %s: %s", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	printf("Reading firmware from %s...\n", argv[1]);
	do {
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

		for (i = 0; i < r; i++) {
			if (!validate_nibble(buf[i])) {
				fprintf(stderr, "Firmware file not valid!\n");
				exit(EXIT_FAILURE);
			}
		}

		len = (ascii_to_nibble(buf[0]) & 0xf)<< 4;
		len |= ascii_to_nibble(buf[1]) & 0xf;
		len <<= 8;
		len |= (ascii_to_nibble(buf[2]) & 0xf)<< 4;
		len |= ascii_to_nibble(buf[3]) & 0xf;

		/* This might be wrong, but it works for current fw */
		if (len > MAX_BLOCK_LENGTH) {
			fprintf(stderr, "Invalid block-length %u > %u for block %d!\n", len, MAX_BLOCK_LENGTH, fw_blocks+1);
			exit(EXIT_FAILURE);
		}

		fw = realloc(fw, sizeof(uint8_t*) * (fw_blocks + 1));
		if (fw == NULL) {
			perror("Can't reallocate fw-blocklist");
			exit(EXIT_FAILURE);
		}

		fw[fw_blocks] = malloc(len + 4);
		if (fw[fw_blocks] == NULL) {
			perror("Can't allocate memory for fw-block");
			exit(EXIT_FAILURE);
		}

		fw[fw_blocks][0] = (fw_blocks >> 8) & 0xff;
		fw[fw_blocks][1] = fw_blocks & 0xff;
		fw[fw_blocks][2] = (len >> 8) & 0xff;
		fw[fw_blocks][3] = len & 0xff;

		r = read(fd, buf, len * 2);
		if (r < 0) {
			perror("read");
			exit(EXIT_FAILURE);
		} else if (r < len * 2) {
			fprintf(stderr, "short read, aborting (%d < %d)\n", r, len * 2);
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < r; i+=2) {
			if ((!validate_nibble(buf[i])) ||
			    (!validate_nibble(buf[i+1]))) {
				fprintf(stderr, "Firmware file not valid!\n");
				exit(EXIT_FAILURE);
			}

			fw[fw_blocks][(i/2) + 4] = (ascii_to_nibble(buf[i]) & 0xf)<< 4;
			fw[fw_blocks][(i/2) + 4] |= ascii_to_nibble(buf[i+1]) & 0xf;
		}

		fw_blocks++;
		if (debug)
			printf("Firmware block %d with length %u read.\n", fw_blocks, len);
	} while(r > 0);

	if (fw_blocks == 0) {
		fprintf(stderr, "Firmware file not valid!\n");
		exit(EXIT_FAILURE);
	}

	printf("Firmware with %d blocks successfully read.\n", fw_blocks);

	hmcfgusb_set_debug(debug);

	memset(&rdata, 0, sizeof(rdata));

	dev = hmcfgusb_init(parse_hmcfgusb, &rdata);
	if (!dev) {
		fprintf(stderr, "Can't initialize HM-CFG-USB\n");
		exit(EXIT_FAILURE);
	}

	if (!dev->bootloader) {
		fprintf(stderr, "\nHM-CFG-USB not in bootloader mode, entering bootloader.\n");
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

	printf("\nHM-CFG-USB opened.\n\n");


	printf("Flasing %d blocks", fw_blocks);
	if (debug) {
		printf("\n");
	} else {
		printf(": %c", twiddlie[0]);
		fflush(stdout);
	}

	for (block = 0; block < fw_blocks; block++) {
		len = fw[block][2] << 8;
		len |= fw[block][3];

		len += 4; /* block nr., length */

		if (debug)
			hexdump(fw[block], len, "F> ");

		rdata.ack = 0;
		if (!hmcfgusb_send(dev, fw[block], len, 0)) {
			perror("\n\nhmcfgusb_send");
			exit(EXIT_FAILURE);
		}

		if (debug)
			printf("Waiting for ack...\n");
		do {
			errno = 0;
			pfd = hmcfgusb_poll(dev, 1);
			if ((pfd < 0) && errno) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
			if (rdata.ack) {
				break;
			}
		} while (pfd < 0);

		if (rdata.ack == 2) {
			printf("\n\nFirmware update successfull!\n");
			break;
		}

		if (rdata.ack != 1) {
			fprintf(stderr, "\n\nError flashing block %d, status: %u\n", block, rdata.ack);
			exit(EXIT_FAILURE);
		}

		if (!debug) {
			printf("\b%c", twiddlie[block % sizeof(twiddlie)]);
			fflush(stdout);
		}
		free(fw[block]);
	}

	free(fw);

	hmcfgusb_close(dev);

	return EXIT_SUCCESS;
}
