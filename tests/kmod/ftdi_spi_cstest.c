// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * Test client for the chip select handling of an SPI controller
 *
 * cs_setup, cs_hold, cs_inactive, cs_change_delay and cs_off have no user
 * space interface in spidev. This driver is bound by hand to one SPI
 * device (tests/init.sh, mode spi-cs) and sends these messages from its
 * probe:
 *
 *   A  4 bytes                           cs_setup 300 us, cs_hold 500 us
 *   B  4 bytes                           cs_inactive 700 us since A
 *   C  4 bytes, cs_change with a 2 ms cs_change_delay, 4 bytes
 *   D  2 bytes, 3 bytes with cs_off, 2 bytes
 *
 * The device must send back what it received one byte earlier (the
 * shift-register slaves of ftdi_usbip --spi-echo): D checks that the
 * bytes clocked with the chip select off did not reach it. The emulator
 * measures the times, frame by frame.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

#define CSTEST_SPEED_HZ		100000

static int cstest_msg(struct spi_device *spi, struct spi_transfer *xfers,
		      unsigned int n, const char *what)
{
	int ret = spi_sync_transfer(spi, xfers, n);

	dev_info(&spi->dev, "cstest: message %s: %d\n", what, ret);
	return ret;
}

static int cstest_probe(struct spi_device *spi)
{
	struct spi_transfer x[3];
	u8 *tx, *rx;
	int ret;

	/* DMA-safe buffers, as the SPI core requires */
	tx = devm_kzalloc(&spi->dev, 32, GFP_KERNEL);
	rx = devm_kzalloc(&spi->dev, 32, GFP_KERNEL);
	if (!tx || !rx)
		return -ENOMEM;
	memcpy(tx, "\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc", 12);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = CSTEST_SPEED_HZ;
	spi->cs_setup = (struct spi_delay){ 300, SPI_DELAY_UNIT_USECS };
	spi->cs_hold = (struct spi_delay){ 500, SPI_DELAY_UNIT_USECS };
	spi->cs_inactive = (struct spi_delay){ 700, SPI_DELAY_UNIT_USECS };
	ret = spi_setup(spi);
	if (ret)
		return ret;

	memset(x, 0, sizeof(x));
	x[0].tx_buf = tx;
	x[0].len = 4;
	ret = cstest_msg(spi, x, 1, "A");
	if (!ret)
		ret = cstest_msg(spi, x, 1, "B");
	if (ret)
		return ret;

	memset(x, 0, sizeof(x));
	x[0].tx_buf = tx;
	x[0].len = 4;
	x[0].cs_change = 1;
	x[0].cs_change_delay = (struct spi_delay){ 2000, SPI_DELAY_UNIT_USECS };
	x[1].tx_buf = tx + 4;
	x[1].len = 4;
	ret = cstest_msg(spi, x, 2, "C");
	if (ret)
		return ret;

	/*
	 * D: the slave has 0x88 in its shift register. It gets 0x11 0x22,
	 * not 0x33 0x44 0x55 (chip select off), then 0x66 0x77: it answers
	 * 0x88 0x11, then 0x22 0x66.
	 */
	memset(x, 0, sizeof(x));
	x[0].tx_buf = tx;
	x[0].rx_buf = rx;
	x[0].len = 2;
	x[1].tx_buf = tx + 2;
	x[1].len = 3;
	x[1].cs_off = 1;
	x[2].tx_buf = tx + 5;
	x[2].rx_buf = rx + 2;
	x[2].len = 2;
	ret = cstest_msg(spi, x, 3, "D");
	if (ret)
		return ret;

	dev_info(&spi->dev, "cstest: D received %*ph\n", 4, rx);
	if (rx[0] == 0x88 && rx[1] == 0x11 && rx[2] == 0x22 && rx[3] == 0x66)
		dev_info(&spi->dev, "cstest: cs_off PASS\n");
	else
		dev_err(&spi->dev, "cstest: cs_off FAIL\n");

	dev_info(&spi->dev, "cstest: done\n");
	return 0;
}

static struct spi_driver cstest_driver = {
	.driver = {
		.name = "ftdi_spi_cstest",
	},
	.probe = cstest_probe,
};
module_spi_driver(cstest_driver);

MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("Test client for SPI chip select timing");
MODULE_LICENSE("GPL");
