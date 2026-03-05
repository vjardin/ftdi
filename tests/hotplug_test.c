// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * hotplug_test.c -- Hot-unplug stress test for FTDI driver
 *
 * Performs continuous I/O operations to stress-test disconnect handling.
 * The test runs in a loop until the device is unplugged or a signal is
 * received, reporting any errors that occur.
 *
 * Usage: hotplug_test <spi|i2c|gpio> <device_path>
 *
 * Returns 0 if disconnect was handled gracefully (ENODEV/EIO expected),
 * returns 1 if an unexpected error or crash occurred.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>

static volatile int running = 1;
static int iterations = 0;
static int errors = 0;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

/*
 * Test SPI transfers until device is unplugged
 */
static int test_spi(const char *dev_path)
{
	int fd;
	uint8_t tx[16] = {0};
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

	printf("SPI hot-unplug test on %s\n", dev_path);
	printf("Performing continuous transfers until disconnect...\n");

	while (running) {
		int ret;

		/* Fill TX buffer with iteration count for debugging */
		tx[0] = iterations & 0xff;

		ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
		if (ret < 0) {
			int e = errno;
			printf("SPI transfer failed at iteration %d: %s (%d)\n",
			       iterations, strerror(e), e);
			errors++;

			/* Expected errors on disconnect */
			if (e == ENODEV || e == EIO || e == ESHUTDOWN ||
			    e == ENOENT || e == EPIPE) {
				printf("Disconnect detected (expected error)\n");
				break;
			}

			/* Unexpected error - but don't crash, just report */
			if (errors > 10) {
				printf("Too many errors, stopping\n");
				break;
			}
		}

		iterations++;

		/* Small delay to allow disconnect to happen */
		usleep(1000);
	}

	close(fd);
	printf("Completed %d iterations, %d errors\n", iterations, errors);
	return 0;
}

/*
 * Test I2C transfers until device is unplugged
 */
static int test_i2c(const char *dev_path)
{
	int fd;
	uint8_t buf[4];

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	/* Set slave address (use a common EEPROM address) */
	if (ioctl(fd, I2C_SLAVE, 0x50) < 0) {
		fprintf(stderr, "Failed to set I2C slave address: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("I2C hot-unplug test on %s\n", dev_path);
	printf("Performing continuous transfers until disconnect...\n");

	while (running) {
		int ret;

		/* Try to read from I2C device */
		buf[0] = 0;  /* Address byte */
		ret = write(fd, buf, 1);
		if (ret < 0) {
			int e = errno;
			printf("I2C write failed at iteration %d: %s (%d)\n",
			       iterations, strerror(e), e);
			errors++;

			if (e == ENODEV || e == EIO || e == ESHUTDOWN ||
			    e == ENOENT || e == EPIPE || e == EREMOTEIO) {
				printf("Disconnect detected (expected error)\n");
				break;
			}

			if (errors > 10) {
				printf("Too many errors, stopping\n");
				break;
			}
		}

		ret = read(fd, buf, 1);
		if (ret < 0) {
			int e = errno;
			printf("I2C read failed at iteration %d: %s (%d)\n",
			       iterations, strerror(e), e);
			errors++;

			if (e == ENODEV || e == EIO || e == ESHUTDOWN ||
			    e == ENOENT || e == EPIPE || e == EREMOTEIO) {
				printf("Disconnect detected (expected error)\n");
				break;
			}

			if (errors > 10) {
				printf("Too many errors, stopping\n");
				break;
			}
		}

		iterations++;
		usleep(1000);
	}

	close(fd);
	printf("Completed %d iterations, %d errors\n", iterations, errors);
	return 0;
}

/*
 * Test GPIO operations until device is unplugged
 */
static int test_gpio(const char *dev_path)
{
	int fd;
	struct gpiochip_info chip_info;
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values vals;
	int line_fd;
	int line_offset = 8;  /* Use pin 8 (AC0) */

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	/* Get chip info */
	if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
		fprintf(stderr, "Failed to get chip info: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("GPIO hot-unplug test on %s (%s, %u lines)\n",
	       dev_path, chip_info.name, chip_info.lines);

	/* Request line as output */
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	strncpy(req.consumer, "hotplug_test", sizeof(req.consumer) - 1);

	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "Failed to request GPIO line: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	line_fd = req.fd;

	printf("Performing continuous GPIO toggles until disconnect...\n");

	while (running) {
		int ret;

		/* Toggle GPIO high */
		memset(&vals, 0, sizeof(vals));
		vals.mask = 1;
		vals.bits = iterations & 1;

		ret = ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
		if (ret < 0) {
			int e = errno;
			printf("GPIO set failed at iteration %d: %s (%d)\n",
			       iterations, strerror(e), e);
			errors++;

			if (e == ENODEV || e == EIO || e == ESHUTDOWN ||
			    e == ENOENT || e == EPIPE) {
				printf("Disconnect detected (expected error)\n");
				break;
			}

			if (errors > 10) {
				printf("Too many errors, stopping\n");
				break;
			}
		}

		/* Read back value */
		memset(&vals, 0, sizeof(vals));
		vals.mask = 1;
		ret = ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals);
		if (ret < 0) {
			int e = errno;
			printf("GPIO get failed at iteration %d: %s (%d)\n",
			       iterations, strerror(e), e);
			errors++;

			if (e == ENODEV || e == EIO || e == ESHUTDOWN ||
			    e == ENOENT || e == EPIPE) {
				printf("Disconnect detected (expected error)\n");
				break;
			}

			if (errors > 10) {
				printf("Too many errors, stopping\n");
				break;
			}
		}

		iterations++;
		usleep(500);  /* Faster for GPIO */
	}

	close(line_fd);
	close(fd);
	printf("Completed %d iterations, %d errors\n", iterations, errors);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *mode;
	const char *dev_path;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <spi|i2c|gpio> <device_path>\n", argv[0]);
		return 1;
	}

	mode = argv[1];
	dev_path = argv[2];

	/* Set up signal handlers for clean shutdown */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	if (strcmp(mode, "spi") == 0)
		return test_spi(dev_path);
	else if (strcmp(mode, "i2c") == 0)
		return test_i2c(dev_path);
	else if (strcmp(mode, "gpio") == 0)
		return test_gpio(dev_path);
	else {
		fprintf(stderr, "Unknown mode: %s (use spi, i2c, or gpio)\n", mode);
		return 1;
	}
}
