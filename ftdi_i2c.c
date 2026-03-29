// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE I2C child driver
 *
 * I2C adapter using the MPSSE engine on FT232H / FT2232H / FT4232H.
 * Pin mapping (MPSSE mode with 3-phase clocking):
 *   AD0 = SCL,  AD1 = SDA out (DO),  AD2 = SDA in (DI)
 *   AD1 and AD2 wired together externally.
 *
 * Open-drain handling:
 *   FT232H:  hardware open-drain via 0x9E (DRIVE_ZERO_ONLY)
 *   FT2232H/FT4232H: software open-drain emulation -- drive low for '0', tristate for '1'
 *
 * Command batching: one USB round-trip per I2C byte (write and read).
 *
 * References: AN_108, AN_113, AN_135, i2c-dln2.c
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi-i2c"

/* MPSSE low-byte pin definitions for I2C */
#define PIN_SCL		BIT(0)	/* AD0 */
#define PIN_SDA_OUT	BIT(1)	/* AD1 -- DO */
#define PIN_SDA_IN	BIT(2)	/* AD2 -- DI (wired to AD1) */

/* Forward declarations for bus recovery / clock stretching */
static int ftdi_i2c_get_scl(struct i2c_adapter *adap);
static int ftdi_i2c_get_sda(struct i2c_adapter *adap);

/*
 * Maximum bytes per I2C message. With per-byte USB flush, the MPSSE
 * command buffer usage is constant (12 bytes) regardless of message
 * length, so there is no buffer-related limit. Set generously to
 * support full page writes including the address/offset byte(s).
 */
#define FTDI_I2C_MAX_XFER_SIZE	4096

static unsigned int i2c_speed = 100;
module_param(i2c_speed, uint, 0444);
MODULE_PARM_DESC(i2c_speed, "I2C bus speed in kHz (10-400, default 100)");

/*
 * MPSSE adaptive clocking (0x96) monitors GPIOL3 (AD7), not the SCL
 * line (AD0).  Per AN_255 §2.4: "the MPSSE does not automatically
 * support clock stretching for I2C."  To use this feature, the user
 * must wire SCL to both AD0 and GPIOL3 (AD7) externally.
 */
static bool clock_stretching;
module_param(clock_stretching, bool, 0444);
MODULE_PARM_DESC(clock_stretching,
		 "Enable adaptive clocking for I2C clock stretching "
		 "(requires SCL wired to both AD0 and GPIOL3/AD7, default 0)");

static char *i2c_bus_nr_map;
module_param(i2c_bus_nr_map, charp, 0444);
MODULE_PARM_DESC(i2c_bus_nr_map,
		 "Per-device I2C bus number map: devpath:nr,... (e.g. 3-2:40,3-3:210)");

static char *i2c_delay_us_map;
module_param(i2c_delay_us_map, charp, 0444);
MODULE_PARM_DESC(i2c_delay_us_map,
		 "Per-device I2C delay map: devpath:us,... (e.g. 3-4:300,3-5:300)");

/*
 * Per-adapter state.  buf, rsp, and pin state are only accessed from
 * the I2C algorithm xfer callback, which is serialised by the I2C core's
 * bus_lock mutex -- no additional locking required here.
 */
struct ftdi_i2c {
	struct platform_device *pdev;
	struct i2c_adapter adapter;
	u8 *buf;		/* command assembly buffer */
	u8 *rsp;		/* response buffer for batched xfers */
	u8 sda_hi_val;		/* pin value to release SDA */
	u8 sda_hi_dir;		/* pin direction when SDA released */
	bool open_drain_hw;	/* FT232H hardware open-drain active */
	unsigned int speed_khz;	/* current I2C bus speed in kHz */
	unsigned int delay_us;	/* per-device inter-phase delay */

	/* Transfer statistics (sysfs-readable) */
	atomic_t xfer_ok;	/* successful transfers (I2C_RDWR calls) */
	atomic_t xfer_fail;	/* failed transfers */
	atomic_t nack_count;	/* NACK on address byte */
	atomic_t data_nack;	/* NACK on data byte (mid-write) */
	atomic_t scl_stretch;	/* SCL stuck low timeout (>100 ms) */
	atomic_t bus_recovery;	/* i2c_recover_bus() called */
	atomic_t timeout_count;	/* USB/MPSSE level timeout */
	atomic64_t bytes_tx;	/* total bytes written to slave */
	atomic64_t bytes_rx;	/* total bytes read from slave */
};

/*
 * All pin helpers use precomputed sda_hi_val / sda_hi_dir so there are
 * no branches on the hot path.
 *
 * Software open-drain (FT2232H, FT4232H):
 *   sda_hi_val = 0x00       (value irrelevant, SDA becomes input)
 *   sda_hi_dir = PIN_SCL    (only SCL is output; SDA direction = input)
 *
 * Hardware open-drain (FT232H, 0x9E active):
 *   sda_hi_val = PIN_SDA_OUT  (writing 1 releases the open-drain pin)
 *   sda_hi_dir = PIN_SCL | PIN_SDA_OUT  (both remain outputs)
 */

static unsigned int ftdi_i2c_idle_pins(struct ftdi_i2c *fi2c, u8 *buf,
				       unsigned int pos)
{
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL | fi2c->sda_hi_val;
	buf[pos++] = fi2c->sda_hi_dir;
	return pos;
}

/*
 * I2C START condition: SDA goes low while SCL is high.
 *
 * Per AN_113 §2.3.1, the sequence has 3 phases, each repeated 4x:
 *   1. SDA=1, SCL=1 (idle — ensure bus is released before START)
 *   2. SDA=0, SCL=1 (START setup — SDA falls while SCL high)
 *   3. SDA=0, SCL=0 (hold — prepare for first data byte)
 *
 * The 4x repetition guarantees I2C setup/hold times
 * (tSU;STA = 600 ns standard, 260 ns fast mode).
 */
static unsigned int ftdi_i2c_start(struct ftdi_i2c *fi2c, u8 *buf,
				   unsigned int pos)
{
	int i;

	/* Phase 1: ensure bus idle — SDA=1, SCL=1 (per AN_113) */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = PIN_SCL | fi2c->sda_hi_val;
		buf[pos++] = fi2c->sda_hi_dir;
	}

	/* Phase 2: SDA low while SCL high — START setup */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = PIN_SCL;			/* SCL=1, SDA=0 */
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	/* Phase 3: SCL low — hold START */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = 0x00;			/* SCL=0, SDA=0 */
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	return pos;
}

/*
 * Repeated START between messages.  Unlike start(), this must first
 * release SDA (which is still driven low from the previous byte's
 * "reclaim" step) so that the HIGH-to-LOW SDA transition while SCL
 * is high constitutes a valid START condition.
 */
/*
 * Flush SDA release + SCL raise to USB so the pull-up has time
 * to bring SDA high before the START condition.
 */
static int ftdi_i2c_repeated_start(struct ftdi_i2c *fi2c,
				   u8 *buf, unsigned int *ppos)
{
	unsigned int pos = 0;
	int i;

	/*
	 * Repeated START: all phases in ONE buffer (no intermediate USB
	 * flush).  The MPSSE processes them back-to-back (~500ns from
	 * SCL HIGH to SDA fall).  A separate USB flush would create a
	 * ~250µs gap that PIO slaves cannot detect.
	 *
	 * See doc/hw-design.md "Repeated START and PIO Slave Timing".
	 */

	/* Phase 1: release SDA (4x for rise time) */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = fi2c->sda_hi_val;
		buf[pos++] = fi2c->sda_hi_dir;
	}

	/* Phase 1b: raise SCL (4x for rise time) */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = PIN_SCL | fi2c->sda_hi_val;
		buf[pos++] = fi2c->sda_hi_dir;
	}

	/* Phase 2: SDA low while SCL high — START (4x per AN_113) */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = PIN_SCL;
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	/* Phase 3: SCL low — hold (4x) */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = 0x00;
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	*ppos = pos;
	return 0;
}

/*
 * I2C STOP condition: SDA goes high while SCL is high.
 * Each phase repeated 4x per AN_113/AN_255 for timing compliance.
 */
static unsigned int ftdi_i2c_stop(struct ftdi_i2c *fi2c, u8 *buf,
				  unsigned int pos)
{
	int i;

	/* SDA low, SCL low — setup */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = 0x00;
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	/* SCL high, SDA still low — STOP setup */
	for (i = 0; i < 4; i++) {
		buf[pos++] = MPSSE_SET_BITS_LOW;
		buf[pos++] = PIN_SCL;
		buf[pos++] = PIN_SCL | PIN_SDA_OUT;
	}

	/* SDA high while SCL high — STOP, then idle */
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = PIN_SCL | fi2c->sda_hi_val;
	buf[pos++] = fi2c->sda_hi_dir;

	return pos;
}

/*
 * Append commands for one I2C write byte to buf[pos].
 * Generates 11 CMD bytes, expects 1 RSP byte (ACK bit).
 */
/*
 * Write one byte, read ACK, flush to USB.
 * One USB round-trip per byte — matches libmpsse behaviour.
 * Returns 0 on success; *ack = 0 (ACK) or 1 (NACK).
 */
static int ftdi_i2c_write_byte_flush(struct ftdi_i2c *fi2c,
				     u8 *buf, unsigned int *ppos,
				     u8 byte, u8 *ack)
{
	unsigned int pos = *ppos;
	u8 rsp;
	int ret;

	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 7;
	buf[pos++] = byte;

	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = fi2c->sda_hi_val;
	buf[pos++] = fi2c->sda_hi_dir;

	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 0;

	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;

	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, &rsp, 1);
	if (ret)
		return ret;

	*ack = rsp & 0x01;
	dev_dbg(&fi2c->pdev->dev, "write_byte 0x%02x rsp=0x%02x ack=%d\n",
		byte, rsp, *ack);
	*ppos = 0;
	return 0;
}

/*
 * Read one byte, send ACK/NACK, flush to USB.
 * One USB round-trip per byte — matches write path behaviour and gives
 * the open-drain pull-up time to settle SDA between bytes.
 *
 * Batching multiple read bytes per USB round-trip (like libmpsse does)
 * is faster but unreliable: the MPSSE engine transitions from ACK to
 * the next SDA release faster than the open-drain pull-up can settle,
 * causing bit errors on the second and subsequent bytes.  libmpsse has
 * the same issue (~14% verification failures on 256B write+read).
 *
 * Returns 0 on success; *data = the byte read from the slave.
 */
static int ftdi_i2c_read_byte_flush(struct ftdi_i2c *fi2c,
				    u8 *buf, unsigned int *ppos,
				    bool ack, u8 *data)
{
	unsigned int pos = *ppos;
	u8 rsp;
	int ret;

	/* Release SDA for reading */
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = fi2c->sda_hi_val;
	buf[pos++] = fi2c->sda_hi_dir;

	/* Clock in 8 bits on +ve edge */
	buf[pos++] = MPSSE_CLK_BITS_IN_PVE;
	buf[pos++] = 7;		/* 8 bits */

	/* Reclaim SDA, clock out ACK or NACK */
	buf[pos++] = MPSSE_SET_BITS_LOW;
	buf[pos++] = 0x00;
	buf[pos++] = PIN_SCL | PIN_SDA_OUT;

	buf[pos++] = MPSSE_CLK_BITS_OUT_NVE;
	buf[pos++] = 0;		/* 1 bit */
	buf[pos++] = ack ? 0x00 : 0x80;	/* ACK=0, NACK=1 (MSB) */

	buf[pos++] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, buf, pos, &rsp, 1);
	if (ret)
		return ret;

	*data = rsp;
	*ppos = 0;
	return 0;
}

static int ftdi_i2c_xfer(struct i2c_adapter *adapter,
			 struct i2c_msg *msgs, int num)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adapter);
	u8 *buf = fi2c->buf;
	u8 *rsp = fi2c->rsp;
	unsigned int pos;
	int i, j, ret;

	ftdi_mpsse_bus_lock(fi2c->pdev);

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		u8 ack;

		pos = 0;

		if (i == 0) {
			pos = ftdi_i2c_start(fi2c, buf, pos);
		} else {
			ret = ftdi_i2c_repeated_start(fi2c, buf, &pos);
			if (ret)
				goto err_stop;
		}

		/* Send address byte — one USB round-trip */
		ret = ftdi_i2c_write_byte_flush(fi2c, buf, &pos,
				i2c_8bit_addr_from_msg(msg), &ack);
		if (ret)
			goto err_stop;
		if (ack) {
			atomic_inc(&fi2c->nack_count);
			ret = -ENXIO;
			goto err_stop;
		}

		if (msg->flags & I2C_M_RD) {
			/* Read data — one USB round-trip per byte */
			for (j = 0; j < msg->len; j++) {
				bool last = (j == msg->len - 1);

				ret = ftdi_i2c_read_byte_flush(fi2c, buf,
						&pos, !last, &msg->buf[j]);
				if (ret)
					goto err_stop;
			}
		} else {
			/* Write data — one USB round-trip per byte */
			for (j = 0; j < msg->len; j++) {
				ret = ftdi_i2c_write_byte_flush(fi2c, buf,
						&pos, msg->buf[j], &ack);
				if (ret)
					goto err_stop;
				if (ack) {
					atomic_inc(&fi2c->data_nack);
					ret = -EPIPE;
					goto err_stop;
				}
			}
		}

		if (fi2c->delay_us)
			udelay(fi2c->delay_us);
	}

	pos = 0;
	pos = ftdi_i2c_stop(fi2c, buf, pos);
	ret = ftdi_mpsse_write(fi2c->pdev, buf, pos);
	if (ret)
		goto unlock;

	/* Count successful transfer and bytes */
	atomic_inc(&fi2c->xfer_ok);
	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			atomic64_add(msgs[i].len, &fi2c->bytes_rx);
		else
			atomic64_add(msgs[i].len, &fi2c->bytes_tx);
	}

	ftdi_mpsse_bus_unlock(fi2c->pdev);
	return num;

err_stop:
	atomic_inc(&fi2c->xfer_fail);
	if (ret == -ETIMEDOUT)
		atomic_inc(&fi2c->timeout_count);
	pos = 0;
	pos = ftdi_i2c_stop(fi2c, buf, pos);
	ftdi_mpsse_write(fi2c->pdev, buf, pos);

	/*
	 * If the transfer failed, the slave may be clock-stretching
	 * (holding SCL low) or holding SDA low (stuck bus).
	 *
	 * The MPSSE does not natively detect I2C clock stretching
	 * (AN_255 §2.4).  Poll SCL via GET_BITS_LOW to give the slave
	 * time to finish processing before attempting recovery.
	 *
	 * If SCL is free but SDA is stuck, do full bus recovery:
	 * toggle SCL up to 9 times until the slave releases SDA.
	 *
	 * The bus_lock is released first because i2c_recover_bus()
	 * re-acquires it via prepare_recovery().
	 */
	ftdi_mpsse_bus_unlock(fi2c->pdev);

	/* Wait for clock stretching to complete (up to 100 ms) */
	{
		int timeout = 100;

		while (ftdi_i2c_get_scl(adapter) == 0 && timeout-- > 0)
			usleep_range(1000, 2000);
		if (timeout <= 0) {
			atomic_inc(&fi2c->scl_stretch);
			dev_warn(&adapter->dev,
				 "SCL stuck low after %d ms (clock stretching timeout)\n",
				 100);
		}
	}

	/* If SDA is stuck low, do full 9-clock recovery */
	if (ftdi_i2c_get_sda(adapter) == 0) {
		atomic_inc(&fi2c->bus_recovery);
		i2c_recover_bus(adapter);
	}

	return ret;

unlock:
	ftdi_mpsse_bus_unlock(fi2c->pdev);
	return ret;
}

static u32 ftdi_i2c_func(struct i2c_adapter *adapter)
{
	/*
	 * 10-bit addressing: the MPSSE can generate the two-byte address
	 * waveform and the Linux I2C core decomposes 10-bit transactions
	 * into proper message sequences.  Not yet validated with a real
	 * 10-bit slave (Zephyr DW I2C target needs a patch for 10-bit).
	 */
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ftdi_i2c_algo = {
	.xfer = ftdi_i2c_xfer,
	.functionality = ftdi_i2c_func,
};

static const struct i2c_adapter_quirks ftdi_i2c_quirks = {
	.max_read_len = FTDI_I2C_MAX_XFER_SIZE,
	.max_write_len = FTDI_I2C_MAX_XFER_SIZE,
};

/*
 * If a transfer is interrupted mid-byte (USB error, device reset), a
 * slave can hold SDA low indefinitely, locking the bus.  The standard
 * recovery is to toggle SCL up to 9 times until the slave releases SDA,
 * then send a STOP condition.
 *
 * We implement the get_scl/set_scl/get_sda callbacks using MPSSE
 * GET_BITS_LOW / SET_BITS_LOW commands and let i2c_generic_scl_recovery()
 * drive the standard recovery algorithm.
 */

static int ftdi_i2c_get_scl(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 cmd[2];
	u8 val;
	int ret;

	cmd[0] = MPSSE_GET_BITS_LOW;
	cmd[1] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, cmd, sizeof(cmd), &val, 1);
	if (ret)
		return ret;

	return !!(val & PIN_SCL);
}

static void ftdi_i2c_set_scl(struct i2c_adapter *adap, int val)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 cmd[3];
	int ret;

	cmd[0] = MPSSE_SET_BITS_LOW;
	cmd[1] = (val ? PIN_SCL : 0x00) | fi2c->sda_hi_val;
	cmd[2] = fi2c->sda_hi_dir;

	ret = ftdi_mpsse_write(fi2c->pdev, cmd, sizeof(cmd));
	if (ret)
		dev_warn(&adap->dev, "recovery set_scl failed: %d\n", ret);
}

static int ftdi_i2c_get_sda(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 cmd[2];
	u8 val;
	int ret;

	cmd[0] = MPSSE_GET_BITS_LOW;
	cmd[1] = MPSSE_SEND_IMMEDIATE;
	ret = ftdi_mpsse_xfer(fi2c->pdev, cmd, sizeof(cmd), &val, 1);
	if (ret)
		return ret;

	/* Read AD2 (SDA_IN) -- wired to AD1 externally */
	return !!(val & PIN_SDA_IN);
}

static void ftdi_i2c_prepare_recovery(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);

	ftdi_mpsse_bus_lock(fi2c->pdev);
}

static void ftdi_i2c_unprepare_recovery(struct i2c_adapter *adap)
{
	struct ftdi_i2c *fi2c = i2c_get_adapdata(adap);
	u8 buf[3];
	unsigned int pos = 0;
	int ret;

	pos = ftdi_i2c_idle_pins(fi2c, buf, pos);
	ret = ftdi_mpsse_write(fi2c->pdev, buf, pos);
	if (ret)
		dev_warn(&adap->dev, "recovery unprepare failed: %d\n", ret);

	ftdi_mpsse_bus_unlock(fi2c->pdev);
}

static struct i2c_bus_recovery_info ftdi_i2c_recovery = {
	.recover_bus = i2c_generic_scl_recovery,
	.get_scl = ftdi_i2c_get_scl,
	.set_scl = ftdi_i2c_set_scl,
	.get_sda = ftdi_i2c_get_sda,
	.prepare_recovery = ftdi_i2c_prepare_recovery,
	.unprepare_recovery = ftdi_i2c_unprepare_recovery,
};

static int ftdi_i2c_hw_init(struct ftdi_i2c *fi2c)
{
	u8 *buf = fi2c->buf;
	unsigned int pos = 0;
	unsigned int speed;
	u16 div;

	/* Disable CLK/5 -> 60 MHz base clock */
	buf[pos++] = MPSSE_DISABLE_CLK_DIV5;

	/* Enable 3-phase data clocking (required for I2C per AN_113) */
	buf[pos++] = MPSSE_ENABLE_3PHASE;

	/*
	 * Adaptive clocking for clock stretching.  Monitors GPIOL3 (AD7),
	 * not SCL (AD0) — requires external wiring of SCL to AD7.
	 */
	buf[pos++] = clock_stretching ? MPSSE_ENABLE_ADAPTIVE
				      : MPSSE_DISABLE_ADAPTIVE;

	/*
	 * Clock divisor.  With 3-phase clocking the effective I2C rate is:
	 *   freq = 60 MHz / ((1 + div) * 3)
	 * Round up the divisor so the actual clock never exceeds the target.
	 */
	speed = clamp_val(fi2c->speed_khz, 10, 400);
	div = DIV_ROUND_UP(60000, speed * 3) - 1;
	buf[pos++] = MPSSE_SET_CLK_DIVISOR;
	buf[pos++] = div & 0xff;
	buf[pos++] = (div >> 8) & 0xff;

	buf[pos++] = MPSSE_LOOPBACK_OFF;

	/* Hardware open-drain (FT232H only): low-byte SCL + SDA out + SDA in */
	if (fi2c->open_drain_hw) {
		buf[pos++] = MPSSE_DRIVE_ZERO_ONLY;
		buf[pos++] = PIN_SCL | PIN_SDA_OUT | PIN_SDA_IN; /* 0x07 per AN_255 */
		buf[pos++] = 0x00;			/* high byte mask */
	}

	pos = ftdi_i2c_idle_pins(fi2c, buf, pos);

	return ftdi_mpsse_write(fi2c->pdev, buf, pos);
}

static void ftdi_i2c_check_eeprom(struct platform_device *pdev)
{
	const struct ftdi_eeprom *ee;
	u8 drive_ma;
	bool schmitt, slow_slew;

	ee = ftdi_mpsse_get_eeprom(pdev);
	if (!ee)
		return;

	if (ftdi_mpsse_get_eeprom_drive(pdev, &drive_ma, &schmitt, &slow_slew))
		return;

	if (!schmitt)
		dev_warn(&pdev->dev,
			 "EEPROM: Schmitt trigger OFF on data bus, I2C requires Schmitt for reliable operation\n");

	if (drive_ma <= 4)
		dev_notice(&pdev->dev,
			   "EEPROM: data bus drive current is %u mA, consider 8 or 16 mA for I2C with pull-ups\n",
			   drive_ma);

	if (!slow_slew)
		dev_notice(&pdev->dev,
			   "EEPROM: fast slew rate on data bus, slow slew recommended for I2C\n");

	if (ee->pulldown)
		dev_notice(&pdev->dev,
			   "EEPROM: suspend_pull_downs enabled, may conflict with I2C pull-up resistors\n");
}

/*
 * Look up a USB device path in a "devpath:value,..." map string.
 * Format: "devpath1:val1,devpath2:val2,..."
 * Example: "3-2:40,3-3:210,3-4:220"
 * Returns the value if found, or -1 if not found.
 */
static long ftdi_i2c_lookup_map(const char *map, const char *devpath)
{
	const char *p;
	size_t pathlen;

	if (!map || !devpath)
		return -1;

	pathlen = strlen(devpath);
	p = map;

	while (*p) {
		/* Skip leading whitespace/commas */
		while (*p == ',' || *p == ' ')
			p++;
		if (!*p)
			break;

		/* Match "devpath:" prefix */
		if (!strncmp(p, devpath, pathlen) && p[pathlen] == ':') {
			long val;

			if (!kstrtol(p + pathlen + 1, 10, &val))
				return val;
		}

		/* Skip to next entry */
		while (*p && *p != ',')
			p++;
	}

	return -1;
}

static void ftdi_i2c_del_adapter(void *data)
{
	i2c_del_adapter(data);
}

/*
 * Sysfs attributes for transfer statistics.
 * Read via: cat /sys/devices/.../i2c_stats
 * Reset via: echo 0 > /sys/devices/.../i2c_stats
 */
static ssize_t i2c_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"xfer_ok=%d\n"
		"xfer_fail=%d\n"
		"nack=%d\n"
		"data_nack=%d\n"
		"timeout=%d\n"
		"scl_stretch=%d\n"
		"bus_recovery=%d\n"
		"bytes_tx=%lld\n"
		"bytes_rx=%lld\n",
		atomic_read(&fi2c->xfer_ok),
		atomic_read(&fi2c->xfer_fail),
		atomic_read(&fi2c->nack_count),
		atomic_read(&fi2c->data_nack),
		atomic_read(&fi2c->timeout_count),
		atomic_read(&fi2c->scl_stretch),
		atomic_read(&fi2c->bus_recovery),
		(long long)atomic64_read(&fi2c->bytes_tx),
		(long long)atomic64_read(&fi2c->bytes_rx));
}

static ssize_t i2c_stats_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);

	atomic_set(&fi2c->xfer_ok, 0);
	atomic_set(&fi2c->xfer_fail, 0);
	atomic_set(&fi2c->nack_count, 0);
	atomic_set(&fi2c->data_nack, 0);
	atomic_set(&fi2c->timeout_count, 0);
	atomic_set(&fi2c->scl_stretch, 0);
	atomic_set(&fi2c->bus_recovery, 0);
	atomic64_set(&fi2c->bytes_tx, 0);
	atomic64_set(&fi2c->bytes_rx, 0);

	return count;
}

static DEVICE_ATTR_RW(i2c_stats);

/*
 * Runtime I2C bus speed control.
 * Read: current speed in kHz.
 * Write: new speed in kHz (10-400), re-programs the MPSSE clock divisor live.
 */
static ssize_t i2c_speed_khz_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", fi2c->speed_khz);
}

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

	speed = clamp_val(speed, 10, 400);
	fi2c->speed_khz = speed;

	ftdi_mpsse_bus_lock(fi2c->pdev);
	ret = ftdi_i2c_hw_init(fi2c);
	ftdi_mpsse_bus_unlock(fi2c->pdev);
	if (ret)
		return ret;

	dev_info(dev, "I2C speed changed to %u kHz\n", speed);
	return count;
}

static DEVICE_ATTR_RW(i2c_speed_khz);

static int ftdi_i2c_probe(struct platform_device *pdev)
{
	struct ftdi_i2c *fi2c;
	enum ftdi_mpsse_chip_type chip;
	int ret;

	fi2c = devm_kzalloc(&pdev->dev, sizeof(*fi2c), GFP_KERNEL);
	if (!fi2c)
		return -ENOMEM;

	fi2c->buf = devm_kmalloc(&pdev->dev, FTDI_MPSSE_BUF_SIZE, GFP_KERNEL);
	if (!fi2c->buf)
		return -ENOMEM;

	fi2c->rsp = devm_kmalloc(&pdev->dev, FTDI_MPSSE_BUF_SIZE, GFP_KERNEL);
	if (!fi2c->rsp)
		return -ENOMEM;

	fi2c->pdev = pdev;
	fi2c->speed_khz = clamp_val(i2c_speed, 10, 400);

	/* Software open-drain defaults (FT2232H, FT4232H) */
	fi2c->sda_hi_val = 0x00;
	fi2c->sda_hi_dir = PIN_SCL;

	/* FT232H supports hardware open-drain via 0x9E */
	chip = ftdi_mpsse_get_chip_type(pdev);
	if (chip == FTDI_CHIP_FT232H) {
		fi2c->open_drain_hw = true;
		fi2c->sda_hi_val = PIN_SDA_OUT;
		fi2c->sda_hi_dir = PIN_SCL | PIN_SDA_OUT;
	}

	ftdi_mpsse_bus_lock(pdev);
	ret = ftdi_i2c_hw_init(fi2c);
	ftdi_mpsse_bus_unlock(pdev);
	if (ret) {
		dev_err(&pdev->dev, "MPSSE I2C init failed: %d\n", ret);
		return ret;
	}

	ftdi_i2c_check_eeprom(pdev);

	fi2c->adapter.owner = THIS_MODULE;
	fi2c->adapter.class = I2C_CLASS_HWMON;
	fi2c->adapter.algo = &ftdi_i2c_algo;
	fi2c->adapter.quirks = &ftdi_i2c_quirks;
	fi2c->adapter.bus_recovery_info = &ftdi_i2c_recovery;
	fi2c->adapter.dev.parent = &pdev->dev;
	i2c_set_adapdata(&fi2c->adapter, fi2c);
	snprintf(fi2c->adapter.name, sizeof(fi2c->adapter.name),
		 "FTDI MPSSE I2C (%s)", dev_name(pdev->dev.parent));

	platform_set_drvdata(pdev, fi2c);

	{
		struct usb_device *udev = ftdi_mpsse_get_udev(pdev);
		const char *devpath = udev ? dev_name(&udev->dev) : NULL;
		long val;
		int nr;

		/* Per-device inter-phase delay */
		val = ftdi_i2c_lookup_map(i2c_delay_us_map, devpath);
		fi2c->delay_us = (val >= 0) ? (unsigned int)val : 0;

		nr = (int)ftdi_i2c_lookup_map(i2c_bus_nr_map, devpath);
		if (nr >= 0) {
			fi2c->adapter.nr = nr;
			ret = i2c_add_numbered_adapter(&fi2c->adapter);
			if (ret) {
				dev_err(&pdev->dev,
					"failed to add I2C adapter nr %d: %d\n",
					nr, ret);
				return ret;
			}
			ret = devm_add_action_or_reset(&pdev->dev,
						       ftdi_i2c_del_adapter,
						       &fi2c->adapter);
			if (ret)
				return ret;
		} else {
			ret = devm_i2c_add_adapter(&pdev->dev, &fi2c->adapter);
			if (ret) {
				dev_err(&pdev->dev,
					"failed to add I2C adapter: %d\n",
					ret);
				return ret;
			}
		}
	}

	dev_info(&pdev->dev, "FTDI MPSSE I2C adapter at %u kHz%s%s\n",
		 fi2c->speed_khz,
		 fi2c->open_drain_hw ? ", HW open-drain" : "",
		 clock_stretching ? ", clock stretching (AD7)" : "");

	ret = device_create_file(&pdev->dev, &dev_attr_i2c_stats);
	if (ret)
		dev_warn(&pdev->dev, "failed to create i2c_stats sysfs: %d\n",
			 ret);

	ret = device_create_file(&pdev->dev, &dev_attr_i2c_speed_khz);
	if (ret)
		dev_warn(&pdev->dev,
			 "failed to create i2c_speed_khz sysfs: %d\n", ret);

	return 0;
}

static int ftdi_i2c_resume(struct device *dev)
{
	struct ftdi_i2c *fi2c = dev_get_drvdata(dev);
	int ret;

	ftdi_mpsse_bus_lock(fi2c->pdev);
	ret = ftdi_i2c_hw_init(fi2c);
	ftdi_mpsse_bus_unlock(fi2c->pdev);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_i2c_pm_ops, NULL, ftdi_i2c_resume);

static struct platform_driver ftdi_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&ftdi_i2c_pm_ops),
	},
	.probe = ftdi_i2c_probe,
};
module_platform_driver(ftdi_i2c_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI MPSSE I2C child driver");
MODULE_LICENSE("GPL");
