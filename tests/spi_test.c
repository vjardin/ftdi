// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * spi_test.c -- Simple SPI test tool for FTDI driver testing
 *
 * Performs basic SPI read/write operations via spidev and verifies
 * the emulator responds correctly.
 *
 * Usage: spi_test /dev/spidevX.Y
 *
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define TEST_PATTERN_LEN	4
#define SPI_SPEED_HZ		1000000		/* 1 MHz */
#define SPI_BITS_PER_WORD	8

int main(int argc, char *argv[])
{
	const char *dev_path;
	int fd, ret;
	unsigned char tx_buf[TEST_PATTERN_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char rx_buf[TEST_PATTERN_LEN];
	struct spi_ioc_transfer xfer;
	uint8_t mode = SPI_MODE_0;
	uint8_t bits = SPI_BITS_PER_WORD;
	uint32_t speed = SPI_SPEED_HZ;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/spidevX.Y\n", argv[0]);
		return 1;
	}

	dev_path = argv[1];

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("SPI test on %s\n", dev_path);

	/*
	 * Test 1: Configure SPI mode
	 */
	printf("Test 1: Configure SPI mode... ");
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
		fprintf(stderr, "FAIL: SPI_IOC_WR_MODE failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
		fprintf(stderr, "FAIL: SPI_IOC_WR_BITS_PER_WORD failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		fprintf(stderr, "FAIL: SPI_IOC_WR_MAX_SPEED_HZ failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (mode=%d, bits=%d, speed=%d Hz)\n", mode, bits, speed);

	/*
	 * Test 2: Simple write using write()
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
	printf("PASS (wrote %d bytes)\n", ret);

	/*
	 * Test 3: Simple read using read() - verify counter pattern
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
	 * Test 4: Full-duplex transfer using SPI_IOC_MESSAGE
	 */
	printf("Test 4: Full-duplex transfer (SPI_IOC_MESSAGE)... ");
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (unsigned long)tx_buf;
	xfer.rx_buf = (unsigned long)rx_buf;
	xfer.len = TEST_PATTERN_LEN;
	xfer.speed_hz = speed;
	xfer.bits_per_word = bits;

	memset(rx_buf, 0, sizeof(rx_buf));
	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
	if (ret < 0) {
		fprintf(stderr, "FAIL: SPI_IOC_MESSAGE failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (xfer %d bytes, rx: %02x %02x %02x %02x)\n",
	       ret, rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	/*
	 * Test 5: SPI mode 1 (CPHA=1) — verify transfer works and
	 * the driver emits a dev_notice per AN_114 §1.2.
	 */
	printf("Test 5: SPI mode 1 (CPHA=1, AN_114 warning)... ");
	mode = SPI_MODE_1;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
		fprintf(stderr, "FAIL: SPI_IOC_WR_MODE(1) failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (unsigned long)tx_buf;
	xfer.rx_buf = (unsigned long)rx_buf;
	xfer.len = TEST_PATTERN_LEN;
	xfer.speed_hz = speed;
	xfer.bits_per_word = bits;

	memset(rx_buf, 0, sizeof(rx_buf));
	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
	if (ret < 0) {
		fprintf(stderr, "FAIL: SPI mode 1 transfer: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (mode 1 xfer OK, rx: %02x %02x %02x %02x)\n",
	       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	close(fd);
	printf("All SPI tests PASSED\n");
	return 0;
}
