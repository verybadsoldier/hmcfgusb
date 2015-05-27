/* HM-sniffer for HM-CFG-USB
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
#include <sys/time.h>
#include <libusb-1.0/libusb.h>

#include "version.h"
#include "hexdump.h"
#include "hmcfgusb.h"

/* See HMConfig.pm */
char *hm_message_types(uint8_t type)
{
	switch(type) {
		case 0x00:
			return "Device Info";
			break;
		case 0x01:
			return "Configuration";
			break;
		case 0x02:
			return "Acknowledge";
			break;
		case 0x03:
			return "AES";
			break;
		case 0x04:
			return "AES-Key";
			break;
		case 0x10:
			return "Information";
			break;
		case 0x11:
			return "SET";
			break;
		case 0x12:
			return "HAVE_DATA";
			break;
		case 0x3e:
			return "Switch";
			break;
		case 0x3f:
			return "Timestamp";
			break;
		case 0x40:
			return "Remote";
			break;
		case 0x41:
			return "Sensor";
			break;
		case 0x53:
			return "Water sensor";
			break;
		case 0x58:
			return "Climate event";
			break;
		case 0x70:
			return "Weather event";
			break;
		default:
			return "?";
			break;
	}
}

static void dissect_hm(uint8_t *buf, int len)
{
	struct timeval tv;
	struct tm *tmp;
	char ts[32];
	int i;

	gettimeofday(&tv, NULL);
	tmp = localtime(&tv.tv_sec);
	memset(ts, 0, sizeof(ts));
	strftime(ts, sizeof(ts)-1, "%Y-%m-%d %H:%M:%S", tmp);
	printf("%s.%06ld: ", ts, tv.tv_usec);

	for (i = 0; i < len; i++) {
		printf("%02X", buf[i]);
	}
	printf("\n");
	printf("Packet information:\n");
	printf("\tLength: %u\n", buf[0]);
	printf("\tMessage ID: %u\n", buf[1]);
	printf("\tSender: %02x%02x%02x\n", buf[4], buf[5], buf[6]);
	printf("\tReceiver: %02x%02x%02x\n", buf[7], buf[8], buf[9]);
	printf("\tControl Byte: 0x%02x\n", buf[2]);
	printf("\t\tFlags: ");
	if (buf[2] & (1 << 0)) printf("WAKEUP ");
	if (buf[2] & (1 << 1)) printf("WAKEMEUP ");
	if (buf[2] & (1 << 2)) printf("CFG ");
	if (buf[2] & (1 << 3)) printf("? ");
	if (buf[2] & (1 << 4)) printf("BURST ");
	if (buf[2] & (1 << 5)) printf("BIDI ");
	if (buf[2] & (1 << 6)) printf("RPTED ");
	if (buf[2] & (1 << 7)) printf("RPTEN ");
	printf("\n");
	printf("\tMessage type: %s (0x%02x)\n", hm_message_types(buf[3]), buf[3]);
	printf("\tMesage: ");
	for (i = 10; i < len; i++) {
		printf("%02X", buf[i]);
	}
	printf("\n");

	printf("\n");

}

struct recv_data {
	int wrong_hmid;
};

static int parse_hmcfgusb(uint8_t *buf, int buf_len, void *data)
{
	struct recv_data *rdata = data;

	if (buf_len < 1)
		return 1;

	switch(buf[0]) {
		case 'E':
			dissect_hm(buf + 13, buf[13] + 1);
			break;
		case 'H':
			if ((buf[27] != 0x00) ||
			    (buf[28] != 0x00) ||
			    (buf[29] != 0x00)) {
				printf("hmId is currently set to: %02x%02x%02x\n", buf[27], buf[28], buf[29]);
				rdata->wrong_hmid = 1;
			}
			break;
		case 'R':
		case 'I':
			break;
		default:
			hexdump(buf, buf_len, "Unknown> ");
			break;
	}

	return 1;
}

int main(int argc, char **argv)
{
	struct hmcfgusb_dev *dev;
	struct recv_data rdata;
	int quit = 0;

	hmcfgusb_set_debug(0);

	do {
		memset(&rdata, 0, sizeof(rdata));
		rdata.wrong_hmid = 0;

		dev = hmcfgusb_init(parse_hmcfgusb, &rdata);
		if (!dev) {
			fprintf(stderr, "Can't initialize HM-CFG-USB, retrying in 1s...\n");
			sleep(1);
			continue;
		}
		printf("HM-CFG-USB opened!\n");

		hmcfgusb_send_null_frame(dev, 1);
		hmcfgusb_send(dev, (unsigned char*)"K", 1, 1);

		while(!quit) {
			int fd;

			if (rdata.wrong_hmid) {
				printf("changing hmId to 000000, this might reboot the device!\n");
				hmcfgusb_send(dev, (unsigned char*)"A\00\00\00", 4, 1);
				rdata.wrong_hmid = 0;
				hmcfgusb_send(dev, (unsigned char*)"K", 1, 1);
			}
			fd = hmcfgusb_poll(dev, 1000);
			if (fd >= 0) {
				fprintf(stderr, "activity on unknown fd %d!\n", fd);
				continue;
			} else if (fd == -1) {
				if (errno) {
					if (errno != ETIMEDOUT) {
						perror("hmcfgusb_poll");
						break;
					} else {
						/* periodically wakeup the device */
						hmcfgusb_send_null_frame(dev, 1);
					}
				}
			}
		}

		hmcfgusb_close(dev);
	} while (!quit);

	hmcfgusb_exit();

	return EXIT_SUCCESS;
}
