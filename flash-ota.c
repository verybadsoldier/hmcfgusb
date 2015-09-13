/* flasher for HomeMatic-devices supporting OTA updates
 *
 * Copyright (c) 2014-15 Michael Gernoth <michael@gernoth.net>
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
#include "culfw.h"
#include "util.h"

#define MAX_RETRIES		5
#define NORMAL_MAX_PAYLOAD	37
#define LOWER_MAX_PAYLOAD	17

extern char *optarg;

uint32_t hmid = 0;
uint32_t my_hmid = 0;
uint8_t key[16] = {0};
int32_t kNo = -1;

/* Maximum payloadlen supported by IO */
uint32_t max_payloadlen = NORMAL_MAX_PAYLOAD;

enum device_type {
	DEVICE_TYPE_HMCFGUSB,
	DEVICE_TYPE_CULFW,
};

struct ota_dev {
	int type;
	struct hmcfgusb_dev *hmcfgusb;
	struct culfw_dev *culfw;
};

enum message_type {
	MESSAGE_TYPE_E = 1,
	MESSAGE_TYPE_R = 2,
};

struct recv_data {
	uint8_t message[64];
	enum message_type message_type;
	uint16_t status;
	int speed;
	uint16_t version;
	uint8_t credits;
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
			rdata->version = (buf[11] << 8) | buf[12];
			rdata->credits = buf[36];
			my_hmid = (buf[0x1b] << 16) | (buf[0x1c] << 8) | buf[0x1d];
			break;
		default:
			break;
	}

	if (buf_len != 1)
		return 1;

	return 1;
}

static int parse_culfw(uint8_t *buf, int buf_len, void *data)
{
	struct recv_data *rdata = data;
	int pos = 0;

	memset(rdata, 0, sizeof(struct recv_data));

	if (buf_len <= 3)
		return 0;

	switch(buf[0]) {
		case 'A':
			if (buf[1] == 's')
				return 0;

			while(validate_nibble(buf[(pos * 2) + 1]) &&
			      validate_nibble(buf[(pos * 2) + 2]) &&
			      (pos + 1 < buf_len)) {
				rdata->message[pos] = ascii_to_nibble(buf[(pos * 2) + 1]) << 4;
				rdata->message[pos] |= ascii_to_nibble(buf[(pos * 2) + 2]);
				pos++;
			}

			if (hmid && (SRC(rdata->message) != hmid))
				return 0;

			rdata->message_type = MESSAGE_TYPE_E;
			break;
		case 'V':
			{
				uint8_t v;
				char *s;
				char *e;

				s = ((char*)buf) + 2;
				e = strchr(s, '.');
				if (!e) {
					fprintf(stderr, "Unknown response from CUL: %s", buf);
					return 0;
				}
				*e = '\0';
				v = atoi(s);
				rdata->version = v << 8;

				s = e + 1;
				e = strchr(s, ' ');
				if (!e) {
					fprintf(stderr, "Unknown response from CUL: %s", buf);
					return 0;
				}
				*e = '\0';
				v = atoi(s);
				rdata->version |= v;

				s = e + 1;
				e = strchr(s, ' ');
				if (!e) {
					break;
				}
				*e = '\0';
				if (!strcmp(s, "a-culfw")) {
					rdata->version = 0xffff;
				}
			}
			break;
		case 'E':
			{
				if (!strncmp((char*)buf, "ERR:CCA", 7)) {
					fprintf(stderr, "CCA didn't complete, too much traffic\n");
				}
				break;
			}
		default:
			fprintf(stderr, "Unknown response from CUL: %s", buf);
			return 0;
			break;
	}

	return 1;
}

int send_hm_message(struct ota_dev *dev, struct recv_data *rdata, uint8_t *msg)
{
	static uint32_t id = 1;
	struct timeval tv;
	uint8_t out[0x40];
	int pfd;

	switch(dev->type) {
		case DEVICE_TYPE_HMCFGUSB:
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
			hmcfgusb_send(dev->hmcfgusb, out, sizeof(out), 1);

			while (1) {
				if (rdata->message_type == MESSAGE_TYPE_R) {
					if (((rdata->status & 0xdf) == 0x01) ||
					    ((rdata->status & 0xdf) == 0x02)) {
						break;
					} else {
						if ((rdata->status & 0xff00) == 0x0400) {
							fprintf(stderr, "\nOut of credits!\n");
						} else if ((rdata->status & 0xff) == 0x08) {
							fprintf(stderr, "\nMissing ACK!\n");
						} else if ((rdata->status & 0xff) == 0x30) {
							fprintf(stderr, "\nUnknown AES-key requested!\n");
						} else {
							fprintf(stderr, "\nInvalid status: %04x\n", rdata->status);
						}
						return 0;
					}
				}
				errno = 0;
				pfd = hmcfgusb_poll(dev->hmcfgusb, 1000);
				if ((pfd < 0) && errno) {
					if (errno != ETIMEDOUT) {
						perror("\n\nhmcfgusb_poll");
						exit(EXIT_FAILURE);
					}
				}
			}
			break;
		case DEVICE_TYPE_CULFW:
			{
				char buf[256];
				int i;

				memset(buf, 0, sizeof(buf));
				buf[0] = 'A';
				buf[1] = 's';
				for (i = 0; i < msg[0] + 1; i++) {
					buf[2 + (i * 2)] = nibble_to_ascii((msg[i] >> 4) & 0xf);
					buf[2 + (i * 2) + 1] = nibble_to_ascii(msg[i] & 0xf);
				}
				buf[2 + (i * 2) ] = '\r';
				buf[2 + (i * 2) + 1] = '\n';

				memset(rdata, 0, sizeof(struct recv_data));
				if (culfw_send(dev->culfw, buf, 2 + (i * 2) + 1) == 0) {
					fprintf(stderr, "culfw_send failed!\n");
					exit(EXIT_FAILURE);
				}

				if (msg[CTL] & 0x20) {
					int cnt = 5;
					int pfd;
					do {
						errno = 0;
						pfd = culfw_poll(dev->culfw, 200);
						if ((pfd < 0) && errno) {
							if (errno != ETIMEDOUT) {
								perror("\n\nculfw_poll");
								exit(EXIT_FAILURE);
							}
						}
						if (rdata->message_type == MESSAGE_TYPE_E) {
							if (rdata->message[TYPE] == 0x02) {
								if (rdata->message[PAYLOAD] == 0x04) {
									int32_t req_kNo;
									uint8_t challenge[6];
									uint8_t respbuf[16];
									uint8_t *resp;

									req_kNo = rdata->message[rdata->message[LEN]] / 2;
									memcpy(challenge, &(rdata->message[PAYLOAD+1]), 6);

									if (req_kNo != kNo) {
										fprintf(stderr, "AES request for unknown key %d!\n", req_kNo);
									} else {
										resp = hm_sign(key, challenge, msg, NULL, respbuf);
										if (resp) {
											uint8_t rbuf[64];

											memset(rbuf, 0, sizeof(rbuf));
											rbuf[MSGID] = rdata->message[MSGID];
											rbuf[CTL] = rdata->message[CTL];
											rbuf[TYPE] = 0x03;
											SET_SRC(rbuf, DST(rdata->message));
											SET_DST(rbuf, SRC(rdata->message));
											memcpy(&(rbuf[PAYLOAD]), resp, 16);
											SET_LEN_FROM_PAYLOADLEN(rbuf, 16);

											return send_hm_message(dev, rdata, rbuf);
										}
									}
								} else if (rdata->message[PAYLOAD] >= 0x80 && rdata->message[PAYLOAD] <= 0x8f) {
									fprintf(stderr, "NACK\n");
								} else {	/* ACK or ACKinfo */
									break;
								}
							} else {
								fprintf(stderr, "Unexpected message received: ");
								for (i = 0; i < rdata->message[LEN]; i++) {
									fprintf(stderr, "%02x", rdata->message[i+1]);
								}
								fprintf(stderr, "\n");
							}
						}
					} while(cnt--);

					if (cnt == -1) {
						fprintf(stderr, "\nMissing ACK!\n");
						return 0;
					}
				}
			}
			break;
	}

	id++;
	return 1;
}

static int switch_speed(struct ota_dev *dev, struct recv_data *rdata, uint8_t speed)
{
	uint8_t out[0x40];
	int pfd;

	printf("Entering %uk-mode\n", speed);

	switch(dev->type) {
		case DEVICE_TYPE_HMCFGUSB:
			memset(out, 0, sizeof(out));
			out[0] = 'G';
			out[1] = speed;

			hmcfgusb_send(dev->hmcfgusb, out, sizeof(out), 1);

			while (1) {
				errno = 0;
				pfd = hmcfgusb_poll(dev->hmcfgusb, 1000);
				if ((pfd < 0) && errno) {
					if (errno != ETIMEDOUT) {
						perror("\n\nhmcfgusb_poll");
						exit(EXIT_FAILURE);
					}
				}
				if (rdata->speed == speed)
					break;
			}
			break;
		case DEVICE_TYPE_CULFW:
			if (speed == 100) {
				return culfw_send(dev->culfw, "AR\r\n", 4);
			} else {
				return culfw_send(dev->culfw, "Ar\r\n", 4);
			}
			break;
	}

	return 1;
}

void flash_ota_syntax(char *prog)
{
	fprintf(stderr, "Syntax: %s parameters options\n\n", prog);
	fprintf(stderr, "Mandatory parameters:\n");
	fprintf(stderr, "\t-f firmware.eq3\tfirmware file to flash\n");
	fprintf(stderr, "\t-s SERIAL\tserial of device to flash (optional when using -D)\n");
	fprintf(stderr, "\nOptional parameters:\n");
	fprintf(stderr, "\t-c device\tenable CUL-mode with CUL at path \"device\"\n");
	fprintf(stderr, "\t-b bps\t\tuse CUL with speed \"bps\" (default: %u)\n", DEFAULT_CUL_BPS);
	fprintf(stderr, "\t-l\t\tlower payloadlen (required for devices with little RAM, e.g. CUL v2 and CUL v4)\n");
	fprintf(stderr, "\t-S serial\tuse HM-CFG-USB with given serial\n");
	fprintf(stderr, "\t-h\t\tthis help\n");
	fprintf(stderr, "\nOptional parameters for automatically sending device to bootloader\n");
	fprintf(stderr, "\t-C\t\tHMID of central (3 hex-bytes, no prefix, e.g. ABCDEF)\n");
	fprintf(stderr, "\t-D\t\tHMID of device (3 hex-bytes, no prefix, e.g. 123456)\n");
	fprintf(stderr, "\t-K\t\tKNO:KEY AES key-number and key (hex) separated by colon (Fhem hmKey attribute)\n");
}

int main(int argc, char **argv)
{
	const char twiddlie[] = { '-', '\\', '|', '/' };
	const uint8_t cc1101_regs[] = { 0x10, 0x5B, 0x11, 0xF8, 0x15, 0x47 };
	char *fw_file = NULL;
	char *serial = NULL;
	char *culfw_dev = NULL;
	char *endptr = NULL;
	unsigned int bps = DEFAULT_CUL_BPS;
	struct ota_dev dev;
	struct recv_data rdata;
	uint8_t out[0x40];
	uint8_t *pos;
	uint8_t msgid = 0x1;
	uint16_t len;
	struct firmware *fw;
	char *hmcfgusb_serial = NULL;
	int block;
	int pfd;
	int debug = 0;
	int cnt;
	int switchcnt = 0;
	int msgnum = 0;
	int switched = 0;
	int opt;

	printf("HomeMatic OTA flasher version " VERSION "\n\n");

	while((opt = getopt(argc, argv, "b:c:f:hls:C:D:K:S:")) != -1) {
		switch (opt) {
			case 'b':
				bps = atoi(optarg);
				break;
			case 'c':
				culfw_dev = optarg;
				break;
			case 'f':
				fw_file = optarg;
				break;
			case 'l':
				printf("Reducing payload-len from %d to %d\n", max_payloadlen, LOWER_MAX_PAYLOAD);
				max_payloadlen = LOWER_MAX_PAYLOAD;
				break;
			case 's':
				serial = optarg;
				break;
			case 'C':
				my_hmid = strtoul(optarg, &endptr, 16);
				if (*endptr != '\0') {
					fprintf(stderr, "Invalid central HMID!\n\n");
					flash_ota_syntax(argv[0]);
					exit(EXIT_FAILURE);
				}
				break;
			case 'D':
				hmid = strtoul(optarg, &endptr, 16);
				if (*endptr != '\0') {
					fprintf(stderr, "Invalid device HMID!\n\n");
					flash_ota_syntax(argv[0]);
					exit(EXIT_FAILURE);
				}
				break;
			case 'K':
				kNo = strtoul(optarg, &endptr, 10);
				if (*endptr != ':') {
					fprintf(stderr, "Invalid key number!\n\n");
					flash_ota_syntax(argv[0]);
					exit(EXIT_FAILURE);
				}
				endptr++;
				for (cnt = 0; cnt < 16; cnt++) {
					if (*endptr == '\0' || *(endptr+1) == '\0' ||
					    !validate_nibble(*endptr) ||
					    !validate_nibble(*(endptr+1))) {
						fprintf(stderr, "Invalid key!\n\n");
						flash_ota_syntax(argv[0]);
						exit(EXIT_FAILURE);
					}
					key[cnt] = ascii_to_nibble(*endptr) << 4 | ascii_to_nibble(*(endptr+1));
					endptr += 2;
				}
				break;
			case 'S':
				hmcfgusb_serial = optarg;
				break;
			case 'h':
			case ':':
			case '?':
			default:
				flash_ota_syntax(argv[0]);
				exit(EXIT_FAILURE);
				break;

		}
	}

	if (!fw_file || (!serial && !hmid)) {
		flash_ota_syntax(argv[0]);
		exit(EXIT_FAILURE);
	}

	fw = firmware_read_firmware(fw_file, debug);
	if (!fw)
		exit(EXIT_FAILURE);

	memset(&rdata, 0, sizeof(rdata));
	memset(&dev, 0, sizeof(struct ota_dev));

	if (culfw_dev) {
		printf("Opening culfw-device at path %s with speed %u\n", culfw_dev, bps);
		dev.culfw = culfw_init(culfw_dev, bps, parse_culfw, &rdata);
		if (!dev.culfw) {
			fprintf(stderr, "Can't initialize CUL at %s with rate %u\n", culfw_dev, bps);
			exit(EXIT_FAILURE);
		}
		dev.type = DEVICE_TYPE_CULFW;

		printf("Requesting firmware version\n");
		culfw_send(dev.culfw, "\r\n", 2);
		culfw_flush(dev.culfw);

		while (1) {
			culfw_send(dev.culfw, "V\r\n", 3);

			errno = 0;
			pfd = culfw_poll(dev.culfw, 1000);
			if ((pfd < 0) && errno) {
				if (errno != ETIMEDOUT) {
					perror("\n\nhmcfgusb_poll");
					exit(EXIT_FAILURE);
				}
			}
			if (rdata.version)
				break;
		}

		printf("culfw-device firmware version: ");
		if (rdata.version != 0xffff) {
			printf("%u.%02u\n",
				(rdata.version >> 8) & 0xff,
				rdata.version & 0xff);
		} else {
			printf("a-culfw\n");
		}

		if (rdata.version < 0x013a) {
			fprintf(stderr, "\nThis version does _not_ support firmware upgrade mode, you need at least 1.58!\n");
			exit(EXIT_FAILURE);
		}
	} else {
		uint32_t new_hmid = my_hmid;

		hmcfgusb_set_debug(debug);

		dev.hmcfgusb = hmcfgusb_init(parse_hmcfgusb, &rdata, hmcfgusb_serial);
		if (!dev.hmcfgusb) {
			fprintf(stderr, "Can't initialize HM-CFG-USB\n");
			exit(EXIT_FAILURE);
		}
		dev.type = DEVICE_TYPE_HMCFGUSB;

		memset(out, 0, sizeof(out));
		out[0] = 'K';
		hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);

		while (1) {
			errno = 0;
			pfd = hmcfgusb_poll(dev.hmcfgusb, 1000);
			if ((pfd < 0) && errno) {
				if (errno != ETIMEDOUT) {
					perror("\n\nhmcfgusb_poll");
					exit(EXIT_FAILURE);
				}
			}
			if (rdata.version)
				break;
		}

		if (rdata.version < 0x3c7) {
			fprintf(stderr, "HM-CFG-USB firmware too low: %u < 967\n", rdata.version);
			exit(EXIT_FAILURE);
		}

		printf("HM-CFG-USB firmware version: %u, used credits: %u%%\n", rdata.version, rdata.credits);

		if (rdata.credits >= 40) {
			printf("\nRebooting HM-CFG-USB to avoid running out of credits\n\n");

			if (!dev.hmcfgusb->bootloader) {
				printf("HM-CFG-USB not in bootloader mode, entering bootloader.\n");
				printf("Waiting for device to reappear...\n");

				do {
					if (dev.hmcfgusb) {
						if (!dev.hmcfgusb->bootloader)
							hmcfgusb_enter_bootloader(dev.hmcfgusb);
						hmcfgusb_close(dev.hmcfgusb);
					}
					sleep(1);
				} while (((dev.hmcfgusb = hmcfgusb_init(parse_hmcfgusb, &rdata, hmcfgusb_serial)) == NULL) || (!dev.hmcfgusb->bootloader));
			}

			if (dev.hmcfgusb->bootloader) {
				printf("HM-CFG-USB in bootloader mode, rebooting\n");

				do {
					if (dev.hmcfgusb) {
						if (dev.hmcfgusb->bootloader)
							hmcfgusb_leave_bootloader(dev.hmcfgusb);
						hmcfgusb_close(dev.hmcfgusb);
					}
					sleep(1);
				} while (((dev.hmcfgusb = hmcfgusb_init(parse_hmcfgusb, &rdata, hmcfgusb_serial)) == NULL) || (dev.hmcfgusb->bootloader));
			}
		}

		printf("\n\nHM-CFG-USB opened\n\n");

		if (new_hmid && (my_hmid != new_hmid)) {
			printf("Changing hmid from %06x to %06x\n", my_hmid, new_hmid);

			memset(out, 0, sizeof(out));
			out[0] = 'A';
			out[1] = (new_hmid >> 16) & 0xff;
			out[2] = (new_hmid >> 8) & 0xff;
			out[3] = new_hmid & 0xff;

			hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);

			my_hmid = new_hmid;
		}

		if (kNo > 0) {
			printf("Setting AES-key\n");

			memset(out, 0, sizeof(out));
			out[0] = 'Y';
			out[1] = 0x01;
			out[2] = kNo;
			out[3] = sizeof(key);
			memcpy(&(out[4]), key, sizeof(key));
			hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);

			memset(out, 0, sizeof(out));
			out[0] = 'Y';
			out[1] = 0x02;
			out[2] = 0x00;
			out[3] = 0x00;
			hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);

			memset(out, 0, sizeof(out));
			out[0] = 'Y';
			out[1] = 0x03;
			out[2] = 0x00;
			out[3] = 0x00;
			hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);
		}
	}

	if (!switch_speed(&dev, &rdata, 10)) {
		fprintf(stderr, "Can't switch speed!\n");
		exit(EXIT_FAILURE);
	}

	if (hmid && my_hmid) {
		printf("Sending device with hmid %06x to bootloader\n", hmid);
		out[CTL] = 0x30;
		out[TYPE] = 0x11;
		SET_SRC(out, my_hmid);
		SET_DST(out, hmid);
		out[PAYLOAD] = 0xCA;
		SET_LEN_FROM_PAYLOADLEN(out, 1);

		cnt = 3;
		do {
			out[MSGID] = msgid++;
			if (send_hm_message(&dev, &rdata, out)) {
				break;
			}
		} while (cnt--);
		if (cnt == -1) {
			printf("Failed to send device to bootloader, please enter bootloader manually.\n");
		}
	}

	if (serial) {
		printf("Waiting for device with serial %s\n", serial);
	} else {
		printf("Waiting for device with HMID %06x\n", hmid);
	}

	while (1) {
		errno = 0;
		switch (dev.type) {
			case DEVICE_TYPE_CULFW:
				pfd = culfw_poll(dev.culfw, 1000);
				break;
			case DEVICE_TYPE_HMCFGUSB:
			default:
				pfd = hmcfgusb_poll(dev.hmcfgusb, 1000);
				break;
		}

		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\npoll");
				exit(EXIT_FAILURE);
			}
		}

		if ((rdata.message[LEN] == 0x14) && /* Length */
		    (rdata.message[MSGID] == 0x00) && /* Message ID */
		    (rdata.message[CTL] == 0x00) && /* Control Byte */
		    (rdata.message[TYPE] == 0x10) && /* Messagte type: Information */
		    (DST(rdata.message) == 0x000000) && /* Broadcast */
		    (rdata.message[PAYLOAD] == 0x00)) { /* FUP? */
			if (serial && !strncmp((char*)&(rdata.message[0x0b]), serial, 10)) {
				hmid = SRC(rdata.message);
				break;
			} else if (!serial && SRC(rdata.message) == hmid) {
				serial = (char*)&(rdata.message[0x0b]);
				break;
			}
		}
	}

	printf("Device with serial %s (HMID: %06x) entered firmware-update-mode\n", serial, hmid);

	if (dev.type == DEVICE_TYPE_HMCFGUSB) {
		printf("Adding HMID\n");

		memset(out, 0, sizeof(out));
		out[0] = '+';
		out[1] = (hmid >> 16) & 0xff;
		out[2] = (hmid >> 8) & 0xff;
		out[3] = hmid & 0xff;

		hmcfgusb_send(dev.hmcfgusb, out, sizeof(out), 1);
	}

	switchcnt = 3;
	do {
		printf("Initiating remote switch to 100k\n");

		memset(out, 0, sizeof(out));

		out[MSGID] = msgid++;
		out[CTL] = 0x00;
		out[TYPE] = 0xCB;
		SET_SRC(out, my_hmid);
		SET_DST(out, hmid);

		memcpy(&out[PAYLOAD], cc1101_regs, sizeof(cc1101_regs));
		SET_LEN_FROM_PAYLOADLEN(out, sizeof(cc1101_regs));

		if (!send_hm_message(&dev, &rdata, out)) {
			exit(EXIT_FAILURE);
		}

		if (!switch_speed(&dev, &rdata, 100)) {
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

		memcpy(&out[PAYLOAD], cc1101_regs, sizeof(cc1101_regs));
		SET_LEN_FROM_PAYLOADLEN(out, sizeof(cc1101_regs));

		cnt = 3;
		do {
			if (send_hm_message(&dev, &rdata, out)) {
				/* A0A02000221B9AD00000000 */
				switched = 1;
				break;
			}
		} while (cnt--);

		if (!switched) {
			printf("No!\n");

			if (!switch_speed(&dev, &rdata, 10)) {
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
			int payloadlen = max_payloadlen - 2;
			int ack = 0;

			if (first) {
				payloadlen = max_payloadlen;
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

			if (send_hm_message(&dev, &rdata, out)) {
				pos += payloadlen;
			} else {
				pos = &(fw->fw[block][2]);
				cnt++;
				if (cnt == MAX_RETRIES) {
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

	if (!switch_speed(&dev, &rdata, 10)) {
		fprintf(stderr, "Can't switch speed!\n");
		exit(EXIT_FAILURE);
	}

	printf("Waiting for device to reboot\n");

	cnt = 10;
	do {
		errno = 0;
		switch(dev.type) {
			case DEVICE_TYPE_CULFW:
				pfd = culfw_poll(dev.culfw, 1000);
				break;
			case DEVICE_TYPE_HMCFGUSB:
			default:
				pfd = hmcfgusb_poll(dev.hmcfgusb, 1000);
				break;
		}
		if ((pfd < 0) && errno) {
			if (errno != ETIMEDOUT) {
				perror("\n\npoll");
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

	switch(dev.type) {
		case DEVICE_TYPE_HMCFGUSB:
			hmcfgusb_close(dev.hmcfgusb);
			hmcfgusb_exit();
			break;
		case DEVICE_TYPE_CULFW:
			culfw_close(dev.culfw);
			break;
	}

	return EXIT_SUCCESS;
}
