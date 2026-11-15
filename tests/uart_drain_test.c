// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * uart_drain_test.c -- does the UART wait for the data still in flight?
 *
 * Once the tty layer has handed its data to the driver, several KiB are
 * still on their way: in the write URBs and in the chip's TX buffer.
 * tcdrain() and close() must wait for them. Needs an emulator started
 * with --uart-pace (data leaves the chip at the baud rate; the loopback
 * plug returns each byte when it is transmitted).
 *
 * Usage: uart_drain_test /dev/ttyFTDIx drain|close [bytes]
 *
 *   drain  write, tcdrain(), then everything must have been transmitted:
 *          all the bytes are read back within a short deadline (a thread
 *          reads all along)
 *   close  write, close(); the caller compares the emulator's count of
 *          transmitted bytes
 *
 * 19200 baud keeps the transfer long (about 0.5 ms per byte) compared to
 * the scheduling jitter of a loaded test machine.
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <termios.h>

#define DEFAULT_BYTES	12288
#define READ_DEADLINE	2000	/* ms after tcdrain() returned */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned char pattern(int i)
{
	return (unsigned char)(i * 31 + (i >> 8) + 7);
}

static int rd_fd, rd_len;
static unsigned char *rd_buf;
static atomic_int rd_got, rd_stop;

static void *reader(void *arg)
{
	(void)arg;
	while (!atomic_load(&rd_stop)) {
		struct pollfd pfd = { .fd = rd_fd, .events = POLLIN };
		int got = atomic_load(&rd_got);
		int r;

		if (got >= rd_len)
			break;
		if (poll(&pfd, 1, 50) <= 0)
			continue;
		r = read(rd_fd, rd_buf + got, rd_len - got);
		if (r > 0)
			atomic_fetch_add(&rd_got, r);
	}
	return NULL;
}

static int write_all(int fd, const unsigned char *buf, int len)
{
	int done = 0;

	while (done < len) {
		int r = write(fd, buf + done, len - done);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "FAIL: write: %s\n", strerror(errno));
			return -1;
		}
		done += r;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int len = argc > 3 ? atoi(argv[3]) : DEFAULT_BYTES;
	unsigned char *tx, *rx;
	struct termios tio;
	long long t0, t1, deadline;
	int fd, i, got, at_drain;
	pthread_t th;

	if (argc < 3 || len <= 0 ||
	    (strcmp(argv[2], "drain") && strcmp(argv[2], "close"))) {
		fprintf(stderr, "Usage: %s /dev/ttyFTDIx drain|close [bytes]\n",
			argv[0]);
		return 1;
	}

	tx = malloc(len);
	rx = malloc(len);
	if (!tx || !rx)
		return 1;
	for (i = 0; i < len; i++)
		tx[i] = pattern(i);

	fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	/* Raw 8N1, no flow control: nothing but the baud rate holds data back */
	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	cfsetispeed(&tio, B19200);
	cfsetospeed(&tio, B19200);
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "FAIL: tcsetattr: %s\n", strerror(errno));
		return 1;
	}
	tcflush(fd, TCIOFLUSH);

	if (strcmp(argv[2], "close")) {
		rd_fd = fd;
		rd_buf = rx;
		rd_len = len;
		if (pthread_create(&th, NULL, reader, NULL)) {
			fprintf(stderr, "FAIL: pthread_create\n");
			return 1;
		}
	}

	t0 = now_ms();
	if (write_all(fd, tx, len) < 0)
		return 1;

	if (!strcmp(argv[2], "close")) {
		close(fd);
		printf("wrote %d bytes at 19200 baud, close() returned after %lld ms (%d ms on the wire)\n",
		       len, now_ms() - t0, len * 10 * 1000 / 19200);
		return 0;
	}

	if (tcdrain(fd) < 0) {
		fprintf(stderr, "FAIL: tcdrain: %s\n", strerror(errno));
		return 1;
	}
	t1 = now_ms();
	at_drain = atomic_load(&rd_got);
	printf("wrote %d bytes at 19200 baud, tcdrain() returned after %lld ms (%d ms on the wire), %d bytes read back by then\n",
	       len, t1 - t0, len * 10 * 1000 / 19200, at_drain);

	/* All of it was sent, so all of it is back already */
	deadline = t1 + READ_DEADLINE;
	while (atomic_load(&rd_got) < len && now_ms() < deadline)
		usleep(10000);
	atomic_store(&rd_stop, 1);
	pthread_join(th, NULL);
	got = atomic_load(&rd_got);

	if (got < len) {
		fprintf(stderr, "FAIL: tcdrain() returned with %d of %d bytes still to be sent\n",
			len - got, len);
		return 1;
	}
	if (memcmp(tx, rx, len)) {
		for (i = 0; i < len && tx[i] == rx[i]; i++)
			;
		fprintf(stderr, "FAIL: data differs at byte %d: 0x%02x, sent 0x%02x\n",
			i, rx[i], tx[i]);
		return 1;
	}

	printf("PASS: all %d bytes were on the wire when tcdrain() returned\n", len);
	close(fd);
	return 0;
}
