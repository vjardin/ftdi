// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * uart_test.c -- UART test tool for FTDI driver testing
 *
 * The emulator attaches a loopback plug to every UART (TXD->RXD,
 * RTS->CTS, DTR->DSR+DCD), so everything written must come back
 * unchanged and the modem inputs must follow the modem outputs.
 *
 * Usage: uart_test /dev/ttyFTDIx [bulk_bytes]
 *        uart_test /dev/ttyFTDIx hold SECS   keep the port open at 57600
 *                                            baud for SECS seconds, then
 *                                            check that the loopback
 *                                            still works on that open
 *
 * Every read has a timeout: a dead UART fails the test instead of
 * hanging it. Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/serial.h>

#define TEST_PATTERN_LEN	4
#define DEFAULT_BULK		(64 * 1024)

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Returns the number of bytes read before the timeout, or -errno */
static int read_exact(int fd, unsigned char *buf, int len, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	int got = 0;

	while (got < len) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long long left = deadline - now_ms();
		int r;

		if (left <= 0)
			break;
		r = poll(&pfd, 1, (int)left);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		r = read(fd, buf + got, len - got);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -errno;
		}
		got += r;
	}
	return got;
}

static int set_line(int fd, speed_t speed)
{
	struct termios tio;

	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	return tcsetattr(fd, TCSANOW, &tio);
}

static int echo_check(int fd, const unsigned char *tx, int len,
		      const char *what)
{
	unsigned char rx[256];
	int r;

	if (write(fd, tx, len) != len) {
		fprintf(stderr, "FAIL: %s: write failed: %s\n", what,
			strerror(errno));
		return -1;
	}
	r = read_exact(fd, rx, len, 2000);
	if (r != len) {
		fprintf(stderr, "FAIL: %s: read %d/%d bytes (loopback dead?)\n",
			what, r, len);
		return -1;
	}
	if (memcmp(tx, rx, len)) {
		fprintf(stderr, "FAIL: %s: data mismatch %02x %02x %02x %02x\n",
			what, rx[0], rx[1], rx[2], rx[3]);
		return -1;
	}
	return 0;
}

/* 7 data bits: the eighth one is not on the wire */
static int test_7bits(int fd)
{
	static const unsigned char tx[4] = { 0xC1, 0x7E, 0x80, 0xFF };
	static const unsigned char want[4] = { 0x41, 0x7E, 0x00, 0x7F };
	unsigned char rx[4];
	struct termios tio;
	int r;

	if (tcgetattr(fd, &tio) < 0)
		return -1;
	tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS7;
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "FAIL: tcsetattr(CS7): %s\n", strerror(errno));
		return -1;
	}
	if (write(fd, tx, sizeof(tx)) != sizeof(tx)) {
		fprintf(stderr, "FAIL: 7 bits: write: %s\n", strerror(errno));
		return -1;
	}
	r = read_exact(fd, rx, sizeof(rx), 2000);
	if (r != sizeof(rx) || memcmp(rx, want, sizeof(rx))) {
		fprintf(stderr, "FAIL: 7 bits: read %d: %02x %02x %02x %02x, expected %02x %02x %02x %02x\n",
			r, rx[0], rx[1], rx[2], rx[3], want[0], want[1],
			want[2], want[3]);
		return -1;
	}
	return 0;
}

/* PARMRK: the line discipline marks a break FF 00 00 */
static int test_break(int fd)
{
	static const unsigned char want[7] = {
		'A', 'B', 0xFF, 0x00, 0x00, 'C', 'D'
	};
	struct serial_icounter_struct ic0, ic1;
	unsigned char rx[7];
	struct termios tio;
	int r;

	if (tcgetattr(fd, &tio) < 0)
		return -1;
	tio.c_iflag |= PARMRK;
	tio.c_iflag &= ~(IGNBRK | BRKINT);
	if (tcsetattr(fd, TCSANOW, &tio) < 0 ||
	    ioctl(fd, TIOCGICOUNT, &ic0) < 0) {
		fprintf(stderr, "FAIL: break: setup: %s\n", strerror(errno));
		return -1;
	}
	if (write(fd, "AB", 2) != 2 || tcsendbreak(fd, 0) < 0 ||
	    write(fd, "CD", 2) != 2) {
		fprintf(stderr, "FAIL: break: send: %s\n", strerror(errno));
		return -1;
	}
	memset(rx, 0xEE, sizeof(rx));
	r = read_exact(fd, rx, sizeof(rx), 3000);
	if (r != sizeof(rx) || memcmp(rx, want, sizeof(rx))) {
		fprintf(stderr, "FAIL: break: read %d: %02x %02x %02x %02x %02x %02x %02x, expected 41 42 ff 00 00 43 44\n",
			r, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6]);
		return -1;
	}
	if (ioctl(fd, TIOCGICOUNT, &ic1) < 0 || ic1.brk != ic0.brk + 1) {
		fprintf(stderr, "FAIL: break: %d break(s) counted, expected 1\n",
			ic1.brk - ic0.brk);
		return -1;
	}
	return 0;
}

static int wait_modem(int fd, int mask, int want)
{
	long long deadline = now_ms() + 1000;
	int st = 0;

	do {
		if (ioctl(fd, TIOCMGET, &st) < 0)
			return -1;
		if ((st & mask) == want)
			return 0;
		usleep(5000);
	} while (now_ms() < deadline);
	fprintf(stderr, "FAIL: modem lines 0x%03x, want 0x%03x (mask 0x%03x)\n",
		st & mask, want, mask);
	return -1;
}

int main(int argc, char *argv[])
{
	const char *dev_path;
	int fd, i, bulk = DEFAULT_BULK;
	unsigned char tx_buf[TEST_PATTERN_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char *tx, *rx;
	int bits;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/ttyFTDIx [bulk_bytes]\n", argv[0]);
		return 1;
	}
	dev_path = argv[1];
	if (argc > 2 && strcmp(argv[2], "hold"))
		bulk = atoi(argv[2]);

	fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	/*
	 * A port that stays open while something happens to the device (a
	 * suspend and resume): its line settings must still be those of the
	 * chip afterwards.
	 */
	if (argc > 3 && !strcmp(argv[2], "hold")) {
		tcflush(fd, TCIOFLUSH);
		if (set_line(fd, B57600) < 0) {
			fprintf(stderr, "FAIL: tcsetattr failed: %s\n",
				strerror(errno));
			return 1;
		}
		if (echo_check(fd, tx_buf, TEST_PATTERN_LEN, "hold: before"))
			return 1;
		printf("HOLD: %s open at 57600 baud for %s s\n", dev_path,
		       argv[3]);
		fflush(stdout);
		sleep(atoi(argv[3]));
		tcflush(fd, TCIOFLUSH);
		if (echo_check(fd, tx_buf, TEST_PATTERN_LEN, "hold: after"))
			return 1;
		printf("PASS: loopback still works on the port held open\n");
		close(fd);
		return 0;
	}

	printf("UART test on %s\n", dev_path);

	printf("Test 1: Configure UART (115200 8N1)... ");
	tcflush(fd, TCIOFLUSH);
	if (set_line(fd, B115200) < 0) {
		fprintf(stderr, "FAIL: tcsetattr failed: %s\n", strerror(errno));
		return 1;
	}
	printf("PASS\n");

	printf("Test 2: Loopback write/read... ");
	if (echo_check(fd, tx_buf, TEST_PATTERN_LEN, "loopback"))
		return 1;
	printf("PASS (de ad be ef echoed)\n");

	printf("Test 3: Second write/read cycle... ");
	if (echo_check(fd, (const unsigned char *)"FTDI", 4, "second cycle"))
		return 1;
	printf("PASS\n");

	printf("Test 4: Modem lines (RTS->CTS, DTR->DSR+DCD)... ");
	bits = TIOCM_RTS | TIOCM_DTR;
	if (ioctl(fd, TIOCMBIS, &bits) < 0 ||
	    wait_modem(fd, TIOCM_CTS | TIOCM_DSR | TIOCM_CD,
		       TIOCM_CTS | TIOCM_DSR | TIOCM_CD))
		return 1;
	bits = TIOCM_RTS;
	if (ioctl(fd, TIOCMBIC, &bits) < 0 ||
	    wait_modem(fd, TIOCM_CTS | TIOCM_DSR, TIOCM_DSR))
		return 1;
	bits = TIOCM_DTR;
	if (ioctl(fd, TIOCMBIC, &bits) < 0 ||
	    wait_modem(fd, TIOCM_CTS | TIOCM_DSR | TIOCM_CD, 0))
		return 1;
	bits = TIOCM_RTS | TIOCM_DTR;
	ioctl(fd, TIOCMBIS, &bits);
	printf("PASS\n");

	printf("Test 5: Baud change 9600 -> 3000000 -> 115200... ");
	if (set_line(fd, B9600) ||
	    echo_check(fd, (const unsigned char *)"9600", 4, "9600 baud") ||
	    set_line(fd, B3000000) ||
	    echo_check(fd, (const unsigned char *)"3M  ", 4, "3 Mbaud") ||
	    set_line(fd, B115200))
		return 1;
	printf("PASS\n");

	printf("Test 6: %d-byte integrity with concurrent read... ", bulk);
	tx = malloc(bulk);
	rx = malloc(bulk);
	if (!tx || !rx)
		return 1;
	srand(1234);
	for (i = 0; i < bulk; i++)
		tx[i] = rand() & 0xFF;
	{
		int wr = 0, rd = 0;
		long long deadline = now_ms() + 30000;

		while (rd < bulk && now_ms() < deadline) {
			struct pollfd pfd = {
				.fd = fd,
				.events = POLLIN | (wr < bulk ? POLLOUT : 0),
			};
			int r = poll(&pfd, 1, 200);

			if (r <= 0)
				continue;
			if ((pfd.revents & POLLOUT) && wr < bulk) {
				r = write(fd, tx + wr, bulk - wr > 4096 ? 4096 : bulk - wr);
				if (r > 0)
					wr += r;
				else if (r < 0 && errno != EAGAIN) {
					fprintf(stderr, "FAIL: write: %s\n", strerror(errno));
					return 1;
				}
			}
			if (pfd.revents & POLLIN) {
				r = read(fd, rx + rd, bulk - rd);
				if (r > 0)
					rd += r;
				else if (r < 0 && errno != EAGAIN) {
					fprintf(stderr, "FAIL: read: %s\n", strerror(errno));
					return 1;
				}
			}
		}
		if (rd != bulk) {
			fprintf(stderr, "FAIL: wrote %d, read back %d of %d bytes\n",
				wr, rd, bulk);
			return 1;
		}
		for (i = 0; i < bulk; i++) {
			if (tx[i] != rx[i]) {
				fprintf(stderr, "FAIL: mismatch at byte %d: %02x != %02x\n",
					i, rx[i], tx[i]);
				return 1;
			}
		}
	}
	printf("PASS\n");

	printf("Test 7: 7 data bits... ");
	if (test_7bits(fd) || set_line(fd, B115200))
		return 1;
	printf("PASS (c1 7e 80 ff read as 41 7e 00 7f)\n");

	printf("Test 8: Break between data... ");
	if (test_break(fd) || set_line(fd, B115200))
		return 1;
	printf("PASS (AB, break, CD)\n");

	close(fd);
	printf("All UART tests PASSED\n");
	return 0;
}
