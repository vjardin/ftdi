// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Vincent Jardin <vjardin@free.fr>
//
// I2C performance benchmark — kernel driver path (ioctl I2C_RDWR)
// Usage: i2c_bench_kernel <bus> <addr> [iterations]

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define EEPROM_SIZE	256
#define DEFAULT_ITER	1000

static int i2c_rdwr(int fd, struct i2c_msg *msgs, int nmsgs)
{
	struct i2c_rdwr_ioctl_data data = {
		.msgs = msgs,
		.nmsgs = nmsgs,
	};

	return ioctl(fd, I2C_RDWR, &data);
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Write N bytes at offset, no read-back. */
static int eeprom_write(int fd, int addr, int offset,
			const unsigned char *data, int len)
{
	unsigned char wbuf[EEPROM_SIZE + 1];
	struct i2c_msg msg;

	wbuf[0] = (unsigned char)offset;
	memcpy(wbuf + 1, data, len);
	msg.addr = addr;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = wbuf;
	return i2c_rdwr(fd, &msg, 1);
}

/* Set pointer + read N bytes. */
static int eeprom_read(int fd, int addr, int offset,
		       unsigned char *buf, int len)
{
	unsigned char obuf = (unsigned char)offset;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &obuf },
		{ .addr = addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};

	return i2c_rdwr(fd, msgs, 2);
}

/* Write + read-back + verify. */
static int eeprom_write_read_verify(int fd, int addr, int offset,
				    const unsigned char *data, int len)
{
	unsigned char rbuf[EEPROM_SIZE];
	int ret;

	ret = eeprom_write(fd, addr, offset, data, len);
	if (ret < 0)
		return -1;
	ret = eeprom_read(fd, addr, offset, rbuf, len);
	if (ret < 0)
		return -2;
	if (memcmp(rbuf, data, len) != 0)
		return -3;
	return 0;
}

/* Compound w+r+w+r (4 messages, 3 repeated STARTs). */
static int eeprom_compound_read(int fd, int addr, int offset,
				unsigned char *rd1, unsigned char *rd2)
{
	unsigned char ptr = (unsigned char)offset;
	struct i2c_msg msgs[4] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &ptr },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = rd1 },
		{ .addr = addr, .flags = 0, .len = 1, .buf = &ptr },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = rd2 },
	};

	return i2c_rdwr(fd, msgs, 4);
}

struct bench_result {
	const char *name;
	int iterations;
	int ok;
	int fail;
	double elapsed;
	int bytes_per_op;
};

static void print_result(const struct bench_result *r)
{
	double ops_s = r->ok / r->elapsed;
	double bytes_s = ops_s * r->bytes_per_op;

	printf("  %-28s %5d/%d  %8.1f ops/s  %8.0f B/s  %6.2fs\n",
	       r->name, r->ok, r->iterations, ops_s, bytes_s, r->elapsed);
}

static struct bench_result bench_write_read(int fd, int addr, int len,
					    int iterations)
{
	struct bench_result r = { .iterations = iterations, .bytes_per_op = len };
	char name[64];
	double t0;
	int i;

	snprintf(name, sizeof(name), "write+read %dB", len);
	r.name = strdup(name);
	t0 = now_sec();

	for (i = 0; i < iterations; i++) {
		unsigned char data[EEPROM_SIZE];
		int j;

		for (j = 0; j < len; j++)
			data[j] = (i * 7 + j + 0x42) & 0xFF;
		if (eeprom_write_read_verify(fd, addr, 0, data, len) == 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_write_only(int fd, int addr, int len,
					    int iterations)
{
	struct bench_result r = { .iterations = iterations, .bytes_per_op = len };
	char name[64];
	double t0;
	int i;

	snprintf(name, sizeof(name), "write-only %dB", len);
	r.name = strdup(name);
	t0 = now_sec();

	for (i = 0; i < iterations; i++) {
		unsigned char data[EEPROM_SIZE];
		int j;

		for (j = 0; j < len; j++)
			data[j] = (i * 11 + j) & 0xFF;
		if (eeprom_write(fd, addr, 0, data, len) >= 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_read_only(int fd, int addr, int len,
					   int iterations)
{
	struct bench_result r = { .iterations = iterations, .bytes_per_op = len };
	char name[64];
	double t0;
	int i;

	snprintf(name, sizeof(name), "read-only %dB", len);
	r.name = strdup(name);
	t0 = now_sec();

	for (i = 0; i < iterations; i++) {
		unsigned char buf[EEPROM_SIZE];

		if (eeprom_read(fd, addr, 0, buf, len) >= 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_compound(int fd, int addr, int iterations)
{
	struct bench_result r = {
		.name = "compound w+r+w+r",
		.iterations = iterations,
		.bytes_per_op = 2,
	};
	double t0;
	int i;

	t0 = now_sec();
	for (i = 0; i < iterations; i++) {
		unsigned char val = (i * 17 + 0x23) & 0xFF;
		unsigned char rd1, rd2;

		if (eeprom_write(fd, addr, 0, &val, 1) < 0) {
			r.fail++;
			continue;
		}
		if (eeprom_compound_read(fd, addr, 0, &rd1, &rd2) < 0) {
			r.fail++;
			continue;
		}
		if (rd1 == val && rd2 == val)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

int main(int argc, char *argv[])
{
	struct bench_result results[16];
	int nresults = 0;
	int bus, addr, iterations, fd;
	char devpath[32];

	if (argc < 3)
		errx(EXIT_FAILURE,
		     "Usage: %s <bus> <addr_hex> [iterations]", argv[0]);

	bus = atoi(argv[1]);
	addr = (int)strtol(argv[2], NULL, 0);
	iterations = argc > 3 ? atoi(argv[3]) : DEFAULT_ITER;

	snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);
	fd = open(devpath, O_RDWR);
	if (fd < 0)
		err(EXIT_FAILURE, "%s", devpath);

	printf("=== I2C Benchmark: kernel driver (%s, addr=0x%02x, "
	       "iter=%d) ===\n\n", devpath, addr, iterations);

	results[nresults++] = bench_write_read(fd, addr, 1, iterations);
	results[nresults++] = bench_write_read(fd, addr, 4, iterations);
	results[nresults++] = bench_write_read(fd, addr, 32, iterations);
	results[nresults++] = bench_write_read(fd, addr, 256, iterations);
	results[nresults++] = bench_write_only(fd, addr, 1, iterations);
	results[nresults++] = bench_write_only(fd, addr, 32, iterations);
	results[nresults++] = bench_write_only(fd, addr, 256, iterations);
	results[nresults++] = bench_read_only(fd, addr, 1, iterations);
	results[nresults++] = bench_read_only(fd, addr, 32, iterations);
	results[nresults++] = bench_read_only(fd, addr, 256, iterations);
	results[nresults++] = bench_compound(fd, addr, iterations);

	printf("  %-28s %9s  %12s  %10s  %7s\n",
	       "Test", "Pass", "Throughput", "Bandwidth", "Time");
	printf("  %-28s %9s  %12s  %10s  %7s\n",
	       "---", "---", "---", "---", "---");

	for (int i = 0; i < nresults; i++)
		print_result(&results[i]);

	close(fd);
	printf("\n");
	return 0;
}
