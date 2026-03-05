// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * gpio_test.c -- GPIO test tool for FTDI driver testing
 *
 * Uses the modern GPIO chardev v2 ioctl interface (same as libgpiod)
 * to test GPIO line direction setting and value read/write.
 *
 * Usage: gpio_test /dev/gpiochipN [line_offset]
 *
 * If line_offset is not specified, the test will find the first
 * available (non-reserved) line to test.
 *
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#define GPIO_CONSUMER	"gpio_test"

/* Find first available (non-used) line */
static int find_available_line(int fd, struct gpiochip_info *info)
{
	struct gpio_v2_line_info linfo;
	unsigned int i;

	for (i = 0; i < info->lines; i++) {
		memset(&linfo, 0, sizeof(linfo));
		linfo.offset = i;

		if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &linfo) < 0)
			continue;

		/* Skip lines that are already in use */
		if (linfo.flags & GPIO_V2_LINE_FLAG_USED)
			continue;

		return i;
	}

	return -1;
}

int main(int argc, char *argv[])
{
	const char *dev_path;
	int fd, line_offset = -1;
	struct gpiochip_info chip_info;
	struct gpio_v2_line_info line_info;
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values vals;
	int line_fd;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/gpiochipN [line_offset]\n", argv[0]);
		return 1;
	}

	dev_path = argv[1];
	if (argc >= 3)
		line_offset = atoi(argv[2]);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("GPIO test on %s\n", dev_path);

	/*
	 * Test 1: Get chip info
	 */
	printf("Test 1: Get chip info... ");
	memset(&chip_info, 0, sizeof(chip_info));
	if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
		fprintf(stderr, "FAIL: GPIO_GET_CHIPINFO_IOCTL failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (name=%s, label=%s, lines=%u)\n",
	       chip_info.name, chip_info.label, chip_info.lines);

	/*
	 * Find a line to test
	 */
	if (line_offset < 0) {
		line_offset = find_available_line(fd, &chip_info);
		if (line_offset < 0) {
			fprintf(stderr, "No available GPIO lines found\n");
			close(fd);
			return 1;
		}
	}

	if ((unsigned int)line_offset >= chip_info.lines) {
		fprintf(stderr, "Line offset %d out of range (max %u)\n",
			line_offset, chip_info.lines - 1);
		close(fd);
		return 1;
	}

	printf("Testing line %d\n", line_offset);

	/*
	 * Test 2: Get line info
	 */
	printf("Test 2: Get line info... ");
	memset(&line_info, 0, sizeof(line_info));
	line_info.offset = line_offset;
	if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line_info) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_GET_LINEINFO_IOCTL failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (name=%s, flags=0x%llx)\n",
	       line_info.name[0] ? line_info.name : "(none)",
	       (unsigned long long)line_info.flags);

	/*
	 * Test 3: Request line as output
	 */
	printf("Test 3: Request line as output... ");
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	strncpy(req.consumer, GPIO_CONSUMER, sizeof(req.consumer) - 1);

	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_GET_LINE_IOCTL failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	line_fd = req.fd;
	printf("PASS (line_fd=%d)\n", line_fd);

	/*
	 * Test 4: Set output high
	 */
	printf("Test 4: Set output high... ");
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	vals.bits = 1;
	if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_LINE_SET_VALUES_IOCTL failed: %s\n",
			strerror(errno));
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS\n");

	/*
	 * Test 5: Read back value (should be high)
	 */
	printf("Test 5: Read back value (expect high)... ");
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_LINE_GET_VALUES_IOCTL failed: %s\n",
			strerror(errno));
		close(line_fd);
		close(fd);
		return 1;
	}
	if (!(vals.bits & 1)) {
		fprintf(stderr, "FAIL: expected high, got low\n");
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS (value=1)\n");

	/*
	 * Test 6: Set output low
	 */
	printf("Test 6: Set output low... ");
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	vals.bits = 0;
	if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_LINE_SET_VALUES_IOCTL failed: %s\n",
			strerror(errno));
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS\n");

	/*
	 * Test 7: Read back value (should be low)
	 */
	printf("Test 7: Read back value (expect low)... ");
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_LINE_GET_VALUES_IOCTL failed: %s\n",
			strerror(errno));
		close(line_fd);
		close(fd);
		return 1;
	}
	if (vals.bits & 1) {
		fprintf(stderr, "FAIL: expected low, got high\n");
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS (value=0)\n");

	/* Release the line */
	close(line_fd);

	/*
	 * Test 8: Request line as input
	 */
	printf("Test 8: Request line as input... ");
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
	strncpy(req.consumer, GPIO_CONSUMER, sizeof(req.consumer) - 1);

	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_GET_LINE_IOCTL (input) failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	line_fd = req.fd;
	printf("PASS (line_fd=%d)\n", line_fd);

	/*
	 * Test 9: Read input value
	 */
	printf("Test 9: Read input value... ");
	memset(&vals, 0, sizeof(vals));
	vals.mask = 1;
	if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: GPIO_V2_LINE_GET_VALUES_IOCTL failed: %s\n",
			strerror(errno));
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS (value=%d)\n", (int)(vals.bits & 1));

	close(line_fd);
	close(fd);

	printf("All GPIO tests PASSED\n");
	return 0;
}
