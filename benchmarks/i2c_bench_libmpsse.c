// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Vincent Jardin <vjardin@free.fr>
//
// I2C performance benchmark — libmpsse userspace path
// Usage: i2c_bench_libmpsse [iterations]

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mpsse.h>

#define EEPROM_SIZE	256
#define EEPROM_ADDR_W	0xA8	/* 0x54 << 1 */
#define EEPROM_ADDR_R	0xA9
#define DEFAULT_ITER	1000

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Write N bytes at offset. */
static int eeprom_write(struct mpsse_context *ctx, int offset,
			const unsigned char *data, int len)
{
	char addr_w = EEPROM_ADDR_W;
	char obuf = (char)offset;
	int i;

	Start(ctx);
	Write(ctx, &addr_w, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	Write(ctx, &obuf, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }

	for (i = 0; i < len; i++) {
		char byte = (char)data[i];

		Write(ctx, &byte, 1);
		if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	}

	Stop(ctx);
	return 0;
}

/* Set pointer + read N bytes. */
static int eeprom_read(struct mpsse_context *ctx, int offset,
		       unsigned char *buf, int len)
{
	char addr_w = EEPROM_ADDR_W;
	char addr_r = EEPROM_ADDR_R;
	char obuf = (char)offset;
	char *data;

	Start(ctx);
	Write(ctx, &addr_w, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	Write(ctx, &obuf, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }

	Start(ctx);
	Write(ctx, &addr_r, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }

	if (len > 1) {
		SendAcks(ctx);
		data = Read(ctx, len - 1);
		if (!data) { Stop(ctx); return -2; }
		memcpy(buf, data, len - 1);
		free(data);
	}

	SendNacks(ctx);
	data = Read(ctx, 1);
	if (!data) { Stop(ctx); return -3; }
	buf[len - 1] = (unsigned char)data[0];
	free(data);

	Stop(ctx);
	return 0;
}

/* Write + read-back + verify. */
static int eeprom_write_read_verify(struct mpsse_context *ctx, int offset,
				    const unsigned char *data, int len)
{
	unsigned char rbuf[EEPROM_SIZE];
	int ret;

	ret = eeprom_write(ctx, offset, data, len);
	if (ret < 0)
		return -1;
	ret = eeprom_read(ctx, offset, rbuf, len);
	if (ret < 0)
		return -2;
	if (memcmp(rbuf, data, len) != 0)
		return -3;
	return 0;
}

/* Compound w+r+w+r (3 repeated STARTs). */
static int eeprom_compound_read(struct mpsse_context *ctx, int offset,
				unsigned char *rd1, unsigned char *rd2)
{
	char addr_w = EEPROM_ADDR_W;
	char addr_r = EEPROM_ADDR_R;
	char obuf = (char)offset;
	char *data;

	/* First pair: set pointer + read */
	Start(ctx);
	Write(ctx, &addr_w, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	Write(ctx, &obuf, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }

	Start(ctx);
	Write(ctx, &addr_r, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	SendNacks(ctx);
	data = Read(ctx, 1);
	if (!data) { Stop(ctx); return -2; }
	*rd1 = (unsigned char)data[0];
	free(data);

	/* Second pair: set pointer + read */
	Start(ctx);
	Write(ctx, &addr_w, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	Write(ctx, &obuf, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }

	Start(ctx);
	Write(ctx, &addr_r, 1);
	if (GetAck(ctx) != ACK) { Stop(ctx); return -1; }
	SendNacks(ctx);
	data = Read(ctx, 1);
	if (!data) { Stop(ctx); return -2; }
	*rd2 = (unsigned char)data[0];
	free(data);

	Stop(ctx);
	return 0;
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

static struct bench_result bench_write_read(struct mpsse_context *ctx,
					    int len, int iterations)
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
		if (eeprom_write_read_verify(ctx, 0, data, len) == 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_write_only(struct mpsse_context *ctx,
					    int len, int iterations)
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
		if (eeprom_write(ctx, 0, data, len) >= 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_read_only(struct mpsse_context *ctx,
					   int len, int iterations)
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

		if (eeprom_read(ctx, 0, buf, len) >= 0)
			r.ok++;
		else
			r.fail++;
	}

	r.elapsed = now_sec() - t0;
	return r;
}

static struct bench_result bench_compound(struct mpsse_context *ctx,
					  int iterations)
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

		if (eeprom_write(ctx, 0, &val, 1) < 0) {
			r.fail++;
			continue;
		}
		if (eeprom_compound_read(ctx, 0, &rd1, &rd2) < 0) {
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

static void check_kernel_driver_not_loaded(void)
{
	const char *paths[] = {
		"/sys/bus/usb/drivers/ftdi_mpsse",
		"/sys/module/ftdi_mpsse",
		NULL,
	};

	for (int i = 0; paths[i]; i++) {
		if (access(paths[i], F_OK) == 0)
			errx(EXIT_FAILURE,
			     "kernel module ftdi_mpsse is loaded (%s exists)\n"
			     "Unload first: sudo rmmod ftdi_i2c; "
			     "sudo rmmod ftdi_mpsse", paths[i]);
	}
}

int main(int argc, char *argv[])
{
	struct mpsse_context *ctx;
	struct bench_result results[16];
	int nresults = 0;
	int iterations;

	iterations = argc > 1 ? atoi(argv[1]) : DEFAULT_ITER;

	check_kernel_driver_not_loaded();

	ctx = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB);
	if (!ctx || !ctx->open)
		errx(EXIT_FAILURE, "failed to init MPSSE: %s",
		     ErrorString(ctx));

	printf("=== I2C Benchmark: libmpsse (%s, %d Hz, iter=%d) ===\n\n",
	       GetDescription(ctx), GetClock(ctx), iterations);

	results[nresults++] = bench_write_read(ctx, 1, iterations);
	results[nresults++] = bench_write_read(ctx, 4, iterations);
	results[nresults++] = bench_write_read(ctx, 32, iterations);
	results[nresults++] = bench_write_read(ctx, 256, iterations);
	results[nresults++] = bench_write_only(ctx, 1, iterations);
	results[nresults++] = bench_write_only(ctx, 32, iterations);
	results[nresults++] = bench_write_only(ctx, 256, iterations);
	results[nresults++] = bench_read_only(ctx, 1, iterations);
	results[nresults++] = bench_read_only(ctx, 32, iterations);
	results[nresults++] = bench_read_only(ctx, 256, iterations);
	results[nresults++] = bench_compound(ctx, iterations);

	printf("  %-28s %9s  %12s  %10s  %7s\n",
	       "Test", "Pass", "Throughput", "Bandwidth", "Time");
	printf("  %-28s %9s  %12s  %10s  %7s\n",
	       "---", "---", "---", "---", "---");

	for (int i = 0; i < nresults; i++)
		print_result(&results[i]);

	Close(ctx);
	printf("\n");
	return 0;
}
