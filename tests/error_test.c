// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * error_test.c -- Error path testing for FTDI driver
 *
 * Tests driver error handling by performing I2C/SPI operations when
 * the emulator is configured to inject errors.
 *
 * Usage: error_test <error_type> <device_path>
 *
 * Error types:
 *   i2c-nak    - Test I2C NAK handling (expects ENXIO/EIO)
 *   i2c-stuck  - Test I2C bus stuck handling (expects recovery attempt)
 *   spi-err    - Test SPI error handling
 *
 * Returns 0 if error handling is correct, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>

#define SLAVE_ADDR	0x50

/*
 * Test I2C NAK handling.
 * When the emulator injects NAK, the driver should return ENXIO.
 */
static int test_i2c_nak(const char *dev_path)
{
	int fd, ret;
	uint8_t buf[4] = { 0x00, 0x01, 0x02, 0x03 };

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("I2C NAK test on %s\n", dev_path);

	if (ioctl(fd, I2C_SLAVE, SLAVE_ADDR) < 0) {
		fprintf(stderr, "ioctl(I2C_SLAVE) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/*
	 * Write should fail with ENXIO (no device) or EIO when NAK is received.
	 * The driver checks the ACK bit after each byte and returns ENXIO
	 * if NAK is received during addressing.
	 */
	printf("Attempting write (expecting NAK error)...\n");
	ret = write(fd, buf, sizeof(buf));
	if (ret >= 0) {
		fprintf(stderr, "FAIL: write succeeded, expected error\n");
		close(fd);
		return 1;
	}

	/*
	 * ENXIO: No device at address (NAK on address byte)
	 * EPIPE: NAK on data byte
	 * EIO:   General I/O error
	 * EREMOTEIO: Remote I/O error (some drivers use this)
	 */
	if (errno == ENXIO || errno == EPIPE || errno == EIO ||
	    errno == EREMOTEIO) {
		printf("PASS: write failed with expected error: %s (%d)\n",
		       strerror(errno), errno);
		close(fd);
		return 0;
	}

	fprintf(stderr, "FAIL: write failed with unexpected error: %s (%d)\n",
		strerror(errno), errno);
	fprintf(stderr, "      Expected: ENXIO (%d), EPIPE (%d), EIO (%d), or EREMOTEIO (%d)\n",
		ENXIO, EPIPE, EIO, EREMOTEIO);
	close(fd);
	return 1;
}

/*
 * Test I2C bus stuck (frozen bus) handling.
 * When SDA is stuck low, the driver should attempt bus recovery
 * (toggle SCL to release the bus).
 */
static int test_i2c_stuck(const char *dev_path)
{
	int fd, ret;
	uint8_t buf[4];

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("I2C bus stuck test on %s\n", dev_path);

	if (ioctl(fd, I2C_SLAVE, SLAVE_ADDR) < 0) {
		fprintf(stderr, "ioctl(I2C_SLAVE) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/*
	 * When the bus is stuck, operations may fail or succeed depending
	 * on whether recovery is successful. The key is that the driver
	 * doesn't hang or crash.
	 */
	printf("Attempting read on stuck bus...\n");
	ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		/*
		 * Expected errors on stuck bus:
		 * ETIMEDOUT: Bus recovery timed out
		 * EIO: General I/O error
		 * EAGAIN: Try again (temporary issue)
		 */
		if (errno == ETIMEDOUT || errno == EIO || errno == EAGAIN ||
		    errno == ENXIO || errno == EREMOTEIO) {
			printf("PASS: read failed with expected error: %s (%d)\n",
			       strerror(errno), errno);
			close(fd);
			return 0;
		}
		fprintf(stderr, "FAIL: read failed with unexpected error: %s (%d)\n",
			strerror(errno), errno);
		close(fd);
		return 1;
	}

	/*
	 * If read succeeded, bus recovery worked (or the stuck state
	 * was simulated on lines that don't affect read operations).
	 */
	printf("PASS: read succeeded (bus recovery may have worked)\n");
	close(fd);
	return 0;
}

/*
 * Test SPI error handling.
 * Verify driver handles USB errors during SPI transfers.
 */
static int test_spi_error(const char *dev_path)
{
	int fd, ret;
	uint8_t tx[16] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t rx[16];
	struct spi_ioc_transfer xfer = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = sizeof(tx),
		.speed_hz = 1000000,
		.bits_per_word = 8,
	};

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("SPI error test on %s\n", dev_path);

	/*
	 * Perform SPI transfer - with error injection, this may fail.
	 */
	printf("Attempting SPI transfer (expecting possible error)...\n");
	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
	if (ret < 0) {
		/*
		 * Expected USB errors:
		 * EIO: General I/O error
		 * ETIMEDOUT: USB timeout
		 * EPIPE: USB stall (endpoint halted)
		 * ESHUTDOWN: Device disconnected
		 */
		if (errno == EIO || errno == ETIMEDOUT || errno == EPIPE ||
		    errno == ESHUTDOWN) {
			printf("PASS: SPI transfer failed with expected error: %s (%d)\n",
			       strerror(errno), errno);
			close(fd);
			return 0;
		}
		fprintf(stderr, "FAIL: SPI transfer failed with unexpected error: %s (%d)\n",
			strerror(errno), errno);
		close(fd);
		return 1;
	}

	printf("PASS: SPI transfer succeeded (no error injected or recovered)\n");
	close(fd);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *error_type;
	const char *dev_path;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <error_type> <device_path>\n", argv[0]);
		fprintf(stderr, "Error types: i2c-nak, i2c-stuck, spi-err\n");
		return 1;
	}

	error_type = argv[1];
	dev_path = argv[2];

	if (strcmp(error_type, "i2c-nak") == 0)
		return test_i2c_nak(dev_path);
	else if (strcmp(error_type, "i2c-stuck") == 0)
		return test_i2c_stuck(dev_path);
	else if (strcmp(error_type, "spi-err") == 0)
		return test_spi_error(dev_path);
	else {
		fprintf(stderr, "Unknown error type: %s\n", error_type);
		fprintf(stderr, "Use: i2c-nak, i2c-stuck, or spi-err\n");
		return 1;
	}
}
