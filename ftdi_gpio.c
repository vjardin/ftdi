// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE GPIO child driver
 *
 * Exposes AD0-AD7 (low byte) and AC0-AC7 (high byte) as a 16-pin gpio_chip
 * when the parent FTDI device is in MPSSE mode.  Pins reserved by a sibling
 * bus driver (SPI/I2C) are excluded via init_valid_mask.
 *
 * Reference: AN_108 (MPSSE command processor), gpio-mpsse.c in-tree driver.
 */

#include <linux/cleanup.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "ftdi_mpsse.h"

#define DRIVER_NAME		"ftdi-gpio"
#define FTDI_GPIO_NGPIO		16	/* 8 low (AD) + 8 high (AC) */
#define FTDI_GPIO_NGPIO_COMPAT	12	/* 8 high (AC) + 4 low (AD4-7) */
#define FTDI_GPIO_IRQ_POLL_MS	1	/* IRQ polling interval */

static bool gpio_offset_compat;
module_param(gpio_offset_compat, bool, 0444);
MODULE_PARM_DESC(gpio_offset_compat,
		 "Reverse GPIO offset mapping: 0-7=AC, 8-11=AD4-7 (default 0)");

static int gpio_base = -1;
module_param(gpio_base, int, 0444);
MODULE_PARM_DESC(gpio_base, "GPIO chip base number (-1 = auto, default -1)");

/*
 * Translate user-visible GPIO offset to hardware offset.
 * Default:  offset 0-7  = low byte  AD0-AD7 (hw 0-7)
 *           offset 8-15 = high byte AC0-AC7 (hw 8-15)
 * Compat:   offset 0-7  = high byte AC0-AC7 (hw 8-15)
 *           offset 8-11 = low byte  AD4-AD7 (hw 4-7)
 */
static unsigned int ftdi_gpio_to_hw(unsigned int offset)
{
	if (!gpio_offset_compat)
		return offset;
	return (offset >= 8) ? (offset - 8 + 4) : (offset + 8);
}

/*
 * Translate hardware offset back to user-visible GPIO offset.
 */
static unsigned int ftdi_gpio_from_hw(unsigned int hw)
{
	if (!gpio_offset_compat)
		return hw;
	return (hw >= 8) ? (hw - 8) : (hw - 4 + 8);
}

struct ftdi_gpio {
	struct platform_device *pdev;
	struct gpio_chip gc;
	u16 reserved_mask;	/* pins not available (bus driver owns them) */
	u16 dir_in;		/* bitmask: pins valid for input */
	u16 dir_out;		/* bitmask: pins valid for output */
	u8 gpio_dir[2];		/* direction: 1 = output, 0 = input */
	u8 gpio_val[2];		/* output value cache */
	u8 od_mask[2];		/* per-pin open-drain: 1 = open-drain */
	bool has_od_hw;		/* true on FT232H (supports 0x9E) */
	struct mutex lock;	/* protects dir/val/od and serialises I/O */

#ifdef CONFIG_GPIOLIB_IRQCHIP
	/* IRQ polling support */
	struct delayed_work irq_work;
	atomic_t irq_enabled;		/* bitmask: which pins have IRQ */
	int irq_type[FTDI_GPIO_NGPIO];	/* per-pin: IRQ_TYPE_EDGE_xxx */
	u16 prev_values;		/* previous pin states for edge detect */
#endif
};

static int ftdi_gpio_set_bank(struct ftdi_gpio *fg, unsigned int bank)
{
	u8 cmd[3];

	cmd[0] = bank ? MPSSE_SET_BITS_HIGH : MPSSE_SET_BITS_LOW;
	cmd[1] = fg->gpio_val[bank];
	cmd[2] = fg->gpio_dir[bank];

	return ftdi_mpsse_write(fg->pdev, cmd, sizeof(cmd));
}

static int ftdi_gpio_get_bank(struct ftdi_gpio *fg, unsigned int bank)
{
	u8 cmd[2];
	u8 val;
	int ret;

	cmd[0] = bank ? MPSSE_GET_BITS_HIGH : MPSSE_GET_BITS_LOW;
	cmd[1] = MPSSE_SEND_IMMEDIATE;

	ret = ftdi_mpsse_xfer(fg->pdev, cmd, sizeof(cmd), &val, 1);
	if (ret)
		return ret;

	return val;
}

static int ftdi_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;

	guard(mutex)(&fg->lock);

	return (fg->gpio_dir[bank] & BIT(bit)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ftdi_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	if (!(fg->dir_in & BIT(hw)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	fg->gpio_dir[bank] &= ~BIT(bit);
	ret = ftdi_gpio_set_bank(fg, bank);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	if (!(fg->dir_out & BIT(hw)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	fg->gpio_dir[bank] |= BIT(bit);
	if (value)
		fg->gpio_val[bank] |= BIT(bit);
	else
		fg->gpio_val[bank] &= ~BIT(bit);
	ret = ftdi_gpio_set_bank(fg, bank);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned int hw = ftdi_gpio_to_hw(offset);
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
	unsigned int hw = ftdi_gpio_to_hw(offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	if (value)
		fg->gpio_val[bank] |= BIT(bit);
	else
		fg->gpio_val[bank] &= ~BIT(bit);
	ret = ftdi_gpio_set_bank(fg, bank);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_gpio_get_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	int ret;

	*bits = 0;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);

	/* Low bank (AD0-AD7) */
	if (*mask & 0x00ff) {
		ret = ftdi_gpio_get_bank(fg, 0);
		if (ret < 0)
			goto out;
		*bits |= ret & *mask & 0xff;
	}

	/* High bank (AC0-AC7) */
	if (*mask & 0xff00) {
		ret = ftdi_gpio_get_bank(fg, 1);
		if (ret < 0)
			goto out;
		*bits |= ((unsigned long)ret << 8) & *mask & 0xff00;
	}

	ret = 0;
out:
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);
	return ret;
}

static int ftdi_gpio_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	int ret = 0;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);

	if (*mask & 0x00ff) {
		fg->gpio_val[0] = (fg->gpio_val[0] & ~(*mask & 0xff)) |
				   (*bits & *mask & 0xff);
		ret = ftdi_gpio_set_bank(fg, 0);
		if (ret)
			goto out;
	}

	if (*mask & 0xff00) {
		u8 hi_mask = (*mask >> 8) & 0xff;
		u8 hi_bits = (*bits >> 8) & 0xff;

		fg->gpio_val[1] = (fg->gpio_val[1] & ~hi_mask) |
				   (hi_bits & hi_mask);
		ret = ftdi_gpio_set_bank(fg, 1);
	}

out:
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);
	return ret;
}

static int ftdi_gpio_init_valid_mask(struct gpio_chip *gc,
				     unsigned long *valid_mask,
				     unsigned int ngpios)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	unsigned long usable = fg->dir_in | fg->dir_out;
	unsigned int i;

	bitmap_zero(valid_mask, ngpios);
	for_each_set_bit(i, &usable, FTDI_GPIO_NGPIO) {
		unsigned int user = ftdi_gpio_from_hw(i);

		if (user < ngpios)
			set_bit(user, valid_mask);
	}

	return 0;
}

/*
 * Send the MPSSE_DRIVE_ZERO_ONLY command with the combined open-drain mask.
 * OR with reserved_mask so that pins owned by a sibling bus driver (e.g. I2C
 * SCL/SDA) retain their open-drain setting -- the I2C driver also sends 0x9E
 * for those pins, and a plain GPIO 0x9E would clobber them if not merged.
 */
static int ftdi_gpio_send_od(struct ftdi_gpio *fg)
{
	u8 cmd[3];

	cmd[0] = MPSSE_DRIVE_ZERO_ONLY;
	cmd[1] = fg->od_mask[0] | (fg->reserved_mask & 0xff);
	cmd[2] = fg->od_mask[1] | ((fg->reserved_mask >> 8) & 0xff);
	return ftdi_mpsse_write(fg->pdev, cmd, sizeof(cmd));
}

static int ftdi_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
				unsigned long config)
{
	struct ftdi_gpio *fg = gpiochip_get_data(gc);
	enum pin_config_param param = pinconf_to_config_param(config);
	unsigned int hw = ftdi_gpio_to_hw(offset);
	unsigned int bank = hw / 8;
	unsigned int bit = hw % 8;
	int ret;

	if (!fg->has_od_hw)
		return -ENOTSUPP;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);

	switch (param) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		fg->od_mask[bank] |= BIT(bit);
		ret = ftdi_gpio_send_od(fg);
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		fg->od_mask[bank] &= ~BIT(bit);
		ret = ftdi_gpio_send_od(fg);
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
	int lo, hi;

	lo = ftdi_gpio_get_bank(fg, 0);
	if (lo < 0)
		return lo;

	hi = ftdi_gpio_get_bank(fg, 1);
	if (hi < 0)
		return hi;

	return lo | (hi << 8);
}

#ifdef CONFIG_GPIOLIB_IRQCHIP
static void ftdi_gpio_irq_poll(struct work_struct *work)
{
	struct ftdi_gpio *fg = container_of(work, struct ftdi_gpio,
					    irq_work.work);
	unsigned int enabled = atomic_read(&fg->irq_enabled);
	unsigned long changed_bits;
	u16 cur, changed;
	int ret, offset;

	if (!enabled)
		return;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_gpio_read_all(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret < 0)
		goto reschedule;

	cur = ret;
	changed = cur ^ fg->prev_values;
	changed_bits = changed;

	for_each_set_bit(offset, &changed_bits, FTDI_GPIO_NGPIO) {
		unsigned int user = ftdi_gpio_from_hw(offset);
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
		schedule_delayed_work(&fg->irq_work,
				      msecs_to_jiffies(FTDI_GPIO_IRQ_POLL_MS));
}

static void ftdi_gpio_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct ftdi_gpio *fg = gpiochip_get_data(gc);

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));

	/* Schedule poller if this is the first enabled IRQ */
	if (!atomic_fetch_or(BIT(irqd_to_hwirq(d)), &fg->irq_enabled))
		schedule_delayed_work(&fg->irq_work,
				      msecs_to_jiffies(FTDI_GPIO_IRQ_POLL_MS));
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
	unsigned long input_pins = fg->dir_in;
	unsigned int i;

	/* Only input-capable pins can be IRQ sources */
	bitmap_zero(valid_mask, ngpios);
	for_each_set_bit(i, &input_pins, FTDI_GPIO_NGPIO) {
		unsigned int user = ftdi_gpio_from_hw(i);

		if (user < ngpios)
			set_bit(user, valid_mask);
	}
}
#endif /* CONFIG_GPIOLIB_IRQCHIP */

/*
 * Read current pin values from hardware so that gpio_val[] and prev_values
 * reflect physical state at probe, avoiding spurious edge-IRQ triggers and
 * stale-zero first writes.
 */
static int ftdi_gpio_sync_hw(struct ftdi_gpio *fg)
{
	int lo, hi;

	lo = ftdi_gpio_get_bank(fg, 0);
	if (lo < 0)
		return lo;
	hi = ftdi_gpio_get_bank(fg, 1);
	if (hi < 0)
		return hi;

	fg->gpio_val[0] = lo;
	fg->gpio_val[1] = hi;
#ifdef CONFIG_GPIOLIB_IRQCHIP
	fg->prev_values = lo | (hi << 8);
#endif
	return 0;
}

static int ftdi_gpio_probe(struct platform_device *pdev)
{
	struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct ftdi_gpio *fg;
	int ret;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->pdev = pdev;
	fg->reserved_mask = pdata ? pdata->gpio_reserved_mask : 0;

	/* Per-pin direction masks; default to ~reserved for backward compat */
	if (pdata && (pdata->gpio_dir_in || pdata->gpio_dir_out)) {
		fg->dir_in = pdata->gpio_dir_in;
		fg->dir_out = pdata->gpio_dir_out;
	} else {
		fg->dir_in = ~fg->reserved_mask & GENMASK(15, 0);
		fg->dir_out = ~fg->reserved_mask & GENMASK(15, 0);
	}

	if (pdata && pdata->gpio_quirk) {
		const struct ftdi_gpio_quirk *q = pdata->gpio_quirk;

		if (q->dir_in || q->dir_out) {
			fg->dir_in = q->dir_in;
			fg->dir_out = q->dir_out;
		}
	}

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
	fg->gc.label = DRIVER_NAME;
	fg->gc.get_direction = ftdi_gpio_get_direction;
	fg->gc.direction_input = ftdi_gpio_direction_input;
	fg->gc.direction_output = ftdi_gpio_direction_output;
	fg->gc.get = ftdi_gpio_get;
	fg->gc.set = ftdi_gpio_set;
	fg->gc.init_valid_mask = ftdi_gpio_init_valid_mask;
	fg->gc.base = gpio_base;
	fg->gc.can_sleep = true;	/* USB I/O can sleep */

	if (gpio_offset_compat) {
		fg->gc.ngpio = FTDI_GPIO_NGPIO_COMPAT;
		/* Per-pin ops handle offset translation; skip _multiple */
	} else {
		fg->gc.ngpio = FTDI_GPIO_NGPIO;
		fg->gc.get_multiple = ftdi_gpio_get_multiple;
		fg->gc.set_multiple = ftdi_gpio_set_multiple;
	}

	/* FT232H supports per-pin open-drain via MPSSE opcode 0x9E */
	if (ftdi_mpsse_get_chip_type(pdev) == FTDI_CHIP_FT232H) {
		fg->has_od_hw = true;
		fg->gc.set_config = ftdi_gpio_set_config;
	}

	if (pdata && pdata->gpio_quirk)
		fg->gc.names = pdata->gpio_quirk->names;

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

	ret = devm_gpiochip_add_data(&pdev->dev, &fg->gc, fg);
	if (ret) {
		dev_err(&pdev->dev, "failed to register GPIO chip: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "FTDI MPSSE GPIO: 16 pins, reserved mask 0x%04x\n",
		 fg->reserved_mask);

	return 0;
}

static void ftdi_gpio_remove(struct platform_device *pdev)
{
#ifdef CONFIG_GPIOLIB_IRQCHIP
	struct ftdi_gpio *fg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&fg->irq_work);
#endif
}

static int ftdi_gpio_suspend(struct device *dev)
{
#ifdef CONFIG_GPIOLIB_IRQCHIP
	struct ftdi_gpio *fg = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&fg->irq_work);
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
	if (atomic_read(&fg->irq_enabled))
		schedule_delayed_work(&fg->irq_work,
				      msecs_to_jiffies(FTDI_GPIO_IRQ_POLL_MS));
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
