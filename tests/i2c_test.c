// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * i2c_test.c -- Simple I2C test tool for FTDI driver testing
 *
 * Usage: i2c_test /dev/i2c-X [slave_addr]	slave_addr: a counter device
 *        i2c_test /dev/i2c-X mem		only the memory device test
 *
 * The emulator's bus has counter devices (0x20, 0x22, 0x50, 0x51: every
 * byte read is the next value of a counter) and a memory device at 0x57
 * that works like a small EEPROM: the first byte written is the offset,
 * the following ones are stored when the STOP arrives, reads return the
 * memory from the offset on. Nothing answers at 0x30. The counter is
 * the same for every counter address and goes on from one transfer to
 * the next: every byte read from it is checked.
 *
 * Returns 0 on success, 1 on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "counter.h"

#define DEFAULT_SLAVE_ADDR	0x50	/* a counter device */
#define TEST_PATTERN_LEN	4
#define MEM_ADDR		0x57	/* the emulator's memory device */
#define NOBODY_ADDR		0x30	/* no device there */
#define MEM_LEN			8

/*
 * Write MEM_LEN bytes at an offset of the memory device, read them back
 * with "write offset, repeated START, read". Without the STOP the write
 * is not stored, without the repeated START the read is not a read.
 */
static int mem_round(int fd, unsigned char off, unsigned char seed)
{
	unsigned char wr[1 + MEM_LEN], rd[MEM_LEN];
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msgs[2];
	int i;

	wr[0] = off;
	for (i = 0; i < MEM_LEN; i++)
		wr[1 + i] = seed + i * 0x1d;

	msgs[0] = (struct i2c_msg){ MEM_ADDR, 0, sizeof(wr), wr };
	rdwr.msgs = msgs;
	rdwr.nmsgs = 1;
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
		fprintf(stderr, "FAIL: write to the memory device: %s\n",
			strerror(errno));
		return 1;
	}

	memset(rd, 0, sizeof(rd));
	msgs[0] = (struct i2c_msg){ MEM_ADDR, 0, 1, &off };
	msgs[1] = (struct i2c_msg){ MEM_ADDR, I2C_M_RD, sizeof(rd), rd };
	rdwr.nmsgs = 2;
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
		fprintf(stderr, "FAIL: read of the memory device: %s\n",
			strerror(errno));
		return 1;
	}

	if (memcmp(rd, wr + 1, MEM_LEN)) {
		fprintf(stderr, "FAIL: memory device at 0x%02x: wrote", off);
		for (i = 0; i < MEM_LEN; i++)
			fprintf(stderr, " %02x", wr[1 + i]);
		fprintf(stderr, ", read");
		for (i = 0; i < MEM_LEN; i++)
			fprintf(stderr, " %02x", rd[i]);
		fprintf(stderr, "\n");
		return 1;
	}
	return 0;
}

static int mem_test(int fd)
{
	printf("Memory device: write, STOP, write offset + repeated START + read... ");
	fflush(stdout);
	if (mem_round(fd, 0x10, 0xC3) || mem_round(fd, 0x80, 0x5A) ||
	    mem_round(fd, 0x14, 0x07))
		return 1;
	printf("PASS (memory device data OK)\n");
	return 0;
}

int main(int argc, char *argv[])
{
	const char *dev_path;
	int slave_addr = DEFAULT_SLAVE_ADDR;
	int fd, ret, last = -1;
	unsigned char tx_buf[TEST_PATTERN_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char rx_buf[TEST_PATTERN_LEN];
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msgs[2];

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/i2c-X [slave_addr]\n", argv[0]);
		return 1;
	}

	dev_path = argv[1];
	if (argc >= 3 && strcmp(argv[2], "mem"))
		slave_addr = strtol(argv[2], NULL, 0);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	if (argc >= 3 && !strcmp(argv[2], "mem")) {
		ret = mem_test(fd);
		close(fd);
		return ret;
	}

	printf("I2C test on %s, slave address 0x%02x\n", dev_path, slave_addr);

	printf("Test 1: Simple write... ");
	if (ioctl(fd, I2C_SLAVE, slave_addr) < 0) {
		fprintf(stderr, "FAIL: ioctl(I2C_SLAVE) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	ret = write(fd, tx_buf, TEST_PATTERN_LEN);
	if (ret < 0) {
		fprintf(stderr, "FAIL: write failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: short write (%d/%d)\n", ret, TEST_PATTERN_LEN);
		close(fd);
		return 1;
	}
	printf("PASS (wrote %d bytes)\n", ret);

	printf("Test 2: Simple read... ");
	memset(rx_buf, 0, sizeof(rx_buf));
	ret = read(fd, rx_buf, TEST_PATTERN_LEN);
	if (ret < 0) {
		fprintf(stderr, "FAIL: read failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (ret != TEST_PATTERN_LEN) {
		fprintf(stderr, "FAIL: short read (%d/%d)\n", ret, TEST_PATTERN_LEN);
		close(fd);
		return 1;
	}
	if (!counter_ok("read", rx_buf, TEST_PATTERN_LEN, &last)) {
		close(fd);
		return 1;
	}
	printf("PASS (read %d bytes: %02x %02x %02x %02x - counter pattern OK)\n",
	       ret, rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	printf("Test 3: Combined write-read (I2C_RDWR)... ");
	msgs[0].addr = slave_addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = tx_buf;

	msgs[1].addr = slave_addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = TEST_PATTERN_LEN;
	msgs[1].buf = rx_buf;

	rdwr.msgs = msgs;
	rdwr.nmsgs = 2;

	memset(rx_buf, 0, sizeof(rx_buf));
	ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret < 0) {
		fprintf(stderr, "FAIL: ioctl(I2C_RDWR) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (!counter_ok("write-read", rx_buf, TEST_PATTERN_LEN, &last)) {
		close(fd);
		return 1;
	}
	printf("PASS (read %02x %02x %02x %02x)\n",
	       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	{
		struct i2c_msg comp_msgs[4];
		unsigned char ptr = 0x00;
		unsigned char rd[2];

		printf("Test 4: Compound w+r+w+r (3 repeated STARTs)... ");
		comp_msgs[0].addr = slave_addr;
		comp_msgs[0].flags = 0;
		comp_msgs[0].len = 1;
		comp_msgs[0].buf = &ptr;
		comp_msgs[1].addr = slave_addr;
		comp_msgs[1].flags = I2C_M_RD;
		comp_msgs[1].len = 1;
		comp_msgs[1].buf = &rd[0];
		comp_msgs[2].addr = slave_addr;
		comp_msgs[2].flags = 0;
		comp_msgs[2].len = 1;
		comp_msgs[2].buf = &ptr;
		comp_msgs[3].addr = slave_addr;
		comp_msgs[3].flags = I2C_M_RD;
		comp_msgs[3].len = 1;
		comp_msgs[3].buf = &rd[1];

		rdwr.msgs = comp_msgs;
		rdwr.nmsgs = 4;
		ret = ioctl(fd, I2C_RDWR, &rdwr);
		if (ret < 0) {
			fprintf(stderr, "FAIL: compound I2C_RDWR: %s\n",
				strerror(errno));
			close(fd);
			return 1;
		}
		if (!counter_ok("w+r+w+r", rd, 2, &last)) {
			close(fd);
			return 1;
		}
		printf("PASS (rd1=0x%02x rd2=0x%02x)\n", rd[0], rd[1]);
	}

	{
#define LARGE_LEN	256
		unsigned char large_tx[LARGE_LEN];
		unsigned char large_rx[LARGE_LEN];
		struct i2c_msg large_msgs[2];
		int i;

		printf("Test 5: 256-byte write+read... ");
		for (i = 0; i < LARGE_LEN; i++)
			large_tx[i] = (i * 3 + 0x42) & 0xFF;

		large_msgs[0].addr = slave_addr;
		large_msgs[0].flags = 0;
		large_msgs[0].len = LARGE_LEN;
		large_msgs[0].buf = large_tx;
		rdwr.msgs = large_msgs;
		rdwr.nmsgs = 1;
		ret = ioctl(fd, I2C_RDWR, &rdwr);
		if (ret < 0) {
			fprintf(stderr, "FAIL: 256B write: %s\n",
				strerror(errno));
			close(fd);
			return 1;
		}

		memset(large_rx, 0, sizeof(large_rx));
		large_msgs[0].addr = slave_addr;
		large_msgs[0].flags = I2C_M_RD;
		large_msgs[0].len = LARGE_LEN;
		large_msgs[0].buf = large_rx;
		ret = ioctl(fd, I2C_RDWR, &rdwr);
		if (ret < 0) {
			fprintf(stderr, "FAIL: 256B read: %s\n",
				strerror(errno));
			close(fd);
			return 1;
		}

		if (!counter_ok("256B read", large_rx, LARGE_LEN, &last)) {
			close(fd);
			return 1;
		}
		printf("PASS (256B counter pattern OK)\n");
	}

	printf("Test 6: ");
	if (mem_test(fd)) {
		close(fd);
		return 1;
	}

	printf("Test 7: No device at 0x%02x... ", NOBODY_ADDR);
	msgs[0] = (struct i2c_msg){ NOBODY_ADDR, I2C_M_RD, 1, rx_buf };
	rdwr.msgs = msgs;
	rdwr.nmsgs = 1;
	ret = ioctl(fd, I2C_RDWR, &rdwr);
	if (ret >= 0 || errno != ENXIO) {
		fprintf(stderr, "FAIL: expected ENXIO, got %s\n",
			ret >= 0 ? "success" : strerror(errno));
		close(fd);
		return 1;
	}
	printf("PASS (ENXIO)\n");

	close(fd);
	printf("All I2C tests PASSED\n");
	return 0;
}
