// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Vincent Jardin <vjardin@free.fr>
//
// I2C batched read diagnostic — raw MPSSE via libftdi (userland).
//
// Purpose:
//   Isolate the FT232H MPSSE command layer from the kernel USB transport.
//   Builds the EXACT same MPSSE command bytes that ftdi_i2c.c would send
//   (SET_BITS_LOW, CLK_BITS_IN_PVE, CLK_BITS_OUT_NVE, SEND_IMMEDIATE)
//   and sends them via libftdi's ftdi_write_data / ftdi_read_data.
//
// Design validation:
//   This tool proved that the MPSSE command sequence for batched I2C reads
//   is correct — 256-byte reads return MATCH when sent through libftdi.
//   It also proved that the data corruption seen in the kernel driver is
//   NOT in the MPSSE commands but in the kernel's USB bulk-read transport
//   (ftdi_mpsse_bulk_read truncates excess payload bytes that libftdi
//   caches via its readbuffer_remaining mechanism).
//
// Tests run:
//   1. Per-byte flush (one xfer per byte, like kernel v20) — baseline
//   2. Batched (all read bytes in one write + one read) — optimization
//   Both are run at a fixed I2C speed (100 kHz default) and compared.
//
// Usage:
//   Unload the kernel ftdi_mpsse module first.
//   gcc -o i2c_read_diag i2c_read_diag.c -lftdi1 -lusb-1.0
//   ./i2c_read_diag [num_bytes]        # default: 4
//
// Requires: libftdi1-dev, FT232H connected, RP2040 I2C EEPROM slave.

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libftdi1/ftdi.h>

/* MPSSE command opcodes (same defines as kernel ftdi_mpsse.h) */
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

/* Pin definitions (same as kernel) */
#define PIN_SCL         0x01
#define PIN_SDA_OUT     0x02

/* FT232H hardware open-drain: sda_hi_val/dir */
#define SDA_HI_VAL      PIN_SDA_OUT          /* 0x02 */
#define SDA_HI_DIR      (PIN_SCL | PIN_SDA_OUT) /* 0x03 */

static void hexdump(const char *label, const unsigned char *buf, int len)
{
	printf("  %s (%d bytes):", label, len);
	for (int i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("\n    ");
		printf("%02x ", buf[i]);
	}
	printf("\n");
}

static int xfer(struct ftdi_context *ftdi, unsigned char *cmd, int cmd_len,
		unsigned char *rsp, int rsp_len, const char *label)
{
	int ret;

	hexdump(label, cmd, cmd_len);

	ret = ftdi_write_data(ftdi, cmd, cmd_len);
	if (ret < 0)
		errx(1, "write failed: %s", ftdi_get_error_string(ftdi));

	if (rsp_len > 0) {
		int total = 0;
		int retries = 0;

		while (total < rsp_len && retries < 100) {
			ret = ftdi_read_data(ftdi, rsp + total,
					     rsp_len - total);
			if (ret < 0)
				errx(1, "read failed: %s",
				     ftdi_get_error_string(ftdi));
			if (ret == 0) {
				retries++;
				usleep(1000);
				continue;
			}
			retries = 0;
			total += ret;
		}
		if (total < rsp_len) {
			printf("  SHORT READ: got %d of %d bytes\n",
			       total, rsp_len);
			hexdump("partial rsp", rsp, total);
			return -1;
		}
		hexdump("rsp", rsp, rsp_len);
	}
	return 0;
}

/* Build kernel-identical I2C write byte command (flush per byte) */
static int build_write_byte_flush(unsigned char *buf, int pos,
				  unsigned char byte)
{
	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 7;
	buf[pos++] = byte;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = SDA_HI_VAL;
	buf[pos++] = SDA_HI_DIR;
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 0;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	return pos;
}

/* Build kernel-identical I2C read byte command (no flush) */
static int build_read_byte(unsigned char *buf, int pos, int ack)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = SDA_HI_VAL;
	buf[pos++] = SDA_HI_DIR;
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 7;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 0;
	buf[pos++] = ack ? 0x00 : 0x80;
	return pos;
}

/* Build START condition */
static int build_start(unsigned char *buf, int pos)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	return pos;
}

/* Build STOP condition */
static int build_stop(unsigned char *buf, int pos)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL | SDA_HI_VAL;
	buf[pos++] = SDA_HI_DIR;
	return pos;
}

/* Build repeated START (with USB flush for settle time) */
static int build_repeated_start_phase1(unsigned char *buf, int pos)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = SDA_HI_VAL;
	buf[pos++] = SDA_HI_DIR;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL | SDA_HI_VAL;
	buf[pos++] = SDA_HI_DIR;
	return pos;
}

static int build_repeated_start_phase2(unsigned char *buf, int pos)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	return pos;
}

int main(int argc, char *argv[])
{
	struct ftdi_context *ftdi;
	unsigned char cmd[4096], rsp[256];
	int pos, ret, read_len;
	unsigned char ack;
	unsigned char pattern[256];
	int i;

	read_len = argc > 1 ? atoi(argv[1]) : 4;

	if (access("/sys/module/ftdi_mpsse", F_OK) == 0)
		errx(1, "kernel ftdi_mpsse loaded, unload first");

	ftdi = ftdi_new();
	if (!ftdi)
		errx(1, "ftdi_new failed");

	ret = ftdi_usb_open(ftdi, 0x0403, 0x6014);
	if (ret < 0)
		errx(1, "ftdi_usb_open: %s", ftdi_get_error_string(ftdi));

	ftdi_set_interface(ftdi, INTERFACE_A);
	ftdi_usb_reset(ftdi);
	ftdi_set_latency_timer(ftdi, 1);
	ftdi_set_bitmode(ftdi, 0, 0);
	ftdi_set_bitmode(ftdi, 0, 2); /* MPSSE mode */
	usleep(50000);
	ftdi_usb_purge_buffers(ftdi);

	/* MPSSE init (same as kernel ftdi_i2c_hw_init) */
	pos = 0;
	cmd[pos++] = MPSSE_DISABLE_CLK_DIV5;
	cmd[pos++] = MPSSE_ENABLE_3PHASE;
	cmd[pos++] = MPSSE_DISABLE_ADAPTIVE;
	/* 100 kHz: div = 60000/(100*3) - 1 = 199 */
	cmd[pos++] = MPSSE_SET_CLK_DIVISOR;
	cmd[pos++] = 199 & 0xFF;
	cmd[pos++] = (199 >> 8) & 0xFF;
	cmd[pos++] = MPSSE_LOOPBACK_OFF;
	cmd[pos++] = MPSSE_DRIVE_ZERO_ONLY;
	cmd[pos++] = PIN_SCL | PIN_SDA_OUT;
	cmd[pos++] = 0x00;
	/* Idle: SCL=1, SDA=1 */
	cmd[pos++] = MPSSE_SET_BITS_LOW;
	cmd[pos++] = PIN_SCL | SDA_HI_VAL;
	cmd[pos++] = SDA_HI_DIR;
	xfer(ftdi, cmd, pos, NULL, 0, "INIT");

	/* Write known pattern at offset 0 */
	for (i = 0; i < read_len; i++)
		pattern[i] = (i * 3 + 0xA5) & 0xFF;

	printf("\n--- Write %d bytes at offset 0 ---\n", read_len);

	/* START + addr(W) */
	pos = build_start(cmd, 0);
	pos = build_write_byte_flush(cmd, pos, 0xA8); /* 0x54<<1 | W */
	xfer(ftdi, cmd, pos, &ack, 1, "START+ADDR_W");
	printf("  ACK=%d\n", ack & 1);

	/* Offset byte */
	pos = build_write_byte_flush(cmd, 0, 0x00);
	xfer(ftdi, cmd, pos, &ack, 1, "OFFSET");
	printf("  ACK=%d\n", ack & 1);

	/* Data bytes */
	for (i = 0; i < read_len; i++) {
		pos = build_write_byte_flush(cmd, 0, pattern[i]);
		xfer(ftdi, cmd, pos, &ack, 1, "DATA_W");
	}

	/* STOP */
	pos = build_stop(cmd, 0);
	xfer(ftdi, cmd, pos, NULL, 0, "STOP");

	printf("\n--- Read %d bytes: per-byte flush (kernel v20 style) ---\n",
	       read_len);

	/* START + addr(W) for set pointer */
	pos = build_start(cmd, 0);
	pos = build_write_byte_flush(cmd, pos, 0xA8);
	xfer(ftdi, cmd, pos, &ack, 1, "START+ADDR_W");

	/* Offset = 0 */
	pos = build_write_byte_flush(cmd, 0, 0x00);
	xfer(ftdi, cmd, pos, &ack, 1, "OFFSET");

	/* Repeated START + addr(R) */
	pos = build_repeated_start_phase1(cmd, 0);
	xfer(ftdi, cmd, pos, NULL, 0, "Sr_PHASE1");
	pos = build_repeated_start_phase2(cmd, 0);
	pos = build_write_byte_flush(cmd, pos, 0xA9);
	xfer(ftdi, cmd, pos, &ack, 1, "Sr+ADDR_R");
	printf("  ACK=%d\n", ack & 1);

	/* Read per-byte */
	memset(rsp, 0, sizeof(rsp));
	for (i = 0; i < read_len; i++) {
		int last = (i == read_len - 1);

		pos = build_read_byte(cmd, 0, !last);
		cmd[pos++] = MPSSE_SEND_IMMEDIATE;
		xfer(ftdi, cmd, pos, &rsp[i], 1, "READ_BYTE");
	}

	/* STOP */
	pos = build_stop(cmd, 0);
	xfer(ftdi, cmd, pos, NULL, 0, "STOP");

	printf("\n  Per-byte result: ");
	int per_byte_ok = 1;
	for (i = 0; i < read_len; i++) {
		printf("%02x ", rsp[i]);
		if (rsp[i] != pattern[i])
			per_byte_ok = 0;
	}
	printf("→ %s\n", per_byte_ok ? "MATCH" : "MISMATCH");

	printf("\n--- Read %d bytes: BATCHED (kernel v21 style) ---\n",
	       read_len);

	/* START + addr(W) for set pointer */
	pos = build_start(cmd, 0);
	pos = build_write_byte_flush(cmd, pos, 0xA8);
	xfer(ftdi, cmd, pos, &ack, 1, "START+ADDR_W");

	/* Offset = 0 */
	pos = build_write_byte_flush(cmd, 0, 0x00);
	xfer(ftdi, cmd, pos, &ack, 1, "OFFSET");

	/* Repeated START + addr(R) */
	pos = build_repeated_start_phase1(cmd, 0);
	xfer(ftdi, cmd, pos, NULL, 0, "Sr_PHASE1");
	pos = build_repeated_start_phase2(cmd, 0);
	pos = build_write_byte_flush(cmd, pos, 0xA9);
	xfer(ftdi, cmd, pos, &ack, 1, "Sr+ADDR_R");
	printf("  ACK=%d\n", ack & 1);

	/* Batched read: all bytes in ONE write + ONE read */
	pos = 0;
	for (i = 0; i < read_len; i++) {
		int last = (i == read_len - 1);

		pos = build_read_byte(cmd, pos, !last);
	}
	cmd[pos++] = MPSSE_SEND_IMMEDIATE;

	memset(rsp, 0, sizeof(rsp));
	xfer(ftdi, cmd, pos, rsp, read_len, "BATCHED_READ");

	/* STOP */
	pos = build_stop(cmd, 0);
	xfer(ftdi, cmd, pos, NULL, 0, "STOP");

	printf("\n  Batched result:  ");
	int batched_ok = 1;
	for (i = 0; i < read_len; i++) {
		printf("%02x ", rsp[i]);
		if (rsp[i] != pattern[i])
			batched_ok = 0;
	}
	printf("→ %s\n", batched_ok ? "MATCH" : "MISMATCH");

	printf("\n  Expected:        ");
	for (i = 0; i < read_len; i++)
		printf("%02x ", pattern[i]);
	printf("\n");

	ftdi_usb_close(ftdi);
	ftdi_free(ftdi);
	return (per_byte_ok && batched_ok) ? 0 : 1;
}
