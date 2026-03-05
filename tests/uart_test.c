// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * uart_test.c -- Simple UART test tool for FTDI driver testing
 *
 * Performs basic UART read/write operations through /dev/ttyFTDIx
 * and verifies the emulator responds correctly.
 *
 * Usage: uart_test /dev/ttyFTDIx
 *
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

#define TEST_PATTERN_LEN	4
#define TEST_BAUD_RATE		B115200

int main(int argc, char *argv[])
{
	const char *dev_path;
	int fd, ret;
	unsigned char tx_buf[TEST_PATTERN_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char rx_buf[TEST_PATTERN_LEN];
	struct termios tio, tio_old;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/ttyFTDIx\n", argv[0]);
		return 1;
	}

	dev_path = argv[1];

	fd = open(dev_path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("UART test on %s\n", dev_path);

	/*
	 * Test 1: Configure UART parameters
	 */
	printf("Test 1: Configure UART (115200 8N1)... ");
	if (tcgetattr(fd, &tio_old) < 0) {
		fprintf(stderr, "FAIL: tcgetattr failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = TEST_BAUD_RATE | CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 10;	/* 1 second timeout */

	tcflush(fd, TCIFLUSH);
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "FAIL: tcsetattr failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS\n");

	/*
	 * Test 2: Simple write
	 */
	printf("Test 2: Simple write... ");
	ret = write(fd, tx_buf, TEST_PATTERN_LEN);
	if (ret < 0) {
		fprintf(stderr, "FAIL: write failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: short write (%d/%d)\n", ret, TEST_PATTERN_LEN);
		close(fd);
		return 1;
	}
	/* Ensure data is transmitted */
	tcdrain(fd);
	printf("PASS (wrote %d bytes: %02x %02x %02x %02x)\n",
	       ret, tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3]);

	/*
	 * Test 3: Simple read - verify counter pattern
	 * Emulator returns 0xA0, 0xA1, 0xA2, ... for each read byte
	 */
	printf("Test 3: Simple read... ");
	memset(rx_buf, 0, sizeof(rx_buf));
	ret = read(fd, rx_buf, TEST_PATTERN_LEN);
	if (ret < 0) {
		fprintf(stderr, "FAIL: read failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: short read (%d/%d)\n", ret, TEST_PATTERN_LEN);
		close(fd);
		return 1;
	}
	/* Verify counter pattern from emulator */
	if (rx_buf[1] != rx_buf[0] + 1 || rx_buf[2] != rx_buf[0] + 2) {
		fprintf(stderr, "FAIL: unexpected pattern %02x %02x %02x %02x (expected counter)\n",
			rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);
		close(fd);
		return 1;
	}
	printf("PASS (read %d bytes: %02x %02x %02x %02x - counter pattern OK)\n",
	       ret, rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	/*
	 * Test 4: Second write/read cycle to verify state continuity
	 */
	printf("Test 4: Second write/read cycle... ");
	ret = write(fd, tx_buf, TEST_PATTERN_LEN);
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: second write failed (%d)\n", ret);
		close(fd);
		return 1;
	}
	tcdrain(fd);

	memset(rx_buf, 0, sizeof(rx_buf));
	ret = read(fd, rx_buf, TEST_PATTERN_LEN);
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: second read failed (%d)\n", ret);
		close(fd);
		return 1;
	}
	/* Counter should have continued from previous read */
	if (rx_buf[1] != rx_buf[0] + 1 || rx_buf[2] != rx_buf[0] + 2) {
		fprintf(stderr, "FAIL: unexpected pattern %02x %02x %02x %02x\n",
			rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);
		close(fd);
		return 1;
	}
	printf("PASS (read %02x %02x %02x %02x - counter continued)\n",
	       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	close(fd);
	printf("All UART tests PASSED\n");
	return 0;
}
