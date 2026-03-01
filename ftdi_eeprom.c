// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI EEPROM reader, decoder, and sysfs interface
 *
 * Reads the on-chip EEPROM at probe time via USB control messages,
 * decodes per-chip fields (channel type, CBUS mux, drive strength),
 * and exposes both text and binary sysfs attributes.
 *
 * Compiled into ftdi_mpsse.ko as part of the composite module.
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string_choices.h>
#include <linux/usb.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define FTDI_EE_READ_TIMEOUT	5000	/* ms */

static u16 rotate_left_16(u16 val, unsigned int n)
{
	return (val << n) | (val >> (16 - n));
}

static bool ftdi_eeprom_checksum_ok(const struct ftdi_eeprom *ee)
{
	u16 checksum = 0xaaaa;
	u16 i;

	for (i = 0; i < ee->size; i++) {
		u16 word = ee->data[i * 2] | (ee->data[i * 2 + 1] << 8);

		checksum ^= word;
		checksum = rotate_left_16(checksum, 1);
	}

	return checksum == 0;
}

/*
 * FTDI EEPROMs store strings as standard USB string descriptors:
 *   byte 0: bLength (total length including this 2-byte header)
 *   byte 1: bDescriptorType (0x03)
 *   byte 2+: UTF-16LE characters
 *
 * The EEPROM header at off_byte / len_byte contains the offset and
 * total length of the descriptor within the EEPROM data.
 */
static void ftdi_eeprom_decode_string(const struct ftdi_eeprom *ee,
				      u8 off_byte, u8 len_byte,
				      char *out, size_t out_size)
{
	unsigned int off, len, nchars, i;

	out[0] = '\0';

	off = ee->data[off_byte];
	len = ee->data[len_byte];

	if (len < 2 || off + len > FTDI_EEPROM_MAX_BYTES)
		return;

	if (ee->data[off + 1] != 0x03)
		return;

	nchars = (len - 2) / 2;
	if (nchars >= out_size)
		nchars = out_size - 1;

	for (i = 0; i < nchars; i++) {
		u16 wc = ee->data[off + 2 + i * 2] |
			 (ee->data[off + 2 + i * 2 + 1] << 8);

		out[i] = (wc > 0 && wc < 0x80) ? (char)wc : '?';
	}
	out[nchars] = '\0';
}

static void ftdi_eeprom_decode(struct ftdi_mpsse_dev *fdev,
			       struct ftdi_eeprom *ee,
			       enum ftdi_mpsse_chip_type chip)
{
	u8 cfg_desc, chip_cfg;
	int i;

	ee->vid = ee->data[FTDI_EE_VID_L] |
		  (ee->data[FTDI_EE_VID_L + 1] << 8);
	ee->pid = ee->data[FTDI_EE_PID_L] |
		  (ee->data[FTDI_EE_PID_L + 1] << 8);
	ee->release = ee->data[FTDI_EE_RELEASE_L] |
		      (ee->data[FTDI_EE_RELEASE_L + 1] << 8);

	ftdi_eeprom_decode_string(ee, FTDI_EE_STR_MFR_OFF,
				  FTDI_EE_STR_MFR_LEN,
				  ee->manufacturer, sizeof(ee->manufacturer));
	ftdi_eeprom_decode_string(ee, FTDI_EE_STR_PROD_OFF,
				  FTDI_EE_STR_PROD_LEN,
				  ee->product, sizeof(ee->product));
	ftdi_eeprom_decode_string(ee, FTDI_EE_STR_SER_OFF,
				  FTDI_EE_STR_SER_LEN,
				  ee->serial, sizeof(ee->serial));

	cfg_desc = ee->data[FTDI_EE_CFG_DESC];
	ee->self_powered = !!(cfg_desc & FTDI_EE_CFG_SELF_POWERED);
	ee->remote_wakeup = !!(cfg_desc & FTDI_EE_CFG_REMOTE_WAKEUP);
	ee->max_power_ma = ee->data[FTDI_EE_MAX_POWER] * 2;

	chip_cfg = ee->data[FTDI_EE_CHIP_CFG];
	ee->pulldown = !!(chip_cfg & FTDI_EE_CFG_PULLDOWN);
	ee->use_serial = !!(chip_cfg & FTDI_EE_CFG_USE_SERIAL);

	ee->group0_drive = ee->data[FTDI_EE_DRIVE0] & 0x0f;
	ee->group1_drive = ee->data[FTDI_EE_DRIVE1] & 0x0f;

	ee->group0_drive_ma = FTDI_EE_DRIVE_MA(ee->group0_drive);
	ee->group0_schmitt = !!(ee->group0_drive & FTDI_EE_DRIVE_SCHMITT);
	ee->group0_slow_slew = !!(ee->group0_drive & FTDI_EE_DRIVE_SLOW_SLEW);
	ee->group1_drive_ma = FTDI_EE_DRIVE_MA(ee->group1_drive);
	ee->group1_schmitt = !!(ee->group1_drive & FTDI_EE_DRIVE_SCHMITT);
	ee->group1_slow_slew = !!(ee->group1_drive & FTDI_EE_DRIVE_SLOW_SLEW);

	/* User area protocol hint (common to all chips) */
	ee->protocol_hint = ee->data[FTDI_EE_PROTOCOL_HINT];

	switch (chip) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		ee->channel_a_type = ee->data[FTDI_EE_CHAN_A] & 0x0f;
		ee->channel_b_type = FTDI_EE_CHAN_UART;
		ee->cha_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_232H_VCP);
		ee->num_cbus = 10;
		for (i = 0; i < 5; i++) {
			u8 byte = ee->data[FTDI_EE_232H_CBUS_BASE + i];

			ee->cbus[i * 2] = byte & 0x0f;
			ee->cbus[i * 2 + 1] = (byte >> 4) & 0x0f;
		}
		break;

	case FTDI_CHIP_FT2232H:
	case FTDI_CHIP_FT2232HP:
	case FTDI_CHIP_FT2233HP:
		ee->channel_a_type = ee->data[FTDI_EE_CHAN_A] & 0x07;
		ee->channel_b_type = ee->data[FTDI_EE_CHAN_B] & 0x07;
		ee->cha_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_2232H_VCP);
		ee->chb_vcp = !!(ee->data[FTDI_EE_CHAN_B] & FTDI_EE_2232H_VCP);
		ee->num_cbus = 0;
		break;

	case FTDI_CHIP_FT4232H:
	case FTDI_CHIP_FT4232HA:
	case FTDI_CHIP_FT4232HP:
	case FTDI_CHIP_FT4233HP: {
		u8 invert = ee->data[FTDI_EE_INVERT];

		/* A/B channel types from EEPROM (same layout as FT2232H) */
		ee->channel_a_type = ee->data[FTDI_EE_CHAN_A] & 0x07;
		ee->channel_b_type = ee->data[FTDI_EE_CHAN_B] & 0x07;
		/* C/D are hardware-fixed to UART */
		ee->channel_c_type = FTDI_EE_CHAN_UART;
		ee->channel_d_type = FTDI_EE_CHAN_UART;

		/* VCP bits: A/B use bit 3, C/D use bit 4 */
		ee->cha_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_2232H_VCP);
		ee->chb_vcp = !!(ee->data[FTDI_EE_CHAN_B] & FTDI_EE_2232H_VCP);
		ee->chc_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_4232H_CHC_VCP);
		ee->chd_vcp = !!(ee->data[FTDI_EE_CHAN_B] & FTDI_EE_4232H_CHD_VCP);

		/* RS485 and TXDEN from invert byte (0x0B) */
		ee->cha_rs485 = !!(invert & FTDI_EE_4232H_CHA_RS485);
		ee->chb_rs485 = !!(invert & FTDI_EE_4232H_CHB_RS485);
		ee->chc_rs485 = !!(invert & FTDI_EE_4232H_CHC_RS485);
		ee->chd_rs485 = !!(invert & FTDI_EE_4232H_CHD_RS485);
		ee->cha_txden = !!(invert & FTDI_EE_4232H_CHA_TXDEN);
		ee->chb_txden = !!(invert & FTDI_EE_4232H_CHB_TXDEN);

		ee->num_cbus = 0;
		break;
	}

	default:
		ee->channel_a_type = FTDI_EE_CHAN_UART;
		ee->channel_b_type = FTDI_EE_CHAN_UART;
		ee->num_cbus = 0;
		break;
	}
}

int ftdi_eeprom_read(struct ftdi_mpsse_dev *fdev)
{
	struct usb_interface *intf = ftdi_mpsse_dev_get_intf(fdev);
	struct usb_device *udev = ftdi_mpsse_dev_get_udev(fdev);
	enum ftdi_mpsse_chip_type chip = ftdi_mpsse_dev_get_chip(fdev);
	struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(fdev);
	u8 *buf;
	bool all_ff = true;
	int i, ret;

	ee->size = FTDI_EEPROM_MAX_WORDS;
	ee->valid = false;
	ee->empty = true;

	buf = kmalloc(2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < ee->size; i++) {
		ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				      FTDI_SIO_READ_EEPROM_REQUEST,
				      FTDI_SIO_REQTYPE_IN,
				      0, i, buf, 2, FTDI_EE_READ_TIMEOUT);
		if (ret < 2) {
			dev_warn(&intf->dev,
				 "EEPROM read failed at word %d: %d\n",
				 i, ret);
			kfree(buf);
			return 0;	/* non-fatal */
		}

		ee->data[i * 2] = buf[0];
		ee->data[i * 2 + 1] = buf[1];

		if (buf[0] != 0xff || buf[1] != 0xff)
			all_ff = false;
	}

	kfree(buf);

	ee->empty = all_ff;
	ee->valid = true;

	if (all_ff) {
		dev_info(&intf->dev, "EEPROM: empty (all 0xFF)\n");
		return 0;
	}

	ee->checksum_ok = ftdi_eeprom_checksum_ok(ee);
	if (!ee->checksum_ok)
		dev_warn(&intf->dev, "EEPROM: checksum mismatch\n");

	ftdi_eeprom_decode(fdev, ee, chip);

	dev_info(&intf->dev,
		 "EEPROM: VID=0x%04x PID=0x%04x chan_a=%d checksum=%s\n",
		 ee->vid, ee->pid, ee->channel_a_type,
		 ee->checksum_ok ? "ok" : "BAD");

	return 0;
}

u8 ftdi_eeprom_channel_type(const struct ftdi_eeprom *ee, u8 channel)
{
	switch (channel) {
	case FTDI_CHANNEL_B:
		return ee->channel_b_type;
	case FTDI_CHANNEL_C:
		return ee->channel_c_type;
	case FTDI_CHANNEL_D:
		return ee->channel_d_type;
	default:
		return ee->channel_a_type;
	}
}

static struct ftdi_mpsse_dev *ftdi_mpsse_dev_from_dev(struct device *dev)
{
	struct usb_interface *intf = to_usb_interface(dev);

	return usb_get_intfdata(intf);
}

static struct ftdi_eeprom *ftdi_eeprom_from_dev(struct device *dev)
{
	return ftdi_mpsse_dev_get_eeprom_ptr(ftdi_mpsse_dev_from_dev(dev));
}

static ssize_t eeprom_valid_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	return sysfs_emit(buf, "%d\n", ee->valid);
}
static DEVICE_ATTR_RO(eeprom_valid);

static ssize_t eeprom_empty_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	return sysfs_emit(buf, "%d\n", ee->empty);
}
static DEVICE_ATTR_RO(eeprom_empty);

static ssize_t eeprom_checksum_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%s\n", ee->checksum_ok ? "ok" : "bad");
}
static DEVICE_ATTR_RO(eeprom_checksum);

static ssize_t eeprom_vid_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "0x%04x\n", ee->vid);
}
static DEVICE_ATTR_RO(eeprom_vid);

static ssize_t eeprom_pid_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "0x%04x\n", ee->pid);
}
static DEVICE_ATTR_RO(eeprom_pid);

static const char *ftdi_ee_chan_type_str(u8 type)
{
	switch (type) {
	case FTDI_EE_CHAN_UART:		return "uart";
	case FTDI_EE_CHAN_FIFO:		return "fifo";
	case FTDI_EE_CHAN_OPTO:		return "opto";
	case FTDI_EE_CHAN_CPU:		return "cpu";
	case FTDI_EE_CHAN_FT1284:	return "ft1284";
	default:			return "unknown";
	}
}

static ssize_t eeprom_channel_type_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_dev_from_dev(dev);
	struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(fdev);
	u8 channel = ftdi_mpsse_dev_get_channel(fdev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%s\n",
			  ftdi_ee_chan_type_str(
				  ftdi_eeprom_channel_type(ee, channel)));
}
static DEVICE_ATTR_RO(eeprom_channel_type);

static ssize_t eeprom_self_powered_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%d\n", ee->self_powered);
}
static DEVICE_ATTR_RO(eeprom_self_powered);

static ssize_t eeprom_remote_wakeup_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%d\n", ee->remote_wakeup);
}
static DEVICE_ATTR_RO(eeprom_remote_wakeup);

static ssize_t eeprom_max_power_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%u\n", ee->max_power_ma);
}
static DEVICE_ATTR_RO(eeprom_max_power);

static const char *ftdi_cbush_func_str(u8 func)
{
	static const char * const names[] = {
		[FTDI_CBUSH_TRISTATE]	= "tristate",
		[FTDI_CBUSH_TXLED]	= "txled",
		[FTDI_CBUSH_RXLED]	= "rxled",
		[FTDI_CBUSH_TXRXLED]	= "txrxled",
		[FTDI_CBUSH_PWREN]	= "pwren",
		[FTDI_CBUSH_SLEEP]	= "sleep",
		[FTDI_CBUSH_DRIVE_0]	= "drive_0",
		[FTDI_CBUSH_DRIVE_1]	= "drive_1",
		[FTDI_CBUSH_IOMODE]	= "iomode",
		[FTDI_CBUSH_TXDEN]	= "txden",
		[FTDI_CBUSH_CLK30]	= "clk30",
		[FTDI_CBUSH_CLK15]	= "clk15",
		[FTDI_CBUSH_CLK7_5]	= "clk7_5",
	};

	if (func < ARRAY_SIZE(names) && names[func])
		return names[func];

	return "unknown";
}

static ssize_t eeprom_cbus_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);
	ssize_t len = 0;
	int i;

	if (!ee->valid || ee->empty || ee->num_cbus == 0)
		return sysfs_emit(buf, "n/a\n");

	for (i = 0; i < ee->num_cbus; i++)
		len += sysfs_emit_at(buf, len, "CBUS%d: %s\n",
				     i, ftdi_cbush_func_str(ee->cbus[i]));

	return len;
}
static DEVICE_ATTR_RO(eeprom_cbus);

static ssize_t eeprom_drive_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	static const unsigned int ma_table[] = { 4, 8, 12, 16 };
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);
	ssize_t len = 0;
	u8 drv;

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	drv = ee->group0_drive;
	len += sysfs_emit_at(buf, len,
			     "group0: %umA slew=%s schmitt=%s\n",
			     ma_table[drv & FTDI_EE_DRIVE_MA_MASK],
			     (drv & FTDI_EE_DRIVE_SLOW_SLEW) ? "slow" : "fast",
			     str_on_off(drv & FTDI_EE_DRIVE_SCHMITT));

	drv = ee->group1_drive;
	len += sysfs_emit_at(buf, len,
			     "group1: %umA slew=%s schmitt=%s\n",
			     ma_table[drv & FTDI_EE_DRIVE_MA_MASK],
			     (drv & FTDI_EE_DRIVE_SLOW_SLEW) ? "slow" : "fast",
			     str_on_off(drv & FTDI_EE_DRIVE_SCHMITT));

	return len;
}
static DEVICE_ATTR_RO(eeprom_drive);

static const char *ftdi_ee_chan_type_ini(u8 type)
{
	switch (type) {
	case FTDI_EE_CHAN_UART:		return "UART";
	case FTDI_EE_CHAN_FIFO:		return "FIFO";
	case FTDI_EE_CHAN_OPTO:		return "OPTO";
	case FTDI_EE_CHAN_CPU:		return "CPU";
	case FTDI_EE_CHAN_FT1284:	return "FT1284";
	default:			return "UART";
	}
}

static const char *ftdi_cbush_func_ini(u8 func)
{
	static const char * const names[] = {
		[FTDI_CBUSH_TRISTATE]	= "TRISTATE",
		[FTDI_CBUSH_TXLED]	= "TXLED",
		[FTDI_CBUSH_RXLED]	= "RXLED",
		[FTDI_CBUSH_TXRXLED]	= "TXRXLED",
		[FTDI_CBUSH_PWREN]	= "PWREN",
		[FTDI_CBUSH_SLEEP]	= "SLEEP",
		[FTDI_CBUSH_DRIVE_0]	= "DRIVE_0",
		[FTDI_CBUSH_DRIVE_1]	= "DRIVE_1",
		[FTDI_CBUSH_IOMODE]	= "IOMODE",
		[FTDI_CBUSH_TXDEN]	= "TXDEN",
		[FTDI_CBUSH_CLK30]	= "CLK30",
		[FTDI_CBUSH_CLK15]	= "CLK15",
		[FTDI_CBUSH_CLK7_5]	= "CLK7_5",
	};

	if (func < ARRAY_SIZE(names) && names[func])
		return names[func];

	return "UNKNOWN";
}

static ssize_t eeprom_config_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);
	struct usb_device *udev = ftdi_mpsse_dev_get_udev(fdev);
	enum ftdi_mpsse_chip_type chip = ftdi_mpsse_dev_get_chip(fdev);
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);
	u8 channel = ftdi_mpsse_dev_get_channel(fdev);
	ssize_t len = 0;
	bool is_ft232h, is_ft2232h, is_ft4232h;
	int i;

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "# EEPROM not available or empty\n");

	if (!ee->checksum_ok)
		return sysfs_emit(buf, "# EEPROM checksum bad, data unreliable\n");

	is_ft232h = (chip == FTDI_CHIP_FT232H ||
		     chip == FTDI_CHIP_FT232HP ||
		     chip == FTDI_CHIP_FT233HP);
	is_ft2232h = (chip == FTDI_CHIP_FT2232H ||
		      chip == FTDI_CHIP_FT2232HP ||
		      chip == FTDI_CHIP_FT2233HP);
	is_ft4232h = (chip == FTDI_CHIP_FT4232H ||
		      chip == FTDI_CHIP_FT4232HA ||
		      chip == FTDI_CHIP_FT4232HP ||
		      chip == FTDI_CHIP_FT4233HP);

	len += sysfs_emit_at(buf, len, "vendor_id=0x%04x\n", ee->vid);
	len += sysfs_emit_at(buf, len, "product_id=0x%04x\n", ee->pid);

	if (ee->manufacturer[0]) {
		len += sysfs_emit_at(buf, len, "manufacturer=%s\n",
				     ee->manufacturer);
		if (udev->manufacturer &&
		    strcmp(ee->manufacturer, udev->manufacturer))
			len += sysfs_emit_at(buf, len,
					     "# WARNING: USB device reports manufacturer=\"%s\"\n",
					     udev->manufacturer);
	} else if (udev->manufacturer) {
		len += sysfs_emit_at(buf, len,
				     "# manufacturer not in EEPROM, USB device reports \"%s\"\n",
				     udev->manufacturer);
	}

	if (ee->product[0]) {
		len += sysfs_emit_at(buf, len, "product=%s\n", ee->product);
		if (udev->product &&
		    strcmp(ee->product, udev->product))
			len += sysfs_emit_at(buf, len,
					     "# WARNING: USB device reports product=\"%s\"\n",
					     udev->product);
	} else if (udev->product) {
		len += sysfs_emit_at(buf, len,
				     "# product not in EEPROM, USB device reports \"%s\"\n",
				     udev->product);
	}

	if (ee->serial[0]) {
		len += sysfs_emit_at(buf, len, "serial=%s\n", ee->serial);
		if (udev->serial && strcmp(ee->serial, udev->serial))
			len += sysfs_emit_at(buf, len,
					     "# WARNING: USB device reports serial=\"%s\"\n",
					     udev->serial);
	} else if (udev->serial) {
		len += sysfs_emit_at(buf, len,
				     "# serial not in EEPROM, USB device reports \"%s\"\n",
				     udev->serial);
	}

	len += sysfs_emit_at(buf, len, "self_powered=%s\n",
			     str_true_false(ee->self_powered));
	len += sysfs_emit_at(buf, len, "remote_wakeup=%s\n",
			     str_true_false(ee->remote_wakeup));
	len += sysfs_emit_at(buf, len, "max_power=%u\n", ee->max_power_ma);
	len += sysfs_emit_at(buf, len, "use_serial=%s\n",
			     str_true_false(ee->use_serial));
	len += sysfs_emit_at(buf, len, "suspend_pull_downs=%s\n",
			     str_true_false(ee->pulldown));

	/* Per-channel config: show this interface's channel */
	if (is_ft4232h) {
		static const char * const ch_prefix[] = {
			[FTDI_CHANNEL_A] = "cha",
			[FTDI_CHANNEL_B] = "chb",
			[FTDI_CHANNEL_C] = "chc",
			[FTDI_CHANNEL_D] = "chd",
		};
		const char *pfx = (channel >= FTDI_CHANNEL_A &&
				    channel <= FTDI_CHANNEL_D) ?
				   ch_prefix[channel] : "ch?";
		u8 chan_type = ftdi_eeprom_channel_type(ee, channel);
		bool vcp = false;
		bool rs485 = false;

		switch (channel) {
		case FTDI_CHANNEL_A:
			vcp = ee->cha_vcp;
			rs485 = ee->cha_rs485;
			break;
		case FTDI_CHANNEL_B:
			vcp = ee->chb_vcp;
			rs485 = ee->chb_rs485;
			break;
		case FTDI_CHANNEL_C:
			vcp = ee->chc_vcp;
			rs485 = ee->chc_rs485;
			break;
		case FTDI_CHANNEL_D:
			vcp = ee->chd_vcp;
			rs485 = ee->chd_rs485;
			break;
		}

		len += sysfs_emit_at(buf, len, "%s_type=%s\n",
				     pfx, ftdi_ee_chan_type_ini(chan_type));
		len += sysfs_emit_at(buf, len, "%s_vcp=%s\n",
				     pfx, str_true_false(vcp));
		len += sysfs_emit_at(buf, len, "%s_rs485=%s\n",
				     pfx, str_true_false(rs485));
	} else {
		len += sysfs_emit_at(buf, len, "cha_type=%s\n",
				     ftdi_ee_chan_type_ini(ee->channel_a_type));
		if (is_ft232h || is_ft2232h)
			len += sysfs_emit_at(buf, len, "cha_vcp=%s\n",
					     str_true_false(ee->cha_vcp));
		if (is_ft2232h) {
			len += sysfs_emit_at(buf, len, "chb_type=%s\n",
					     ftdi_ee_chan_type_ini(ee->channel_b_type));
			len += sysfs_emit_at(buf, len, "chb_vcp=%s\n",
					     str_true_false(ee->chb_vcp));
		}
	}

	len += sysfs_emit_at(buf, len, "group0_drive=%u\n",
			     ee->group0_drive_ma);
	len += sysfs_emit_at(buf, len, "group0_schmitt=%s\n",
			     str_true_false(ee->group0_schmitt));
	if (!ee->group0_schmitt)
		len += sysfs_emit_at(buf, len,
				     "# WARNING: Schmitt trigger off on group0 (%s)\n",
				     is_ft232h ? "ACBUS" : "AL");
	len += sysfs_emit_at(buf, len, "group0_slew=%s\n",
			     ee->group0_slow_slew ? "slow" : "fast");
	if (!ee->group0_slow_slew)
		len += sysfs_emit_at(buf, len,
				     "# WARNING: slew rate not limited on group0 (%s)\n",
				     is_ft232h ? "ACBUS" : "AL");

	len += sysfs_emit_at(buf, len, "group1_drive=%u\n",
			     ee->group1_drive_ma);
	len += sysfs_emit_at(buf, len, "group1_schmitt=%s\n",
			     str_true_false(ee->group1_schmitt));
	if (!ee->group1_schmitt)
		len += sysfs_emit_at(buf, len,
				     "# WARNING: Schmitt trigger off on group1 (%s)\n",
				     is_ft232h ? "ADBUS" : "AH");
	len += sysfs_emit_at(buf, len, "group1_slew=%s\n",
			     ee->group1_slow_slew ? "slow" : "fast");
	if (!ee->group1_slow_slew)
		len += sysfs_emit_at(buf, len,
				     "# WARNING: slew rate not limited on group1 (%s)\n",
				     is_ft232h ? "ADBUS" : "AH");

	for (i = 0; i < ee->num_cbus; i++)
		len += sysfs_emit_at(buf, len, "cbush%d=%s\n",
				     i, ftdi_cbush_func_ini(ee->cbus[i]));

	return len;
}
static DEVICE_ATTR_RO(eeprom_config);

static struct attribute *ftdi_eeprom_text_attrs[] = {
	&dev_attr_eeprom_valid.attr,
	&dev_attr_eeprom_empty.attr,
	&dev_attr_eeprom_checksum.attr,
	&dev_attr_eeprom_vid.attr,
	&dev_attr_eeprom_pid.attr,
	&dev_attr_eeprom_channel_type.attr,
	&dev_attr_eeprom_self_powered.attr,
	&dev_attr_eeprom_remote_wakeup.attr,
	&dev_attr_eeprom_max_power.attr,
	&dev_attr_eeprom_cbus.attr,
	&dev_attr_eeprom_drive.attr,
	&dev_attr_eeprom_config.attr,
	NULL,
};

static ssize_t eeprom_read(struct file *filp, struct kobject *kobj,
			   const struct bin_attribute *attr,
			   char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid)
		return -ENODATA;

	if (off >= FTDI_EEPROM_MAX_BYTES)
		return 0;
	if (off + count > FTDI_EEPROM_MAX_BYTES)
		count = FTDI_EEPROM_MAX_BYTES - off;

	memcpy(buf, ee->data + off, count);

	return count;
}
static BIN_ATTR_RO(eeprom, FTDI_EEPROM_MAX_BYTES);

static const struct bin_attribute *const ftdi_eeprom_bin_attrs[] = {
	&bin_attr_eeprom,
	NULL,
};

const struct attribute_group ftdi_eeprom_attr_group = {
	.attrs = ftdi_eeprom_text_attrs,
	.bin_attrs = ftdi_eeprom_bin_attrs,
};
