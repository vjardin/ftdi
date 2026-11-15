// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * spi_echo_test.c -- SPI modes and timing against the emulator's
 * shift-register slaves (ftdi_usbip --spi-echo)
 *
 * One slave per SPI mode behind the chip selects AD4-AD7: with
 * spi_cs=3,4,5,6,7 they are spidevB.1 (mode 0) to spidevB.4 (mode 3).
 * Each is an 8-bit delay line between MOSI and MISO, clocked on the edges
 * of its mode: what is sent comes back one byte later, unchanged, only if
 * the master changes and samples the data on the right clock edges.
 *
 * Usage:
 *   spi_echo_test modes PREFIX          PREFIX.1 .. PREFIX.4, modes 0-3
 *   spi_echo_test wrong-mode PREFIX     mode 0 on the mode 1 slave: must
 *                                       not echo (the slaves do tell)
 *   spi_echo_test sync DEV [BYTES]      a write (250 bytes by default)
 *                                       returns once it is on the wire
 *   spi_echo_test delay DEV USECS       transfer delay before CS release
 *   spi_echo_test cs-change DEV         CS released between 2 transfers
 *   spi_echo_test word-delay DEV USECS  delay between the words (spidev:
 *                                       255 us at most)
 *
 * The last three only send the message: the emulator logs the timing of
 * every frame ("SPI frame ...") and tests/init.sh checks it.
 *
 * Returns 0 on success, 1 on failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define SLOW_HZ		10000	/* slow enough for the chip buffer to matter */

static int spi_open(const char *path, uint32_t mode, uint32_t speed)
{
	uint8_t bits = 8;
	int fd = open(path, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "FAIL: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		fprintf(stderr, "FAIL: %s: mode 0x%x, %u Hz: %s\n", path, mode,
			speed, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void fill(uint8_t *b, int n, uint8_t seed)
{
	int i;

	/* No two neighbours alike, both edges of every bit position */
	for (i = 0; i < n; i++)
		b[i] = seed + i * 0x4d + (i & 1 ? 0x80 : 0);
}

static void dump(const char *what, const uint8_t *b, int n)
{
	int i;

	fprintf(stderr, "  %s:", what);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %02x", b[i]);
	fprintf(stderr, "\n");
}

/* rx must be tx delayed by one byte; prev is the byte sent before tx[0] */
static int check_echo(const char *what, const uint8_t *tx, const uint8_t *rx,
		      int n, uint8_t prev)
{
	int i;

	for (i = 0; i < n; i++) {
		uint8_t want = i ? tx[i - 1] : prev;

		if (rx[i] != want) {
			fprintf(stderr, "FAIL: %s: byte %d is %02x, expected %02x\n",
				what, i, rx[i], want);
			dump("sent", tx, n);
			dump("got ", rx, n);
			return 1;
		}
	}
	return 0;
}

/* One device in its mode: full duplex, split transfers, write then read */
static int echo_one(const char *path, uint32_t mode, uint32_t speed)
{
	struct spi_ioc_transfer xf[3];
	uint8_t tx[40], rx[40];
	char what[96];
	int fd, ret = 1;

	fd = spi_open(path, mode, speed);
	if (fd < 0)
		return 1;
	snprintf(what, sizeof(what), "%s mode 0x%x %u Hz", path, mode, speed);

	/* Prime the delay line with a known byte */
	tx[0] = 0x3C;
	if (write(fd, tx, 1) != 1) {
		fprintf(stderr, "FAIL: %s: write: %s\n", what, strerror(errno));
		goto out;
	}

	fill(tx, 16, 0x81 + mode);
	memset(xf, 0, sizeof(xf));
	xf[0].tx_buf = (uintptr_t)tx;
	xf[0].rx_buf = (uintptr_t)rx;
	xf[0].len = 16;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), xf) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		goto out;
	}
	if (check_echo(what, tx, rx, 16, 0x3C))
		goto out;

	/* Three transfers in one frame: the clock must not hiccup between */
	fill(tx, 40, 0x17 + mode);
	memset(rx, 0, sizeof(rx));
	memset(xf, 0, sizeof(xf));
	xf[0].tx_buf = (uintptr_t)tx;
	xf[0].rx_buf = (uintptr_t)rx;
	xf[0].len = 3;
	xf[1].tx_buf = (uintptr_t)(tx + 3);
	xf[1].rx_buf = (uintptr_t)(rx + 3);
	xf[1].len = 30;
	xf[2].tx_buf = (uintptr_t)(tx + 33);
	xf[2].rx_buf = (uintptr_t)(rx + 33);
	xf[2].len = 7;
	if (ioctl(fd, SPI_IOC_MESSAGE(3), xf) < 0) {
		fprintf(stderr, "FAIL: %s: 3 transfers: %s\n", what,
			strerror(errno));
		goto out;
	}
	/* 0x81 + mode + 15 * 0x4d + 0x80: the last byte of the first pass */
	if (check_echo(what, tx, rx, 40,
		       (uint8_t)(0x81 + mode + 15 * 0x4d + 0x80)))
		goto out;

	/* Write only, then read only: the read starts with the last byte */
	fill(tx, 9, 0x55);
	memset(rx, 0xEE, sizeof(rx));
	memset(xf, 0, sizeof(xf));
	xf[0].tx_buf = (uintptr_t)tx;
	xf[0].len = 9;
	xf[1].rx_buf = (uintptr_t)rx;
	xf[1].len = 4;
	if (ioctl(fd, SPI_IOC_MESSAGE(2), xf) < 0) {
		fprintf(stderr, "FAIL: %s: write+read: %s\n", what,
			strerror(errno));
		goto out;
	}
	if (rx[0] != tx[8] || rx[1] || rx[2] || rx[3]) {
		fprintf(stderr, "FAIL: %s: read after write: %02x %02x %02x %02x, expected %02x 00 00 00\n",
			what, rx[0], rx[1], rx[2], rx[3], tx[8]);
		goto out;
	}
	ret = 0;
out:
	close(fd);
	return ret;
}

static int test_modes(const char *prefix)
{
	static const uint32_t speeds[] = { 1000000, 100000, 30000000 };
	char path[64];
	unsigned int m, i;

	int failed = 0;

	/* Every mode is tried, whatever the others do */
	for (m = 0; m < 4; m++) {
		int bad = 0;

		snprintf(path, sizeof(path), "%s.%u", prefix, m + 1);
		for (i = 0; i < 3 && !bad; i++)
			bad = echo_one(path, m, speeds[i]) ||
			      echo_one(path, m | SPI_LSB_FIRST, speeds[i]);
		if (bad) {
			failed++;
			continue;
		}
		printf("PASS: SPI mode %u echo on %s (MSB and LSB first, 3 speeds)\n",
		       m, path);
	}
	if (failed)
		return 1;
	printf("All SPI mode tests PASSED\n");
	return 0;
}

static int test_wrong_mode(const char *prefix)
{
	struct spi_ioc_transfer xf;
	uint8_t tx[16], rx[16];
	char path[64];
	int fd, i, same = 0;

	snprintf(path, sizeof(path), "%s.2", prefix);	/* the mode 1 slave */
	fd = spi_open(path, SPI_MODE_0, 1000000);
	if (fd < 0)
		return 1;
	fill(tx, 16, 0x29);
	memset(&xf, 0, sizeof(xf));
	xf.tx_buf = (uintptr_t)tx;
	xf.rx_buf = (uintptr_t)rx;
	xf.len = 16;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xf) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	for (i = 1; i < 16; i++)
		same += rx[i] == tx[i - 1];
	if (same == 15) {
		fprintf(stderr, "FAIL: the mode 1 slave echoes a mode 0 master\n");
		return 1;
	}
	printf("PASS: mode 0 on the mode 1 slave does not echo (%d/15 bytes)\n",
	       same);
	return 0;
}

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/*
 * spi_sync() of a write must not return before the data left the pins.
 * With enough bytes the wire time exceeds the driver's response timeout
 * (2 s): the write must still succeed.
 */
static int test_sync(const char *path, int len, int hz)
{
	static uint8_t tx[4096];
	long long t0, dt, wire;
	int fd;

	if (len < 1 || len > (int)sizeof(tx))
		len = 250;
	if (hz < 1)
		hz = SLOW_HZ;
	fd = spi_open(path, SPI_MODE_0, hz);
	if (fd < 0)
		return 1;
	fill(tx, len, 0x11);
	wire = len * 8 * 1000000LL / hz;

	t0 = now_us();
	if (write(fd, tx, len) != len) {
		fprintf(stderr, "FAIL: write of %d bytes at %d Hz: %s\n", len,
			hz, strerror(errno));
		close(fd);
		return 1;
	}
	dt = now_us() - t0;
	close(fd);

	if (dt < wire) {
		fprintf(stderr, "FAIL: write of %d bytes at %d Hz returned after %lld us, the wire needs %lld us\n",
			len, hz, dt, wire);
		return 1;
	}
	printf("PASS: write of %d bytes at %d Hz returned after %lld us (%lld us on the wire)\n",
	       len, hz, dt, wire);
	return 0;
}

static int test_timing(const char *what, const char *path, int usecs)
{
	struct spi_ioc_transfer xf[2];
	uint8_t tx[64];
	int fd, n = 1;

	fd = spi_open(path, SPI_MODE_0, SLOW_HZ);
	if (fd < 0)
		return 1;
	fill(tx, sizeof(tx), 0x42);
	memset(xf, 0, sizeof(xf));
	xf[0].tx_buf = (uintptr_t)tx;

	if (!strcmp(what, "delay")) {
		/* 50 bytes still in the chip when the write returns */
		xf[0].len = 50;
		xf[0].delay_usecs = usecs;
	} else if (!strcmp(what, "cs-change")) {
		xf[0].len = 50;
		xf[0].cs_change = 1;
		xf[1].tx_buf = (uintptr_t)tx;
		xf[1].len = 4;
		n = 2;
	} else {
		xf[0].len = 6;
		xf[0].word_delay_usecs = usecs;
	}

	if (ioctl(fd, SPI_IOC_MESSAGE(n), xf) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("PASS: %s message sent\n", what);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s modes|wrong-mode PREFIX\n"
			"       %s sync DEV [BYTES [HZ]]\n"
			"       %s cs-change DEV\n"
			"       %s delay|word-delay DEV USECS\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}
	if (!strcmp(argv[1], "modes"))
		return test_modes(argv[2]);
	if (!strcmp(argv[1], "wrong-mode"))
		return test_wrong_mode(argv[2]);
	if (!strcmp(argv[1], "sync"))
		return test_sync(argv[2], argc >= 4 ? atoi(argv[3]) : 250,
				 argc >= 5 ? atoi(argv[4]) : 0);
	if (!strcmp(argv[1], "cs-change"))
		return test_timing(argv[1], argv[2], 0);
	if ((!strcmp(argv[1], "delay") || !strcmp(argv[1], "word-delay")) &&
	    argc >= 4)
		return test_timing(argv[1], argv[2], atoi(argv[3]));

	fprintf(stderr, "Unknown test: %s\n", argv[1]);
	return 1;
}
