// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI bit-bang GPIO child driver
 *
 * Exposes DBUS0-DBUS7 as an 8-pin gpio_chip on FT4232H channels C/D
 * which lack MPSSE hardware.  Uses async bit-bang mode (bitmode 0x01)
 * with the SET_BITMODE vendor command and READ_PINS for input.
 *
 * Pins used by UART (TXD/RXD, optionally RTS/CTS) are excluded via
 * init_valid_mask.  The driver shares bulk endpoints with ftdi_uart,
 * serialised by bus_lock in the parent core.
 */

#include <linux/cleanup.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb.h>

#include "ftdi_mpsse.h"

#define DRIVER_NAME		"ftdi-gpio-bitbang"
#define FTDI_BB_NGPIO		8	/* DBUS0-DBUS7 */

struct ftdi_gpio_bb {
	struct platform_device *pdev;
	struct gpio_chip gc;
	u16 reserved_mask;	/* pins not available (UART owns them) */
	u16 dir_in;		/* bitmask: pins valid for input */
	u16 dir_out;		/* bitmask: pins valid for output */
	u8 gpio_dir;		/* direction: 1 = output, 0 = input */
	u8 gpio_val;		/* output value cache */
	struct mutex lock;	/* protects dir/val and serialises I/O */
};

/*
 * Write GPIO state via SET_BITMODE.
 * wValue encoding: low byte = pin output values, high byte = bitmode.
 * For async bit-bang (bitmode 0x01), the direction is set by the
 * direction byte in the SET_BITMODE command itself (low byte of wValue
 * = output pin mask when entering bit-bang; subsequent bulk writes set
 * pin values).
 *
 * We re-issue SET_BITMODE with bitmode 0x01 each time direction changes.
 * For value-only changes, a bulk write is sufficient, but SET_BITMODE
 * also updates values, so we use it uniformly for simplicity.
 */
static int ftdi_bb_update_hw(struct ftdi_gpio_bb *fg)
{
	u16 val;

	/*
	 * SET_BITMODE: wValue = (bitmode << 8) | direction_mask
	 * In async bit-bang, direction_mask has 1=output, 0=input.
	 * The actual pin values are written via a bulk OUT transfer,
	 * but we can also use READ_PINS to read input pins.
	 *
	 * Re-enter bit-bang mode with current direction mask.
	 */
	val = (FTDI_SIO_BITMODE_BITBANG << 8) | fg->gpio_dir;
	return ftdi_mpsse_cfg_msg(fg->pdev, FTDI_SIO_SET_BITMODE_REQUEST, val);
}

/*
 * Read current pin values via the READ_PINS vendor request.
 * This works in any bitmode and returns the instantaneous pin state.
 */
static int ftdi_bb_read_pins(struct ftdi_gpio_bb *fg)
{
	struct usb_device *udev;
	u8 *buf;
	int ret;

	udev = ftdi_mpsse_get_udev(fg->pdev);
	if (!udev)
		return -ENODEV;

	buf = kmalloc(1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      FTDI_SIO_READ_PINS_REQUEST,
			      USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
			      0, ftdi_mpsse_get_channel(fg->pdev),
			      buf, 1, FTDI_MPSSE_CTRL_TIMEOUT);
	if (ret == 1)
		ret = buf[0];
	else if (ret >= 0)
		ret = -EIO;

	kfree(buf);
	return ret;
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
	int ret;

	if (!(fg->dir_in & BIT(offset)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	fg->gpio_dir &= ~BIT(offset);
	ret = ftdi_bb_update_hw(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_bb_direction_output(struct gpio_chip *gc, unsigned int offset,
				    int value)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	int ret;

	if (!(fg->dir_out & BIT(offset)))
		return -EINVAL;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	fg->gpio_dir |= BIT(offset);
	if (value)
		fg->gpio_val |= BIT(offset);
	else
		fg->gpio_val &= ~BIT(offset);
	ret = ftdi_bb_update_hw(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_bb_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	ret = ftdi_bb_read_pins(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	if (ret < 0)
		return ret;

	return !!(ret & BIT(offset));
}

static int ftdi_bb_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	int ret;

	ftdi_mpsse_bus_lock(fg->pdev);
	mutex_lock(&fg->lock);
	if (value)
		fg->gpio_val |= BIT(offset);
	else
		fg->gpio_val &= ~BIT(offset);
	ret = ftdi_bb_update_hw(fg);
	mutex_unlock(&fg->lock);
	ftdi_mpsse_bus_unlock(fg->pdev);

	return ret;
}

static int ftdi_bb_init_valid_mask(struct gpio_chip *gc,
				   unsigned long *valid_mask,
				   unsigned int ngpios)
{
	struct ftdi_gpio_bb *fg = gpiochip_get_data(gc);
	unsigned long usable = fg->dir_in | fg->dir_out;

	bitmap_from_u64(valid_mask, usable);

	return 0;
}

static int ftdi_gpio_bb_probe(struct platform_device *pdev)
{
	struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
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

	/*
	 * Enter bit-bang mode with all non-reserved pins as inputs.
	 * This sets the FTDI chip to async bit-bang (bitmode 0x01)
	 * so that READ_PINS returns the physical pin states.
	 */
	ftdi_mpsse_bus_lock(pdev);
	fg->gpio_dir = 0;
	fg->gpio_val = 0;
	ret = ftdi_bb_update_hw(fg);
	ftdi_mpsse_bus_unlock(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enter bit-bang mode\n");

	fg->gc.owner = THIS_MODULE;
	fg->gc.parent = &pdev->dev;
	fg->gc.label = DRIVER_NAME;
	fg->gc.get_direction = ftdi_bb_get_direction;
	fg->gc.direction_input = ftdi_bb_direction_input;
	fg->gc.direction_output = ftdi_bb_direction_output;
	fg->gc.get = ftdi_bb_get;
	fg->gc.set = ftdi_bb_set;
	fg->gc.init_valid_mask = ftdi_bb_init_valid_mask;
	fg->gc.base = -1;
	fg->gc.ngpio = FTDI_BB_NGPIO;
	fg->gc.can_sleep = true;

	platform_set_drvdata(pdev, fg);

	ret = devm_gpiochip_add_data(&pdev->dev, &fg->gc, fg);
	if (ret) {
		dev_err(&pdev->dev, "failed to register GPIO chip: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev,
		 "FTDI bit-bang GPIO: 8 pins, reserved mask 0x%02x\n",
		 fg->reserved_mask & 0xff);

	return 0;
}

static void ftdi_gpio_bb_remove(struct platform_device *pdev)
{
	/*
	 * Reset bitmode to UART-native (0x00) on remove so that
	 * the UART driver can continue operating normally.
	 */
	ftdi_mpsse_bus_lock(pdev);
	ftdi_mpsse_cfg_msg(pdev, FTDI_SIO_SET_BITMODE_REQUEST,
			   FTDI_SIO_BITMODE_RESET << 8);
	ftdi_mpsse_bus_unlock(pdev);
}

static struct platform_driver ftdi_gpio_bb_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = ftdi_gpio_bb_probe,
	.remove = ftdi_gpio_bb_remove,
};
module_platform_driver(ftdi_gpio_bb_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI bit-bang GPIO child driver (non-MPSSE channels)");
MODULE_LICENSE("GPL");
