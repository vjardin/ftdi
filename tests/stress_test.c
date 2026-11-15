// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * stress_test.c -- concurrent I/O stress for the FTDI driver suite
 *
 * Runs one thread per worker for a fixed duration and checks every
 * successful transfer against what the emulator must return:
 *
 *   spi-loop:DEV      SPI_LOOP transfers (MPSSE loopback): rx == tx, random
 *                     length, mode, speed, bits per word, LSB first, and
 *                     multi-transfer messages
 *   spi:DEV           full duplex without loopback, SPI modes 0 and 3: rx is
 *                     the emulator's counter pattern, MSB first
 *   i2c:DEV[:ADDR]    I2C write / read / write+read (repeated START) and
 *                     SMBus transfers; read data must be consecutive
 *   gpio:DEV:L,L,... toggle output lines, read them back
 *   gpioin:DEV:L[:V]  request a line as input with edge detection, read it
 *                     (V expected, default 1), release, repeat
 *   uart:DEV          loopback-plug integrity with baud changes
 *   mctrl:DEV         toggle RTS/DTR and check CTS/DSR follow
 *   sysfs:FILE[=VALS] read FILE (and write the comma-separated VALS)
 *
 * Usage: stress_test [-t seconds] [-k] worker...
 *   -k  tolerant: I/O errors (device unbound, unplugged, suspended) are
 *       counted and the device is reopened; data mismatches on transfers
 *       that reported success are always fatal.
 *
 * Prints one line per worker and "STRESS RESULT: PASS|FAIL".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>

#define MAX_WORKERS	32

struct worker {
	char kind[16];
	char path[256];
	char arg[128];
	pthread_t thr;
	unsigned long ops, errors, mismatches;
	char first_error[160];
	char first_mismatch[160];
	unsigned int seed;
};

static int duration = 10;
static int tolerant;
static volatile int stop;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int rnd(struct worker *w, unsigned int n)
{
	return n ? (unsigned int)rand_r(&w->seed) % n : 0;
}

static void note(struct worker *w, int mismatch, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void note(struct worker *w, int mismatch, const char *fmt, ...)
{
	char *first = mismatch ? w->first_mismatch : w->first_error;
	va_list ap;

	if (mismatch)
		w->mismatches++;
	else
		w->errors++;
	if (!first[0]) {
		va_start(ap, fmt);
		vsnprintf(first, sizeof(w->first_error), fmt, ap);
		va_end(ap);
	}
}

/* Open with retries in tolerant mode (node may come back after rebind) */
static int open_dev(struct worker *w, int flags)
{
	while (!stop) {
		int fd = open(w->path, flags);

		if (fd >= 0)
			return fd;
		if (!tolerant) {
			note(w, 0, "open %s: %s", w->path, strerror(errno));
			return -1;
		}
		usleep(20000);
	}
	return -1;
}

/* After an I/O error: fatal in strict mode, reopen in tolerant mode */
static int io_error(struct worker *w, int *fd, int flags, const char *what)
{
	note(w, 0, "%s %s: %s", what, w->path, strerror(errno));
	if (!tolerant)
		return -1;
	close(*fd);
	usleep(10000 + rnd(w, 40000));
	*fd = open_dev(w, flags);
	return *fd < 0 ? -1 : 0;
}

static const uint32_t spi_speeds[] = {
	30000000, 15000000, 10000000, 6000000, 1000000, 500000, 100000
};

#define SPIDEV_BUFSIZ	4096	/* spidev "bufsiz" module parameter default */
#define SPIDEV_ALIGN	64	/* largest ARCH_DMA_MINALIGN */

/* The slave sends MSB first: an LSB-first master reads its bytes reversed */
static uint8_t msb_first(uint8_t b, uint32_t mode)
{
	uint8_t r = 0;
	int i;

	if (!(mode & SPI_LSB_FIRST))
		return b;
	for (i = 0; i < 8; i++)
		if (b & (1u << i))
			r |= 0x80 >> i;
	return r;
}

static void *spi_worker(void *arg)
{
	struct worker *w = arg;
	int loop = !strcmp(w->kind, "spi-loop");
	int fd = open_dev(w, O_RDWR);
	uint8_t *tx = malloc(4096 * 3), *rx = malloc(4096 * 3);

	if (fd < 0 || !tx || !rx)
		return NULL;

	while (!stop) {
		struct spi_ioc_transfer xf[3];
		uint32_t mode = loop ? rnd(w, 4) :
				rnd(w, 2) ? SPI_MODE_3 : SPI_MODE_0;
		int nxf = 1 + rnd(w, 3), i, j, off = 0, used = 0;
		uint8_t bpw = 8;

		if (rnd(w, 4) == 0)
			bpw = 1 + rnd(w, 8);
		if (rnd(w, 4) == 0)
			mode |= SPI_LSB_FIRST;
		if (loop)
			mode |= SPI_LOOP;

		if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0) {
			if (io_error(w, &fd, O_RDWR, "SPI_IOC_WR_MODE32"))
				break;
			continue;
		}

		memset(xf, 0, sizeof(xf));
		for (i = 0; i < nxf; i++) {
			/*
			 * spidev bounces a whole message through one bufsiz
			 * (4096) buffer per direction, each transfer rounded
			 * up to ARCH_DMA_MINALIGN: keep room for the others.
			 */
			int room = SPIDEV_BUFSIZ - used -
				   (nxf - i - 1) * SPIDEV_ALIGN;
			int len = 1 + rnd(w, rnd(w, 8) ? 64 : room);

			if (len > room)
				len = room;
			used += (len + SPIDEV_ALIGN - 1) & ~(SPIDEV_ALIGN - 1);

			for (j = 0; j < len; j++)
				tx[off + j] = rand_r(&w->seed);
			memset(rx + off, 0x5A, len);
			xf[i].tx_buf = (uintptr_t)(tx + off);
			xf[i].rx_buf = (uintptr_t)(rx + off);
			xf[i].len = len;
			xf[i].bits_per_word = bpw;
			xf[i].speed_hz = spi_speeds[rnd(w, 7)];
			xf[i].cs_change = rnd(w, 4) == 0;
			if (rnd(w, 8) == 0)
				xf[i].delay_usecs = rnd(w, 50);
			off += len;
		}

		if (ioctl(fd, SPI_IOC_MESSAGE(nxf), xf) < 0) {
			if (io_error(w, &fd, O_RDWR, "SPI_IOC_MESSAGE"))
				break;
			continue;
		}
		w->ops++;

		for (i = 0, off = 0; i < nxf; off += xf[i].len, i++) {
			uint8_t mask = bpw >= 8 ? 0xFF : (1u << bpw) - 1;

			for (j = 0; j < (int)xf[i].len; j++) {
				uint8_t r = rx[off + j];

				if (loop) {
					if ((r & mask) != (tx[off + j] & mask)) {
						note(w, 1, "loopback xfer %d byte %d/%u: rx %02x tx %02x (bpw %u mode 0x%x)",
						     i, j, xf[i].len, r, tx[off + j], bpw, mode);
						break;
					}
				} else if (bpw == 8 && j > 0 &&
					   msb_first(r, mode) !=
					   (uint8_t)(msb_first(rx[off + j - 1], mode) + 1)) {
					note(w, 1, "counter break xfer %d byte %d/%u: %02x after %02x (mode 0x%x)",
					     i, j, xf[i].len, r, rx[off + j - 1], mode);
					break;
				}
			}
		}
	}
	close(fd);
	free(tx);
	free(rx);
	return NULL;
}

static int check_counter(struct worker *w, const uint8_t *b, int n,
			 const char *what)
{
	int i;

	for (i = 1; i < n; i++) {
		if (b[i] != (uint8_t)(b[i - 1] + 1)) {
			note(w, 1, "%s: counter break at %d/%d: %02x after %02x",
			     what, i, n, b[i], b[i - 1]);
			return -1;
		}
	}
	return 0;
}

static void *i2c_worker(void *arg)
{
	struct worker *w = arg;
	int addr = w->arg[0] ? (int)strtol(w->arg, NULL, 0) : 0x50;
	int fd = open_dev(w, O_RDWR);
	uint8_t wbuf[64], rbuf[4096];

	if (fd < 0)
		return NULL;

	while (!stop) {
		struct i2c_msg msgs[2];
		struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs };
		int op = rnd(w, 4), wl = 1 + rnd(w, 32), rl, i;

		rl = 1 + rnd(w, rnd(w, 8) ? 32 : sizeof(rbuf));
		for (i = 0; i < wl; i++)
			wbuf[i] = rand_r(&w->seed);
		memset(msgs, 0, sizeof(msgs));

		switch (op) {
		case 0:
			msgs[0] = (struct i2c_msg){ addr, 0, wl, wbuf };
			rdwr.nmsgs = 1;
			break;
		case 1:
			msgs[0] = (struct i2c_msg){ addr, I2C_M_RD, rl, rbuf };
			rdwr.nmsgs = 1;
			break;
		case 2:
			msgs[0] = (struct i2c_msg){ addr, 0, 1 + rnd(w, 2), wbuf };
			msgs[1] = (struct i2c_msg){ addr, I2C_M_RD, rl, rbuf };
			rdwr.nmsgs = 2;
			break;
		default: {
			union i2c_smbus_data d;
			struct i2c_smbus_ioctl_data sd = {
				.read_write = I2C_SMBUS_READ,
				.command = wbuf[0],
				.size = I2C_SMBUS_WORD_DATA,
				.data = &d,
			};

			if (ioctl(fd, I2C_SLAVE, addr) < 0 ||
			    ioctl(fd, I2C_SMBUS, &sd) < 0) {
				if (io_error(w, &fd, O_RDWR, "I2C_SMBUS"))
					goto out;
				continue;
			}
			w->ops++;
			if ((uint8_t)(d.word >> 8) != (uint8_t)((d.word & 0xFF) + 1))
				note(w, 1, "smbus word %04x not consecutive", d.word);
			continue;
		}
		}

		if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
			if (io_error(w, &fd, O_RDWR, "I2C_RDWR"))
				break;
			continue;
		}
		w->ops++;
		if (op != 0)
			check_counter(w, rbuf, rl, op == 1 ? "read" : "write+read");
	}
out:
	close(fd);
	return NULL;
}

static int parse_lines(const char *s, unsigned int *lines, int max)
{
	int n = 0;

	while (*s && n < max) {
		lines[n++] = strtoul(s, (char **)&s, 0);
		if (*s == ',')
			s++;
	}
	return n;
}

static void *gpio_worker(void *arg)
{
	struct worker *w = arg;
	unsigned int lines[16];
	int nl = parse_lines(w->arg, lines, 16), fd = -1, lfd = -1, i;

	while (!stop) {
		struct gpio_v2_line_request req;
		struct gpio_v2_line_values v;
		uint64_t mask = (nl >= 64) ? ~0ULL : ((1ULL << nl) - 1);

		if (fd < 0) {
			fd = open_dev(w, O_RDWR);
			if (fd < 0)
				break;
		}
		if (lfd < 0) {
			memset(&req, 0, sizeof(req));
			for (i = 0; i < nl; i++)
				req.offsets[i] = lines[i];
			req.num_lines = nl;
			req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
			strcpy(req.consumer, "stress");
			if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
				if (io_error(w, &fd, O_RDWR, "GPIO_V2_GET_LINE"))
					break;
				continue;
			}
			lfd = req.fd;
		}

		v.mask = mask;
		v.bits = ((uint64_t)rand_r(&w->seed) << 32 | rand_r(&w->seed)) & mask;
		if (ioctl(lfd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) {
			uint64_t want = v.bits;

			(void)want;
			close(lfd);
			lfd = -1;
			if (io_error(w, &fd, O_RDWR, "GPIO set"))
				break;
			continue;
		}
		{
			uint64_t want = v.bits;

			v.mask = mask;
			v.bits = 0;
			if (ioctl(lfd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) {
				close(lfd);
				lfd = -1;
				if (io_error(w, &fd, O_RDWR, "GPIO get"))
					break;
				continue;
			}
			w->ops++;
			if ((v.bits & mask) != want)
				note(w, 1, "GPIO readback %llx, wrote %llx",
				     (unsigned long long)(v.bits & mask),
				     (unsigned long long)want);
		}
		if (rnd(w, 64) == 0) {
			close(lfd);
			lfd = -1;
		}
	}
	if (lfd >= 0)
		close(lfd);
	if (fd >= 0)
		close(fd);
	return NULL;
}

static void *gpioin_worker(void *arg)
{
	struct worker *w = arg;
	char *end;
	unsigned int line = strtoul(w->arg, &end, 0);
	unsigned int level = *end == ':' ? !!atoi(end + 1) : 1;
	int fd = -1, noedge = 0;

	while (!stop) {
		struct gpio_v2_line_request req;
		struct gpio_v2_line_values v = { .mask = 1 };
		struct pollfd pfd;

		if (fd < 0) {
			fd = open_dev(w, O_RDWR);
			if (fd < 0)
				break;
		}
		memset(&req, 0, sizeof(req));
		req.offsets[0] = line;
		req.num_lines = 1;
		req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
		if (!noedge)
			req.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING |
					    GPIO_V2_LINE_FLAG_EDGE_FALLING;
		strcpy(req.consumer, "stress-in");
		if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
			/* chip without IRQ support: plain input then */
			if (errno == ENXIO && !noedge) {
				noedge = 1;
				continue;
			}
			if (io_error(w, &fd, O_RDWR, "GPIO input request"))
				break;
			continue;
		}
		if (ioctl(req.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) {
			close(req.fd);
			if (io_error(w, &fd, O_RDWR, "GPIO input get"))
				break;
			continue;
		}
		w->ops++;
		if ((v.bits & 1) != level)
			note(w, 1, "input line %u reads %u, the pull gives %u",
			     line, (unsigned int)(v.bits & 1), level);
		/* Let the edge-polling work run, then release mid-poll */
		pfd.fd = req.fd;
		pfd.events = POLLIN;
		poll(&pfd, 1, rnd(w, 20));
		close(req.fd);
	}
	if (fd >= 0)
		close(fd);
	return NULL;
}

static const speed_t uart_speeds[] = { B115200, B9600, B921600, B3000000, B230400 };

static int uart_setup(int fd, speed_t sp)
{
	struct termios t;

	memset(&t, 0, sizeof(t));
	t.c_cflag = CS8 | CLOCAL | CREAD;
	t.c_iflag = IGNPAR;
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	return tcsetattr(fd, TCSANOW, &t);
}

static void *uart_worker(void *arg)
{
	struct worker *w = arg;
	int flags = O_RDWR | O_NOCTTY | O_NONBLOCK;
	int fd = open_dev(w, flags);
	uint8_t *tx = malloc(8192), *rx = malloc(8192);

	if (fd < 0 || !tx || !rx || uart_setup(fd, B115200) < 0)
		return NULL;
	tcflush(fd, TCIOFLUSH);

	while (!stop) {
		int len = 1 + rnd(w, rnd(w, 4) ? 256 : 8192);
		int wr = 0, rd = 0, i;
		long long deadline = now_ms() + 5000;

		if (rnd(w, 16) == 0) {
			tcdrain(fd);
			if (uart_setup(fd, uart_speeds[rnd(w, 5)]) < 0) {
				if (io_error(w, &fd, flags, "tcsetattr"))
					break;
				uart_setup(fd, B115200);
				continue;
			}
		}
		for (i = 0; i < len; i++)
			tx[i] = rand_r(&w->seed);

		while (rd < len && now_ms() < deadline && !stop) {
			struct pollfd pfd = {
				.fd = fd,
				.events = POLLIN | (wr < len ? POLLOUT : 0),
			};
			int r = poll(&pfd, 1, 100);

			if (r <= 0)
				continue;
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EIO;
				break;
			}
			if ((pfd.revents & POLLOUT) && wr < len) {
				r = write(fd, tx + wr, len - wr);
				if (r > 0)
					wr += r;
				else if (r < 0 && errno != EAGAIN)
					break;
			}
			if (pfd.revents & POLLIN) {
				r = read(fd, rx + rd, len - rd);
				if (r > 0)
					rd += r;
				else if (r < 0 && errno != EAGAIN)
					break;
				else if (r == 0) {
					errno = EIO;	/* hangup */
					break;
				}
			}
		}
		if (stop)
			break;
		if (rd < len) {
			if (io_error(w, &fd, flags, rd ? "short loopback" : "loopback"))
				break;
			if (uart_setup(fd, B115200) == 0)
				tcflush(fd, TCIOFLUSH);
			continue;
		}
		w->ops++;
		if (memcmp(tx, rx, len)) {
			for (i = 0; i < len && tx[i] == rx[i]; i++)
				;
			note(w, 1, "UART loopback mismatch at %d/%d: %02x != %02x",
			     i, len, rx[i], tx[i]);
			tcflush(fd, TCIOFLUSH);
		}
	}
	close(fd);
	free(tx);
	free(rx);
	return NULL;
}

static void *mctrl_worker(void *arg)
{
	struct worker *w = arg;
	int flags = O_RDWR | O_NOCTTY | O_NONBLOCK;
	int fd = open_dev(w, flags);

	if (fd < 0)
		return NULL;

	while (!stop) {
		int set = rnd(w, 4), bits = TIOCM_RTS | TIOCM_DTR, st = 0;
		long long deadline = now_ms() + 1000;
		int want = ((set & 1) ? TIOCM_CTS : 0) |
			   ((set & 2) ? TIOCM_DSR : 0);

		if (ioctl(fd, TIOCMBIC, &bits) < 0) {
			if (io_error(w, &fd, flags, "TIOCMBIC"))
				break;
			continue;
		}
		bits = ((set & 1) ? TIOCM_RTS : 0) | ((set & 2) ? TIOCM_DTR : 0);
		if (bits && ioctl(fd, TIOCMBIS, &bits) < 0) {
			if (io_error(w, &fd, flags, "TIOCMBIS"))
				break;
			continue;
		}
		do {
			if (ioctl(fd, TIOCMGET, &st) < 0)
				break;
			if ((st & (TIOCM_CTS | TIOCM_DSR)) == want)
				break;
			usleep(2000);
		} while (now_ms() < deadline && !stop);
		w->ops++;
		if ((st & (TIOCM_CTS | TIOCM_DSR)) != want && !stop)
			note(w, 1, "modem in 0x%x, want 0x%x", st & (TIOCM_CTS | TIOCM_DSR), want);
	}
	close(fd);
	return NULL;
}

static void *sysfs_worker(void *arg)
{
	struct worker *w = arg;
	char vals[8][16], buf[4096];
	int nv = 0;
	char *s = w->arg;

	while (*s && nv < 8) {
		char *c = strchr(s, ',');
		int l = c ? c - s : (int)strlen(s);

		snprintf(vals[nv++], sizeof(vals[0]), "%.*s", l, s);
		s += l + (c ? 1 : 0);
	}

	while (!stop) {
		int fd = open(w->path, nv ? O_RDWR : O_RDONLY);

		if (fd < 0) {
			if (!tolerant)
				note(w, 0, "open %s: %s", w->path, strerror(errno));
			usleep(20000);
			if (!tolerant)
				break;
			continue;
		}
		if (nv && rnd(w, 2)) {
			const char *v = vals[rnd(w, nv)];

			if (write(fd, v, strlen(v)) < 0 && !tolerant)
				note(w, 0, "write %s=%s: %s", w->path, v, strerror(errno));
		} else if (read(fd, buf, sizeof(buf)) < 0 && !tolerant) {
			note(w, 0, "read %s: %s", w->path, strerror(errno));
		}
		close(fd);
		w->ops++;
		usleep(rnd(w, 2000));
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	struct worker *ws = calloc(MAX_WORKERS, sizeof(*ws));
	int nw = 0, i, opt, fail = 0;
	long long end;

	while ((opt = getopt(argc, argv, "t:k")) != -1) {
		switch (opt) {
		case 't':
			duration = atoi(optarg);
			break;
		case 'k':
			tolerant = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-t sec] [-k] kind:path[:arg]...\n", argv[0]);
			return 2;
		}
	}

	for (i = optind; i < argc && nw < MAX_WORKERS; i++) {
		struct worker *w = &ws[nw];
		char *a = strdup(argv[i]), *p, *q;

		p = strchr(a, ':');
		if (!p) {
			fprintf(stderr, "bad worker spec %s\n", argv[i]);
			return 2;
		}
		*p++ = 0;
		snprintf(w->kind, sizeof(w->kind), "%s", a);
		q = strchr(p, !strcmp(a, "sysfs") ? '=' : ':');
		if (q)
			*q++ = 0;
		snprintf(w->path, sizeof(w->path), "%s", p);
		snprintf(w->arg, sizeof(w->arg), "%s", q ? q : "");
		w->seed = 0x1234 + nw * 7919;
		nw++;
		free(a);
	}

	for (i = 0; i < nw; i++) {
		void *(*fn)(void *) = NULL;
		const char *k = ws[i].kind;

		if (!strcmp(k, "spi") || !strcmp(k, "spi-loop"))
			fn = spi_worker;
		else if (!strcmp(k, "i2c"))
			fn = i2c_worker;
		else if (!strcmp(k, "gpio"))
			fn = gpio_worker;
		else if (!strcmp(k, "gpioin"))
			fn = gpioin_worker;
		else if (!strcmp(k, "uart"))
			fn = uart_worker;
		else if (!strcmp(k, "mctrl"))
			fn = mctrl_worker;
		else if (!strcmp(k, "sysfs"))
			fn = sysfs_worker;
		if (!fn) {
			fprintf(stderr, "unknown worker kind %s\n", k);
			return 2;
		}
		pthread_create(&ws[i].thr, NULL, fn, &ws[i]);
	}

	end = now_ms() + duration * 1000LL;
	while (now_ms() < end)
		usleep(100000);
	stop = 1;
	for (i = 0; i < nw; i++)
		pthread_join(ws[i].thr, NULL);

	for (i = 0; i < nw; i++) {
		struct worker *w = &ws[i];
		int bad = w->mismatches || (!tolerant && w->errors) || !w->ops;

		printf("STRESS %-8s %-34s ops=%lu errors=%lu mismatches=%lu %s%s%s%s%s\n",
		       w->kind, w->path, w->ops, w->errors, w->mismatches,
		       bad ? "FAIL" : "ok",
		       w->first_mismatch[0] ? " first mismatch: " : "",
		       w->first_mismatch,
		       w->first_error[0] ? " first error: " : "", w->first_error);
		fail |= bad;
	}
	printf("STRESS RESULT: %s\n", fail ? "FAIL" : "PASS");
	return fail;
}
