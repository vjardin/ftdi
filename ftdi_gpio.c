// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE GPIO child driver
 *
 * Exposes AD0-AD7 (low byte) and AC0-AC7 (high byte) as a gpio_chip when
 * the parent FTDI device is in MPSSE mode: 16 lines, 8 on the FT4232H (no
 * high byte), 12 with gpio_offset_compat. Pins reserved by a sibling bus
 * driver (SPI/I2C) are excluded via init_valid_mask.
 *
 * Reference: AN_108 (MPSSE command processor), gpio-mpsse.c in-tree driver.
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "ftdi_mpsse.h"

#define DRIVER_NAME		"ftdi-gpio"
#define FTDI_GPIO_NGPIO		16	/* 8 low (AD) + 8 high (AC) */
#define FTDI_GPIO_NGPIO_COMPAT	12	/* 8 high (AC) + 4 low (AD4-7) */
#define FTDI_GPIO_IRQ_POLL_MS	1

static bool gpio_offset_compat;
module_param(gpio_offset_compat, bool, 0444);
MODULE_PARM_DESC(gpio_offset_compat,
		 "Reverse GPIO offset mapping: 0-7=AC, 8-11=AD4-7, ignored on FT4232H (no AC pins; default 0)");

static int gpio_base = -1;
static atomic_t gpio_base_used = ATOMIC_INIT(0);
module_param(gpio_base, int, 0444);
MODULE_PARM_DESC(gpio_base, "GPIO chip base number (-1 = auto, default -1)");

struct ftdi_gpio {
	struct platform_device *pdev;
	struct gpio_chip gc;
	u16 reserved_mask;	/* pins the bus driver owns */
	u16 dir_in;		/* bitmask: pins valid for input */
	u16 dir_out;		/* bitmask: pins valid for output */
	u8 gpio_dir[2];		/* 1 = output, 0 = input */
	u8 gpio_val[2];		/* output value cache */
	u8 od_mask[2];		/* 1 = open-drain */
	u8 own[2];		/* pins of each bank owned by this driver */
	bool has_gpioh;		/* MPSSE high byte exists (not FT4232H) */
	bool compat;		/* gpio_offset_compat, on a chip with AC pins */
	struct mutex lock;	/* protects dir/val/od and serialises I/O */

#ifdef CONFIG_GPIOLIB_IRQCHIP
	struct delayed_work irq_work;
	atomic_t irq_enabled;		/* bitmask: which pins have IRQ */
	int irq_type[FTDI_GPIO_NGPIO];	/* per-pin: IRQ_TYPE_EDGE_xxx */
	u16 prev_values;
	atomic_t irq_fresh;		/* just enabled: resample, no edge */
#endif
};

/*
 * User-visible GPIO offset to hardware offset.
 * Default:  offset 0-7  = low byte  AD0-AD7 (hw 0-7)
 *           offset 8-15 = high byte AC0-AC7 (hw 8-15)
 * Compat:   offset 0-7  = high byte AC0-AC7 (hw 8-15)
 *           offset 8-11 = low byte  AD4-AD7 (hw 4-7)
 * The compat layout needs the high byte: not on the FT4232H.
 */
static unsigned int ftdi_gpio_to_hw(const struct ftdi_gpio *fg,
				    unsigned int offset)
{
	if (!fg->compat)
		return offset;
	return (offset >= 8) ? (offset - 8 + 4) : (offset + 8);
}

static unsigned int ftdi_gpio_from_hw(const struct ftdi_gpio *fg,
				      unsigned int hw)
{
	if (!fg->compat)
		return hw;
	/* AD0-AD3 have no line in the compat layout: out of range */
	if (hw < 4)
		return FTDI_GPIO_NGPIO;
	return (hw >= 8) ? (hw - 8) : (hw - 4 + 8);
}

/*
 * The low byte is shared with the bus driver of the channel: only this
 * driver's pins come from the cache, the others from the core's shadow.
 * A pin the bus driver claimed at run time (I2C: AD7) is its own: the
 * cache follows it as an input. Called with the bus lock held.
 */
static int ftdi_gpio_set_bank(struct ftdi_gpio *fg, unsigned int bank)
{
	u8 claimed, cmd[3];

	if (bank && !fg->has_gpioh)
		return 0;

	claimed = ftdi_mpsse_pins_claimed(fg->pdev, bank) & fg->own[bank];
	fg->gpio_dir[bank] &= ~claimed;
	ftdi_mpsse_pins_set(fg->pdev, bank, fg->own[bank] & ~claimed,
			    fg->gpio_val[bank], fg->gpio_dir[bank]);
	cmd[0] = bank ? MPSSE_SET_BITS_HIGH : MPSSE_SET_BITS_LOW;
	ftdi_mpsse_pins_get(fg->pdev, bank, &cmd[1], &cmd[2]);

	return ftdi_mpsse_write(fg->pdev, cmd, sizeof(cmd));
}

/*
 * A failed write must leave the cache and the core's shadow unchanged:
 * resume and the MPSSE recovery replay them. The old state is sent again
 * in case the recovery replayed the new one before the error came back.
 * Called with the bus lock and fg->lock held.
 */
static int ftdi_gpio_update_bank(struct ftdi_gpio *fg, unsigned int bank,
				 u8 val, u8 dir)
{
	u8 old_val = fg->gpio_val[bank];
	u8 old_dir = fg->gpio_dir[bank];
	int ret;

	fg->gpio_val[bank] = val;
	fg->gpio_dir[bank] = dir;
	ret = ftdi_gpio_set_bank(fg, bank);
	if (ret) {
		fg->gpio_val[bank] = old_val;
		fg->gpio_dir[bank] = old_dir;
		ftdi_gpio_set_bank(fg, bank);
	}
	return ret;
}

/*
 * Pin levels of the banks in mask (bit 0: low byte, bit 1: high byte) in
 * one round trip: low byte in bits 0-7, high byte in 8-15. The FT4232H
 * has no high byte: 0x83 is never sent there. Called with the bus lock.
 */
static int ftdi_gpio_read_banks(struct ftdi_gpio *fg, unsigned int mask)
{
	u8 cmd[3], val[2] = { };
	unsigned int n = 0, nrx = 0;
	int ret;

	if (!fg->has_gpioh)
		mask &= BIT(0);
	if (mask & BIT(0))
		cmd[n++] = MPSSE_GET_BITS_LOW;
	if (mask & BIT(1))
		cmd[n++] = MPSSE_GET_BITS_HIGH;
	if (!n)
		return 0;
	nrx = n;
	cmd[n++] = MPSSE_SEND_IMMEDIATE;

	ret = ftdi_mpsse_xfer(fg->pdev, cmd, n, val, nrx);
	if (ret)
		return ret;

	if (mask & BIT(0))
		return val[0] | ((mask & BIT(1)) ? val[1] << 8 : 0);
	return val[0] << 8;
}

static int ftdi_gpio_get_bank(struct ftdi_gpio *fg, unsigned int bank)
{
	int ret = ftdi_gpio_read_banks(fg, BIT(bank));

	return ret < 0 ? ret : (ret >> (bank * 8)) & 0xff;
}

static int ftdi_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;

	guard(mutex)(&fg->lock);

	return (fg->gpio_dir[bank] & BIT(bit)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ftdi_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	if (!(fg->dir_in & BIT(hw)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_update_bank(fg, bank, fg->gpio_val[bank],
				    fg->gpio_dir[bank] & ~BIT(bit));
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	if (!(fg->dir_out & BIT(hw)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_update_bank(fg, bank,
				    value ? fg->gpio_val[bank] | BIT(bit)
					  : fg->gpio_val[bank] & ~BIT(bit),
				    fg->gpio_dir[bank] | BIT(bit));
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_get_bank(fg, bank);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret < 0)
		return ret;

	return !!(ret & BIT(bit));
}

static int ftdi_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_update_bank(fg, bank,
				    value ? fg->gpio_val[bank] | BIT(bit)
					  : fg->gpio_val[bank] & ~BIT(bit),
				    fg->gpio_dir[bank]);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_get_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_read_banks(fg, ((*mask & 0x00ff) ? BIT(0) : 0) |
				       ((*mask & 0xff00) ? BIT(1) : 0));
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret < 0)
		return ret;
	*bits = ret & *mask;
	return 0;
}

static int ftdi_gpio_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	int ret = 0;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);

	if (*mask & 0x00ff) {
		ret = ftdi_gpio_update_bank(fg, 0,
					    (fg->gpio_val[0] & ~(*mask & 0xff)) |
					    (*bits & *mask & 0xff),
					    fg->gpio_dir[0]);
		if (ret)
			goto out;
	}

	if (*mask & 0xff00) {
		u8 hi_mask = (*mask >> 8) & 0xff;
		u8 hi_bits = (*bits >> 8) & 0xff;

		ret = ftdi_gpio_update_bank(fg, 1,
					    (fg->gpio_val[1] & ~hi_mask) |
					    (hi_bits & hi_mask),
					    fg->gpio_dir[1]);
	}

out:
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);
	return ret;
}

/*
 * A pin no mode reserves stays a line, but the bus driver of the channel
 * may take it at run time (I2C: AD7 as SCL sense, ftdi_mpsse_pins_claim()).
 * The core arbitrates: -EBUSY if the bus driver has this one.
 */
static int ftdi_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	ret = ftdi_mpsse_pins_gpio_request(fg->pdev, hw / 8, BIT(hw % 8));
	ftdi_mpsse_bus_unlock(fg->pdev);
	if (ret == -EBUSY)
		dev_dbg(&fg->pdev->dev,
			"line %u is in use by the bus driver of the channel\n",
			offset);
	return ret;
}

static void ftdi_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);

	ftdi_mpsse_bus_lock(fg->pdev);
	ftdi_mpsse_pins_gpio_free(fg->pdev, hw / 8, BIT(hw % 8));
	ftdi_mpsse_bus_unlock(fg->pdev);
}

/*
 * gpiolib does not call .free for a line a consumer still holds when the
 * chip is removed: give them all back to the core.
 */
static void ftdi_gpio_free_all(void *data)
{
	struct ftdi_gpio *fg = data;

	ftdi_mpsse_bus_lock(fg->pdev);
	ftdi_mpsse_pins_gpio_free(fg->pdev, 0, 0xff);
	ftdi_mpsse_pins_gpio_free(fg->pdev, 1, 0xff);
	ftdi_mpsse_bus_unlock(fg->pdev);
}

/* The lines of the pins in @pins (chip numbering) */
static void ftdi_gpio_pins_to_lines(struct ftdi_gpio *fg, unsigned long pins,
				    unsigned long *lines, unsigned int ngpios)
{
	unsigned int i;

	bitmap_zero(lines, ngpios);
	for_each_set_bit(i, &pins, FTDI_GPIO_NGPIO) {
		unsigned int user = ftdi_gpio_from_hw(fg, i);

		if (user < ngpios)
			set_bit(user, lines);
	}
}

static int ftdi_gpio_init_valid_mask(struct gpio_chip *gc,
				     unsigned long *valid_mask,
				     unsigned int ngpios)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	ftdi_gpio_pins_to_lines(fg, fg->dir_in | fg->dir_out, valid_mask,
				ngpios);
	return 0;
}

/*
 * MPSSE_DRIVE_ZERO_ONLY carries the open-drain pins of every child of the
 * channel: this driver's lines plus what the bus driver set (I2C SCL/SDA).
 * Called with the bus lock held.
 */
static int ftdi_gpio_send_od(struct ftdi_gpio *fg)
{
	u8 cmd[3];

	ftdi_mpsse_od_set(fg->pdev, 0, fg->own[0], fg->od_mask[0]);
	ftdi_mpsse_od_set(fg->pdev, 1, fg->own[1], fg->od_mask[1]);
	cmd[0] = MPSSE_DRIVE_ZERO_ONLY;
	ftdi_mpsse_od_get(fg->pdev, &cmd[1], &cmd[2]);
	return ftdi_mpsse_write(fg->pdev, cmd, sizeof(cmd));
}

/* Same rule as ftdi_gpio_update_bank() */
static int ftdi_gpio_update_od(struct ftdi_gpio *fg, unsigned int bank, u8 od)
{
	u8 old = fg->od_mask[bank];
	int ret;

	fg->od_mask[bank] = od;
	ret = ftdi_gpio_send_od(fg);
	if (ret) {
		fg->od_mask[bank] = old;
		ftdi_gpio_send_od(fg);
	}
	return ret;
}

static int ftdi_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
				unsigned long config)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	enum pin_config_param param = pinconf_to_config_param(config);
	unsigned int hw = ftdi_gpio_to_hw(fg, offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);

	/*
	 * gpiolib asks for push-pull on every direction_output(): only talk
	 * to the chip when the drive mode really changes.
	 */
	switch (param) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		ret = 0;
		if (!(fg->od_mask[bank] & BIT(bit)))
			ret = ftdi_gpio_update_od(fg, bank,
						  fg->od_mask[bank] | BIT(bit));
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		ret = 0;
		if (fg->od_mask[bank] & BIT(bit))
			ret = ftdi_gpio_update_od(fg, bank,
						  fg->od_mask[bank] & ~BIT(bit));
		break;
	default:
		ret = -ENOTSUPP;
		break;
	}

	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);
	return ret;
}

static int ftdi_gpio_read_all(struct ftdi_gpio *fg)
{
	return ftdi_gpio_read_banks(fg, BIT(0) | BIT(1));
}

#ifdef CONFIG_GPIOLIB_IRQCHIP
static void ftdi_gpio_poll_later(struct ftdi_gpio *fg)
{
	queue_delayed_work(system_power_efficient_wq, &fg->irq_work,
			   msecs_to_jiffies(FTDI_GPIO_IRQ_POLL_MS));
}

static void ftdi_gpio_irq_poll(struct work_struct *work)
{
	struct ftdi_gpio *fg = container_of(work, struct ftdi_gpio,
					    irq_work.work);
	unsigned int enabled = atomic_read(&fg->irq_enabled);
	unsigned long changed_bits;
	unsigned int fresh, offset;
	u16 cur, changed;
	int ret;

	if (!enabled)
		return;

	/*
	 * Lines enabled before this point only take a fresh sample from the
	 * read below; the ones enabled after it keep their bit for the next
	 * poll, the read may predate them.
	 */
	fresh = atomic_xchg(&fg->irq_fresh, 0);

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_read_all(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret < 0) {
		atomic_or(fresh, &fg->irq_fresh);
		dev_warn_ratelimited(&fg->pdev->dev,
				     "IRQ poll: pin read failed: %d\n", ret);
		goto reschedule;
	}

	cur = ret;
	changed = cur ^ fg->prev_values;
	changed_bits = changed & ~fresh;

	for_each_set_bit(offset, &changed_bits, FTDI_GPIO_NGPIO) {
		unsigned int user = ftdi_gpio_from_hw(fg, offset);
		int type;
		bool fire = false;

		if (user >= fg->gc.ngpio)
			continue;
		if (!(enabled & BIT(user)))
			continue;

		type = READ_ONCE(fg->irq_type[user]);
		if ((type & IRQ_TYPE_EDGE_RISING) && (cur & BIT(offset)))
			fire = true;
		if ((type & IRQ_TYPE_EDGE_FALLING) && !(cur & BIT(offset)))
			fire = true;

		if (fire) {
			int irq = irq_find_mapping(fg->gc.irq.domain, user);

			handle_nested_irq(irq);
		}
	}

	fg->prev_values = cur;

reschedule:
	if (atomic_read(&fg->irq_enabled))
		ftdi_gpio_poll_later(fg);
}

static void ftdi_gpio_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	atomic_or(BIT(ftdi_gpio_to_hw(fg, irqd_to_hwirq(d))), &fg->irq_fresh);

	if (!atomic_fetch_or(BIT(irqd_to_hwirq(d)), &fg->irq_enabled))
		ftdi_gpio_poll_later(fg);
}

static void ftdi_gpio_irq_disable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	atomic_and(~BIT(irqd_to_hwirq(d)), &fg->irq_enabled);
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static int ftdi_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	if (!(type & IRQ_TYPE_EDGE_BOTH))
		return -EINVAL;

	WRITE_ONCE(fg->irq_type[irqd_to_hwirq(d)], type & IRQ_TYPE_EDGE_BOTH);

	return 0;
}

static const struct irq_chip ftdi_gpio_irq_chip = {
	.name		= "ftdi-gpio-irq",
	.irq_enable	= ftdi_gpio_irq_enable,
	.irq_disable	= ftdi_gpio_irq_disable,
	.irq_set_type	= ftdi_gpio_irq_set_type,
	.flags		= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void ftdi_gpio_irq_init_valid_mask(struct gpio_chip *gc,
					  unsigned long *valid_mask,
					  unsigned int ngpios)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	ftdi_gpio_pins_to_lines(fg, fg->dir_in, valid_mask, ngpios);
}
#endif /* CONFIG_GPIOLIB_IRQCHIP */

/*
 * The cache starts from what the chip does: the shadow holds what is
 * driven (outputs a previous instance of this driver left are kept), the
 * pins give the level of the inputs. No spurious edge IRQ, no stale zero
 * in the first write. Called with the bus lock held.
 */
static int ftdi_gpio_sync_hw(struct ftdi_gpio *fg)
{
	unsigned int bank;
	u8 val, dir, od[2];
	int pins;

	pins = ftdi_gpio_read_all(fg);
	if (pins < 0)
		return pins;

	/* A previous instance may have left lines open drain (0x9E) */
	ftdi_mpsse_od_get(fg->pdev, &od[0], &od[1]);
	for (bank = 0; bank < 2; bank++) {
		u8 in = pins >> (bank * 8);

		ftdi_mpsse_pins_get(fg->pdev, bank, &val, &dir);
		fg->gpio_dir[bank] = dir & fg->own[bank];
		fg->gpio_val[bank] = ((val & dir) | (in & ~dir)) & fg->own[bank];
		fg->od_mask[bank] = od[bank] & fg->own[bank];
	}
#ifdef CONFIG_GPIOLIB_IRQCHIP
	fg->prev_values = pins;
#endif
	return 0;
}

/* gpio_base goes to the next chip once the one that had it is gone */
static void ftdi_gpio_release_base(void *data)
{
	atomic_set(&gpio_base_used, 0);
}

static int ftdi_gpio_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct ftdi_gpio *fg;
	int ret;

	if (!pdata)
		return -EINVAL;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->pdev = pdev;
	fg->reserved_mask = pdata->gpio_reserved_mask;

	if (pdata->gpio_dir_in || pdata->gpio_dir_out) {
		fg->dir_in = pdata->gpio_dir_in;
		fg->dir_out = pdata->gpio_dir_out;
	} else {
		fg->dir_in = ~fg->reserved_mask & GENMASK(15, 0);
		fg->dir_out = ~fg->reserved_mask & GENMASK(15, 0);
	}

	fg->has_gpioh = pdata->has_gpioh;
	fg->compat = gpio_offset_compat && fg->has_gpioh;
	if (!fg->has_gpioh) {
		/* FT4232H family: 8 xDBUS pins, no compat (AC-first) layout */
		fg->dir_in &= 0x00ff;
		fg->dir_out &= 0x00ff;
	}
	fg->own[0] = (fg->dir_in | fg->dir_out) & 0xff;
	fg->own[1] = (fg->dir_in | fg->dir_out) >> 8;

	ret = devm_mutex_init(&pdev->dev, &fg->lock);
	if (ret)
		return ret;

	ftdi_mpsse_bus_lock(pdev);
	ret = ftdi_gpio_sync_hw(fg);
	ftdi_mpsse_bus_unlock(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read initial GPIO state\n");

	fg->gc.owner = THIS_MODULE;
	fg->gc.parent = &pdev->dev;
	/* The USB interface tells the chips of several channels apart */
	fg->gc.label = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-%s",
				      DRIVER_NAME, dev_name(pdev->dev.parent));
	if (!fg->gc.label)
		return -ENOMEM;
	fg->gc.get_direction = ftdi_gpio_get_direction;
	fg->gc.direction_input = ftdi_gpio_direction_input;
	fg->gc.direction_output = ftdi_gpio_direction_output;
	fg->gc.get = ftdi_gpio_get;
	fg->gc.set = ftdi_gpio_set;
	fg->gc.request = ftdi_gpio_request;
	fg->gc.free = ftdi_gpio_free;
	fg->gc.init_valid_mask = ftdi_gpio_init_valid_mask;
	/* One chip at a time has the fixed base (deprecated anyway) */
	fg->gc.base = -1;
	if (gpio_base >= 0 && !atomic_xchg(&gpio_base_used, 1)) {
		ret = devm_add_action_or_reset(&pdev->dev,
					       ftdi_gpio_release_base, NULL);
		if (ret)
			return ret;
		fg->gc.base = gpio_base;
	}
	fg->gc.can_sleep = true;

	if (!fg->has_gpioh) {
		fg->gc.ngpio = 8;
		fg->gc.get_multiple = ftdi_gpio_get_multiple;
		fg->gc.set_multiple = ftdi_gpio_set_multiple;
	} else if (fg->compat) {
		fg->gc.ngpio = FTDI_GPIO_NGPIO_COMPAT;
		/* Per-pin ops handle offset translation; skip _multiple */
	} else {
		fg->gc.ngpio = FTDI_GPIO_NGPIO;
		fg->gc.get_multiple = ftdi_gpio_get_multiple;
		fg->gc.set_multiple = ftdi_gpio_set_multiple;
	}

	/* FT232H only: per-pin open drain (0x9E) */
	if (pdata->has_drive_zero)
		fg->gc.set_config = ftdi_gpio_set_config;

#ifdef CONFIG_GPIOLIB_IRQCHIP
	INIT_DELAYED_WORK(&fg->irq_work, ftdi_gpio_irq_poll);

	gpio_irq_chip_set_chip(&fg->gc.irq, &ftdi_gpio_irq_chip);
	fg->gc.irq.parent_handler = NULL;
	fg->gc.irq.num_parents = 0;
	fg->gc.irq.default_type = IRQ_TYPE_NONE;
	fg->gc.irq.handler = handle_simple_irq;
	fg->gc.irq.threaded = true;
	fg->gc.irq.init_valid_mask = ftdi_gpio_irq_init_valid_mask;
#endif

	platform_set_drvdata(pdev, fg);

	/* Runs after the chip is removed (devres order) */
	ret = devm_add_action(&pdev->dev, ftdi_gpio_free_all, fg);
	if (ret)
		return ret;

	ret = devm_gpiochip_add_data(&pdev->dev, &fg->gc, fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register GPIO chip\n");

	dev_info(&pdev->dev, "FTDI MPSSE GPIO: %u pins, reserved mask 0x%04x\n",
		 fg->gc.ngpio, fg->reserved_mask);

	return 0;
}

static void ftdi_gpio_remove(struct platform_device *pdev)
{
#ifdef CONFIG_GPIOLIB_IRQCHIP
	struct ftdi_gpio *fg = platform_get_drvdata(pdev);

	/*
	 * devres removes the chip after this returns and an IRQ can still be
	 * enabled until then: disable the poller, a cancel could be re-armed.
	 */
	disable_delayed_work_sync(&fg->irq_work);
#endif
}

static int ftdi_gpio_suspend(struct device *dev)
{
#ifdef CONFIG_GPIOLIB_IRQCHIP
	struct ftdi_gpio *fg = dev_get_drvdata(dev);

	/* Disabled, not cancelled: an irq_enable meanwhile must not re-arm it */
	disable_delayed_work_sync(&fg->irq_work);
#endif

	return 0;
}

static int ftdi_gpio_resume(struct device *dev)
{
	struct ftdi_gpio *fg = dev_get_drvdata(dev);
	int ret;

	/* Re-read all pins so prev_values reflects physical state */
	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_read_all(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret >= 0) {
#ifdef CONFIG_GPIOLIB_IRQCHIP
		fg->prev_values = ret;
#endif
		ret = 0;
	}

#ifdef CONFIG_GPIOLIB_IRQCHIP
	enable_delayed_work(&fg->irq_work);
	if (atomic_read(&fg->irq_enabled))
		ftdi_gpio_poll_later(fg);
#endif

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_gpio_pm_ops,
				ftdi_gpio_suspend, ftdi_gpio_resume);

static struct platform_driver ftdi_gpio_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&ftdi_gpio_pm_ops),
	},
	.probe = ftdi_gpio_probe,
	.remove = ftdi_gpio_remove,
};
module_platform_driver(ftdi_gpio_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI MPSSE GPIO child driver");
MODULE_LICENSE("GPL");
