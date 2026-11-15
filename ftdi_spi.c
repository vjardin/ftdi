// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE SPI child driver
 *
 * SPI controller using the MPSSE engine on FT232H / FT2232H / FT4232H.
 * Pins:  AD0 = SCK, AD1 = MOSI (DO), AD2 = MISO (DI), AD3 = CS0,
 *        AD4-AD7 available as additional CS lines.
 * Clock: 60 MHz / ((1 + divisor) * 2) with DIV5 disabled (AN_108 3.8.2),
 *        max 30 MHz, min ~458 Hz.
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
 * Opt-in: the SPI core's new_device (CONFIG_SPI_DYNAMIC) is the standard
 * way to instantiate a client, spidev included, and a chip select taken
 * here is unavailable to it.
 */
static bool register_spidev;
module_param(register_spidev, bool, 0444);
MODULE_PARM_DESC(register_spidev,
		 "Register a spidev device on every chip select (default: N; else use the controller's new_device)");

#define FTDI_SPI_BASE_CLK	60000000UL
#define FTDI_SPI_MAX_SPEED	(FTDI_SPI_BASE_CLK / 2)
/* Rounded up, so that the slowest clock is never faster than advertised */
#define FTDI_SPI_MIN_SPEED	DIV_ROUND_UP(FTDI_SPI_BASE_CLK, 2 * (1 + 0xffff))

/* Room kept free in the command buffer for the trailing commands */
#define FTDI_SPI_TAIL		8

/* MPSSE low byte (AD0-AD7) */
#define PIN_SCK		BIT(0)
#define PIN_MOSI	BIT(1)
#define PIN_MISO	BIT(2)

/*
 * low_dir/low_val (the SPI pins of the low byte), cs_active_high and
 * tx_buf are used with the parent's bus lock held.
 */
struct ftdi_spi {
	struct platform_device *pdev;
	struct spi_controller *host;
	u8 *tx_buf;		/* command assembly buffer */
	u8 cs_pin[FTDI_SPI_CS_MAX];	/* AD pin number for each CS */
	u8 cs_active_high;	/* bitmask: CS lines with SPI_CS_HIGH */
	u8 own_mask;		/* low-byte pins owned by SPI */
	u8 low_dir;
	u8 low_val;
	unsigned int max_rx;	/* response bytes per write (chip RX FIFO) */
	int cs_kept;		/* CS left asserted by cs_change, or -1 */
	bool cs_on;		/* CS of the message in progress is asserted */
	u32 bit_ns;		/* SCK period of the transfer in progress */
	u64 busy_ns;		/* clocking queued since the last response */
};

static u16 ftdi_spi_divisor(u32 speed_hz)
{
	u32 div;

	if (speed_hz >= FTDI_SPI_MAX_SPEED)
		return 0;

	div = DIV_ROUND_UP(FTDI_SPI_BASE_CLK, 2 * speed_hz) - 1;
	return min_t(u32, div, 0xffff);
}

static u32 ftdi_spi_rate(u16 div)
{
	return FTDI_SPI_BASE_CLK / (2 * (1 + (u32)div));
}

/* SCK period of a divisor in ns, rounded up */
static u32 ftdi_spi_bit_ns(u16 div)
{
	return DIV_ROUND_UP((1 + (u32)div) * 100, 3);
}

/*
 * SCK level while data is shifted. The MPSSE only does SPI modes 0 and 2
 * (AN_114 1.2, AN_108 2.2/3.3). CPHA = 1 runs half a cycle ahead: SCK goes
 * active after CS assert, data is shifted with the opcode pair of that
 * level, SCK goes idle after CS release. From AN_108, not scope-verified.
 */
static bool ftdi_spi_sck_active(struct spi_device *spi)
{
	return !(spi->mode & SPI_CPOL) == !!(spi->mode & SPI_CPHA);
}

static void ftdi_spi_set_sck(struct ftdi_spi *fspi, bool high)
{
	if (high)
		fspi->low_val |= PIN_SCK;
	else
		fspi->low_val &= ~PIN_SCK;
}

static void ftdi_spi_set_cs_level(struct ftdi_spi *fspi, unsigned int cs,
				  bool active)
{
	u8 cs_bit = BIT(fspi->cs_pin[cs]);
	bool high = (fspi->cs_active_high & BIT(cs)) ? active : !active;

	if (high)
		fspi->low_val |= cs_bit;
	else
		fspi->low_val &= ~cs_bit;
}

static void ftdi_spi_init_pins(struct ftdi_spi *fspi, bool cpol)
{
	unsigned int num_cs = fspi->host->num_chipselect;
	unsigned int i;

	fspi->low_dir = PIN_SCK | PIN_MOSI;
	fspi->low_val = cpol ? PIN_SCK : 0;
	for (i = 0; i < num_cs; i++) {
		fspi->low_dir |= BIT(fspi->cs_pin[i]);
		ftdi_spi_set_cs_level(fspi, i, false);
	}
}

/*
 * SET_BITS_LOW is built from the core's pin shadow: the low byte is shared
 * with the ftdi-gpio sibling, whose outputs on the free AD pins must stay.
 */
static unsigned int ftdi_spi_emit_pins(struct ftdi_spi *fspi, u8 *buf,
				       unsigned int pos)
{
	u8 val, dir;

	ftdi_mpsse_pins_set(fspi->pdev, 0, fspi->own_mask,
			    fspi->low_val, fspi->low_dir);
	ftdi_mpsse_pins_get(fspi->pdev, 0, &val, &dir);
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = val;
	buf[pos++] = dir;
	return pos;
}

/*
 * The chip select of @spi released, then SCK back to its idle level, in
 * that order: with CPHA = 1 SCK is still active, and its edge must not
 * reach a selected slave.
 */
static unsigned int ftdi_spi_release_cs(struct ftdi_spi *fspi,
					struct spi_device *spi,
					unsigned int pos)
{
	ftdi_spi_set_cs_level(fspi, spi_get_chipselect(spi, 0), false);
	pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, pos);
	ftdi_spi_set_sck(fspi, spi->mode & SPI_CPOL);
	return ftdi_spi_emit_pins(fspi, fspi->tx_buf, pos);
}

/*
 * Byte-mode clock opcode. The pair follows the level SCK has when the
 * data is shifted (AN_108 2.2), see ftdi_spi_sck_active():
 *
 * SPI mode 0 (CPOL=0, CPHA=0), SCK low:  out on -ve, in on +ve
 * SPI mode 1 (CPOL=0, CPHA=1), SCK high: out on +ve, in on -ve
 * SPI mode 2 (CPOL=1, CPHA=0), SCK high: out on +ve, in on -ve
 * SPI mode 3 (CPOL=1, CPHA=1), SCK low:  out on -ve, in on +ve
 */
static u8 ftdi_spi_clk_cmd(struct spi_device *spi, bool rx)
{
	bool m03 = (spi->mode & SPI_MODE_X_MASK) == SPI_MODE_0 ||
		   (spi->mode & SPI_MODE_X_MASK) == SPI_MODE_3;
	u8 cmd;

	/*
	 * A transfer without tx_buf shifts zeros (include/linux/spi/spi.h):
	 * reads are full duplex.
	 */
	if (rx)
		cmd = m03 ? MPSSE_CLK_BYTES_INOUT_PVE_NVE
			  : MPSSE_CLK_BYTES_INOUT_NVE_PVE;
	else
		cmd = m03 ? MPSSE_CLK_BYTES_OUT_NVE : MPSSE_CLK_BYTES_OUT_PVE;

	if (spi->mode & SPI_LSB_FIRST)
		cmd |= MPSSE_CLK_LSB_FIRST;

	return cmd;
}

/* Clocking time queued since the last response, for the core's timeouts */
static unsigned int ftdi_spi_busy_ms(struct ftdi_spi *fspi)
{
	return min_t(u64, DIV_ROUND_UP_ULL(fspi->busy_ns, NSEC_PER_MSEC),
		     UINT_MAX);
}

static int ftdi_spi_flush_out(struct ftdi_spi *fspi, unsigned int *pos)
{
	int ret = 0;

	if (*pos)
		ret = ftdi_mpsse_xfer_busy(fspi->pdev, fspi->tx_buf, *pos,
					   NULL, 0, ftdi_spi_busy_ms(fspi));
	*pos = 0;
	return ret;
}

/* The response comes last: once it is here, the engine has run everything */
static int ftdi_spi_flush_in(struct ftdi_spi *fspi, unsigned int *pos,
			     u8 *rx, unsigned int rx_len)
{
	int ret;

	fspi->tx_buf[(*pos)++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer_busy(fspi->pdev, fspi->tx_buf, *pos, rx, rx_len,
				   ftdi_spi_busy_ms(fspi));
	*pos = 0;
	fspi->busy_ns = 0;
	return ret;
}

static int ftdi_spi_room(struct ftdi_spi *fspi, unsigned int *pos,
			 unsigned int n)
{
	if (*pos + n + FTDI_SPI_TAIL <= FTDI_MPSSE_BUF_SIZE)
		return 0;
	return ftdi_spi_flush_out(fspi, pos);
}

/*
 * Wait for the engine: a write returns when the commands are in the chip's
 * buffer (1-4 KiB), not when the data is on the wire. A GET_BITS_LOW
 * queued behind them (AN_108 3.6.3) answers when the engine gets there;
 * pin writes alone are done within a microsecond and need no barrier.
 */
static int ftdi_spi_sync(struct ftdi_spi *fspi, unsigned int *pos)
{
	u8 pins;

	if (!fspi->busy_ns)
		return ftdi_spi_flush_out(fspi, pos);

	fspi->tx_buf[(*pos)++] = MPSSE_GET_BITS_LOW;
	return ftdi_spi_flush_in(fspi, pos, &pins, 1);
}

/* A delay of the SPI core, counted from the moment the wire got there */
static int ftdi_spi_delay(struct ftdi_spi *fspi, unsigned int *pos,
			  struct spi_delay *delay, struct spi_transfer *xfer)
{
	int ret;

	/* Nothing to wait for, or a unit that cannot be converted */
	if (spi_delay_to_ns(delay, xfer) <= 0)
		return 0;

	ret = ftdi_spi_sync(fspi, pos);
	if (ret)
		return ret;
	return spi_delay_exec(delay, xfer);
}

static int ftdi_spi_put_pins(struct ftdi_spi *fspi, unsigned int *pos)
{
	int ret = ftdi_spi_room(fspi, pos, 3);

	if (ret)
		return ret;
	*pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, *pos);
	return 0;
}

/*
 * Mirrors spi_set_cs() of the SPI core: cs_setup after the assertion,
 * cs_hold before the release, cs_inactive after it. With CPHA = 1 SCK
 * moves after the chip select does, so that the device takes neither edge
 * as a sampling edge.
 */
static int ftdi_spi_set_cs(struct ftdi_spi *fspi, struct spi_device *spi,
			   unsigned int *pos, bool on)
{
	unsigned int cs = spi_get_chipselect(spi, 0);
	bool cpha = spi->mode & SPI_CPHA;
	int ret;

	if (on == fspi->cs_on)
		return 0;

	if (!on) {
		ret = ftdi_spi_delay(fspi, pos, &spi->cs_hold, NULL);
		if (ret)
			return ret;
	}

	ftdi_spi_set_cs_level(fspi, cs, on);
	ret = ftdi_spi_put_pins(fspi, pos);
	if (ret)
		return ret;
	fspi->cs_on = on;

	if (on)
		ret = ftdi_spi_delay(fspi, pos, &spi->cs_setup, NULL);
	if (!ret && cpha) {
		ftdi_spi_set_sck(fspi, on ? ftdi_spi_sck_active(spi)
					  : !!(spi->mode & SPI_CPOL));
		ret = ftdi_spi_put_pins(fspi, pos);
	}
	if (!ret && !on)
		ret = ftdi_spi_delay(fspi, pos, &spi->cs_inactive, NULL);
	return ret;
}

/*
 * "Pull down I/O pins in USB suspend" (default EEPROM configuration,
 * DS_FT232H 7.2, DS_FT2232H and DS_FT4232H 7.1) pulls the active-low
 * chip selects low: every slave is selected while the chip sleeps.
 */
static void ftdi_spi_check_eeprom(struct ftdi_spi *fspi)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_get_eeprom(fspi->pdev);

	if (ee && ee->pulldown)
		dev_warn(&fspi->pdev->dev,
			 "EEPROM: I/O pins pulled down in USB suspend: every CS# is asserted while the chip sleeps\n");
}

static int ftdi_spi_setup(struct spi_device *spi)
{
	struct ftdi_spi *fspi = spi_controller_get_devdata(spi->controller);
	unsigned int cs = spi_get_chipselect(spi, 0);
	unsigned int pos = 0;
	u8 old_high, old_val;
	int old_kept;
	int ret;

	if (cs >= spi->controller->num_chipselect)
		return -EINVAL;

	if (spi->mode & SPI_CPHA)
		dev_notice_once(&spi->dev,
				"SPI mode %lu uses CPHA=1, which the MPSSE does not provide (AN_114): emulated with SCK moved half a cycle ahead, not verified on hardware\n",
				(unsigned long)(spi->mode & SPI_MODE_X_MASK));

	ftdi_mpsse_bus_lock(fspi->pdev);
	old_high = fspi->cs_active_high;
	old_val = fspi->low_val;
	old_kept = fspi->cs_kept;
	if (spi->mode & SPI_CS_HIGH)
		fspi->cs_active_high |= BIT(cs);
	else
		fspi->cs_active_high &= ~BIT(cs);

	if (fspi->cs_kept == cs) {
		fspi->cs_kept = -1;
		pos = ftdi_spi_release_cs(fspi, spi, pos);
	} else {
		/* SPI_CS_HIGH may have changed the idle level of this CS */
		ftdi_spi_set_cs_level(fspi, cs, false);
		pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, pos);
	}
	ret = ftdi_spi_flush_out(fspi, &pos);
	if (ret) {
		/*
		 * spi_setup() fails: the device keeps its old mode. The new
		 * pins may be out (the core's recovery replays the shadow):
		 * the old ones go out again, best effort.
		 */
		fspi->cs_active_high = old_high;
		fspi->low_val = old_val;
		fspi->cs_kept = old_kept;
		pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, 0);
		ftdi_spi_flush_out(fspi, &pos);
	}
	ftdi_mpsse_bus_unlock(fspi->pdev);

	return ret;
}

/*
 * A chip select kept asserted by cs_change on the last transfer of a
 * message is only released by the next message or spi_setup(): release
 * it when its device goes away.
 */
static void ftdi_spi_cleanup(struct spi_device *spi)
{
	struct ftdi_spi *fspi = spi_controller_get_devdata(spi->controller);
	unsigned int pos;

	ftdi_mpsse_bus_lock(fspi->pdev);
	if (fspi->cs_kept == spi_get_chipselect(spi, 0)) {
		fspi->cs_kept = -1;
		pos = ftdi_spi_release_cs(fspi, spi, 0);
		ftdi_spi_flush_out(fspi, &pos);
	}
	ftdi_mpsse_bus_unlock(fspi->pdev);
}

/* Controller gone (unbind, unplug): every chip select deasserted */
static void ftdi_spi_release_bus(void *data)
{
	struct ftdi_spi *fspi = data;
	unsigned int pos = 0;

	ftdi_mpsse_bus_lock(fspi->pdev);
	fspi->cs_kept = -1;
	ftdi_spi_init_pins(fspi, false);
	pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, pos);
	ftdi_spi_flush_out(fspi, &pos);
	ftdi_mpsse_bus_unlock(fspi->pdev);
}

/* Bit mode (1-7 bits per word), one word per command */
static int ftdi_spi_xfer_bits(struct ftdi_spi *fspi, struct spi_device *spi,
			      struct spi_transfer *xfer, unsigned int *pos,
			      u8 clk_cmd)
{
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int bpw = xfer->bits_per_word;
	bool lsb = spi->mode & SPI_LSB_FIRST;
	u8 mask = (1 << bpw) - 1;
	unsigned int i;
	int ret;

	for (i = 0; i < xfer->len; i++) {
		u8 word = tx ? tx[i] & mask : 0;

		if (i) {
			ret = ftdi_spi_delay(fspi, pos, &xfer->word_delay,
					     xfer);
			if (ret)
				return ret;
		}

		ret = ftdi_spi_room(fspi, pos, 3);
		if (ret)
			return ret;
		fspi->busy_ns += (u64)bpw * fspi->bit_ns;

		/*
		 * SPI words are right-justified; MSB-first bit commands
		 * shift out from bit 7 (AN_108 3.3.3, 3.3.4), so align the word.
		 */
		fspi->tx_buf[(*pos)++] = clk_cmd | MPSSE_CLK_BIT_MODE;
		fspi->tx_buf[(*pos)++] = bpw - 1;
		fspi->tx_buf[(*pos)++] = lsb ? word : word << (8 - bpw);

		if (rx) {
			u8 b;

			ret = ftdi_spi_flush_in(fspi, pos, &b, 1);
			if (ret)
				return ret;
			/*
			 * MSB-first reads shift in from bit 0, LSB-first ones
			 * from bit 7 down (AN_108 3.3.7 / 3.4.7).
			 */
			rx[i] = (lsb ? b >> (8 - bpw) : b) & mask;
		}
	}
	return 0;
}

/*
 * Byte mode, chunked so that a response always fits the chip RX FIFO,
 * one word per command when a delay is wanted between the words
 */
static int ftdi_spi_xfer_bytes(struct ftdi_spi *fspi,
			       struct spi_transfer *xfer, unsigned int *pos,
			       u8 clk_cmd)
{
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int max_chunk = FTDI_MPSSE_BUF_SIZE - 3 - FTDI_SPI_TAIL;
	unsigned int done = 0;
	int ret;

	if (rx)
		max_chunk = min(max_chunk, fspi->max_rx);
	if (spi_delay_to_ns(&xfer->word_delay, xfer) > 0)
		max_chunk = 1;

	while (done < xfer->len) {
		unsigned int chunk = min(xfer->len - done, max_chunk);
		u16 mpsse_len = chunk - 1;

		if (done) {
			ret = ftdi_spi_delay(fspi, pos, &xfer->word_delay,
					     xfer);
			if (ret)
				return ret;
		}

		ret = ftdi_spi_room(fspi, pos, 3 + chunk);
		if (ret)
			return ret;
		fspi->busy_ns += (u64)chunk * 8 * fspi->bit_ns;

		fspi->tx_buf[(*pos)++] = clk_cmd;
		fspi->tx_buf[(*pos)++] = mpsse_len & 0xff;
		fspi->tx_buf[(*pos)++] = (mpsse_len >> 8) & 0xff;
		if (tx)
			memcpy(fspi->tx_buf + *pos, tx + done, chunk);
		else
			memset(fspi->tx_buf + *pos, 0, chunk);
		*pos += chunk;

		if (rx) {
			ret = ftdi_spi_flush_in(fspi, pos, rx + done, chunk);
			if (ret)
				return ret;
		}
		done += chunk;
	}
	return 0;
}

/*
 * Mirrors spi_transfer_one_message() of the SPI core (chip select, cs_off,
 * cs_change, delays); each delay runs on the host, after ftdi_spi_sync().
 */
static int ftdi_spi_transfer_one_message(struct spi_controller *host,
					 struct spi_message *msg)
{
	struct ftdi_spi *fspi = spi_controller_get_devdata(host);
	struct spi_device *spi = msg->spi;
	unsigned int cs = spi_get_chipselect(spi, 0);
	struct spi_transfer *xfer;
	u8 *buf = fspi->tx_buf;
	unsigned int pos = 0;
	bool keep_cs = false;
	bool kept;
	u16 div, cur_div;
	int ret;

	ftdi_mpsse_bus_lock(fspi->pdev);

	/* The engine setup, with every message (see ftdi_mpsse_xfer()) */
	buf[pos++] = MPSSE_DISABLE_CLK_DIV5;
	buf[pos++] = MPSSE_DISABLE_ADAPTIVE;
	buf[pos++] = MPSSE_DISABLE_3PHASE;

	cur_div = ftdi_spi_divisor(spi->max_speed_hz);
	fspi->bit_ns = ftdi_spi_bit_ns(cur_div);
	buf[pos++] = MPSSE_SET_CLK_DIVISOR;
	buf[pos++] = cur_div & 0xff;
	buf[pos++] = (cur_div >> 8) & 0xff;

	/*
	 * CS kept asserted by the previous message (cs_change in its last
	 * transfer): this device's stays as it is, SCK included; another
	 * device's is released before SCK moves.
	 */
	kept = fspi->cs_kept == cs;
	if (fspi->cs_kept >= 0 && !kept) {
		ftdi_spi_set_cs_level(fspi, fspi->cs_kept, false);
		pos = ftdi_spi_emit_pins(fspi, buf, pos);
	}
	fspi->cs_kept = -1;

	ftdi_spi_init_pins(fspi, spi->mode & SPI_CPOL);
	if (kept) {
		ftdi_spi_set_cs_level(fspi, cs, true);
		if (spi->mode & SPI_CPHA)
			ftdi_spi_set_sck(fspi, ftdi_spi_sck_active(spi));
	}
	fspi->cs_on = kept;
	pos = ftdi_spi_emit_pins(fspi, buf, pos);

	buf[pos++] = (spi->mode & SPI_LOOP) ? MPSSE_LOOPBACK_ON
					     : MPSSE_LOOPBACK_OFF;

	xfer = list_first_entry(&msg->transfers, struct spi_transfer,
				transfer_list);
	ret = ftdi_spi_set_cs(fspi, spi, &pos, !xfer->cs_off);
	if (ret)
		goto err;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		bool last = list_is_last(&xfer->transfer_list,
					 &msg->transfers);
		u8 clk_cmd;

		if (xfer->speed_hz) {
			div = ftdi_spi_divisor(xfer->speed_hz);
			if (div != cur_div) {
				ret = ftdi_spi_room(fspi, &pos, 3);
				if (ret)
					goto err;
				buf[pos++] = MPSSE_SET_CLK_DIVISOR;
				buf[pos++] = div & 0xff;
				buf[pos++] = (div >> 8) & 0xff;
				cur_div = div;
				fspi->bit_ns = ftdi_spi_bit_ns(cur_div);
			}
		}
		/* What delays in clock cycles are computed from */
		xfer->effective_speed_hz = ftdi_spi_rate(cur_div);

		clk_cmd = ftdi_spi_clk_cmd(spi, !!xfer->rx_buf);

		if (!xfer->tx_buf && !xfer->rx_buf)
			ret = 0;	/* nothing to clock: a delay, a CS change */
		else if (xfer->bits_per_word < 8)
			ret = ftdi_spi_xfer_bits(fspi, spi, xfer, &pos, clk_cmd);
		else
			ret = ftdi_spi_xfer_bytes(fspi, xfer, &pos, clk_cmd);
		if (ret)
			goto err;
		msg->actual_length += xfer->len;

		ret = ftdi_spi_delay(fspi, &pos, &xfer->delay, xfer);
		if (ret)
			goto err;

		if (xfer->cs_change) {
			struct spi_transfer *next;

			if (last) {
				keep_cs = true;
				continue;
			}
			next = list_next_entry(xfer, transfer_list);
			if (!xfer->cs_off)
				ret = ftdi_spi_set_cs(fspi, spi, &pos, false);
			/* The chip select must be up before the time counts */
			if (!ret)
				ret = ftdi_spi_sync(fspi, &pos);
			if (ret)
				goto err;
			spi_transfer_cs_change_delay_exec(msg, xfer);
			if (!next->cs_off)
				ret = ftdi_spi_set_cs(fspi, spi, &pos, true);
		} else if (!last &&
			   xfer->cs_off != list_next_entry(xfer, transfer_list)->cs_off) {
			ret = ftdi_spi_set_cs(fspi, spi, &pos, xfer->cs_off);
		}
		if (ret)
			goto err;
	}

	if (keep_cs && fspi->cs_on)
		fspi->cs_kept = cs;
	else
		ret = ftdi_spi_set_cs(fspi, spi, &pos, false);

	/* The message is over when it is on the wire, not in the chip */
	if (!ret)
		ret = ftdi_spi_sync(fspi, &pos);
	if (!ret)
		goto out;

err:
	/*
	 * After a failed transfer the core has reset the engine and replayed
	 * the pin shadow: release the chip select.
	 */
	fspi->busy_ns = 0;
	fspi->cs_on = false;
	fspi->cs_kept = -1;
	pos = ftdi_spi_release_cs(fspi, spi, 0);
	ftdi_spi_flush_out(fspi, &pos);

out:
	ftdi_mpsse_bus_unlock(fspi->pdev);
	msg->status = ret;
	spi_finalize_current_message(host);
	/* The error is in msg->status: returning it would only log twice */
	return 0;
}

/* spidev, bound with driver_override instead of a borrowed compatible */
static void ftdi_spi_add_spidev(struct ftdi_spi *fspi)
{
	struct spi_controller *host = fspi->host;
	int i, count = 0;

	for (i = 0; i < host->num_chipselect; i++) {
		struct spi_device *spi = spi_alloc_device(host);

		if (!spi)
			break;
		spi_set_chipselect(spi, 0, i);
		spi->cs_index_mask = BIT(0);
		spi->max_speed_hz = FTDI_SPI_MAX_SPEED;
		spi->mode = SPI_MODE_0;
		strscpy(spi->modalias, "spidev", sizeof(spi->modalias));
		if (device_set_driver_override(&spi->dev, "spidev") ||
		    spi_add_device(spi)) {
			spi_dev_put(spi);
			dev_warn(&fspi->pdev->dev,
				 "failed to create spidev for CS%d\n", i);
			continue;
		}
		count++;
	}
	if (count)
		dev_info(&fspi->pdev->dev,
			 "registered %d spidev device(s)\n", count);
}

static int ftdi_spi_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct spi_controller *host;
	struct ftdi_spi *fspi;
	unsigned int pos = 0;
	int i, ret;

	if (!pdata || !pdata->spi_num_cs || pdata->spi_num_cs > FTDI_SPI_CS_MAX)
		return -EINVAL;

	host = devm_spi_alloc_host(&pdev->dev, sizeof(*fspi));
	if (!host)
		return -ENOMEM;

	fspi = spi_controller_get_devdata(host);
	fspi->pdev = pdev;
	fspi->host = host;
	fspi->max_rx = pdata->fifo_size;
	fspi->cs_kept = -1;

	fspi->tx_buf = devm_kmalloc(&pdev->dev, FTDI_MPSSE_BUF_SIZE,
				    GFP_KERNEL);
	if (!fspi->tx_buf)
		return -ENOMEM;

	fspi->own_mask = PIN_SCK | PIN_MOSI | PIN_MISO;
	for (i = 0; i < pdata->spi_num_cs; i++) {
		fspi->cs_pin[i] = pdata->spi_cs_pins[i];
		fspi->own_mask |= BIT(fspi->cs_pin[i]);
	}
	host->num_chipselect = pdata->spi_num_cs;

	host->bus_num = -1;
	host->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH |
			  SPI_LSB_FIRST | SPI_LOOP;
	host->bits_per_word_mask = SPI_BPW_RANGE_MASK(1, 8);
	host->max_speed_hz = FTDI_SPI_MAX_SPEED;
	host->min_speed_hz = FTDI_SPI_MIN_SPEED;
	host->setup = ftdi_spi_setup;
	host->cleanup = ftdi_spi_cleanup;
	host->transfer_one_message = ftdi_spi_transfer_one_message;

	/* The MPSSE defaults the CS pins to inputs: drive them deasserted */
	ftdi_mpsse_bus_lock(pdev);
	ftdi_spi_init_pins(fspi, false);
	pos = ftdi_spi_emit_pins(fspi, fspi->tx_buf, pos);
	ret = ftdi_spi_flush_out(fspi, &pos);
	ftdi_mpsse_bus_unlock(pdev);
	if (ret)
		return ret;

	ftdi_spi_check_eeprom(fspi);

	platform_set_drvdata(pdev, host);

	/* Runs after the controller is unregistered (devres order) */
	ret = devm_add_action_or_reset(&pdev->dev, ftdi_spi_release_bus, fspi);
	if (ret)
		return ret;

	ret = devm_spi_register_controller(&pdev->dev, host);
	if (ret)
		return ret;

	if (register_spidev)
		ftdi_spi_add_spidev(fspi);

	return 0;
}

static int ftdi_spi_suspend(struct device *dev)
{
	struct spi_controller *host = dev_get_drvdata(dev);

	return spi_controller_suspend(host);
}

static int ftdi_spi_resume(struct device *dev)
{
	struct spi_controller *host = dev_get_drvdata(dev);

	/* The core has re-entered MPSSE and replayed the pin shadow */
	return spi_controller_resume(host);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_spi_pm_ops, ftdi_spi_suspend,
				ftdi_spi_resume);

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
