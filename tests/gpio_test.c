// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * gpio_test.c -- GPIO test tool for FTDI driver testing
 *
 * Direction and value of one line, through the GPIO chardev v2 ioctls
 * (those libgpiod uses).
 *
 * Usage: gpio_test CHIP [LINE]             CHIP: /dev/gpiochipN; default
 *                                          LINE: the first one not in use
 *        gpio_test CHIP LINE busy          the line must be refused (EBUSY)
 *        gpio_test CHIP LINE hold SECS     request it as input, keep it
 *        gpio_test CHIP LINE set 0|1 [od]  drive it (open drain with "od")
 *                                          and leave it that way
 *        gpio_test CHIP LINE get 0|1       an input must read that level
 *        gpio_test CHIP LINE edge SECS     print the edge events of an input
 *
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#define GPIO_CONSUMER	"gpio_test"

static int find_available_line(int fd, struct gpiochip_info *info)
{
	struct gpio_v2_line_info linfo;
	unsigned int i;

	for (i = 0; i < info->lines; i++) {
		memset(&linfo, 0, sizeof(linfo));
		linfo.offset = i;

		if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &linfo) < 0)
			continue;

		if (linfo.flags & GPIO_V2_LINE_FLAG_USED)
			continue;

		return i;
	}

	return -1;
}

/* Ask for one line as input; returns the line fd, or -1 with errno set */
static int request_input(int fd, int line_offset)
{
	struct gpio_v2_line_request req;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
	strncpy(req.consumer, GPIO_CONSUMER, sizeof(req.consumer) - 1);
	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
		return -1;
	return req.fd;
}

/* The line belongs to someone else in the kernel: it must be refused */
static int test_busy(int fd, int line_offset)
{
	int line_fd = request_input(fd, line_offset);

	if (line_fd >= 0) {
		fprintf(stderr, "FAIL: line %d was granted, expected EBUSY\n",
			line_offset);
		close(line_fd);
		return 1;
	}
	if (errno != EBUSY) {
		fprintf(stderr, "FAIL: line %d: %s, expected EBUSY\n",
			line_offset, strerror(errno));
		return 1;
	}
	printf("PASS: line %d refused (EBUSY)\n", line_offset);
	return 0;
}

/* Keep the line requested for a while, for another test to run into it */
static int test_hold(int fd, int line_offset, int secs)
{
	int line_fd = request_input(fd, line_offset);

	if (line_fd < 0) {
		fprintf(stderr, "FAIL: line %d: %s\n", line_offset,
			strerror(errno));
		return 1;
	}
	printf("HOLD: line %d requested for %d s\n", line_offset, secs);
	fflush(stdout);
	sleep(secs);
	close(line_fd);
	return 0;
}

static int test_get(int fd, int line_offset, int expect)
{
	struct gpio_v2_line_values vals = { .mask = 1 };
	int line_fd = request_input(fd, line_offset);

	if (line_fd < 0 ||
	    ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
		fprintf(stderr, "FAIL: line %d: %s\n", line_offset,
			strerror(errno));
		return 1;
	}
	close(line_fd);
	if ((int)(vals.bits & 1) != !!expect) {
		fprintf(stderr, "FAIL: line %d reads %d, expected %d\n",
			line_offset, (int)(vals.bits & 1), !!expect);
		return 1;
	}
	printf("PASS: line %d reads %d\n", line_offset, !!expect);
	return 0;
}

static int test_edge(int fd, int line_offset, int secs)
{
	struct gpio_v2_line_request req;
	struct gpio_v2_line_event ev;
	struct pollfd pfd;
	time_t end = time(NULL) + secs;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
			   GPIO_V2_LINE_FLAG_EDGE_RISING |
			   GPIO_V2_LINE_FLAG_EDGE_FALLING;
	strncpy(req.consumer, GPIO_CONSUMER, sizeof(req.consumer) - 1);
	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "FAIL: line %d: %s\n", line_offset,
			strerror(errno));
		return 1;
	}
	printf("HOLD: line %d watched for %d s\n", line_offset, secs);
	fflush(stdout);

	pfd.fd = req.fd;
	pfd.events = POLLIN;
	while (time(NULL) < end) {
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		if (read(req.fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
		printf("EDGE %s\n", ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE ?
		       "rising" : "falling");
		fflush(stdout);
	}
	close(req.fd);
	return 0;
}

/*
 * Drive the line and release it: the driver keeps direction and value,
 * so the pin stays as it is for a later test to look at.
 */
static int test_set(int fd, int line_offset, int value, int open_drain)
{
	struct gpio_v2_line_request req;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = line_offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	if (open_drain)
		req.config.flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = value ? 1 : 0;
	req.config.attrs[0].mask = 1;
	strncpy(req.consumer, GPIO_CONSUMER, sizeof(req.consumer) - 1);
	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "FAIL: line %d: %s\n", line_offset,
			strerror(errno));
		return 1;
	}
	close(req.fd);
	printf("PASS: line %d driven to %d%s\n", line_offset, !!value,
	       open_drain ? " (open drain)" : "");
	return 0;
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

	if (argc >= 4 && !strcmp(argv[3], "busy")) {
		int ret = test_busy(fd, line_offset);

		close(fd);
		return ret;
	}
	if (argc >= 5 && !strcmp(argv[3], "hold")) {
		int ret = test_hold(fd, line_offset, atoi(argv[4]));

		close(fd);
		return ret;
	}
	if (argc >= 5 && !strcmp(argv[3], "get")) {
		int ret = test_get(fd, line_offset, atoi(argv[4]));

		close(fd);
		return ret;
	}
	if (argc >= 5 && !strcmp(argv[3], "edge")) {
		int ret = test_edge(fd, line_offset, atoi(argv[4]));

		close(fd);
		return ret;
	}
	if (argc >= 5 && !strcmp(argv[3], "set")) {
		int ret = test_set(fd, line_offset, atoi(argv[4]),
				   argc >= 6 && !strcmp(argv[5], "od"));

		close(fd);
		return ret;
	}

	printf("GPIO test on %s\n", dev_path);

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

	close(line_fd);

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
	 * Test 7 left the pin driven low. An input is pulled up inside the
	 * chip (DS_FT232H, DS_FT2232H, DS_FT4232H), so it reads 1 only if the
	 * driver really stopped driving it.
	 */
	printf("Test 9: Read input value (expect high, pull-up)... ");
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
		fprintf(stderr, "FAIL: input reads low: the pin is still driven\n");
		close(line_fd);
		close(fd);
		return 1;
	}
	printf("PASS (value=1)\n");

	close(line_fd);
	close(fd);

	printf("All GPIO tests PASSED\n");
	return 0;
}
