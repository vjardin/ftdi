// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * rs485_test.c -- RS-485 mode of an FTDI UART (TIOCSRS485)
 *
 * The chip drives the transmit enable (TXDEN) of an RS-485 transceiver by
 * itself, on a pin that depends on the chip and on its EEPROM; the driver
 * accepts RS-485 mode only when the channel has such a pin.
 *
 * Usage: rs485_test /dev/ttyFTDIx on    RS-485 must be accepted
 *        rs485_test /dev/ttyFTDIx off   RS-485 must be refused (EOPNOTSUPP)
 *
 * Returns 0 on success, 1 on failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/serial.h>

static int rs485_get(int fd, struct serial_rs485 *rs)
{
	memset(rs, 0, sizeof(*rs));
	if (ioctl(fd, TIOCGRS485, rs) < 0) {
		fprintf(stderr, "FAIL: TIOCGRS485: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct serial_rs485 rs;
	int fd, want, ret;

	if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
		fprintf(stderr, "Usage: %s /dev/ttyFTDIx on|off\n", argv[0]);
		return 1;
	}
	want = !strcmp(argv[2], "on");

	fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	memset(&rs, 0, sizeof(rs));
	rs.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
	ret = ioctl(fd, TIOCSRS485, &rs);

	if (!want) {
		if (ret == 0 || errno != EOPNOTSUPP) {
			fprintf(stderr, "FAIL: %s: RS-485 %s, expected EOPNOTSUPP\n",
				argv[1], ret ? strerror(errno) : "accepted");
			close(fd);
			return 1;
		}
		if (rs485_get(fd, &rs) || (rs.flags & SER_RS485_ENABLED)) {
			fprintf(stderr, "FAIL: %s: RS-485 reported enabled after the refusal\n",
				argv[1]);
			close(fd);
			return 1;
		}
		printf("PASS: %s: RS-485 refused (EOPNOTSUPP), port left in RS-232 mode\n",
		       argv[1]);
		close(fd);
		return 0;
	}

	if (ret < 0) {
		fprintf(stderr, "FAIL: %s: TIOCSRS485: %s\n", argv[1],
			strerror(errno));
		close(fd);
		return 1;
	}
	if (rs485_get(fd, &rs) || !(rs.flags & SER_RS485_ENABLED)) {
		fprintf(stderr, "FAIL: %s: RS-485 not reported enabled\n",
			argv[1]);
		close(fd);
		return 1;
	}

	memset(&rs, 0, sizeof(rs));
	if (ioctl(fd, TIOCSRS485, &rs) < 0 || rs485_get(fd, &rs) ||
	    (rs.flags & SER_RS485_ENABLED)) {
		fprintf(stderr, "FAIL: %s: RS-485 cannot be disabled again\n",
			argv[1]);
		close(fd);
		return 1;
	}

	printf("PASS: %s: RS-485 enabled, then disabled\n", argv[1]);
	close(fd);
	return 0;
}
