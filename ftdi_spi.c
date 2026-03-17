// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE SPI child driver
 *
 * SPI controller using the MPSSE engine on FT232H / FT2232H / FT4232H.
 * Pin mapping (MPSSE mode):
 *   AD0 = SCK,  AD1 = MOSI (DO),  AD2 = MISO (DI),  AD3 = CS0
 *   AD4-AD7 available as additional CS lines.
 *
 * Clock: 60 MHz base (DIV5 disabled) -> freq = 60 MHz / ((1 + divisor) * 2)
 *        max 30 MHz, min ~457 Hz.
 *
 * References: AN_108, AN_114, spi-dln2.c
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi-spi"

/*
 * Module parameter to register spidev devices for userspace access.
 * When enabled (default), creates /dev/spidevX.Y for each chip select,
 * allowing userspace applications to communicate with SPI slaves.
 * Disable if using a kernel SPI slave driver instead of spidev.
 */
static bool register_spidev = true;
module_param(register_spidev, bool, 0444);
MODULE_PARM_DESC(register_spidev,
		 "Register spidev devices for userspace access (default: Y)");

#define FTDI_SPI_MAX_CS		5	/* CS0 = AD3, CS1-CS4 = AD4-AD7 */
#define FTDI_SPI_BASE_CLK	60000000UL
#define FTDI_SPI_MAX_SPEED	(FTDI_SPI_BASE_CLK / 2)	/* 30 MHz */
#define FTDI_SPI_MIN_SPEED	(FTDI_SPI_BASE_CLK / (2 * (1 + 0xffff)))

/* Pin masks for MPSSE low byte (AD0-AD7) */
#define PIN_SCK		BIT(0)	/* AD0 -- clock */
#define PIN_MOSI	BIT(1)	/* AD1 -- data out */
#define PIN_MISO	BIT(2)	/* AD2 -- data in */
#define PIN_CS0		BIT(3)	/* AD3 -- chip select 0 */

/*
 * Per-controller state.  low_dir, low_val, and tx_buf are only accessed
 * from SPI core callbacks (setup, transfer_one_message) which are
 * serialised by the SPI core's io_mutex -- no additional locking
 * required here.
 */
struct ftdi_spi {
	struct platform_device *pdev;
	struct spi_controller *host;
	u8 *tx_buf;		/* command assembly buffer (DMA-safe) */
	u8 cs_pin[FTDI_SPI_MAX_CS];	/* GPIO pin number for each CS */
	u8 cs_active_high;	/* bitmask: CS lines with SPI_CS_HIGH */
	u8 low_dir;		/* MPSSE low byte direction cache */
	u8 low_val;		/* MPSSE low byte value cache */
	/* EEPROM drive settings cached for speed-dependent checks */
	bool ee_checked;
	bool ee_slow_slew;
	u8 ee_drive_ma;
	bool speed_warned;
};

static u16 ftdi_spi_divisor(u32 speed_hz)
{
	u32 div;

	if (speed_hz >= FTDI_SPI_MAX_SPEED)
		return 0;

	div = DIV_ROUND_UP(FTDI_SPI_BASE_CLK, 2 * speed_hz) - 1;
	return min_t(u32, div, 0xffff);
}

/*
 * Compute idle pin state for all CS lines.  Called from probe
 * (cs_active_high == 0 -> all idle HIGH) and from transfer_one_message.
 */
static void ftdi_spi_init_pins(struct ftdi_spi *fspi)
{
	unsigned int num_cs = fspi->host->num_chipselect;
	int i;

	fspi->low_dir = PIN_SCK | PIN_MOSI;
	fspi->low_val = 0;
	for (i = 0; i < num_cs; i++) {
		fspi->low_dir |= BIT(fspi->cs_pin[i]);
		if (fspi->cs_active_high & BIT(i))
			fspi->low_val &= ~BIT(fspi->cs_pin[i]);
		else
			fspi->low_val |= BIT(fspi->cs_pin[i]);
	}
}

/*
 * Select MPSSE byte-mode clock opcode based on SPI mode and direction.
 *
 * SPI mode 0 (CPOL=0, CPHA=0): out on -ve, in on +ve
 * SPI mode 1 (CPOL=0, CPHA=1): out on +ve, in on -ve
 * SPI mode 2 (CPOL=1, CPHA=0): out on +ve, in on -ve
 * SPI mode 3 (CPOL=1, CPHA=1): out on -ve, in on +ve
 */
static u8 ftdi_spi_clk_cmd(struct spi_device *spi, bool tx, bool rx)
{
	u8 cmd;

	if (tx && rx) {
		if ((spi->mode & SPI_MODE_X_MASK) == SPI_MODE_0 ||
		    (spi->mode & SPI_MODE_X_MASK) == SPI_MODE_3)
			cmd = MPSSE_CLK_BYTES_INOUT_PVE_NVE;
		else
			cmd = MPSSE_CLK_BYTES_INOUT_NVE_PVE;
	} else if (tx) {
		if ((spi->mode & SPI_MODE_X_MASK) == SPI_MODE_0 ||
		    (spi->mode & SPI_MODE_X_MASK) == SPI_MODE_3)
			cmd = MPSSE_CLK_BYTES_OUT_NVE;
		else
			cmd = MPSSE_CLK_BYTES_OUT_PVE;
	} else {
		if ((spi->mode & SPI_MODE_X_MASK) == SPI_MODE_0 ||
		    (spi->mode & SPI_MODE_X_MASK) == SPI_MODE_3)
			cmd = MPSSE_CLK_BYTES_IN_PVE;
		else
			cmd = MPSSE_CLK_BYTES_IN_NVE;
	}

	if (spi->mode & SPI_LSB_FIRST)
		cmd |= MPSSE_CLK_LSB_FIRST;

	return cmd;
}

static int ftdi_spi_flush(struct ftdi_spi *fspi, u8 *buf, unsigned int len,
			  u8 *rx, unsigned int rx_len)
{
	if (rx) {
		buf[len++] = MPSSE_SEND_IMMEDIATE;
		return ftdi_mpsse_xfer(fspi->pdev, buf, len, rx, rx_len);
	}
	return ftdi_mpsse_write(fspi->pdev, buf, len);
}

static unsigned int ftdi_spi_emit_cs(struct ftdi_spi *fspi, u8 *buf,
				     unsigned int pos, unsigned int cs,
				     bool active)
{
	u8 cs_bit = BIT(fspi->cs_pin[cs]);

	if (active ^ !(fspi->cs_active_high & BIT(cs)))
		fspi->low_val &= ~cs_bit;
	else
		fspi->low_val |= cs_bit;

	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = fspi->low_val;
	buf[pos++] = fspi->low_dir;
	return pos;
}

static void ftdi_spi_check_eeprom(struct ftdi_spi *fspi)
{
	struct platform_device *pdev = fspi->pdev;
	const struct ftdi_eeprom *ee;
	u8 drive_ma;
	bool schmitt, slow_slew;

	ee = ftdi_mpsse_get_eeprom(pdev);
	if (!ee)
		return;

	if (ftdi_mpsse_get_eeprom_drive(pdev, &drive_ma, &schmitt, &slow_slew))
		return;

	fspi->ee_slow_slew = slow_slew;
	fspi->ee_drive_ma = drive_ma;
	fspi->ee_checked = true;

	/* Schmitt trigger critical for MISO sampling at high frequencies */
	if (!schmitt)
		dev_warn(&pdev->dev,
			 "EEPROM: Schmitt trigger OFF on data bus, SPI requires Schmitt for reliable MISO sampling\n");

	/* Suspend pull-downs catastrophic: all CS# pulled low simultaneously */
	if (ee->pulldown)
		dev_warn(&pdev->dev,
			 "EEPROM: suspend_pull_downs enabled, all CS# lines will be pulled low during USB suspend causing bus contention and slave state corruption\n");

	/* VCP must be off for MPSSE access */
	if (ee->cha_vcp)
		dev_notice(&pdev->dev,
			   "EEPROM: VCP driver enabled, may prevent MPSSE access required for SPI\n");
}

static void ftdi_spi_check_speed(struct ftdi_spi *fspi, u32 speed)
{
	struct platform_device *pdev = fspi->pdev;

	if (!fspi->ee_checked || fspi->speed_warned)
		return;

	if (speed > 15000000 && fspi->ee_slow_slew)
		dev_notice(&pdev->dev,
			   "EEPROM: slow slew rate at %u Hz SPI, consider fast slew for >15 MHz\n",
			   speed);
	else if (speed <= 5000000 && !fspi->ee_slow_slew)
		dev_notice(&pdev->dev,
			   "EEPROM: fast slew rate at %u Hz SPI, slow slew recommended for <=5 MHz\n",
			   speed);

	if (speed > 20000000 && fspi->ee_drive_ma < 12)
		dev_notice(&pdev->dev,
			   "EEPROM: data bus drive is %u mA at %u Hz, 12+ mA recommended for >20 MHz SPI\n",
			   fspi->ee_drive_ma, speed);

	fspi->speed_warned = true;
}

static int ftdi_spi_setup(struct spi_device *spi)
{
	struct ftdi_spi *fspi = spi_controller_get_devdata(spi->controller);
	unsigned int cs = spi_get_chipselect(spi, 0);

	if (cs >= FTDI_SPI_MAX_CS)
		return -EINVAL;

	/*
	 * AN_114 §1.2: "FTDI device can only support mode 0 and mode 2
	 * due to the limitation of MPSSE engine."  Modes 1 and 3 work
	 * for most slaves by using the complementary edge opcode pairing,
	 * but are not guaranteed to meet timing for all devices.
	 */
	if (spi->mode & SPI_CPHA)
		dev_notice(&spi->dev,
			   "SPI mode %u uses CPHA=1; only modes 0 and 2 are fully supported by MPSSE (AN_114)\n",
			   spi->mode & SPI_MODE_X_MASK);

	if (spi->mode & SPI_CS_HIGH)
		fspi->cs_active_high |= BIT(cs);
	else
		fspi->cs_active_high &= ~BIT(cs);

	return 0;
}

static int ftdi_spi_transfer_one_message(struct spi_controller *host,
					 struct spi_message *msg)
{
	struct ftdi_spi *fspi = spi_controller_get_devdata(host);
	struct spi_device *spi = msg->spi;
	unsigned int cs = spi_get_chipselect(spi, 0);
	struct spi_transfer *xfer;
	u8 *buf = fspi->tx_buf;
	unsigned int pos = 0;
	u16 div, cur_div;
	int ret = 0;

	ftdi_mpsse_bus_lock(fspi->pdev);

	buf[pos++] = MPSSE_DISABLE_CLK_DIV5;
	buf[pos++] = MPSSE_DISABLE_ADAPTIVE;
	buf[pos++] = MPSSE_DISABLE_3PHASE;

	cur_div = ftdi_spi_divisor(spi->max_speed_hz);
	buf[pos++] = MPSSE_SET_CLK_DIVISOR;
	buf[pos++] = cur_div & 0xff;
	buf[pos++] = (cur_div >> 8) & 0xff;

	ftdi_spi_init_pins(fspi);

	if (spi->mode & SPI_CPOL)
		fspi->low_val |= PIN_SCK;

	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = fspi->low_val;
	buf[pos++] = fspi->low_dir;

	buf[pos++] = (spi->mode & SPI_LOOP) ? MPSSE_LOOPBACK_ON
					     : MPSSE_LOOPBACK_OFF;

	ftdi_spi_check_speed(fspi, spi->max_speed_hz);

	pos = ftdi_spi_emit_cs(fspi, buf, pos, cs, true);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		const u8 *tx_data = xfer->tx_buf;
		u8 *rx_data = xfer->rx_buf;
		unsigned int remaining = xfer->len;
		unsigned int bpw = xfer->bits_per_word;
		u8 clk_cmd;

		if (xfer->speed_hz) {
			div = ftdi_spi_divisor(xfer->speed_hz);
			if (div != cur_div) {
				buf[pos++] = MPSSE_SET_CLK_DIVISOR;
				buf[pos++] = div & 0xff;
				buf[pos++] = (div >> 8) & 0xff;
				cur_div = div;
			}
		}

		clk_cmd = ftdi_spi_clk_cmd(spi, !!tx_data, !!rx_data);

		if (bpw < 8) {
			u8 bit_cmd = clk_cmd | MPSSE_CLK_BIT_MODE;

			while (remaining > 0) {
				if (pos + 4 > FTDI_MPSSE_BUF_SIZE - 16) {
					ret = ftdi_spi_flush(fspi, buf, pos,
							     rx_data, 1);
					if (ret)
						goto err_deassert;
					if (rx_data)
						rx_data++;
					pos = 0;
				}

				buf[pos++] = bit_cmd;
				buf[pos++] = bpw - 1;
				if (tx_data)
					buf[pos++] = *tx_data++;

				if (rx_data) {
					ret = ftdi_spi_flush(fspi, buf, pos,
							     rx_data, 1);
					if (ret)
						goto err_deassert;
					rx_data++;
					pos = 0;
				}

				remaining--;
				msg->actual_length++;
			}
		} else {
			while (remaining > 0) {
				unsigned int chunk = min_t(unsigned int,
							  remaining,
							  FTDI_MPSSE_BUF_SIZE - 16);
				u16 mpsse_len = chunk - 1;

				if (pos + 3 + chunk + 1 > FTDI_MPSSE_BUF_SIZE) {
					ret = ftdi_mpsse_write(fspi->pdev,
							       buf, pos);
					if (ret)
						goto err_deassert;
					pos = 0;
				}

				buf[pos++] = clk_cmd;
				buf[pos++] = mpsse_len & 0xff;
				buf[pos++] = (mpsse_len >> 8) & 0xff;

				if (tx_data) {
					memcpy(buf + pos, tx_data, chunk);
					pos += chunk;
					tx_data += chunk;
				}

				if (rx_data) {
					ret = ftdi_spi_flush(fspi, buf, pos,
							     rx_data, chunk);
					if (ret)
						goto err_deassert;
					rx_data += chunk;
					pos = 0;
				}

				remaining -= chunk;
				msg->actual_length += chunk;
			}
		}

		if (xfer->cs_change &&
		    !list_is_last(&xfer->transfer_list, &msg->transfers)) {
			if (pos > 0) {
				ret = ftdi_mpsse_write(fspi->pdev, buf, pos);
				if (ret)
					goto err_deassert;
				pos = 0;
			}
			pos = ftdi_spi_emit_cs(fspi, buf, pos, cs, false);
			if (xfer->cs_change_delay.value)
				spi_transfer_cs_change_delay_exec(msg, xfer);
			pos = ftdi_spi_emit_cs(fspi, buf, pos, cs, true);
		}
	}

	pos = ftdi_spi_emit_cs(fspi, buf, pos, cs, false);

	if (pos > 0) {
		ret = ftdi_mpsse_write(fspi->pdev, buf, pos);
		if (ret)
			goto out;
	}

	goto out;

err_deassert:
	pos = 0;
	pos = ftdi_spi_emit_cs(fspi, buf, pos, cs, false);
	ftdi_mpsse_write(fspi->pdev, buf, pos);

out:
	ftdi_mpsse_bus_unlock(fspi->pdev);
	msg->status = ret;
	spi_finalize_current_message(host);
	return ret;
}

static int ftdi_spi_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct spi_controller *host;
	struct ftdi_spi *fspi;
	u8 cmd[3];
	int i, ret;

	host = spi_alloc_host(&pdev->dev, sizeof(*fspi));
	if (!host)
		return -ENOMEM;

	fspi = spi_controller_get_devdata(host);
	fspi->pdev = pdev;
	fspi->host = host;

	fspi->tx_buf = devm_kmalloc(&pdev->dev, FTDI_MPSSE_BUF_SIZE,
				    GFP_KERNEL);
	if (!fspi->tx_buf) {
		spi_controller_put(host);
		return -ENOMEM;
	}

	if (pdata && pdata->spi_num_cs > 0) {
		for (i = 0; i < pdata->spi_num_cs && i < FTDI_SPI_MAX_CS; i++)
			fspi->cs_pin[i] = pdata->spi_cs_pins[i];
		host->num_chipselect = pdata->spi_num_cs;
	} else {
		/* Legacy default: CS0=AD3, CS1=AD4, ..., CS4=AD7 */
		for (i = 0; i < FTDI_SPI_MAX_CS; i++)
			fspi->cs_pin[i] = 3 + i;
		host->num_chipselect = FTDI_SPI_MAX_CS;
	}

	host->bus_num = -1;
	host->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH |
			  SPI_LSB_FIRST | SPI_LOOP;
	host->bits_per_word_mask = SPI_BPW_RANGE_MASK(1, 8);
	host->max_speed_hz = FTDI_SPI_MAX_SPEED;
	host->min_speed_hz = FTDI_SPI_MIN_SPEED;
	host->setup = ftdi_spi_setup;
	host->transfer_one_message = ftdi_spi_transfer_one_message;

	/*
	 * Initialise MPSSE pin state: all CS pins as outputs, deasserted.
	 * cs_active_high is zero at this point, so all CS idle HIGH.
	 * Send SET_BITS_LOW to hardware so AD3-AD7 are actually configured
	 * as outputs (MPSSE defaults them to inputs).
	 */
	ftdi_spi_init_pins(fspi);

	cmd[0] = MPSSE_SET_BITS_LOW;
	cmd[1] = fspi->low_val;
	cmd[2] = fspi->low_dir;
	ftdi_mpsse_bus_lock(fspi->pdev);
	ret = ftdi_mpsse_write(fspi->pdev, cmd, sizeof(cmd));
	ftdi_mpsse_bus_unlock(fspi->pdev);
	if (ret) {
		spi_controller_put(host);
		return ret;
	}

	ftdi_spi_check_eeprom(fspi);

	platform_set_drvdata(pdev, host);

	ret = devm_spi_register_controller(&pdev->dev, host);
	if (ret) {
		spi_controller_put(host);
		return ret;
	}

	/* Register spidev devices for userspace SPI access */
	if (register_spidev) {
		struct spi_board_info chip = {
			/*
			 * Use "bk4" modalias - it's whitelisted in spidev
			 * driver for development/educational use (from LWN's
			 * "Linux Kernel Development" book examples).
			 */
			.modalias = "bk4",
			.max_speed_hz = FTDI_SPI_MAX_SPEED,
			.mode = SPI_MODE_0,
		};
		int i, count = 0;

		for (i = 0; i < host->num_chipselect; i++) {
			chip.chip_select = i;
			if (spi_new_device(host, &chip))
				count++;
			else
				dev_warn(&pdev->dev,
					 "failed to create spidev for CS%d\n", i);
		}
		if (count)
			dev_info(&pdev->dev,
				 "registered %d spidev device(s)\n", count);
	}

	return 0;
}

static int ftdi_spi_resume(struct device *dev)
{
	struct spi_controller *host = dev_get_drvdata(dev);
	struct ftdi_spi *fspi = spi_controller_get_devdata(host);
	u8 cmd[3];
	int ret;

	ftdi_mpsse_bus_lock(fspi->pdev);

	ftdi_spi_init_pins(fspi);

	cmd[0] = MPSSE_SET_BITS_LOW;
	cmd[1] = fspi->low_val;
	cmd[2] = fspi->low_dir;

	ret = ftdi_mpsse_write(fspi->pdev, cmd, sizeof(cmd));

	ftdi_mpsse_bus_unlock(fspi->pdev);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_spi_pm_ops, NULL, ftdi_spi_resume);

static struct platform_driver ftdi_spi_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&ftdi_spi_pm_ops),
	},
	.probe = ftdi_spi_probe,
};
module_platform_driver(ftdi_spi_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI MPSSE SPI child driver");
MODULE_LICENSE("GPL");
