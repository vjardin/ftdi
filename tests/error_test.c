// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * error_test.c -- I2C/SPI error paths of the FTDI driver, with the
 * emulator injecting the errors
 *
 * Usage: error_test <error_type> <device_path> [secs]
 *
 *   i2c-nak      a write must fail with ENXIO: address not acknowledged
 *   i2c-nak-sr   a write + repeated START + read must fail with ENXIO
 *   i2c-fail     a write must fail (ENXIO, EIO, EBUSY or ETIMEDOUT)
 *   i2c-fail-read  a 1-byte read must fail, same errors
 *   i2c-stuck    SDA held low for good: two reads in a row must fail
 *                with EBUSY (no START on a held bus), a read that
 *                succeeds returned garbage
 *   i2c-recover  one transfer must fail (ETIMEDOUT, EIO, EPROTO or EPIPE:
 *   spi-recover  a halted endpoint), then, after <secs> (default 4),
 *                transfers must work and carry the
 *                right data: the driver is back in step with the chip
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

/* A write (or a 1-byte read) at SLAVE_ADDR must fail: ENXIO if nak_only */
static int test_i2c_fails(const char *dev_path, int nak_only, int rd)
{
	const char *op = rd ? "read" : "write";
	int fd, ret, e;
	uint8_t buf[4] = { 0x00, 0x01, 0x02, 0x03 };

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("I2C %s on %s, expecting %s\n", op, dev_path,
	       nak_only ? "ENXIO" : "ENXIO, EIO, EBUSY or ETIMEDOUT");

	if (ioctl(fd, I2C_SLAVE, SLAVE_ADDR) < 0) {
		fprintf(stderr, "ioctl(I2C_SLAVE) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	ret = rd ? read(fd, buf, 1) : write(fd, buf, sizeof(buf));
	e = errno;
	close(fd);
	if (ret >= 0) {
		fprintf(stderr, "FAIL: %s succeeded (%02x), expected an error\n",
			op, buf[0]);
		return 1;
	}

	/*
	 * Documentation/i2c/fault-codes.rst: ENXIO = address not acknowledged,
	 * EBUSY = bus held (here: a line low before the START)
	 */
	if (e == ENXIO ||
	    (!nak_only && (e == EIO || e == EBUSY || e == ETIMEDOUT))) {
		printf("PASS: %s failed with expected error: %s (%d)\n", op,
		       strerror(e), e);
		return 0;
	}

	fprintf(stderr, "FAIL: %s failed with unexpected error: %s (%d)\n", op,
		strerror(e), e);
	return 1;
}

static int test_i2c_nak_sr(const char *dev_path)
{
	uint8_t wr = 0x00, rd[2];
	struct i2c_msg msgs[2] = {
		{ .addr = SLAVE_ADDR, .len = 1, .buf = &wr },
		{ .addr = SLAVE_ADDR, .flags = I2C_M_RD, .len = sizeof(rd),
		  .buf = rd },
	};
	struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };
	int fd, ret, e;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}
	ret = ioctl(fd, I2C_RDWR, &xfer);
	e = errno;
	close(fd);
	if (ret >= 0) {
		fprintf(stderr, "FAIL: write+read succeeded (%02x %02x), expected ENXIO\n",
			rd[0], rd[1]);
		return 1;
	}
	if (e != ENXIO) {
		fprintf(stderr, "FAIL: write+read failed with unexpected error: %s (%d)\n",
			strerror(e), e);
		return 1;
	}
	printf("PASS: write+read failed with ENXIO at the repeated START\n");
	return 0;
}

/*
 * SDA held low by the emulator for good: no START can happen, every ACK bit
 * would read as an ACK and every data bit as 0. The driver must see the
 * held line before the START and fail the read; its bus recovery cannot
 * free it, so the next read must fail the same way.
 */
static int test_i2c_stuck(const char *dev_path)
{
	int fd, ret, e, i;
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

	for (i = 1; i <= 2; i++) {
		ret = read(fd, buf, sizeof(buf));
		e = errno;
		if (ret >= 0) {
			fprintf(stderr, "FAIL: read %d succeeded on a bus with SDA held low (%02x %02x %02x %02x)\n",
				i, buf[0], buf[1], buf[2], buf[3]);
			close(fd);
			return 1;
		}
		if (e != EBUSY) {
			fprintf(stderr, "FAIL: read %d: %s (%d), expected EBUSY\n",
				i, strerror(e), e);
			close(fd);
			return 1;
		}
		printf("read %d failed with EBUSY\n", i);
	}

	close(fd);
	printf("PASS: both reads failed with EBUSY\n");
	return 0;
}

/* The injected fault must make this transfer fail, not hang or succeed */
static int expect_failure(int ret, const char *what)
{
	if (ret >= 0) {
		fprintf(stderr, "FAIL: %s succeeded, an error was injected\n", what);
		return 1;
	}
	if (errno != ETIMEDOUT && errno != EIO && errno != EPROTO &&
	    errno != EPIPE) {
		fprintf(stderr, "FAIL: %s failed with unexpected error: %s (%d)\n",
			what, strerror(errno), errno);
		return 1;
	}
	printf("PASS: transfer failed as expected: %s (%d)\n",
	       strerror(errno), errno);
	return 0;
}

/*
 * I2C: a read fails, then the slave's counter pattern (each byte is the
 * previous one plus 1) must come back unbroken. A response that arrives
 * after the driver gave up, and is taken for the answer to a later
 * command, breaks the pattern.
 */
static int test_i2c_recover(const char *dev_path, int secs)
{
	uint8_t buf[16], prev = 0;
	int fd, i, j, have_prev = 0;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}
	if (ioctl(fd, I2C_SLAVE, SLAVE_ADDR) < 0) {
		fprintf(stderr, "ioctl(I2C_SLAVE) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("I2C recovery test on %s\n", dev_path);
	if (expect_failure(read(fd, buf, 4), "I2C read")) {
		close(fd);
		return 1;
	}

	/* Let a late response show up, if the chip still produces one */
	sleep(secs);

	for (i = 0; i < 8; i++) {
		if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
			fprintf(stderr, "FAIL: read %d after the fault: %s\n",
				i, strerror(errno));
			close(fd);
			return 1;
		}
		for (j = 0; j < (int)sizeof(buf); j++) {
			if (have_prev && buf[j] != (uint8_t)(prev + 1)) {
				fprintf(stderr, "FAIL: read %d byte %d is 0x%02x after 0x%02x: out of step\n",
					i, j, buf[j], prev);
				close(fd);
				return 1;
			}
			prev = buf[j];
			have_prev = 1;
		}
	}
	printf("PASS: 8 reads in step after the fault (last byte 0x%02x)\n", prev);
	close(fd);
	return 0;
}

/* SPI: a transfer fails, then SPI_LOOP transfers must return what was sent */
static int test_spi_recover(const char *dev_path, int secs)
{
	uint8_t tx[32], rx[32];
	struct spi_ioc_transfer xfer = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = sizeof(tx),
		.speed_hz = 1000000,
		.bits_per_word = 8,
	};
	uint32_t mode = SPI_MODE_0;
	int fd, i, j;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("SPI recovery test on %s\n", dev_path);
	memset(tx, 0x5A, sizeof(tx));
	if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
	    expect_failure(ioctl(fd, SPI_IOC_MESSAGE(1), &xfer), "SPI transfer")) {
		close(fd);
		return 1;
	}

	sleep(secs);

	mode = SPI_MODE_0 | SPI_LOOP;
	if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0) {
		fprintf(stderr, "FAIL: SPI_LOOP mode: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	for (i = 0; i < 8; i++) {
		for (j = 0; j < (int)sizeof(tx); j++)
			tx[j] = (uint8_t)(i * 37 + j * 11 + 1);
		memset(rx, 0, sizeof(rx));
		if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
			fprintf(stderr, "FAIL: transfer %d after the fault: %s\n",
				i, strerror(errno));
			close(fd);
			return 1;
		}
		if (memcmp(tx, rx, sizeof(tx))) {
			fprintf(stderr, "FAIL: transfer %d after the fault: loopback data differs (rx[0]=0x%02x tx[0]=0x%02x): out of step\n",
				i, rx[0], tx[0]);
			close(fd);
			return 1;
		}
	}
	printf("PASS: 8 loopback transfers in step after the fault\n");
	close(fd);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *error_type;
	const char *dev_path;
	int secs = argc > 3 ? atoi(argv[3]) : 4;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <error_type> <device_path> [secs]\n", argv[0]);
		fprintf(stderr, "Error types: i2c-nak, i2c-nak-sr, i2c-fail, i2c-fail-read, i2c-stuck, i2c-recover, spi-recover\n");
		return 1;
	}

	error_type = argv[1];
	dev_path = argv[2];

	if (strcmp(error_type, "i2c-nak") == 0)
		return test_i2c_fails(dev_path, 1, 0);
	else if (strcmp(error_type, "i2c-nak-sr") == 0)
		return test_i2c_nak_sr(dev_path);
	else if (strcmp(error_type, "i2c-fail") == 0)
		return test_i2c_fails(dev_path, 0, 0);
	else if (strcmp(error_type, "i2c-fail-read") == 0)
		return test_i2c_fails(dev_path, 0, 1);
	else if (strcmp(error_type, "i2c-stuck") == 0)
		return test_i2c_stuck(dev_path);
	else if (strcmp(error_type, "i2c-recover") == 0)
		return test_i2c_recover(dev_path, secs);
	else if (strcmp(error_type, "spi-recover") == 0)
		return test_spi_recover(dev_path, secs);
	else {
		fprintf(stderr, "Unknown error type: %s\n", error_type);
		fprintf(stderr, "Use: i2c-nak, i2c-nak-sr, i2c-fail, i2c-fail-read, i2c-stuck, i2c-recover or spi-recover\n");
		return 1;
	}
}
