// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * hotplug_test.c -- Hot-unplug stress test for FTDI driver
 *
 * Transfers in a loop until the device is unplugged. Until then every
 * transfer must work and carry the right data (the emulated SPI and I2C
 * slaves are counters, a GPIO output reads back what was set); the unplug
 * must then end the loop with ESHUTDOWN (transfer cut short, device going
 * away) or ENODEV (device gone).
 *
 * Usage: hotplug_test <spi|i2c|gpio> <device_path>
 *
 * Returns 0 if the loop ran and the unplug ended it as described, 1 on any
 * other error, wrong data, or a signal before the unplug.
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

#include "counter.h"

static volatile int running = 1;
static int iterations;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

/*
 * A failed call: 0 if it is the unplug (the loop ends there), 1 otherwise.
 * The unplug is only expected once transfers have worked.
 */
static int failed(const char *what, int e)
{
	printf("%s failed at iteration %d: %s (%d)\n", what, iterations,
	       strerror(e), e);
	if ((e == ESHUTDOWN || e == ENODEV) && iterations > 0) {
		printf("Disconnect detected (expected error)\n");
		return 0;
	}
	fprintf(stderr, "FAIL: %s: %s (%d) is not how an unplug ends a transfer\n",
		what, strerror(e), e);
	return 1;
}

/* How the loop ended */
static int verdict(int unplugged)
{
	printf("Completed %d iterations\n", iterations);
	if (!unplugged) {
		fprintf(stderr, "FAIL: stopped before the device was unplugged\n");
		return 1;
	}
	printf("PASS: %d transfers, then the unplug\n", iterations);
	return 0;
}

static int test_spi(const char *dev_path)
{
	int fd, last = -1, unplugged = 0;
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
		if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
			if (failed("SPI transfer", errno)) {
				close(fd);
				return 1;
			}
			unplugged = 1;
			break;
		}
		if (!counter_ok("SPI transfer", rx, sizeof(rx), &last)) {
			fprintf(stderr, "(iteration %d)\n", iterations);
			close(fd);
			return 1;
		}
		iterations++;
		usleep(1000);
	}

	close(fd);
	return verdict(unplugged);
}

static int test_i2c(const char *dev_path)
{
	int fd, last = -1, unplugged = 0;
	uint8_t buf[4];

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	/* 0x50: a counter device of the emulated bus */
	if (ioctl(fd, I2C_SLAVE, 0x50) < 0) {
		fprintf(stderr, "Failed to set I2C slave address: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("I2C hot-unplug test on %s\n", dev_path);
	printf("Performing continuous transfers until disconnect...\n");

	while (running) {
		buf[0] = 0;
		if (write(fd, buf, 1) < 0) {
			if (failed("I2C write", errno)) {
				close(fd);
				return 1;
			}
			unplugged = 1;
			break;
		}
		if (read(fd, buf, sizeof(buf)) < 0) {
			if (failed("I2C read", errno)) {
				close(fd);
				return 1;
			}
			unplugged = 1;
			break;
		}
		if (!counter_ok("I2C read", buf, sizeof(buf), &last)) {
			fprintf(stderr, "(iteration %d)\n", iterations);
			close(fd);
			return 1;
		}
		iterations++;
		usleep(1000);
	}

	close(fd);
	return verdict(unplugged);
}

static int test_gpio(const char *dev_path)
{
	int fd, unplugged = 0;
	struct gpiochip_info chip_info;
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values vals;
	int line_fd;
	int line_offset = 8;	/* AC0 */

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
		fprintf(stderr, "Failed to get chip info: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("GPIO hot-unplug test on %s (%s, %u lines)\n",
	       dev_path, chip_info.name, chip_info.lines);

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
		memset(&vals, 0, sizeof(vals));
		vals.mask = 1;
		vals.bits = iterations & 1;
		if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
			if (failed("GPIO set", errno))
				goto fail;
			unplugged = 1;
			break;
		}

		memset(&vals, 0, sizeof(vals));
		vals.mask = 1;
		if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
			if (failed("GPIO get", errno))
				goto fail;
			unplugged = 1;
			break;
		}
		if ((vals.bits & 1) != (uint64_t)(iterations & 1)) {
			fprintf(stderr, "FAIL: GPIO at iteration %d reads %d, set to %d\n",
				iterations, (int)(vals.bits & 1), iterations & 1);
			goto fail;
		}

		iterations++;
		usleep(500);
	}

	close(line_fd);
	close(fd);
	return verdict(unplugged);

fail:
	close(line_fd);
	close(fd);
	return 1;
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

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	if (strcmp(mode, "spi") == 0)
		return test_spi(dev_path);
	else if (strcmp(mode, "i2c") == 0)
		return test_i2c(dev_path);
	else if (strcmp(mode, "gpio") == 0)
		return test_gpio(dev_path);
	fprintf(stderr, "Unknown mode: %s (use spi, i2c, or gpio)\n", mode);
	return 1;
}
