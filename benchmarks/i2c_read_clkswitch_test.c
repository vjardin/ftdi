// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Vincent Jardin <vjardin@free.fr>
//
// I2C batched read with clock switching — raw MPSSE via libftdi (userland).
//
// Purpose:
//   Test whether switching the MPSSE clock speed mid-transaction
//   (SET_CLK_DIVISOR from 400 kHz to 100 kHz for the read phase,
//   then back to 400 kHz) introduces data corruption.  This evaluates
//   a potential kernel optimization where writes run at the user's
//   configured speed but reads slow down for reliable open-drain
//   pull-up settling.
//
// Design validation:
//   This tool proved that clock switching mid-stream has a ~1% error
//   rate (99/100 at 8B, 198/200 at 32B).  The corruption happens at
//   the speed transition boundary where the first read byte after
//   SET_CLK_DIVISOR may clock SDA before the pull-up has settled at
//   the new (slower) rate.  This ruled out the clock-switch approach
//   for the kernel driver — the v20 per-byte flush at the native
//   speed is more reliable.
//
// Test flow per iteration:
//   1. Write a known pattern at offset 0 (at 400 kHz, per-byte flush)
//   2. Set pointer + repeated START + address (at 400 kHz)
//   3. SET_CLK_DIVISOR → 100 kHz
//   4. Batched read of N bytes (in chunks of 8)
//   5. SET_CLK_DIVISOR → 400 kHz
//   6. STOP, verify data
//
// Usage:
//   Unload the kernel ftdi_mpsse module first.
//   gcc -o i2c_clkswitch_test i2c_read_clkswitch_test.c -lftdi1 -lusb-1.0
//   ./i2c_clkswitch_test [num_bytes] [iterations]  # default: 8 100
//
// Requires: libftdi1-dev, FT232H connected, RP2040 I2C EEPROM slave.

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libftdi1/ftdi.h>

#define MPSSE_SET_BITS_LOW      0x80
#define MPSSE_CLK_BITS_OUT_NVE  0x13
#define MPSSE_CLK_BITS_IN_PVE   0x22
#define MPSSE_SEND_IMMEDIATE    0x87
#define MPSSE_DISABLE_CLK_DIV5  0x8A
#define MPSSE_ENABLE_3PHASE     0x8C
#define MPSSE_DISABLE_ADAPTIVE  0x8D
#define MPSSE_SET_CLK_DIVISOR   0x86
#define MPSSE_LOOPBACK_OFF      0x85
#define MPSSE_DRIVE_ZERO_ONLY   0x9E

#define PIN_SCL         0x01
#define PIN_SDA_OUT     0x02
#define SDA_HI_VAL      PIN_SDA_OUT
#define SDA_HI_DIR      (PIN_SCL | PIN_SDA_OUT)

/* 400 kHz: div = 60000/(400*3) - 1 = 49 */
#define DIV_400K_LO     (49 & 0xFF)
#define DIV_400K_HI     ((49 >> 8) & 0xFF)
/* 100 kHz: div = 60000/(100*3) - 1 = 199 */
#define DIV_100K_LO     (199 & 0xFF)
#define DIV_100K_HI     ((199 >> 8) & 0xFF)

static int xfer(struct ftdi_context *ftdi, unsigned char *cmd, int cmd_len,
		unsigned char *rsp, int rsp_len)
{
	int ret = ftdi_write_data(ftdi, cmd, cmd_len);
	if (ret < 0) return -1;
	if (rsp_len > 0) {
		int total = 0, retries = 0;
		while (total < rsp_len && retries < 200) {
			ret = ftdi_read_data(ftdi, rsp + total, rsp_len - total);
			if (ret < 0) return -2;
			if (ret == 0) { retries++; usleep(500); continue; }
			retries = 0;
			total += ret;
		}
		if (total < rsp_len) return -3;
	}
	return 0;
}

static int build_write_byte_flush(unsigned char *buf, int pos, unsigned char byte)
{
	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE; buf[pos++] = 7; buf[pos++] = byte;
	buf[pos++] = MPSSE_SET_BITS_LOW; buf[pos++] = SDA_HI_VAL; buf[pos++] = SDA_HI_DIR;
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE; buf[pos++] = 0;
	buf[pos++] = MPSSE_SET_BITS_LOW; buf[pos++] = 0x00; buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	return pos;
}

static int build_read_byte(unsigned char *buf, int pos, int ack)
{
	buf[pos++] = MPSSE_SET_BITS_LOW; buf[pos++] = SDA_HI_VAL; buf[pos++] = SDA_HI_DIR;
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE; buf[pos++] = 7;
	buf[pos++] = MPSSE_SET_BITS_LOW; buf[pos++] = 0x00; buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE; buf[pos++] = 0; buf[pos++] = ack ? 0x00 : 0x80;
	return pos;
}

int main(int argc, char *argv[])
{
	struct ftdi_context *ftdi;
	unsigned char cmd[4096], rsp[256], ack;
	unsigned char pattern[256];
	int pos, i, read_len, iterations, ok = 0, fail = 0;

	read_len = argc > 1 ? atoi(argv[1]) : 8;
	iterations = argc > 2 ? atoi(argv[2]) : 100;

	if (access("/sys/module/ftdi_mpsse", F_OK) == 0)
		errx(1, "kernel ftdi_mpsse loaded");

	ftdi = ftdi_new();
	if (!ftdi || ftdi_usb_open(ftdi, 0x0403, 0x6014) < 0)
		errx(1, "ftdi open failed");

	ftdi_set_interface(ftdi, INTERFACE_A);
	ftdi_usb_reset(ftdi);
	ftdi_set_latency_timer(ftdi, 1);
	ftdi_set_bitmode(ftdi, 0, 0);
	ftdi_set_bitmode(ftdi, 0, 2);
	usleep(50000);
	ftdi_tcioflush(ftdi);

	/* Init at 400 kHz */
	pos = 0;
	cmd[pos++] = MPSSE_DISABLE_CLK_DIV5;
	cmd[pos++] = MPSSE_ENABLE_3PHASE;
	cmd[pos++] = MPSSE_DISABLE_ADAPTIVE;
	cmd[pos++] = MPSSE_SET_CLK_DIVISOR;
	cmd[pos++] = DIV_400K_LO; cmd[pos++] = DIV_400K_HI;
	cmd[pos++] = MPSSE_LOOPBACK_OFF;
	cmd[pos++] = MPSSE_DRIVE_ZERO_ONLY;
	cmd[pos++] = PIN_SCL | PIN_SDA_OUT; cmd[pos++] = 0x00;
	cmd[pos++] = MPSSE_SET_BITS_LOW;
	cmd[pos++] = PIN_SCL | SDA_HI_VAL; cmd[pos++] = SDA_HI_DIR;
	xfer(ftdi, cmd, pos, NULL, 0);

	/* Write pattern at offset 0 (at 400 kHz) */
	for (i = 0; i < read_len; i++)
		pattern[i] = (i * 3 + 0xA5) & 0xFF;

	/* START + addr(W) */
	pos = 0;
	cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
	cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = 0x00; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
	pos = build_write_byte_flush(cmd, pos, 0xA8);
	xfer(ftdi, cmd, pos, &ack, 1);
	pos = build_write_byte_flush(cmd, 0, 0x00);
	xfer(ftdi, cmd, pos, &ack, 1);
	for (i = 0; i < read_len; i++) {
		pos = build_write_byte_flush(cmd, 0, pattern[i]);
		xfer(ftdi, cmd, pos, &ack, 1);
	}
	/* STOP */
	pos = 0;
	cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = 0x00; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
	cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
	cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL | SDA_HI_VAL; cmd[pos++] = SDA_HI_DIR;
	xfer(ftdi, cmd, pos, NULL, 0);

	printf("Testing %dB reads (%d iter): 400kHz write → 100kHz batched read\n",
	       read_len, iterations);

	for (int iter = 0; iter < iterations; iter++) {
		/* START + addr(W) + offset at 400 kHz */
		pos = 0;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = 0x00; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		pos = build_write_byte_flush(cmd, pos, 0xA8);
		xfer(ftdi, cmd, pos, &ack, 1);
		pos = build_write_byte_flush(cmd, 0, 0x00);
		xfer(ftdi, cmd, pos, &ack, 1);

		/* Repeated START + addr(R) at 400 kHz */
		pos = 0;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = SDA_HI_VAL; cmd[pos++] = SDA_HI_DIR;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL | SDA_HI_VAL; cmd[pos++] = SDA_HI_DIR;
		xfer(ftdi, cmd, pos, NULL, 0);
		pos = 0;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = 0x00; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		pos = build_write_byte_flush(cmd, pos, 0xA9);
		xfer(ftdi, cmd, pos, &ack, 1);

		/* Batched read in chunks of 8, with clock switch to 100 kHz */
		int done = 0;
		int read_ok = 1;
		unsigned char rbuf[256];

		while (done < read_len) {
			int chunk = read_len - done;
			if (chunk > 8) chunk = 8;

			pos = 0;
			/* Switch to 100 kHz */
			cmd[pos++] = MPSSE_SET_CLK_DIVISOR;
			cmd[pos++] = DIV_100K_LO; cmd[pos++] = DIV_100K_HI;
			for (int k = 0; k < chunk; k++) {
				int last = (done + k == read_len - 1);
				pos = build_read_byte(cmd, pos, !last);
			}
			/* Restore 400 kHz */
			cmd[pos++] = MPSSE_SET_CLK_DIVISOR;
			cmd[pos++] = DIV_400K_LO; cmd[pos++] = DIV_400K_HI;
			cmd[pos++] = MPSSE_SEND_IMMEDIATE;

			/* Split write + read (like kernel) */
			if (ftdi_write_data(ftdi, cmd, pos) < 0) { read_ok = 0; break; }
			int total = 0, retries = 0;
			while (total < chunk && retries < 200) {
				int r = ftdi_read_data(ftdi, rbuf + done + total, chunk - total);
				if (r < 0) { read_ok = 0; break; }
				if (r == 0) { retries++; usleep(500); continue; }
				retries = 0; total += r;
			}
			if (total < chunk) { read_ok = 0; break; }
			done += chunk;
		}

		/* STOP */
		pos = 0;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = 0x00; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL; cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
		cmd[pos++] = MPSSE_SET_BITS_LOW; cmd[pos++] = PIN_SCL | SDA_HI_VAL; cmd[pos++] = SDA_HI_DIR;
		xfer(ftdi, cmd, pos, NULL, 0);

		if (read_ok && memcmp(rbuf, pattern, read_len) == 0) {
			ok++;
		} else {
			if (++fail <= 5) {
				printf("  FAIL iter %d: ", iter);
				for (i = 0; i < read_len; i++) printf("%02x ", rbuf[i]);
				printf("(expected ");
				for (i = 0; i < read_len; i++) printf("%02x ", pattern[i]);
				printf(")\n");
			}
		}
	}

	printf("Result: %d/%d pass\n", ok, iterations);
	ftdi_usb_close(ftdi);
	ftdi_free(ftdi);
	return fail ? 1 : 0;
}
