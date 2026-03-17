// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * i2c_test.c -- Simple I2C test tool for FTDI driver testing
 *
 * Performs basic I2C read/write operations and verifies the emulator
 * responds correctly.
 *
 * Usage: i2c_test /dev/i2c-X [slave_addr]
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

#define DEFAULT_SLAVE_ADDR	0x50	/* Common EEPROM address */
#define TEST_PATTERN_LEN	4

int main(int argc, char *argv[])
{
	const char *dev_path;
	int slave_addr = DEFAULT_SLAVE_ADDR;
	int fd, ret;
	unsigned char tx_buf[TEST_PATTERN_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF };
	unsigned char rx_buf[TEST_PATTERN_LEN];
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msgs[2];

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/i2c-X [slave_addr]\n", argv[0]);
		return 1;
	}

	dev_path = argv[1];
	if (argc >= 3)
		slave_addr = strtol(argv[2], NULL, 0);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error opening %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	printf("I2C test on %s, slave address 0x%02x\n", dev_path, slave_addr);

	/*
	 * Test 1: Simple write using ioctl(I2C_SLAVE) + write()
	 */
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

	/*
	 * Test 2: Simple read - verify counter pattern
	 * Emulator returns 0xA0, 0xA1, 0xA2, ... for each read byte
	 */
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
	/* Verify counter pattern from emulator */
	if (rx_buf[1] != rx_buf[0] + 1 || rx_buf[2] != rx_buf[0] + 2) {
		fprintf(stderr, "FAIL: unexpected pattern %02x %02x %02x %02x (expected counter)\n",
			rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);
		close(fd);
		return 1;
	}
	printf("PASS (read %d bytes: %02x %02x %02x %02x - counter pattern OK)\n",
	       ret, rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	/*
	 * Test 3: Combined write-read using I2C_RDWR ioctl
	 */
	printf("Test 3: Combined write-read (I2C_RDWR)... ");
	msgs[0].addr = slave_addr;
	msgs[0].flags = 0;		/* Write */
	msgs[0].len = 1;
	msgs[0].buf = tx_buf;		/* Register address */

	msgs[1].addr = slave_addr;
	msgs[1].flags = I2C_M_RD;	/* Read */
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
	printf("PASS (read %02x %02x %02x %02x)\n",
	       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	/*
	 * Test 4: Compound 4-message I2C_RDWR (w+r+w+r, 3 repeated STARTs)
	 * Validates that read_in_progress is properly reset between
	 * read→write→read transitions.
	 */
	{
		struct i2c_msg comp_msgs[4];
		unsigned char ptr = 0x00;
		unsigned char rd1, rd2;

		printf("Test 4: Compound w+r+w+r (3 repeated STARTs)... ");
		comp_msgs[0].addr = slave_addr;
		comp_msgs[0].flags = 0;
		comp_msgs[0].len = 1;
		comp_msgs[0].buf = &ptr;
		comp_msgs[1].addr = slave_addr;
		comp_msgs[1].flags = I2C_M_RD;
		comp_msgs[1].len = 1;
		comp_msgs[1].buf = &rd1;
		comp_msgs[2].addr = slave_addr;
		comp_msgs[2].flags = 0;
		comp_msgs[2].len = 1;
		comp_msgs[2].buf = &ptr;
		comp_msgs[3].addr = slave_addr;
		comp_msgs[3].flags = I2C_M_RD;
		comp_msgs[3].len = 1;
		comp_msgs[3].buf = &rd2;

		rdwr.msgs = comp_msgs;
		rdwr.nmsgs = 4;
		ret = ioctl(fd, I2C_RDWR, &rdwr);
		if (ret < 0) {
			fprintf(stderr, "FAIL: compound I2C_RDWR: %s\n",
				strerror(errno));
			close(fd);
			return 1;
		}
		/*
		 * The emulator returns a monotonic counter, so rd1 != rd2.
		 * A real EEPROM at the same offset would return rd1 == rd2.
		 * Here we just verify the ioctl succeeded with valid data.
		 */
		printf("PASS (rd1=0x%02x rd2=0x%02x)\n", rd1, rd2);
	}

	/*
	 * Test 5: Large transfer — 256 bytes write + read
	 * Validates FTDI_I2C_MAX_XFER_SIZE (4096).
	 */
	{
#define LARGE_LEN	256
		unsigned char large_tx[LARGE_LEN];
		unsigned char large_rx[LARGE_LEN];
		struct i2c_msg large_msgs[2];
		int i;

		printf("Test 5: 256-byte write+read... ");
		for (i = 0; i < LARGE_LEN; i++)
			large_tx[i] = (i * 3 + 0x42) & 0xFF;

		/* Write */
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

		/* Read back */
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

		/* Verify counter pattern from emulator */
		if (large_rx[1] == large_rx[0] + 1 &&
		    large_rx[2] == large_rx[0] + 2)
			printf("PASS (256B counter pattern OK)\n");
		else {
			fprintf(stderr,
				"FAIL: 256B pattern: %02x %02x %02x...\n",
				large_rx[0], large_rx[1], large_rx[2]);
			close(fd);
			return 1;
		}
	}

	close(fd);
	printf("All I2C tests PASSED\n");
	return 0;
}
