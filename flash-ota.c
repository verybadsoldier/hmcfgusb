/* flasher for HomeMatic-devices supporting OTA updates
 *
 * Copyright (c) 2014 Michael Gernoth <michael@gernoth.net>
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
#include "firmware.h"
#include "hm.h"
#include "version.h"
#include "hmcfgusb.h"

uint32_t hmid = 0;
uint32_t my_hmid = 0;

enum message_type {
	MESSAGE_TYPE_E,
	MESSAGE_TYPE_R,
};

struct recv_data {
	uint8_t message[64];
	enum message_type message_type;
	uint16_t status;
	int speed;
	uint16_t hmcfgusb_version;
};

static int parse_hmcfgusb(uint8_t *buf, int buf_len, void *data)
{
	struct recv_data *rdata = data;

	if (buf_len < 1)
		return 1;

	switch (buf[0]) {
		case 'E':
			if ((!hmid) ||
			    ((buf[0x11] == ((hmid >> 16) & 0xff)) &&
			    (buf[0x12] == ((hmid >> 8) & 0xff)) &&
			    (buf[0x13] == (hmid & 0xff)))) {
				memset(rdata->message, 0, sizeof(rdata->message));
				memcpy(rdata->message, buf + 0x0d, buf[0x0d] + 1);
				rdata->message_type = MESSAGE_TYPE_E;
			}
			break;
		case 'R':
			memset(rdata->message, 0, sizeof(rdata->message));
			memcpy(rdata->message, buf + 0x0e, buf[0x0e] + 1);
			rdata->status = (buf[5] << 8) | buf[6];
			rdata->message_type = MESSAGE_TYPE_R;
			break;
		case 'G':
			rdata->speed = buf[1];
			break;
		case 'H':
			rdata->hmcfgusb_version = (buf[11] << 8) | buf[12];
			my_hmid = (buf[0x1b] << 16) | (buf[0x1c] << 8) | buf[0x1d];
			break;
		default:
			break;
	}

	if (buf_len != 1)
		return 1;

	return 1;
}

int send_hm_message(struct hmcfgusb_dev *dev, struct recv_data *rdata, uint8_t *msg)
{
	static uint32_t id = 1;
	struct timeval tv;
	uint8_t out[0x40];
	int pfd;

	if (gettimeofday(&tv, NULL) == -1) {
		perror("gettimeofay");
			return 0;
	}

	memset(out, 0, sizeof(out));

	out[0] = 'S';
	out[1] = (id >> 24) & 0xff;
	out[2] = (id >> 16) & 0xff;
	out[3] = (id >> 8) & 0xff;
	out[4] = id & 0xff;
	out[10] = 0x01;
	out[11] = (tv.tv_usec >> 24) & 0xff;
	out[12] = (tv.tv_usec >> 16) & 0xff;
	out[13] = (tv.tv_usec >> 8) & 0xff;
	out[14] = tv.tv_usec & 0xff;
	

	memcpy(&out[0x0f], msg, msg[0] + 1);

	memset(rdata, 0, sizeof(struct recv_data));
	hmcfgusb_send(dev, out, sizeof(out), 1);

	while (1) {
		if (rdata->message_type == MESSAGE_TYPE_R) {
			if (((rdata->status & 0xff) == 0x01) ||
			    ((rdata->status & 0xff) == 0x02)) {
			    	break;
			} else {
				fprintf(stderr, "\nInvalid status: %04x\n", rdata->status);
				return 0;
			}
		}
		errno = 0;
		pfd = hmcfgusb_poll(dev, 1);
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
		}
	}

	id++;
	return 1;
}

static int switch_speed(struct hmcfgusb_dev *dev, struct recv_data *rdata, uint8_t speed)
{
	uint8_t out[0x40];
	int pfd;

	printf("Entering %uk-mode\n", speed);

	memset(out, 0, sizeof(out));
	out[0] = 'G';
	out[1] = speed;

	hmcfgusb_send(dev, out, sizeof(out), 1);

	while (1) {
		errno = 0;
		pfd = hmcfgusb_poll(dev, 1);
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
		}
		if (rdata->speed == speed)
			break;
	}

	return 1;
}

int main(int argc, char **argv)
{
	const char twiddlie[] = { '-', '\\', '|', '/' };
	const uint8_t switch_msg[] = { 0x10, 0x5B, 0x11, 0xF8, 0x15, 0x47 };
	struct hmcfgusb_dev *dev;
	struct recv_data rdata;
	uint8_t out[0x40];
	uint8_t *pos;
	uint8_t msgid = 0x1;
	uint16_t len;
	struct firmware *fw;
	int block;
	int pfd;
	int debug = 0;
	int cnt;
	int switchcnt = 0;
	int msgnum = 0;
	int switched = 0;

	printf("HomeMatic OTA flasher version " VERSION "\n\n");

	if (argc != 3) {
		if (argc == 1)
			fprintf(stderr, "Missing firmware filename!\n\n");

		if (argc == 2)
			fprintf(stderr, "Missing serial!\n\n");

		fprintf(stderr, "Syntax: %s firmware.eq3 SERIALNUMBER\n\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	fw = firmware_read_firmware(argv[1], debug);
	if (!fw)
		exit(EXIT_FAILURE);

	hmcfgusb_set_debug(debug);

	memset(&rdata, 0, sizeof(rdata));

	dev = hmcfgusb_init(parse_hmcfgusb, &rdata);
	if (!dev) {
		fprintf(stderr, "Can't initialize HM-CFG-USB\n");
		exit(EXIT_FAILURE);
	}

	if (dev->bootloader) {
		fprintf(stderr, "\nHM-CFG-USB in bootloader mode, aborting!\n");
		exit(EXIT_FAILURE);
	}

	printf("\nHM-CFG-USB opened\n\n");

	memset(out, 0, sizeof(out));
	out[0] = 'K';
	hmcfgusb_send(dev, out, sizeof(out), 1);

	while (1) {
		errno = 0;
		pfd = hmcfgusb_poll(dev, 1);
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
		}
		if (rdata.hmcfgusb_version)
			break;
	}

	if (rdata.hmcfgusb_version < 0x3c7) {
		fprintf(stderr, "HM-CFG-USB firmware too low: %u < 967\n", rdata.hmcfgusb_version);
		exit(EXIT_FAILURE);
	}

	printf("HM-CFG-USB firmware version: %u\n", rdata.hmcfgusb_version);

	if (!switch_speed(dev, &rdata, 10)) {
		fprintf(stderr, "Can't switch speed!\n");
		exit(EXIT_FAILURE);
	}

	printf("Waiting for device with serial %s\n", argv[2]);

	while (1) {
		errno = 0;
		pfd = hmcfgusb_poll(dev, 1);
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
		}

		if ((rdata.message[LEN] == 0x14) && /* Length */
		    (rdata.message[MSGID] == 0x00) && /* Message ID */
		    (rdata.message[CTL] == 0x00) && /* Control Byte */
		    (rdata.message[TYPE] == 0x10) && /* Messagte type: Information */
		    (DST(rdata.message) == 0x000000) && /* Broadcast */
		    (rdata.message[PAYLOAD] == 0x00) && /* FUP? */
		    (rdata.message[PAYLOAD+2] == 'E') &&
		    (rdata.message[PAYLOAD+3] == 'Q')) {
			if (!strncmp((char*)&(rdata.message[0x0b]), argv[2], 10)) {
				hmid = SRC(rdata.message);
				break;
			}
		}
	}

	printf("Device with serial %s (hmid: %06x) entered firmware-update-mode\n", argv[2], hmid);

	printf("Adding HMID\n");

	memset(out, 0, sizeof(out));
	out[0] = '+';
	out[1] = (hmid >> 16) & 0xff;
	out[2] = (hmid >> 8) & 0xff;
	out[3] = hmid & 0xff;

	hmcfgusb_send(dev, out, sizeof(out), 1);

	switchcnt = 3;
	do {
		printf("Initiating remote switch to 100k\n");

		memset(out, 0, sizeof(out));

		out[MSGID] = msgid++;
		out[CTL] = 0x00;
		out[TYPE] = 0xCB;
		SET_SRC(out, my_hmid);
		SET_DST(out, hmid);

		memcpy(&out[PAYLOAD], switch_msg, sizeof(switch_msg));
		SET_LEN_FROM_PAYLOADLEN(out, sizeof(switch_msg));

		if (!send_hm_message(dev, &rdata, out)) {
			exit(EXIT_FAILURE);
		}

		if (!switch_speed(dev, &rdata, 100)) {
			fprintf(stderr, "Can't switch speed!\n");
			exit(EXIT_FAILURE);
		}

		printf("Has the device switched?\n");

		memset(out, 0, sizeof(out));

		out[MSGID] = msgid++;
		out[CTL] = 0x20;
		out[TYPE] = 0xCB;
		SET_SRC(out, my_hmid);
		SET_DST(out, hmid);

		memcpy(&out[PAYLOAD], switch_msg, sizeof(switch_msg));
		SET_LEN_FROM_PAYLOADLEN(out, sizeof(switch_msg));

		cnt = 3;
		do {
			if (send_hm_message(dev, &rdata, out)) {
				/* A0A02000221B9AD00000000 */
				switched = 1;
				break;
				
			}
		} while (cnt--);

		if (!switched) {
			printf("No!\n");

			if (!switch_speed(dev, &rdata, 10)) {
				fprintf(stderr, "Can't switch speed!\n");
				exit(EXIT_FAILURE);
			}
		}
	} while ((!switched) && (switchcnt--));

	if (!switched) {
		fprintf(stderr, "Too many errors, giving up!\n");
		exit(EXIT_FAILURE);
	}

	printf("Yes!\n");

	printf("Flashing %d blocks", fw->fw_blocks);
	if (debug) {
		printf("\n");
	} else {
		printf(": %04u/%04u %c", 0, fw->fw_blocks, twiddlie[0]);
		fflush(stdout);
	}

	for (block = 0; block < fw->fw_blocks; block++) {
		int first;

		len = fw->fw[block][2] << 8;
		len |= fw->fw[block][3];

		pos = &(fw->fw[block][2]);

		len += 2; /* length */

		if (debug)
			hexdump(pos, len, "F> ");

		first = 1;
		cnt = 0;
		do {
			int payloadlen = 35;
			int ack = 0;

			if (first) {
				payloadlen = 37;
				first = 0;
			}

			if ((len - (pos - &(fw->fw[block][2]))) < payloadlen)
				payloadlen = (len - (pos - &(fw->fw[block][2])));

			if (((pos + payloadlen) - &(fw->fw[block][2])) == len)
				ack = 1;

			memset(&rdata, 0, sizeof(rdata));

			memset(out, 0, sizeof(out));

			out[MSGID] = msgid;
			if (ack)
				out[CTL] = 0x20;
			out[TYPE] = 0xCA;
			SET_SRC(out, my_hmid);
			SET_DST(out, hmid);

			memcpy(&out[PAYLOAD], pos, payloadlen);
			SET_LEN_FROM_PAYLOADLEN(out, payloadlen);

			if (send_hm_message(dev, &rdata, out)) {
				pos += payloadlen;
			} else {
				pos = &(fw->fw[block][2]);
				cnt++;
				if (cnt == 3) {
					fprintf(stderr, "\nToo many errors, giving up!\n");
					exit(EXIT_FAILURE);
				} else {
					printf("Flashing %d blocks: %04u/%04u %c", fw->fw_blocks, block + 1, fw->fw_blocks, twiddlie[msgnum % sizeof(twiddlie)]);
				}
			}

			msgnum++;

			if (!debug) {
				printf("\b\b\b\b\b\b\b\b\b\b\b%04u/%04u %c",
					block + 1, fw->fw_blocks, twiddlie[msgnum % sizeof(twiddlie)]);
				fflush(stdout);
			}
		} while((pos - &(fw->fw[block][2])) < len);
		msgid++;
	}

	firmware_free(fw);

	printf("\n");

	if (!switch_speed(dev, &rdata, 10)) {
		fprintf(stderr, "Can't switch speed!\n");
		exit(EXIT_FAILURE);
	}

	printf("Waiting for device to reboot\n");

	cnt = 10;
	do {
		errno = 0;
		pfd = hmcfgusb_poll(dev, 1);
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\nhmcfgusb_poll");
				exit(EXIT_FAILURE);
			}
		}
		if (rdata.message_type == MESSAGE_TYPE_E) {
			break;
		}
	} while(cnt--);

	if (rdata.message_type == MESSAGE_TYPE_E) {
		printf("Device rebooted\n");
	}

	hmcfgusb_close(dev);

	return EXIT_SUCCESS;
}
