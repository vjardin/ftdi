// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI bit-bang GPIO child driver
 *
 * Exposes the 8 xDBUS pins of a channel as a gpio_chip using async
 * bit-bang mode (bitmode 0x01, AN2232C-02): SET_BITMODE carries the
 * direction mask, each byte written on bulk OUT sets the output pins, and
 * READ_PINS returns the pin levels. Bit-bang replaces the UART of the
 * channel (DS_FT4232H), typically an FT4232H C/D one (no MPSSE): this
 * child is alone on it.
 */

#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

#include "ftdi_mpsse.h"

#define DRIVER_NAME		"ftdi-gpio-bitbang"
#define FTDI_BB_NGPIO		8	/* DBUS0-DBUS7 */

struct ftdi_gpio_bb {
	struct platform_device *pdev;
	struct gpio_chip gc;
	u16 reserved_mask;
	u8 dir_in;		/* bitmask: pins valid for input */
	u8 dir_out;		/* bitmask: pins valid for output */
	u8 gpio_dir;		/* 1 = output, 0 = input */
	u8 gpio_val;		/* output values */
	bool suspended;		/* from suspend to resume: no access */
	bool need_mode;		/* bit-bang to be entered again first */
	bool need_state;	/* value and directions to be sent again first */
	struct mutex lock;	/* protects the state above, serialises I/O */
};

/* Enters async bit-bang, or stays in it with a new direction mask */
static int ftdi_bb_set_mode(struct ftdi_gpio_bb *fg, u8 dir)
{
	return ftdi_mpsse_cfg_msg(fg->pdev, FTDI_SIO_SET_BITMODE_REQUEST,
				  (FTDI_SIO_BITMODE_BITBANG << 8) | dir);
}

static int ftdi_bb_set_dir(struct ftdi_gpio_bb *fg)
{
	return ftdi_bb_set_mode(fg, fg->gpio_dir);
}

static int ftdi_bb_write_val(struct ftdi_gpio_bb *fg)
{
	return ftdi_mpsse_write(fg->pdev, &fg->gpio_val, 1);
}

/*
 * Bit-bang is entered with every pin an input, the value latched, then
 * the outputs enabled: none drives a stale level in between. Until all
 * of it went through, the next access starts with it again: the channel
 * may still be in the UART mode the parent's resume left it in, where a
 * byte written goes out as a serial frame on TXD (DBUS0). Called with
 * fg->lock held.
 */
static int ftdi_bb_enter(struct ftdi_gpio_bb *fg)
{
	int ret;

	ret = ftdi_bb_set_mode(fg, 0);
	if (!ret)
		ret = ftdi_bb_write_val(fg);
	if (!ret)
		ret = ftdi_bb_set_dir(fg);
	fg->need_mode = ret != 0;
	if (!ret)
		fg->need_state = false;
	return ret;
}

/*
 * Value, directions, value again. After a failed update the channel is
 * still in bit-bang mode, its latch and directions unknown: no output is
 * released on the way. Until it went through, the next access starts
 * with it again. Called with fg->lock held.
 */
static int ftdi_bb_send_state(struct ftdi_gpio_bb *fg)
{
	int ret;

	ret = ftdi_bb_write_val(fg);
	if (!ret)
		ret = ftdi_bb_set_dir(fg);
	if (!ret)
		ret = ftdi_bb_write_val(fg);
	fg->need_state = ret != 0;
	return ret;
}

/*
 * What a failed resume or a failed update left to do. From suspend to
 * resume the channel may be in UART mode: no access, as the transport
 * refuses them while the device is suspended.
 */
static int ftdi_bb_catch_up(struct ftdi_gpio_bb *fg)
{
	if (fg->suspended)
		return -ESHUTDOWN;
	if (fg->need_mode)
		return ftdi_bb_enter(fg);
	if (fg->need_state)
		return ftdi_bb_send_state(fg);
	return 0;
}

/*
 * The value goes out before the direction, so that a pin that becomes an
 * output starts at the level asked for, and again once the direction is
 * set. A failed request leaves the cache as it was and the old state is
 * sent again: the chip may have taken part of the update, and resume
 * replays the cache. Called with fg->lock held.
 */
static int ftdi_bb_update(struct ftdi_gpio_bb *fg, u8 val, u8 dir)
{
	u8 old_val = fg->gpio_val;
	u8 old_dir = fg->gpio_dir;
	bool to_output = dir & ~old_dir;
	int ret = 0;

	if (fg->suspended)
		return -ESHUTDOWN;

	fg->gpio_val = val;
	if (fg->need_mode || fg->need_state) {
		/* The whole new state, the way the pending step sends it */
		fg->gpio_dir = dir;
		ret = ftdi_bb_catch_up(fg);
	} else {
		/* Not for a pin that only becomes an input: nothing to drive */
		if (val != old_val || to_output || dir == old_dir)
			ret = ftdi_bb_write_val(fg);
		if (!ret && dir != old_dir) {
			fg->gpio_dir = dir;
			ret = ftdi_bb_set_dir(fg);
			if (!ret && to_output)
				ret = ftdi_bb_write_val(fg);
		}
	}
	if (ret) {
		fg->gpio_val = old_val;
		fg->gpio_dir = old_dir;
		if (!fg->need_mode)
			ftdi_bb_send_state(fg);
	}
	return ret;
}

static int ftdi_bb_read(struct ftdi_gpio_bb *fg, u8 *pins)
{
	scoped_guard(mutex, &fg->lock) {
		int ret = ftdi_bb_catch_up(fg);

		if (ret)
			return ret;
	}
	return ftdi_mpsse_read_pins(fg->pdev, pins);
}

static int ftdi_bb_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);

	guard(mutex)(&fg->lock);

	return (fg->gpio_dir & BIT(offset)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ftdi_bb_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);

	if (!(fg->dir_in & BIT(offset)))
		return -EINVAL;

	guard(mutex)(&fg->lock);
	return ftdi_bb_update(fg, fg->gpio_val, fg->gpio_dir & ~BIT(offset));
}

static int ftdi_bb_direction_output(struct gpio_chip *gc, unsigned int offset,
				    int value)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);

	if (!(fg->dir_out & BIT(offset)))
		return -EINVAL;

	guard(mutex)(&fg->lock);
	return ftdi_bb_update(fg, value ? fg->gpio_val | BIT(offset)
					: fg->gpio_val & ~BIT(offset),
			      fg->gpio_dir | BIT(offset));
}

static int ftdi_bb_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	u8 pins;
	int ret;

	ret = ftdi_bb_read(fg, &pins);
	if (ret)
		return ret;

	return !!(pins & BIT(offset));
}

static int ftdi_bb_get_multiple(struct gpio_chip *gc, unsigned long *mask,
				unsigned long *bits)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	u8 pins;
	int ret;

	ret = ftdi_bb_read(fg, &pins);
	if (ret)
		return ret;

	*bits = (*bits & ~*mask) | (pins & *mask);
	return 0;
}

static int ftdi_bb_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);

	guard(mutex)(&fg->lock);
	return ftdi_bb_update(fg, value ? fg->gpio_val | BIT(offset)
					: fg->gpio_val & ~BIT(offset),
			      fg->gpio_dir);
}

static int ftdi_bb_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				unsigned long *bits)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);

	guard(mutex)(&fg->lock);
	return ftdi_bb_update(fg, (fg->gpio_val & ~*mask) | (*bits & *mask),
			      fg->gpio_dir);
}

static int ftdi_bb_init_valid_mask(struct gpio_chip *gc,
				   unsigned long *valid_mask,
				   unsigned int ngpios)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	unsigned long usable = fg->dir_in | fg->dir_out;
	unsigned int i;

	bitmap_zero(valid_mask, ngpios);
	for_each_set_bit(i, &usable, FTDI_BB_NGPIO)
		if (i < ngpios)
			set_bit(i, valid_mask);

	return 0;
}

/* Unbind or failed probe: give the channel back in its default mode */
static void ftdi_bb_leave(void *data)
{
	struct ftdi_gpio_bb *fg = data;

	ftdi_mpsse_cfg_msg(fg->pdev, FTDI_SIO_SET_BITMODE_REQUEST,
			   FTDI_SIO_BITMODE_RESET);
}

static int ftdi_gpio_bb_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct ftdi_gpio_bb *fg;
	int ret;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->pdev = pdev;
	fg->reserved_mask = pdata ? pdata->gpio_reserved_mask : 0;

	if (pdata && (pdata->gpio_dir_in || pdata->gpio_dir_out)) {
		fg->dir_in = pdata->gpio_dir_in & 0xff;
		fg->dir_out = pdata->gpio_dir_out & 0xff;
	} else {
		fg->dir_in = ~fg->reserved_mask & 0xff;
		fg->dir_out = ~fg->reserved_mask & 0xff;
	}

	ret = devm_mutex_init(&pdev->dev, &fg->lock);
	if (ret)
		return ret;

	/* Every pin an input, value 0: the cache as kzalloc left it */
	ret = ftdi_bb_set_dir(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enter bit-bang mode\n");
	ret = devm_add_action_or_reset(&pdev->dev, ftdi_bb_leave, fg);
	if (ret)
		return ret;

	fg->gc.owner = THIS_MODULE;
	fg->gc.parent = &pdev->dev;
	/*
	 * The USB interface tells the chips of several channels apart, within
	 * the 31 characters of GPIO_GET_CHIPINFO (GPIO_MAX_NAME_SIZE)
	 */
	fg->gc.label = devm_kasprintf(&pdev->dev, GFP_KERNEL, "ftdi-bitbang-%s",
				      dev_name(pdev->dev.parent));
	if (!fg->gc.label)
		return -ENOMEM;
	fg->gc.get_direction = ftdi_bb_get_direction;
	fg->gc.direction_input = ftdi_bb_direction_input;
	fg->gc.direction_output = ftdi_bb_direction_output;
	fg->gc.get = ftdi_bb_get;
	fg->gc.get_multiple = ftdi_bb_get_multiple;
	fg->gc.set = ftdi_bb_set;
	fg->gc.set_multiple = ftdi_bb_set_multiple;
	fg->gc.init_valid_mask = ftdi_bb_init_valid_mask;
	fg->gc.base = -1;
	fg->gc.ngpio = FTDI_BB_NGPIO;
	fg->gc.can_sleep = true;

	platform_set_drvdata(pdev, fg);

	/* Removed by devres before ftdi_bb_leave() runs */
	ret = devm_gpiochip_add_data(&pdev->dev, &fg->gc, fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register GPIO chip\n");

	dev_info(&pdev->dev,
		 "FTDI bit-bang GPIO: 8 pins, reserved mask 0x%02x\n",
		 fg->reserved_mask & 0xff);

	return 0;
}

static int ftdi_gpio_bb_suspend(struct device *dev)
{
	struct ftdi_gpio_bb *fg = dev_get_drvdata(dev);

	guard(mutex)(&fg->lock);
	fg->suspended = true;
	return 0;
}

/* The parent's resume, which runs first, put the channel in UART mode */
static int ftdi_gpio_bb_resume(struct device *dev)
{
	struct ftdi_gpio_bb *fg = dev_get_drvdata(dev);

	guard(mutex)(&fg->lock);
	fg->suspended = false;
	return ftdi_bb_enter(fg);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_gpio_bb_pm_ops, ftdi_gpio_bb_suspend,
				ftdi_gpio_bb_resume);

static struct platform_driver ftdi_gpio_bb_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&ftdi_gpio_bb_pm_ops),
	},
	.probe = ftdi_gpio_bb_probe,
};
module_platform_driver(ftdi_gpio_bb_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI bit-bang GPIO child driver");
MODULE_LICENSE("GPL");
