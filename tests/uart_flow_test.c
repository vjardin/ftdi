// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * uart_flow_test.c -- write to a UART that nobody reads, then read
 *
 * Usage: uart_flow_test /dev/ttyFTDIx none|rtscts|dtrdsr|xonxoff [bytes]
 *
 *   rtscts, dtrdsr, xonxoff   every byte must be read back, no overrun
 *   none                      bytes must be lost and the overrun counted
 *
 * dtrdsr: the caller sets the sysfs attribute dtr_dsr_flow first. Needs an
 * emulator started with --uart-pace.
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/serial.h>

#define DEFAULT_BYTES	16384
#define BAUD		115200
#define IDLE_MS		1500	/* no more data is coming */

static int tty_fd;
static unsigned char *tx;
static int tx_len;
static atomic_int tx_done;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Printable: neither XON/XOFF nor anything the line discipline marks */
static unsigned char pattern(int i)
{
	return (unsigned char)(0x20 + (i + i / 95) % 95);
}

static void *writer(void *arg)
{
	int done = 0;

	(void)arg;
	while (done < tx_len) {
		int r = write(tty_fd, tx + done, tx_len - done);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "FAIL: write: %s\n", strerror(errno));
			atomic_store(&tx_done, -1);
			return NULL;
		}
		done += r;
	}
	atomic_store(&tx_done, 1);
	return NULL;
}

int main(int argc, char *argv[])
{
	struct serial_icounter_struct ic0, ic1;
	struct termios tio;
	unsigned char *rx;
	long long deadline;
	pthread_t th;
	bool flow;
	int i, got = 0, overruns;

	tx_len = argc > 3 ? atoi(argv[3]) : DEFAULT_BYTES;
	if (argc < 3 || tx_len <= 0 ||
	    (strcmp(argv[2], "none") && strcmp(argv[2], "rtscts") &&
	     strcmp(argv[2], "dtrdsr") && strcmp(argv[2], "xonxoff"))) {
		fprintf(stderr, "Usage: %s /dev/ttyFTDIx none|rtscts|dtrdsr|xonxoff [bytes]\n",
			argv[0]);
		return 1;
	}
	flow = strcmp(argv[2], "none");

	tx = malloc(tx_len);
	rx = malloc(tx_len);
	if (!tx || !rx)
		return 1;
	for (i = 0; i < tx_len; i++)
		tx[i] = pattern(i);

	tty_fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (tty_fd < 0) {
		fprintf(stderr, "FAIL: open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_cc[VSTART] = 0x11;
	tio.c_cc[VSTOP] = 0x13;
	if (!strcmp(argv[2], "rtscts"))
		tio.c_cflag |= CRTSCTS;
	if (!strcmp(argv[2], "xonxoff"))
		tio.c_iflag |= IXON;
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	if (tcsetattr(tty_fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "FAIL: tcsetattr: %s\n", strerror(errno));
		return 1;
	}
	tcflush(tty_fd, TCIOFLUSH);
	if (ioctl(tty_fd, TIOCGICOUNT, &ic0) < 0) {
		fprintf(stderr, "FAIL: TIOCGICOUNT: %s\n", strerror(errno));
		return 1;
	}

	if (pthread_create(&th, NULL, writer, NULL)) {
		fprintf(stderr, "FAIL: pthread_create\n");
		return 1;
	}

	/* The time all of it takes on the wire, and a margin */
	usleep((tx_len * 10LL * 1000 / BAUD + 500) * 1000);

	deadline = now_ms() + IDLE_MS;
	while (got < tx_len) {
		struct pollfd pfd = { .fd = tty_fd, .events = POLLIN };
		long long left = deadline - now_ms();
		int r;

		if (left <= 0)
			break;
		r = poll(&pfd, 1, (int)left);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		r = read(tty_fd, rx + got, tx_len - got);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			fprintf(stderr, "FAIL: read: %s\n", strerror(errno));
			return 1;
		}
		got += r;
		deadline = now_ms() + IDLE_MS;
	}

	/* A writer still blocked now would never end: do not join it */
	for (i = 0; i < 300 && !atomic_load(&tx_done); i++)
		usleep(10000);
	if (atomic_load(&tx_done) != 1) {
		fprintf(stderr, "FAIL: %s: the writer %s (%d of %d bytes read back)\n",
			argv[2], atomic_load(&tx_done) ? "failed" :
			"is still blocked", got, tx_len);
		return 1;
	}
	pthread_join(th, NULL);

	if (ioctl(tty_fd, TIOCGICOUNT, &ic1) < 0) {
		fprintf(stderr, "FAIL: TIOCGICOUNT: %s\n", strerror(errno));
		return 1;
	}
	overruns = ic1.overrun - ic0.overrun;
	close(tty_fd);

	if (!flow) {
		if (got == tx_len) {
			fprintf(stderr, "FAIL: none: all %d bytes read back, the receiver never overran (emulator without --uart-pace?)\n",
				tx_len);
			return 1;
		}
		if (overruns <= 0) {
			fprintf(stderr, "FAIL: none: %d of %d bytes lost and no overrun counted\n",
				tx_len - got, tx_len);
			return 1;
		}
		printf("PASS: none: %d of %d bytes lost, %d overrun(s) counted\n",
		       tx_len - got, tx_len, overruns);
		return 0;
	}

	if (got != tx_len) {
		fprintf(stderr, "FAIL: %s: %d of %d bytes read back, %d overrun(s) counted\n",
			argv[2], got, tx_len, overruns);
		return 1;
	}
	if (memcmp(tx, rx, tx_len)) {
		for (i = 0; i < tx_len && tx[i] == rx[i]; i++)
			;
		fprintf(stderr, "FAIL: %s: data differs at byte %d: 0x%02x, sent 0x%02x\n",
			argv[2], i, rx[i], tx[i]);
		return 1;
	}
	if (overruns) {
		fprintf(stderr, "FAIL: %s: %d overrun(s) counted with every byte read back\n",
			argv[2], overruns);
		return 1;
	}
	printf("PASS: %s: all %d bytes read back after the port was left unread, no overrun\n",
	       argv[2], tx_len);
	return 0;
}
