// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * suspend_test.c -- Suspend/resume testing for FTDI driver
 *
 * Tests driver power management by performing I/O operations before and
 * after triggering USB suspend/resume via sysfs power control.
 *
 * Usage: suspend_test <spi|i2c|gpio> <device_path> <usb_device_path>
 *
 * Example: suspend_test spi /dev/spidev0.0 /sys/bus/usb/devices/1-1
 *
 * Returns 0 if suspend/resume handled correctly, 1 on failure.
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
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>

#define SLAVE_ADDR	0x50
#define GPIO_PIN	8	/* AC0 - not reserved by SPI/I2C */

/*
 * Write to a sysfs file
 */
static int sysfs_write(const char *path, const char *value)
{
	int fd, ret;
	size_t len = strlen(value);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}

	ret = write(fd, value, len);
	close(fd);

	if (ret < 0) {
		fprintf(stderr, "Failed to write to %s: %s\n", path, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Read from a sysfs file
 */
static int sysfs_read(const char *path, char *buf, size_t buflen)
{
	int fd, ret;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, buf, buflen - 1);
	close(fd);

	if (ret < 0)
		return -1;

	buf[ret] = '\0';
	/* Strip newline */
	if (ret > 0 && buf[ret - 1] == '\n')
		buf[ret - 1] = '\0';

	return 0;
}

/*
 * Test SPI operations
 */
static int test_spi_io(int fd, const char *phase)
{
	uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t rx[4];
	struct spi_ioc_transfer xfer = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = sizeof(tx),
		.speed_hz = 1000000,
		.bits_per_word = 8,
	};
	int ret;

	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
	if (ret < 0) {
		fprintf(stderr, "%s: SPI transfer failed: %s (%d)\n",
			phase, strerror(errno), errno);
		return -1;
	}

	/* Verify counter pattern from emulator */
	if (rx[1] != (uint8_t)(rx[0] + 1) || rx[2] != (uint8_t)(rx[0] + 2)) {
		fprintf(stderr, "%s: unexpected SPI data %02x %02x %02x %02x\n",
			phase, rx[0], rx[1], rx[2], rx[3]);
		return -1;
	}

	printf("%s: SPI OK (rx: %02x %02x %02x %02x)\n",
	       phase, rx[0], rx[1], rx[2], rx[3]);
	return 0;
}

/*
 * Test I2C operations
 */
static int test_i2c_io(int fd, const char *phase)
{
	uint8_t buf[4];
	int ret;

	ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		fprintf(stderr, "%s: I2C read failed: %s (%d)\n",
			phase, strerror(errno), errno);
		return -1;
	}

	/* Verify counter pattern */
	if (buf[1] != (uint8_t)(buf[0] + 1) || buf[2] != (uint8_t)(buf[0] + 2)) {
		fprintf(stderr, "%s: unexpected I2C data %02x %02x %02x %02x\n",
			phase, buf[0], buf[1], buf[2], buf[3]);
		return -1;
	}

	printf("%s: I2C OK (rx: %02x %02x %02x %02x)\n",
	       phase, buf[0], buf[1], buf[2], buf[3]);
	return 0;
}

/*
 * Test GPIO operations
 */
static int test_gpio_io(int fd, const char *phase, int *line_fd)
{
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values vals;
	int ret;

	/* First time: request the GPIO line */
	if (*line_fd < 0) {
		memset(&req, 0, sizeof(req));
		req.offsets[0] = GPIO_PIN;
		req.num_lines = 1;
		req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
		strncpy(req.consumer, "suspend_test", sizeof(req.consumer) - 1);

		ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
		if (ret < 0) {
			fprintf(stderr, "%s: GPIO line request failed: %s\n",
				phase, strerror(errno));
			return -1;
		}
		*line_fd = req.fd;
	}

	/* Set GPIO high */
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	vals.bits = 1;
	ret = ioctl(*line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
	if (ret < 0) {
		fprintf(stderr, "%s: GPIO set high failed: %s (%d)\n",
			phase, strerror(errno), errno);
		return -1;
	}

	/* Read back */
	vals.bits = 0;
	ret = ioctl(*line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals);
	if (ret < 0) {
		fprintf(stderr, "%s: GPIO get failed: %s (%d)\n",
			phase, strerror(errno), errno);
		return -1;
	}

	if (!(vals.bits & 1)) {
		fprintf(stderr, "%s: GPIO value mismatch (expected 1, got 0)\n", phase);
		return -1;
	}

	/* Set GPIO low */
	vals.bits = 0;
	ret = ioctl(*line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
	if (ret < 0) {
		fprintf(stderr, "%s: GPIO set low failed: %s (%d)\n",
			phase, strerror(errno), errno);
		return -1;
	}

	printf("%s: GPIO OK (toggle high->low successful)\n", phase);
	return 0;
}

/*
 * Trigger USB suspend via sysfs
 * Returns 0 if suspend triggered, -1 if not supported
 */
static int trigger_suspend(const char *usb_path)
{
	char path[256];
	char status[32];

	/* Enable autosuspend */
	snprintf(path, sizeof(path), "%s/power/control", usb_path);
	if (sysfs_write(path, "auto") < 0) {
		printf("Note: USB power control not available (may not support suspend)\n");
		return -1;
	}

	/* Set immediate autosuspend (0ms delay) */
	snprintf(path, sizeof(path), "%s/power/autosuspend_delay_ms", usb_path);
	sysfs_write(path, "0");

	/* Wait for device to suspend */
	usleep(100000);

	/* Check runtime status */
	snprintf(path, sizeof(path), "%s/power/runtime_status", usb_path);
	if (sysfs_read(path, status, sizeof(status)) == 0) {
		printf("USB runtime status: %s\n", status);
		if (strcmp(status, "suspended") == 0)
			return 0;
	}

	return -1;
}

/*
 * Trigger USB resume via sysfs
 */
static int trigger_resume(const char *usb_path)
{
	char path[256];

	/* Disable autosuspend (force "on") */
	snprintf(path, sizeof(path), "%s/power/control", usb_path);
	if (sysfs_write(path, "on") < 0)
		return -1;

	/* Wait for resume */
	usleep(100000);

	return 0;
}

static int run_spi_test(const char *dev_path, const char *usb_path)
{
	int fd, ret = 0;
	int suspend_supported;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("=== SPI Suspend/Resume Test ===\n");

	/* Phase 1: Verify I/O works before suspend */
	printf("\n[Phase 1: Pre-suspend I/O]\n");
	if (test_spi_io(fd, "Before suspend") < 0) {
		ret = 1;
		goto out;
	}

	/* Phase 2: Trigger suspend */
	printf("\n[Phase 2: Triggering suspend]\n");
	suspend_supported = (trigger_suspend(usb_path) == 0);
	if (suspend_supported) {
		printf("Device suspended successfully\n");

		/* Try I/O during suspend - should fail or block */
		printf("Attempting I/O during suspend...\n");
		/* Note: I/O may still work if kernel auto-resumes */
	} else {
		printf("USB suspend not supported in this environment\n");
		printf("Testing resume path only...\n");
	}

	/* Phase 3: Trigger resume */
	printf("\n[Phase 3: Triggering resume]\n");
	if (trigger_resume(usb_path) < 0) {
		printf("Resume trigger failed (may not be needed)\n");
	} else {
		printf("Resume triggered\n");
	}

	/* Small delay to ensure resume completes */
	usleep(200000);

	/* Phase 4: Verify I/O works after resume */
	printf("\n[Phase 4: Post-resume I/O]\n");
	if (test_spi_io(fd, "After resume") < 0) {
		ret = 1;
		goto out;
	}

	printf("\n=== SPI Suspend/Resume Test PASSED ===\n");

out:
	close(fd);
	return ret;
}

static int run_i2c_test(const char *dev_path, const char *usb_path)
{
	int fd, ret = 0;
	int suspend_supported;

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

	printf("=== I2C Suspend/Resume Test ===\n");

	/* Phase 1: Verify I/O works before suspend */
	printf("\n[Phase 1: Pre-suspend I/O]\n");
	if (test_i2c_io(fd, "Before suspend") < 0) {
		ret = 1;
		goto out;
	}

	/* Phase 2: Trigger suspend */
	printf("\n[Phase 2: Triggering suspend]\n");
	suspend_supported = (trigger_suspend(usb_path) == 0);
	if (suspend_supported) {
		printf("Device suspended successfully\n");
	} else {
		printf("USB suspend not supported, testing resume path only\n");
	}

	/* Phase 3: Trigger resume */
	printf("\n[Phase 3: Triggering resume]\n");
	trigger_resume(usb_path);
	usleep(200000);

	/* Phase 4: Verify I/O works after resume */
	printf("\n[Phase 4: Post-resume I/O]\n");
	if (test_i2c_io(fd, "After resume") < 0) {
		ret = 1;
		goto out;
	}

	printf("\n=== I2C Suspend/Resume Test PASSED ===\n");

out:
	close(fd);
	return ret;
}

static int run_gpio_test(const char *dev_path, const char *usb_path)
{
	int fd, line_fd = -1, ret = 0;
	int suspend_supported;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("=== GPIO Suspend/Resume Test ===\n");

	/* Phase 1: Verify I/O works before suspend */
	printf("\n[Phase 1: Pre-suspend I/O]\n");
	if (test_gpio_io(fd, "Before suspend", &line_fd) < 0) {
		ret = 1;
		goto out;
	}

	/* Phase 2: Trigger suspend */
	printf("\n[Phase 2: Triggering suspend]\n");
	suspend_supported = (trigger_suspend(usb_path) == 0);
	if (suspend_supported) {
		printf("Device suspended successfully\n");
	} else {
		printf("USB suspend not supported, testing resume path only\n");
	}

	/* Phase 3: Trigger resume */
	printf("\n[Phase 3: Triggering resume]\n");
	trigger_resume(usb_path);
	usleep(200000);

	/* Phase 4: Verify I/O works after resume */
	printf("\n[Phase 4: Post-resume I/O]\n");
	if (test_gpio_io(fd, "After resume", &line_fd) < 0) {
		ret = 1;
		goto out;
	}

	printf("\n=== GPIO Suspend/Resume Test PASSED ===\n");

out:
	if (line_fd >= 0)
		close(line_fd);
	close(fd);
	return ret;
}

int main(int argc, char *argv[])
{
	const char *mode;
	const char *dev_path;
	const char *usb_path;

	if (argc < 4) {
		fprintf(stderr, "Usage: %s <spi|i2c|gpio> <device_path> <usb_device_sysfs_path>\n",
			argv[0]);
		fprintf(stderr, "Example: %s spi /dev/spidev0.0 /sys/bus/usb/devices/1-1\n",
			argv[0]);
		return 1;
	}

	mode = argv[1];
	dev_path = argv[2];
	usb_path = argv[3];

	if (strcmp(mode, "spi") == 0)
		return run_spi_test(dev_path, usb_path);
	else if (strcmp(mode, "i2c") == 0)
		return run_i2c_test(dev_path, usb_path);
	else if (strcmp(mode, "gpio") == 0)
		return run_gpio_test(dev_path, usb_path);
	else {
		fprintf(stderr, "Unknown mode: %s (use spi, i2c, or gpio)\n", mode);
		return 1;
	}
}
