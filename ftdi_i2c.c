// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE I2C child driver: an I2C adapter on the MPSSE engine of
 * FT232H / FT2232H / FT4232H, one USB round trip per byte.
 *
 * Pins (3-phase clocking): AD0 = SCL, AD1 = SDA out (DO), AD2 = SDA in
 * (DI), AD1 and AD2 wired together externally; AD7 (GPIOL3) = SCL sense
 * for clock stretching, if wired to SCL.
 * A line is released by the open-drain outputs (0x9E) on FT232H and by
 * making it an input on FT2232H/FT4232H (AN_255 2.4). There, the MPSSE
 * drives SCL and the bits it clocks out push-pull: no clock stretching.
 * Data bits are clocked by the engine; START, repeated START and STOP are
 * repeated pin writes (AN_113 2.3.1), see ftdi_i2c_set_timing().
 *
 * References: AN_108, AN_113, AN_135, AN_255, UM10204, i2c-dln2.c
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/usb.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi-i2c"

#define PIN_SCL		BIT(0)	/* AD0 */
#define PIN_SDA_OUT	BIT(1)	/* AD1 -- DO */
#define PIN_SDA_IN	BIT(2)	/* AD2 -- DI (wired to AD1) */
#define PIN_SCL_SENSE	BIT(7)	/* AD7 -- GPIOL3 (clock stretching sense) */

/*
 * Per message. Not a buffer limit, the commands are sent for every byte:
 * room for a page write with its offset bytes.
 */
#define FTDI_I2C_MAX_XFER_SIZE	4096

/* Shortest SET_BITS_LOW command measured (doc/hw-design.md), in ns */
#define FTDI_I2C_PIN_CMD_NS	80

/* Pin writes per step of START and STOP in AN_113 2.3.1 */
#define FTDI_I2C_AN113_REPS	4

/*
 * Pin writes before a released line is read: 5 us, several times the rise
 * time allowed on a standard-mode bus (UM10204, tr 1000 ns), and tBUF.
 */
#define FTDI_I2C_SETTLE_REPS	DIV_ROUND_UP(5000, FTDI_I2C_PIN_CMD_NS)

/* Bytes of a high-speed bulk packet: the engine may pause between two */
#define FTDI_I2C_PACKET		512

/*
 * SCL is held low between two messages for the delay: below the SMBus
 * clock low timeout (tTIMEOUT, 25 ms), after which SMBus slaves give up.
 */
#define FTDI_I2C_DELAY_MAX_US	10000

static unsigned int i2c_speed = 100;
module_param(i2c_speed, uint, 0444);
MODULE_PARM_DESC(i2c_speed, "I2C bus speed in kHz (10-400, default 100)");

/*
 * The MPSSE has no I2C clock stretching (AN_255 2.4). It waits for SCL
 * only while it clocks bits with adaptive clocking (0x96), which watches
 * GPIOL3 (AD7), not AD0 (AN_108 6.9): SCL must be wired to both.
 */
static bool clock_stretching;
module_param(clock_stretching, bool, 0444);
MODULE_PARM_DESC(clock_stretching,
		 "Force adaptive clocking for I2C clock stretching on every FT232H adapter (SCL wired to both AD0 and GPIOL3/AD7; auto-detected per adapter, default 0)");

/*
 * 0 = the four pin writes per step of AN_113 2.3.1, shorter than UM10204
 * asks for: for slaves with a short START window (doc/hw-design.md).
 */
static bool strict_timing = true;
module_param(strict_timing, bool, 0444);
MODULE_PARM_DESC(strict_timing,
		 "Meet the I2C-bus specification minimum times: START/STOP setup and hold, SCL high time in standard mode, i.e. 83 kHz on the bus for 84-100 kHz (default 1); 0 = four pin writes per step as in AN_113, clock at the requested rate");

static bool bus_checks;
module_param(bus_checks, bool, 0444);
MODULE_PARM_DESC(bus_checks,
		 "Run SDA loopback and SCL/SDA short checks at probe time (may desync PIO slaves, use for board bring-up only, default 0)");

static bool check_bus_free;
module_param(check_bus_free, bool, 0444);
MODULE_PARM_DESC(check_bus_free,
		 "Read SCL and SDA in a USB round trip of its own before every transfer: nothing is clocked into a bus that is not free (default 0: only at probe, after a failed transfer and after a resume)");

/* Both maps are keyed by USB path, see ftdi_i2c_map_value() */
static char *i2c_bus_nr_map;
module_param(i2c_bus_nr_map, charp, 0444);
MODULE_PARM_DESC(i2c_bus_nr_map,
		 "I2C bus number map: usbpath:nr,... per USB device (3-2:40) or per channel by interface name (3-2:1.0:40,3-2:1.1:41)");

static char *i2c_delay_us_map;
module_param(i2c_delay_us_map, charp, 0444);
MODULE_PARM_DESC(i2c_delay_us_map,
		 "I2C delay map, 0-10000 us between the messages of a transfer and after its STOP: usbpath:us,... per USB device (3-4:300) or per channel by interface name (3-4:1.1:300)");

/* debugfs i2c_stats */
struct ftdi_i2c_stats {
	atomic_t xfer_ok;
	atomic_t xfer_fail;
	atomic_t nack;		/* address byte not acknowledged */
	atomic_t data_nack;	/* data byte not acknowledged */
	atomic_t timeout;	/* USB/MPSSE response timeout */
	atomic_t scl_stretch;	/* SCL held low past the adapter timeout */
	atomic_t bus_recovery;
	atomic64_t bytes_tx;
	atomic64_t bytes_rx;
};

/*
 * buf, the pins and rec_* are used with the parent's bus lock held, the
 * timing, adaptive, ad7_unknown, bus_free and scl_held under the adapter
 * lock (transfers, speed changes, resume).
 */
struct ftdi_i2c {
	struct platform_device *pdev;
	struct i2c_adapter adapter;
	u8 *buf;		/* command assembly buffer */
	u8 own;			/* low-byte pins owned by I2C */
	u8 other_val;		/* sibling (GPIO) pins of the low byte ... */
	u8 other_dir;		/* ... merged into every SET_BITS_LOW */
	bool open_drain_hw;	/* FT232H: lines released by 0x9E */
	bool adaptive;		/* AD7 wired to SCL: adaptive clocking */
	bool ad7_unknown;	/* AD7 claimed, its wiring not known yet */
	bool bus_free;		/* the last transfer left the bus seen free */
	bool scl_held;		/* this transfer: SCL held low too long */
	bool rec_scl;		/* bus recovery: levels set */
	bool rec_sda;
	unsigned int speed_khz;	/* requested bus speed */
	u16 div;		/* clock divisor */
	unsigned int scl_khz;	/* what the divisor gives on SCL */
	bool scl_limited;	/* ... less than asked, for tHIGH */
	/* SET_BITS_LOW repetitions, see ftdi_i2c_set_timing() */
	u16 n_low;		/* tLOW, tBUF */
	u16 n_rise;		/* tr before tSU;STA/tSU;STO, strict_timing */
	u16 n_su_sta;		/* tSU;STA */
	u16 n_hd;		/* tHD;STA, tSU;STO */
	unsigned int delay_us;	/* i2c_delay_us_map */
	struct ftdi_i2c_stats stats;
};

/*
 * Pin values and directions that put SCL and SDA at the given bus levels
 * (true = released, pulled up). AD2, and AD7 when it is ours, stay inputs.
 */
static void ftdi_i2c_levels(struct ftdi_i2c *fi2c, bool scl, bool sda,
			    u8 *val, u8 *dir)
{
	*val = 0;
	*dir = 0;
	if (fi2c->open_drain_hw) {
		*dir = PIN_SCL | PIN_SDA_OUT;
		*val = (scl ? PIN_SCL : 0) | (sda ? PIN_SDA_OUT : 0);
		return;
	}
	if (!scl)
		*dir |= PIN_SCL;
	if (!sda)
		*dir |= PIN_SDA_OUT;
}

/*
 * SET_BITS_LOW with the sibling's pins of the shared low byte (AD3-AD7 can
 * be GPIOs): I2C never changes a GPIO output.
 */
static unsigned int ftdi_i2c_lo(struct ftdi_i2c *fi2c, u8 *buf,
				unsigned int pos, bool scl, bool sda)
{
	u8 val, dir;

	ftdi_i2c_levels(fi2c, scl, sda, &val, &dir);
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = val | fi2c->other_val;
	buf[pos++] = dir | fi2c->other_dir;
	return pos;
}

/* Hold bus levels for n pin writes, FTDI_I2C_PIN_CMD_NS each at least */
static unsigned int ftdi_i2c_hold(struct ftdi_i2c *fi2c, u8 *buf,
				  unsigned int pos, bool scl, bool sda,
				  unsigned int n)
{
	while (n--)
		pos = ftdi_i2c_lo(fi2c, buf, pos, scl, sda);
	return pos;
}

/*
 * Lengthen the SCL-low hold at pos so that the next len bytes, while SCL
 * is high, do not cross a packet boundary: a pause there would stretch the
 * condition instead of tLOW (see ftdi_i2c_set_timing()).
 */
static unsigned int ftdi_i2c_align(struct ftdi_i2c *fi2c, u8 *buf,
				   unsigned int pos, bool sda,
				   unsigned int len)
{
	unsigned int room = FTDI_I2C_PACKET - pos % FTDI_I2C_PACKET;

	if (len > room)
		pos = ftdi_i2c_hold(fi2c, buf, pos, false, sda,
				    DIV_ROUND_UP(room, 3));
	return pos;
}

/*
 * Take the low byte for a bus-locked section: record the idle bus in the
 * core's shadow and fetch the sibling's current pins.
 */
static void ftdi_i2c_claim_pins(struct ftdi_i2c *fi2c)
{
	u8 val, dir;

	ftdi_i2c_levels(fi2c, true, true, &val, &dir);
	ftdi_mpsse_pins_set(fi2c->pdev, 0, fi2c->own, val, dir);
	ftdi_mpsse_pins_get(fi2c->pdev, 0, &val, &dir);
	fi2c->other_val = val & ~fi2c->own;
	fi2c->other_dir = dir & ~fi2c->own;
}

static int ftdi_i2c_read_pins(struct ftdi_i2c *fi2c, u8 *pins)
{
	static const u8 cmd[] = { MPSSE_GET_BITS_LOW, MPSSE_SEND_IMMEDIATE };

	return ftdi_mpsse_xfer(fi2c->pdev, cmd, sizeof(cmd), pins, 1);
}

/*
 * The engine setup: 60 MHz clock (AN_108 6.1), 3-phase data clocking
 * (AN_108 6.3, AN_113), adaptive clocking or not (6.9), the divisor
 * (3.8.2), no loopback. At the head of every transfer: the core resets
 * the engine to its defaults after a failed transfer and at resume, at the
 * latest in the write that carries this setup.
 */
static unsigned int ftdi_i2c_engine(struct ftdi_i2c *fi2c, u8 *buf,
				    unsigned int pos)
{
	buf[pos++] = MPSSE_DISABLE_CLK_DIV5;
	buf[pos++] = MPSSE_ENABLE_3PHASE;
	buf[pos++] = fi2c->adaptive ? MPSSE_ENABLE_ADAPTIVE
				    : MPSSE_DISABLE_ADAPTIVE;
	buf[pos++] = MPSSE_SET_CLK_DIVISOR;
	buf[pos++] = fi2c->div & 0xff;
	buf[pos++] = fi2c->div >> 8;
	buf[pos++] = MPSSE_LOOPBACK_OFF;
	return pos;
}

/*
 * Send fi2c->buf[0..pos), which releases SCL, and wait until the slave that
 * stretches the clock lets it rise: adaptive clocking only, SCL read on AD7.
 */
static int ftdi_i2c_wait_scl(struct ftdi_i2c *fi2c, unsigned int pos)
{
	unsigned long timeout = jiffies + (fi2c->adapter.timeout ?: HZ);
	u8 *buf = fi2c->buf;
	u8 pins;
	int ret;

	for (;;) {
		buf[pos++] = MPSSE_GET_BITS_LOW;
		buf[pos++] = MPSSE_SEND_IMMEDIATE;
		ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, &pins, 1);
		if (ret)
			return ret;
		if (pins & PIN_SCL_SENSE)
			return 0;
		if (time_after(jiffies, timeout)) {
			fi2c->scl_held = true;
			return -ETIMEDOUT;
		}
		pos = 0;
		usleep_range(100, 200);
	}
}

/*
 * The bus read on its own, nothing clocked: a slave left in the middle of
 * a byte would take a START and the address bits as data. For a transfer
 * that does not follow one which left the bus seen free, and for every
 * transfer with check_bus_free.
 */
static int ftdi_i2c_check_free(struct ftdi_i2c *fi2c)
{
	u8 *buf = fi2c->buf;
	unsigned int pos;
	u8 pins;
	int ret;

	pos = ftdi_i2c_hold(fi2c, buf, 0, true, true, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, &pins, 1);
	if (ret)
		return ret;
	return (~pins & (PIN_SCL | PIN_SDA_IN)) ? -EBUSY : 0;
}

/*
 * START (AN_113 2.3.1) after the engine setup, left in fi2c->buf for the
 * address byte: bus free for tBUF, SDA low for tHD;STA, SCL low. The bus
 * is read again just before SDA falls: a line held low means no START.
 */
static unsigned int ftdi_i2c_start(struct ftdi_i2c *fi2c)
{
	u8 *buf = fi2c->buf;
	unsigned int pos;

	pos = ftdi_i2c_engine(fi2c, buf, 0);
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, true, fi2c->n_low);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, false, fi2c->n_hd);
	pos = ftdi_i2c_hold(fi2c, buf, pos, false, false, fi2c->n_low);
	return pos;
}

/*
 * Repeated START, left in fi2c->buf[0..*ppos) for the address byte. One
 * buffer: no USB round trip between SCL high and the fall of SDA (slaves
 * with a short START window, doc/hw-design.md). Pin writes do not wait for
 * SCL: with adaptive clocking it is read and, if a slave still held it
 * low, the sequence is redone once it has risen. strict_timing reads it
 * once it had time to rise (n_rise): seen high, only the master pulls it
 * low again, and tSU;STA counts from there. The AN_113 timing reads it
 * last, just before SDA falls.
 */
static int ftdi_i2c_repeated_start(struct ftdi_i2c *fi2c, unsigned int *ppos)
{
	u8 *buf = fi2c->buf;
	unsigned int pos, len;
	u8 pins;
	int ret;

	/* SDA released while SCL is low (tLOW) */
	pos = ftdi_i2c_hold(fi2c, buf, 0, false, true, fi2c->n_low);
	len = 3 * (fi2c->n_rise + fi2c->n_su_sta + fi2c->n_hd);
	if (fi2c->adaptive)
		len++;		/* GET_BITS_LOW */
	pos = ftdi_i2c_align(fi2c, buf, pos, true, len);

	/* SCL high: tr, tSU;STA */
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, true, fi2c->n_rise);
	if (fi2c->adaptive && fi2c->n_rise)
		buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, true, fi2c->n_su_sta);
	if (fi2c->adaptive && !fi2c->n_rise)
		buf[pos++] = MPSSE_GET_BITS_LOW;

	/* START (tHD;STA), then SCL low before the first bit (tLOW) */
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, false, fi2c->n_hd);
	pos = ftdi_i2c_hold(fi2c, buf, pos, false, false, fi2c->n_low);
	*ppos = pos;
	if (!fi2c->adaptive)
		return 0;

	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, &pins, 1);
	if (ret)
		return ret;
	*ppos = 0;
	if (pins & PIN_SCL_SENSE)
		return 0;

	/* The slave was stretching: again, SCL first */
	pos = ftdi_i2c_hold(fi2c, buf, 0, false, true, fi2c->n_low);
	pos = ftdi_i2c_lo(fi2c, buf, pos, true, true);
	ret = ftdi_i2c_wait_scl(fi2c, pos);
	if (ret)
		return ret;

	pos = ftdi_i2c_hold(fi2c, buf, 0, true, false, fi2c->n_hd);
	pos = ftdi_i2c_hold(fi2c, buf, pos, false, false, fi2c->n_low);
	*ppos = pos;
	return 0;
}

/*
 * STOP, then the bus levels in *pins once it is free (tBUF). One buffer,
 * as for the repeated START: with adaptive clocking SCL is read where the
 * repeated START reads it, and a STOP that a slave stretching the clock
 * missed is redone.
 */
static int ftdi_i2c_stop(struct ftdi_i2c *fi2c, u8 *pins)
{
	u8 *buf = fi2c->buf;
	unsigned int pos, len, n = 1;
	u8 rsp[2];
	int ret;

	/* SDA low while SCL is low (tLOW) */
	pos = ftdi_i2c_hold(fi2c, buf, 0, false, false, fi2c->n_low);
	len = 3 * (fi2c->n_rise + fi2c->n_hd);
	if (fi2c->adaptive)
		len++;		/* GET_BITS_LOW */
	pos = ftdi_i2c_align(fi2c, buf, pos, false, len);

	/* SCL high: tr, tSU;STO */
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, false, fi2c->n_rise);
	if (fi2c->adaptive && fi2c->n_rise)
		buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, false, fi2c->n_hd);
	if (fi2c->adaptive && !fi2c->n_rise)
		buf[pos++] = MPSSE_GET_BITS_LOW;
	if (fi2c->adaptive)
		n = 2;
	/* SDA rises while SCL is high: the STOP */
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, true, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, rsp, n);
	if (ret)
		return ret;
	*pins = rsp[n - 1];
	if (n == 1 || (rsp[0] & PIN_SCL_SENSE))
		return 0;

	/* The slave was stretching: SCL low, SDA low, then SCL first */
	pos = ftdi_i2c_lo(fi2c, buf, 0, false, true);
	pos = ftdi_i2c_hold(fi2c, buf, pos, false, false, fi2c->n_low);
	pos = ftdi_i2c_lo(fi2c, buf, pos, true, false);
	ret = ftdi_i2c_wait_scl(fi2c, pos);
	if (ret)
		return ret;

	pos = ftdi_i2c_hold(fi2c, buf, 0, true, true, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	return ftdi_mpsse_xfer(fi2c->pdev, buf, pos, pins, 1);
}

/*
 * Write one byte and read its ACK bit, after pos bytes queued in fi2c->buf
 * (a START). If pins is not NULL, a GET_BITS_LOW queued in them answers
 * first. *ack: 0 = ACK, 1 = NACK.
 */
static int ftdi_i2c_write_byte(struct ftdi_i2c *fi2c, unsigned int pos,
			       u8 byte, u8 *pins, u8 *ack)
{
	unsigned int n = pins ? 2 : 1;
	u8 *buf = fi2c->buf;
	u8 rsp[2];
	int ret;

	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 7;
	buf[pos++] = byte;
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, true);
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 0;
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, false);
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, rsp, n);
	if (ret)
		return ret;

	if (pins)
		*pins = rsp[0];
	*ack = rsp[n - 1] & 0x01;
	return 0;
}

/*
 * Read one byte and send ACK or NACK. One USB round trip per byte: with
 * several in one, the engine goes from the ACK to the next SDA release
 * faster than the pull-up settles SDA (bit errors from the second byte).
 */
static int ftdi_i2c_read_byte(struct ftdi_i2c *fi2c, bool ack, u8 *data)
{
	u8 *buf = fi2c->buf;
	unsigned int pos;

	pos = ftdi_i2c_lo(fi2c, buf, 0, false, true);
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 7;		/* 8 bits */
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, false);
	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 0;		/* 1 bit */
	buf[pos++] = ack ? 0x00 : 0x80;	/* ACK = 0, NACK = 1 (MSB first) */
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, true);
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	return ftdi_mpsse_xfer(fi2c->pdev, buf, pos, data, 1);
}

/* From the START to the last byte of the last message */
static int ftdi_i2c_messages(struct ftdi_i2c *fi2c, struct i2c_msg *msgs,
			     int num)
{
	unsigned int pos;
	int i, j, ret;
	u8 ack, pins;

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		u8 addr = i2c_8bit_addr_from_msg(msg);

		if (i == 0) {
			pos = ftdi_i2c_start(fi2c);
			ret = ftdi_i2c_write_byte(fi2c, pos, addr, &pins, &ack);
			if (!ret && (~pins & (PIN_SCL | PIN_SDA_IN)))
				return -EBUSY;
		} else {
			if (fi2c->delay_us)
				fsleep(fi2c->delay_us);
			ret = ftdi_i2c_repeated_start(fi2c, &pos);
			if (!ret)
				ret = ftdi_i2c_write_byte(fi2c, pos, addr,
							  NULL, &ack);
		}
		if (ret)
			return ret;
		if (ack) {
			atomic_inc(&fi2c->stats.nack);
			return -ENXIO;
		}

		for (j = 0; j < msg->len; j++) {
			if (msg->flags & I2C_M_RD) {
				ret = ftdi_i2c_read_byte(fi2c, j < msg->len - 1,
							 &msg->buf[j]);
			} else {
				ret = ftdi_i2c_write_byte(fi2c, 0, msg->buf[j],
							  NULL, &ack);
				if (!ret && ack) {
					atomic_inc(&fi2c->stats.data_nack);
					ret = -EIO;
				}
			}
			if (ret)
				return ret;
		}
	}

	return 0;
}

/*
 * After a failed transfer: STOP, then give a slave that stretches the
 * clock time to release SCL, which the MPSSE does not see (AN_255 2.4), and
 * STOP again, the first one having happened while it held SCL. Tells
 * whether SDA is held low, for a bus recovery.
 */
static bool ftdi_i2c_abort(struct ftdi_i2c *fi2c)
{
	unsigned int tries = 100;
	u8 pins;

	if (ftdi_i2c_stop(fi2c, &pins))
		return false;
	if (pins & PIN_SCL) {
		fi2c->bus_free = pins & PIN_SDA_IN;
		return !(pins & PIN_SDA_IN);
	}

	do {
		if (!--tries) {
			fi2c->scl_held = true;
			dev_warn_ratelimited(&fi2c->adapter.dev,
					     "SCL stuck low after the STOP (clock stretching timeout)\n");
			return false;
		}
		usleep_range(1000, 2000);
		if (ftdi_i2c_read_pins(fi2c, &pins))
			return false;
	} while (!(pins & PIN_SCL));

	if (ftdi_i2c_stop(fi2c, &pins))
		return false;
	fi2c->bus_free = (pins & PIN_SCL) && (pins & PIN_SDA_IN);
	return !(pins & PIN_SDA_IN);
}

/*
 * Does AD7 follow SCL? Read with the bus idle, then with SCL low: in one
 * buffer, to keep the pulse short, and SDA is never driven, so that no
 * slave sees a START. 1: yes, 0: no, -EBUSY: a slave held SCL low and AD7
 * was low too, which a wired AD7 and one held low otherwise both give.
 */
static int ftdi_i2c_detect_ad7(struct ftdi_i2c *fi2c)
{
	u8 *buf = fi2c->buf;
	u8 rsp[2]; /* pins with SCL high, with SCL low */
	unsigned int pos;
	int ret;

	pos = ftdi_i2c_lo(fi2c, buf, 0, true, true);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, true);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_lo(fi2c, buf, pos, true, true);
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, rsp, 2);
	if (ret)
		return ret;
	if (!(rsp[0] & PIN_SCL))
		return (rsp[0] & PIN_SCL_SENSE) ? 0 : -EBUSY;

	return (rsp[0] & PIN_SCL_SENSE) && !(rsp[1] & PIN_SCL_SENSE);
}

/* The outcome of the detection: adaptive clocking, or AD7 given back */
static void ftdi_i2c_set_adaptive(struct ftdi_i2c *fi2c, bool wired)
{
	struct device *dev = &fi2c->pdev->dev;

	if (wired) {
		fi2c->adaptive = true;
		dev_info(dev, "AD7<->SCL wiring detected, adaptive clocking enabled\n");
		return;
	}

	ftdi_mpsse_pins_unclaim(fi2c->pdev, 0, PIN_SCL_SENSE);
	fi2c->own &= ~PIN_SCL_SENSE;
	ftdi_i2c_claim_pins(fi2c);
	dev_info(dev, "AD7 not wired to SCL: no clock stretching (it needs AD7 wired to AD0)\n");
}

/*
 * The wiring was not known at probe (SCL held low, or a failed check):
 * the detection is done again before a transfer, with the bus seen free.
 * Bus and adapter locks held.
 */
static void ftdi_i2c_detect_late(struct ftdi_i2c *fi2c)
{
	int ret = ftdi_i2c_detect_ad7(fi2c);

	if (ret < 0)
		return;
	fi2c->ad7_unknown = false;
	ftdi_i2c_set_adaptive(fi2c, ret);
}

static void ftdi_i2c_count(struct ftdi_i2c *fi2c, struct i2c_msg *msgs,
			   int num, int ret)
{
	struct ftdi_i2c_stats *st = &fi2c->stats;
	int i;

	if (fi2c->scl_held)
		atomic_inc(&st->scl_stretch);
	if (ret) {
		atomic_inc(&st->xfer_fail);
		if (ret == -ETIMEDOUT && !fi2c->scl_held)
			atomic_inc(&st->timeout);
		return;
	}

	atomic_inc(&st->xfer_ok);
	for (i = 0; i < num; i++)
		atomic64_add(msgs[i].len, msgs[i].flags & I2C_M_RD ?
					  &st->bytes_rx : &st->bytes_tx);
}

/*
 * The bus is read before the START and after the STOP. A line held low
 * before the START fails the transfer (-EBUSY); the bus is read on its own
 * first unless the last transfer left it seen free. After the STOP, SCL
 * held low means a slave stretched the clock while the MPSSE clocked on (no
 * adaptive clocking): the bits and the STOP went past it; SDA held low
 * cannot be told from an ACK by the ACK checks. Either fails the transfer
 * (-EIO). A held SDA is followed by a bus recovery.
 */
static int ftdi_i2c_xfer(struct i2c_adapter *adapter,
			 struct i2c_msg *msgs, int num)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adapter);
	bool recover = false;
	int i, ret;
	u8 pins;

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_TEN)
			return -EAFNOSUPPORT;
		if (msgs[i].flags & I2C_M_RECV_LEN)
			return -EOPNOTSUPP;
	}

	ftdi_mpsse_bus_lock(fi2c->pdev);
	ftdi_i2c_claim_pins(fi2c);
	fi2c->scl_held = false;
	ret = 0;
	if (check_bus_free || !fi2c->bus_free)
		ret = ftdi_i2c_check_free(fi2c);
	fi2c->bus_free = false;
	if (!ret && fi2c->ad7_unknown)
		ftdi_i2c_detect_late(fi2c);
	if (!ret)
		ret = ftdi_i2c_messages(fi2c, msgs, num);
	if (ret) {
		recover = ftdi_i2c_abort(fi2c);
	} else {
		ret = ftdi_i2c_stop(fi2c, &pins);
		if (!ret && !(pins & PIN_SCL)) {
			dev_warn_ratelimited(&adapter->dev,
					     "SCL held low at the STOP: a slave stretched the clock, data unreliable\n");
			ret = -EIO;
			recover = ftdi_i2c_abort(fi2c);
		} else if (!ret && !(pins & PIN_SDA_IN)) {
			dev_warn_ratelimited(&adapter->dev,
					     "SDA held low after the STOP: data unreliable\n");
			recover = true;
			ret = -EIO;
		} else if (!ret) {
			fi2c->bus_free = true;
		}
	}
	ftdi_mpsse_bus_unlock(fi2c->pdev);

	/* prepare_recovery() takes the bus lock again */
	if (recover) {
		atomic_inc(&fi2c->stats.bus_recovery);
		i2c_recover_bus(adapter);
	}
	ftdi_i2c_count(fi2c, msgs, num, ret);
	if (fi2c->delay_us)
		fsleep(fi2c->delay_us);

	return ret ?: num;
}

static u32 ftdi_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ftdi_i2c_algo = {
	.xfer = ftdi_i2c_xfer,
	.functionality = ftdi_i2c_func,
};

/*
 * A read of no byte would leave the addressed slave driving SDA with its
 * first bit, where the STOP has to go.
 */
static const struct i2c_adapter_quirks ftdi_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN_READ,
	.max_read_len = FTDI_I2C_MAX_XFER_SIZE,
	.max_write_len = FTDI_I2C_MAX_XFER_SIZE,
};

/*
 * Callbacks of i2c_generic_scl_recovery(), between prepare_recovery() and
 * unprepare_recovery(), which hold the bus lock. With set_sda, SDA
 * follows SCL: every pulse ends with a STOP.
 */
static int ftdi_i2c_get_scl(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 pins;
	int ret;

	ret = ftdi_i2c_read_pins(fi2c, &pins);
	return ret ?: !!(pins & PIN_SCL);
}

static int ftdi_i2c_get_sda(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 pins;
	int ret;

	ret = ftdi_i2c_read_pins(fi2c, &pins);
	return ret ?: !!(pins & PIN_SDA_IN);
}

static void ftdi_i2c_set_lines(struct ftdi_i2c *fi2c)
{
	u8 cmd[3];
	int ret;

	ftdi_i2c_lo(fi2c, cmd, 0, fi2c->rec_scl, fi2c->rec_sda);
	ret = ftdi_mpsse_write(fi2c->pdev, cmd, sizeof(cmd));
	if (ret)
		dev_warn_ratelimited(&fi2c->adapter.dev,
				     "bus recovery: pin write failed: %d\n", ret);
}

static void ftdi_i2c_set_scl(struct i2c_adapter *adap, int val)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);

	fi2c->rec_scl = val;
	ftdi_i2c_set_lines(fi2c);
}

static void ftdi_i2c_set_sda(struct i2c_adapter *adap, int val)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);

	fi2c->rec_sda = val;
	ftdi_i2c_set_lines(fi2c);
}

static void ftdi_i2c_prepare_recovery(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);

	ftdi_mpsse_bus_lock(fi2c->pdev);
	ftdi_i2c_claim_pins(fi2c);
	fi2c->rec_scl = true;
	fi2c->rec_sda = true;
}

static void ftdi_i2c_unprepare_recovery(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);

	fi2c->rec_scl = true;
	fi2c->rec_sda = true;
	ftdi_i2c_set_lines(fi2c);
	ftdi_mpsse_bus_unlock(fi2c->pdev);
}

static struct i2c_bus_recovery_info ftdi_i2c_recovery = {
	.recover_bus = i2c_generic_scl_recovery,
	.get_scl = ftdi_i2c_get_scl,
	.set_scl = ftdi_i2c_set_scl,
	.get_sda = ftdi_i2c_get_sda,
	.set_sda = ftdi_i2c_set_sda,
	.prepare_recovery = ftdi_i2c_prepare_recovery,
	.unprepare_recovery = ftdi_i2c_unprepare_recovery,
};

/*
 * bus_checks: without AD1 wired to AD2 no ACK can be read. SDA low, then
 * released with SCL high: a START and a STOP for the slaves. Warns only.
 */
static int ftdi_i2c_check_sda_loopback(struct ftdi_i2c *fi2c)
{
	struct device *dev = &fi2c->pdev->dev;
	u8 *buf = fi2c->buf;
	unsigned int pos;
	u8 rsp[2];
	int ret;

	pos = ftdi_i2c_hold(fi2c, buf, 0, true, false, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_hold(fi2c, buf, pos, true, true, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, rsp, 2);
	if (ret)
		return ret;

	if (rsp[0] & PIN_SDA_IN)
		dev_warn(dev, "AD2 high while AD1 drives SDA low: I2C needs AD1 and AD2 wired together\n");
	if (!(rsp[1] & PIN_SDA_IN))
		dev_warn(dev, "SDA stuck low after release (check pull-up)\n");
	return 0;
}

/* bus_checks: SCL and SDA shorted together? Warns only. */
static int ftdi_i2c_check_bus_short(struct ftdi_i2c *fi2c)
{
	struct device *dev = &fi2c->pdev->dev;
	u8 *buf = fi2c->buf;
	unsigned int pos;
	u8 rsp[2], pins;
	int ret;

	/* SDA low, SCL released (a START), then SCL low and SDA released */
	pos = ftdi_i2c_hold(fi2c, buf, 0, true, false, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	pos = ftdi_i2c_lo(fi2c, buf, pos, false, false);
	pos = ftdi_i2c_hold(fi2c, buf, pos, false, true, FTDI_I2C_SETTLE_REPS);
	buf[pos++] = MPSSE_GET_BITS_LOW;
	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, rsp, 2);
	if (ret)
		return ret;

	if (!(rsp[0] & PIN_SCL))
		dev_warn(dev, "SCL<->SDA short: SCL low while released, SDA driven low\n");
	if (!(rsp[1] & PIN_SDA_IN))
		dev_warn(dev, "SCL<->SDA short: SDA low while released, SCL driven low\n");

	return ftdi_i2c_stop(fi2c, &pins);
}

/*
 * Clock divisor and START/STOP timing for speed_khz.
 *
 * 3-phase clocking (AN_108 6.3): the bit rate is 60 MHz / ((1 + div) * 3)
 * and SCL is high for one phase, (1 + div) / 60 MHz. div is rounded up.
 *
 * strict_timing: the minimum times of UM10204 table 11, standard mode up
 * to 100 kHz, fast mode above. tHIGH = 4.0 us in standard mode needs
 * 1 + div >= 240, i.e. 83.3 kHz at most; the 0.6 us of fast mode are
 * always met. The other times are pin writes of FTDI_I2C_PIN_CMD_NS.
 *
 * The engine may pause between two 512-byte USB packets. A pause while SCL
 * is held low only lengthens tLOW: ftdi_i2c_align() keeps the conditions of
 * the repeated START and the STOP within one packet, and the START with
 * its engine setup fits in the first one of its buffer. On a full-speed
 * link (64-byte packets) a pause can only lengthen the conditions of the
 * strict timing; the AN_113 ones fit in the first packet.
 */
static void ftdi_i2c_set_timing(struct ftdi_i2c *fi2c)
{
	unsigned int speed = fi2c->speed_khz;
	bool standard = speed <= I2C_MAX_STANDARD_MODE_FREQ / 1000;
	u16 div = DIV_ROUND_UP(60000, speed * 3) - 1;

	fi2c->scl_limited = strict_timing && standard && div < 239;
	if (strict_timing) {
		if (standard)
			div = max_t(u16, div, 239);
		fi2c->n_low = DIV_ROUND_UP(standard ? 4700 : 1300,
					   FTDI_I2C_PIN_CMD_NS);
		fi2c->n_su_sta = DIV_ROUND_UP(standard ? 4700 : 600,
					      FTDI_I2C_PIN_CMD_NS);
		fi2c->n_hd = DIV_ROUND_UP(standard ? 4000 : 600,
					  FTDI_I2C_PIN_CMD_NS);
	} else {
		fi2c->n_low = FTDI_I2C_AN113_REPS;
		fi2c->n_su_sta = FTDI_I2C_AN113_REPS;
		fi2c->n_hd = FTDI_I2C_AN113_REPS;
	}
	/*
	 * strict_timing: the rise time allowed to SCL (UM10204 table 11, tr)
	 * before the setup time of a repeated START or a STOP, which counts
	 * from the reference level. The SCL high time of the data bits
	 * counts from the release.
	 */
	fi2c->n_rise = strict_timing ?
		DIV_ROUND_UP(standard ? 1000 : 300, FTDI_I2C_PIN_CMD_NS) : 0;
	fi2c->div = div;
	fi2c->scl_khz = 60000 / ((1 + div) * 3);
}

/*
 * Engine setup, open-drain outputs and idle bus at probe, bus lock held.
 * On FT232H, SCL, SDA out and SDA in are open drain (0x07, AN_255 2.2.4),
 * merged with the open-drain GPIO lines of the sibling (AN_108 7.1); the
 * core replays the mask after an engine reset.
 */
static int ftdi_i2c_hw_init(struct ftdi_i2c *fi2c)
{
	u8 *buf = fi2c->buf;
	unsigned int pos;
	u8 od_lo, od_hi;

	ftdi_i2c_claim_pins(fi2c);
	pos = ftdi_i2c_engine(fi2c, buf, 0);
	if (fi2c->open_drain_hw) {
		ftdi_mpsse_od_set(fi2c->pdev, 0,
				  PIN_SCL | PIN_SDA_OUT | PIN_SDA_IN,
				  PIN_SCL | PIN_SDA_OUT | PIN_SDA_IN);
		ftdi_mpsse_od_get(fi2c->pdev, &od_lo, &od_hi);
		buf[pos++] = MPSSE_DRIVE_ZERO_ONLY;
		buf[pos++] = od_lo;
		buf[pos++] = od_hi;
	}
	pos = ftdi_i2c_lo(fi2c, buf, pos, true, true);

	return ftdi_mpsse_write(fi2c->pdev, buf, pos);
}

/*
 * AD7 sensing SCL, for adaptive clocking: forced by clock_stretching=1 or
 * detected. FT232H only: without open-drain outputs the MPSSE drives SCL
 * high while it clocks, against a slave holding it low (AN_255 2.4). Bus
 * lock held.
 */
static int ftdi_i2c_setup_ad7(struct ftdi_i2c *fi2c)
{
	struct device *dev = &fi2c->pdev->dev;
	int ret;

	if (!fi2c->open_drain_hw) {
		if (clock_stretching)
			dev_warn(dev, "clock_stretching=1 ignored: SCL is not open drain on this chip\n");
		return 0;
	}

	if (ftdi_mpsse_pins_claim(fi2c->pdev, 0, PIN_SCL_SENSE)) {
		if (clock_stretching) {
			dev_err(dev, "clock_stretching=1 needs AD7, which is in use as a GPIO line\n");
			return -EBUSY;
		}
		dev_info(dev, "AD7 is in use as a GPIO line: not probed for SCL wiring, no clock stretching\n");
		return 0;
	}

	fi2c->own |= PIN_SCL_SENSE;
	ftdi_i2c_claim_pins(fi2c);
	if (clock_stretching) {
		fi2c->adaptive = true;
		return 0;
	}
	ret = ftdi_i2c_detect_ad7(fi2c);
	if (ret < 0) {
		/* AD7 stays claimed until the outcome is known */
		fi2c->ad7_unknown = true;
		if (ret == -EBUSY)
			dev_info(dev, "SCL held low: AD7<->SCL wiring checked once the bus is free\n");
		else
			dev_info(dev, "AD7<->SCL wiring not checked (%d): at the first transfer\n",
				 ret);
		return 0;
	}
	ftdi_i2c_set_adaptive(fi2c, ret);
	return 0;
}

/*
 * "Pull down I/O pins in USB suspend" (default EEPROM configuration,
 * DS_FT232H 7.2, DS_FT2232H and DS_FT4232H 7.1) pulls SCL and SDA low
 * against their pull-ups while the chip sleeps.
 */
static void ftdi_i2c_check_eeprom(struct ftdi_i2c *fi2c)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_get_eeprom(fi2c->pdev);

	if (ee && ee->pulldown)
		dev_warn(&fi2c->pdev->dev,
			 "EEPROM: I/O pins pulled down in USB suspend: SCL and SDA are held low while the chip sleeps\n");
}

/*
 * Value of key in a "key:value,..." map: 0 if found, -ENOENT if not,
 * -EINVAL if the value is not a number from 0 to max. An entry is split
 * at its last ':': a USB interface name has one of its own ("3-2:1.1:41").
 */
static int ftdi_i2c_lookup_map(const char *map, const char *key,
			       unsigned int max, unsigned int *val)
{
	size_t keylen = strlen(key);
	const char *p = map;

	while (*p) {
		size_t len, sep, vlen;
		unsigned int v;
		char tmp[12];

		p += strspn(p, ", ");
		if (!*p)
			break;

		len = strcspn(p, ",");
		for (sep = len; sep && p[sep - 1] != ':'; sep--)
			;
		if (!sep || sep - 1 != keylen || strncmp(p, key, keylen)) {
			p += len;
			continue;
		}

		vlen = len - sep;
		if (!vlen || vlen >= sizeof(tmp))
			return -EINVAL;
		memcpy(tmp, p + sep, vlen);
		tmp[vlen] = '\0';
		if (kstrtouint(tmp, 10, &v) || v > max)
			return -EINVAL;
		*val = v;
		return 0;
	}

	return -ENOENT;
}

/*
 * This adapter's value in a map, as ftdi_i2c_lookup_map(). The keys are
 * USB paths: the interface of the channel ("3-2:1.1" is channel B of 3-2),
 * looked up first, or the device ("3-2"), for every channel of it.
 * *by_intf tells which one matched.
 */
static int ftdi_i2c_map_value(struct ftdi_i2c *fi2c, const char *name,
			      const char *map, unsigned int max,
			      unsigned int *val, bool *by_intf)
{
	struct usb_device *udev = ftdi_mpsse_get_udev(fi2c->pdev);
	struct device *dev = &fi2c->pdev->dev;
	const char *key = dev_name(dev->parent);
	int ret;

	*by_intf = false;
	if (!map)
		return -ENOENT;

	ret = ftdi_i2c_lookup_map(map, key, max, val);
	*by_intf = ret != -ENOENT;
	if (!*by_intf && udev) {
		key = dev_name(&udev->dev);
		ret = ftdi_i2c_lookup_map(map, key, max, val);
	}
	if (ret == -EINVAL)
		dev_warn(dev, "%s: the value for %s is not a number from 0 to %u, ignored\n",
			 name, key, max);
	return ret;
}

/* AD7 is a GPIO line again */
static void ftdi_i2c_release_ad7(void *data)
{
	struct ftdi_i2c *fi2c = data;

	ftdi_mpsse_bus_lock(fi2c->pdev);
	ftdi_mpsse_pins_unclaim(fi2c->pdev, 0, PIN_SCL_SENSE);
	ftdi_mpsse_bus_unlock(fi2c->pdev);
}

static void ftdi_i2c_del_adapter(void *data)
{
	i2c_del_adapter(data);
}

/* With the number i2c_bus_nr_map gives it (i2c-dev minors), or any */
static int ftdi_i2c_add_adapter(struct ftdi_i2c *fi2c)
{
	struct device *dev = &fi2c->pdev->dev;
	unsigned int nr;
	bool by_intf;
	int ret;

	if (ftdi_i2c_map_value(fi2c, "i2c_bus_nr_map", i2c_bus_nr_map,
			       MINORMASK, &nr, &by_intf)) {
		ret = devm_i2c_add_adapter(dev, &fi2c->adapter);
		if (ret)
			return dev_err_probe(dev, ret, "failed to add I2C adapter\n");
		return 0;
	}

	fi2c->adapter.nr = nr;
	ret = i2c_add_numbered_adapter(&fi2c->adapter);
	/* One number, two channels of the same chip? */
	if (ret == -EBUSY && !by_intf)
		dev_err(dev, "i2c_bus_nr_map: an entry for a USB device is for all its I2C channels; give this one its own with the interface name, e.g. %s:%u\n",
			dev_name(dev->parent), nr + 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add I2C adapter nr %u\n", nr);

	return devm_add_action_or_reset(dev, ftdi_i2c_del_adapter,
					&fi2c->adapter);
}

static int ftdi_i2c_stats_show(struct seq_file *s, void *unused)
{
	struct ftdi_i2c *fi2c = s->private;
	struct ftdi_i2c_stats *st = &fi2c->stats;

	seq_printf(s, "xfer_ok=%u\n", (unsigned int)atomic_read(&st->xfer_ok));
	seq_printf(s, "xfer_fail=%u\n", (unsigned int)atomic_read(&st->xfer_fail));
	seq_printf(s, "nack=%u\n", (unsigned int)atomic_read(&st->nack));
	seq_printf(s, "data_nack=%u\n", (unsigned int)atomic_read(&st->data_nack));
	seq_printf(s, "timeout=%u\n", (unsigned int)atomic_read(&st->timeout));
	seq_printf(s, "scl_stretch=%u\n", (unsigned int)atomic_read(&st->scl_stretch));
	seq_printf(s, "bus_recovery=%u\n", (unsigned int)atomic_read(&st->bus_recovery));
	seq_printf(s, "bytes_tx=%lld\n", atomic64_read(&st->bytes_tx));
	seq_printf(s, "bytes_rx=%lld\n", atomic64_read(&st->bytes_rx));
	return 0;
}

/* Writing 0 resets the counters */
static ssize_t ftdi_i2c_stats_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct ftdi_i2c *fi2c = s->private;
	struct ftdi_i2c_stats *st = &fi2c->stats;
	unsigned int val;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 10, &val);
	if (ret)
		return ret;
	if (val)
		return -EINVAL;

	atomic_set(&st->xfer_ok, 0);
	atomic_set(&st->xfer_fail, 0);
	atomic_set(&st->nack, 0);
	atomic_set(&st->data_nack, 0);
	atomic_set(&st->timeout, 0);
	atomic_set(&st->scl_stretch, 0);
	atomic_set(&st->bus_recovery, 0);
	atomic64_set(&st->bytes_tx, 0);
	atomic64_set(&st->bytes_rx, 0);
	return count;
}

DEFINE_SHOW_STORE_ATTRIBUTE(ftdi_i2c_stats);

static ssize_t i2c_speed_khz_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(fi2c->speed_khz));
}

/* Taken by the next transfer, see ftdi_i2c_engine() */
static ssize_t i2c_speed_khz_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);
	unsigned int speed;
	int ret;

	ret = kstrtouint(buf, 10, &speed);
	if (ret)
		return ret;
	if (speed < 10 || speed > 400)
		return -EINVAL;

	i2c_lock_bus(&fi2c->adapter, I2C_LOCK_ROOT_ADAPTER);
	WRITE_ONCE(fi2c->speed_khz, speed);
	ftdi_i2c_set_timing(fi2c);
	i2c_unlock_bus(&fi2c->adapter, I2C_LOCK_ROOT_ADAPTER);

	dev_info(dev, "I2C speed changed to %u kHz\n", speed);
	return count;
}

static DEVICE_ATTR_RW(i2c_speed_khz);

static struct attribute *ftdi_i2c_attrs[] = {
	&dev_attr_i2c_speed_khz.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ftdi_i2c);

static int ftdi_i2c_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct ftdi_i2c *fi2c;
	bool by_intf;
	int ret;

	if (!pdata)
		return -EINVAL;

	fi2c = devm_kzalloc(dev, sizeof(*fi2c), GFP_KERNEL);
	if (!fi2c)
		return -ENOMEM;

	fi2c->buf = devm_kmalloc(dev, FTDI_MPSSE_BUF_SIZE, GFP_KERNEL);
	if (!fi2c->buf)
		return -ENOMEM;

	fi2c->pdev = pdev;
	fi2c->own = PIN_SCL | PIN_SDA_OUT | PIN_SDA_IN;
	fi2c->open_drain_hw = pdata->has_drive_zero;
	fi2c->speed_khz = clamp_val(i2c_speed, 10, 400);
	ftdi_i2c_set_timing(fi2c);
	ftdi_i2c_map_value(fi2c, "i2c_delay_us_map", i2c_delay_us_map,
			   FTDI_I2C_DELAY_MAX_US, &fi2c->delay_us, &by_intf);

	ret = devm_add_action(dev, ftdi_i2c_release_ad7, fi2c);
	if (ret)
		return ret;

	ftdi_mpsse_bus_lock(pdev);
	ret = ftdi_i2c_hw_init(fi2c);
	if (!ret)
		ret = ftdi_i2c_setup_ad7(fi2c);
	ftdi_mpsse_bus_unlock(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "MPSSE I2C init failed\n");

	/* Not by default: the pin toggling may desynchronize slaves */
	if (bus_checks) {
		ftdi_mpsse_bus_lock(pdev);
		ftdi_i2c_claim_pins(fi2c);
		ftdi_i2c_check_sda_loopback(fi2c);
		ftdi_i2c_check_bus_short(fi2c);
		ftdi_mpsse_bus_unlock(pdev);
		usleep_range(5000, 10000);
	}

	/* Margin for the slaves after the SCL pulse of the AD7 detection */
	usleep_range(2000, 3000);

	ftdi_i2c_check_eeprom(fi2c);

	fi2c->adapter.owner = THIS_MODULE;
	fi2c->adapter.class = I2C_CLASS_HWMON;
	fi2c->adapter.algo = &ftdi_i2c_algo;
	fi2c->adapter.quirks = &ftdi_i2c_quirks;
	fi2c->adapter.bus_recovery_info = &ftdi_i2c_recovery;
	fi2c->adapter.dev.parent = dev;
	i2c_set_adapdata(&fi2c->adapter, fi2c);
	snprintf(fi2c->adapter.name, sizeof(fi2c->adapter.name),
		 "FTDI MPSSE I2C (%s)", dev_name(dev->parent));

	platform_set_drvdata(pdev, fi2c);

	ret = ftdi_i2c_add_adapter(fi2c);
	if (ret)
		return ret;

	/* Removed with the adapter's directory, by i2c_del_adapter() */
	debugfs_create_file("i2c_stats", 0600, fi2c->adapter.debugfs, fi2c,
			    &ftdi_i2c_stats_fops);

	dev_info(dev, "FTDI MPSSE I2C adapter at %u kHz%s%s\n",
		 fi2c->speed_khz,
		 fi2c->open_drain_hw ? ", HW open-drain" : "",
		 fi2c->adaptive ? ", clock stretching (AD7)" : "");
	if (!strict_timing)
		dev_info(dev, "strict_timing=0: START/STOP and SCL high times as in AN_113, below the I2C-bus specification minimums\n");
	else if (fi2c->scl_limited)
		dev_info(dev, "SCL runs at %u kHz: the standard-mode SCL high time (4.0 us) with 3-phase clocking\n",
			 fi2c->scl_khz);
	if (check_bus_free)
		dev_info(dev, "check_bus_free=1: SCL and SDA are read before every transfer, one more USB round trip each\n");
	if (fi2c->delay_us)
		dev_info(dev, "inter-phase delay %u us (i2c_delay_us_map)\n",
			 fi2c->delay_us);

	return 0;
}

/*
 * What the bus went through while the system slept is not known: the next
 * transfer reads it before its START.
 */
static int ftdi_i2c_resume(struct device *dev)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);

	i2c_lock_bus(&fi2c->adapter, I2C_LOCK_ROOT_ADAPTER);
	fi2c->bus_free = false;
	i2c_unlock_bus(&fi2c->adapter, I2C_LOCK_ROOT_ADAPTER);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_i2c_pm_ops, NULL, ftdi_i2c_resume);

static struct platform_driver ftdi_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.dev_groups = ftdi_i2c_groups,
		.pm = pm_sleep_ptr(&ftdi_i2c_pm_ops),
	},
	.probe = ftdi_i2c_probe,
};
module_platform_driver(ftdi_i2c_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI MPSSE I2C child driver");
MODULE_LICENSE("GPL");
