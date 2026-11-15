// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI EEPROM reader, decoder and sysfs/debugfs interface, part of
 * ftdi_mpsse.ko
 *
 * The EEPROM is read at probe with vendor requests; its per-chip fields
 * (channel type, CBUS functions, drive strength) are decoded, shown one
 * value per sysfs attribute, and dumped as tables in debugfs.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/string_helpers.h>
#include <linux/usb.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define FTDI_EE_READ_TIMEOUT	5000	/* ms */

static bool ftdi_eeprom_checksum_ok(const struct ftdi_eeprom *ee)
{
	u16 checksum = 0xaaaa;
	u16 i, words = ee->size;

	/*
	 * A 93C46 (64 words) reads back mirrored, words 64-127 repeat words
	 * 0-63, and its checksum is word 63 (same size detection as libftdi).
	 */
	if (words == 128 && !memcmp(ee->data, ee->data + 128, 128))
		words = 64;

	for (i = 0; i < words; i++) {
		u16 word = ee->data[i * 2] | (ee->data[i * 2 + 1] << 8);

		checksum ^= word;
		checksum = rol16(checksum, 1);
	}

	return checksum == 0;
}

/*
 * Strings are stored as USB string descriptors (bLength, bDescriptorType
 * 0x03, UTF-16LE characters); the bytes at off_byte / len_byte hold the
 * offset and the total length of the descriptor in the image.
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

/* Drive groups in libftdi order: their pins, their nibble in ee->drive[] */
struct ftdi_ee_drive_group {
	const char *pins;
	u8 nibble;
};

static const struct ftdi_ee_drive_group ftdi_ee_groups_232h[] = {
	{ "ACBUS", 0 }, { "ADBUS", 2 },
};

static const struct ftdi_ee_drive_group ftdi_ee_groups_2232h[] = {
	{ "AL", 0 }, { "AH", 1 }, { "BL", 2 }, { "BH", 3 },
};

static const struct ftdi_ee_drive_group ftdi_ee_groups_4232h[] = {
	{ "A", 0 }, { "B", 1 }, { "C", 2 }, { "D", 3 },
};

static const struct ftdi_ee_drive_group *
ftdi_ee_drive_groups(enum ftdi_mpsse_chip_type chip, unsigned int *count)
{
	if (ftdi_chip_is_232h(chip)) {
		*count = ARRAY_SIZE(ftdi_ee_groups_232h);
		return ftdi_ee_groups_232h;
	}
	if (ftdi_chip_is_2232h(chip)) {
		*count = ARRAY_SIZE(ftdi_ee_groups_2232h);
		return ftdi_ee_groups_2232h;
	}
	if (ftdi_chip_is_4232h(chip)) {
		*count = ARRAY_SIZE(ftdi_ee_groups_4232h);
		return ftdi_ee_groups_4232h;
	}
	*count = 0;
	return NULL;
}

int ftdi_eeprom_dbus_drive(const struct ftdi_eeprom *ee,
			   enum ftdi_mpsse_chip_type chip, u8 channel)
{
	if (ftdi_chip_is_232h(chip))
		return ee->drive[2];
	if (ftdi_chip_is_2232h(chip) && channel <= FTDI_CHANNEL_B)
		return ee->drive[channel == FTDI_CHANNEL_B ? 2 : 0];
	if (ftdi_chip_is_4232h(chip) && channel >= FTDI_CHANNEL_A &&
	    channel <= FTDI_CHANNEL_D)
		return ee->drive[channel - FTDI_CHANNEL_A];
	return -ENODATA;
}

/*
 * The tag is searched in the whole image: the unused area depends on the
 * chip and on the EEPROM size (libftdi: from 0x19 on FT2232H and FT4232H,
 * from 0x1f on FT232H, up to the strings), no offset is free everywhere.
 */
static bool ftdi_eeprom_tag_char(u8 c)
{
	/* ASCII only: islower() also takes Latin-1 letters, 0xFF included */
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ',';
}

static void ftdi_eeprom_find_bus_mode(struct ftdi_eeprom *ee)
{
	static const char tag[] = FTDI_EE_BUS_MODE_TAG;
	const unsigned int tlen = sizeof(tag) - 1;
	unsigned int i, n = 0;

	ee->bus_mode[0] = '\0';

	for (i = 0; i + tlen < FTDI_EEPROM_MAX_BYTES; i++)
		if (!memcmp(ee->data + i, tag, tlen))
			break;
	ee->bus_mode_tag = i + tlen < FTDI_EEPROM_MAX_BYTES;

	/* Not found: i is past the last possible position, nothing to copy */
	for (i += tlen; i < FTDI_EEPROM_MAX_BYTES; i++) {
		u8 c = ee->data[i];

		if (n == sizeof(ee->bus_mode) - 1 || !ftdi_eeprom_tag_char(c))
			break;
		ee->bus_mode[n++] = c;
	}
	ee->bus_mode[n] = '\0';
}

static void ftdi_eeprom_decode(struct ftdi_eeprom *ee,
			       enum ftdi_mpsse_chip_type chip)
{
	u8 cfg_desc, chip_cfg;
	int i;

	ee->vid = ee->data[FTDI_EE_VID_L] |
		  (ee->data[FTDI_EE_VID_L + 1] << 8);
	ee->pid = ee->data[FTDI_EE_PID_L] |
		  (ee->data[FTDI_EE_PID_L + 1] << 8);

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

	ee->drive[0] = ee->data[FTDI_EE_DRIVE0] & 0x0f;
	ee->drive[1] = ee->data[FTDI_EE_DRIVE0] >> 4;
	ee->drive[2] = ee->data[FTDI_EE_DRIVE1] & 0x0f;
	ee->drive[3] = ee->data[FTDI_EE_DRIVE1] >> 4;

	ftdi_eeprom_find_bus_mode(ee);

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

		/* No FIFO/CPU/OPTO modes, no channel type field (libftdi) */
		ee->channel_a_type = FTDI_EE_CHAN_UART;
		ee->channel_b_type = FTDI_EE_CHAN_UART;
		ee->channel_c_type = FTDI_EE_CHAN_UART;
		ee->channel_d_type = FTDI_EE_CHAN_UART;

		/* A/B: bit 3 of bytes 0x00/0x01, C/D: their bit 7 (libftdi) */
		ee->cha_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_2232H_VCP);
		ee->chb_vcp = !!(ee->data[FTDI_EE_CHAN_B] & FTDI_EE_2232H_VCP);
		ee->chc_vcp = !!(ee->data[FTDI_EE_CHAN_A] & FTDI_EE_4232H_CHC_VCP);
		ee->chd_vcp = !!(ee->data[FTDI_EE_CHAN_B] & FTDI_EE_4232H_CHD_VCP);

		ee->cha_rs485 = !!(invert & FTDI_EE_4232H_CHA_RS485);
		ee->chb_rs485 = !!(invert & FTDI_EE_4232H_CHB_RS485);
		ee->chc_rs485 = !!(invert & FTDI_EE_4232H_CHC_RS485);
		ee->chd_rs485 = !!(invert & FTDI_EE_4232H_CHD_RS485);

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

/* A failed read leaves valid false: nothing is taken from the EEPROM */
void ftdi_eeprom_read(struct ftdi_mpsse_dev *fdev)
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
	ee->empty = false;

	buf = kmalloc(2, GFP_KERNEL);
	if (!buf)
		return;

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
			return;
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
		return;
	}

	ee->checksum_ok = ftdi_eeprom_checksum_ok(ee);
	if (!ee->checksum_ok)
		dev_warn(&intf->dev, "EEPROM: checksum mismatch\n");

	ftdi_eeprom_decode(ee, chip);

	dev_info(&intf->dev,
		 "EEPROM: VID=0x%04x PID=0x%04x chan_a=%d checksum=%s\n",
		 ee->vid, ee->pid, ee->channel_a_type,
		 ee->checksum_ok ? "ok" : "BAD");
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

bool ftdi_eeprom_channel_vcp(const struct ftdi_eeprom *ee, u8 channel)
{
	switch (channel) {
	case FTDI_CHANNEL_B:
		return ee->chb_vcp;
	case FTDI_CHANNEL_C:
		return ee->chc_vcp;
	case FTDI_CHANNEL_D:
		return ee->chd_vcp;
	default:
		return ee->cha_vcp;
	}
}

static bool ftdi_eeprom_channel_rs485(const struct ftdi_eeprom *ee,
				      u8 channel)
{
	switch (channel) {
	case FTDI_CHANNEL_B:
		return ee->chb_rs485;
	case FTDI_CHANNEL_C:
		return ee->chc_rs485;
	case FTDI_CHANNEL_D:
		return ee->chd_rs485;
	default:
		return ee->cha_rs485;
	}
}

/*
 * RS-485 transmit enable, asserted by the chip around every character:
 *   FT232H   the ACBUS pin with the TXDEN function in the EEPROM: ACBUS0
 *            to 6, 8 or 9, not ACBUS7 (DS_FT232H 3.4)
 *   FT2232H  xCBUS0 of a channel in its RS232 configuration, whatever the
 *            EEPROM holds (DS_FT2232H table 3.5)
 *   FT4232H  xDBUS7, if the EEPROM selects TXDEN there instead of RI#
 *            (DS_FT4232H table 3.1, 4.6)
 */
const char *ftdi_eeprom_txden_pin(const struct ftdi_eeprom *ee,
				  enum ftdi_mpsse_chip_type chip, u8 channel)
{
	static const char * const acbus[] = {
		"ACBUS0", "ACBUS1", "ACBUS2", "ACBUS3", "ACBUS4",
		"ACBUS5", "ACBUS6", "ACBUS7", "ACBUS8", "ACBUS9",
	};
	static const char * const cbus0[] = { "ACBUS0", "BCBUS0" };
	static const char * const dbus7[] = {
		"ADBUS7", "BDBUS7", "CDBUS7", "DDBUS7",
	};
	unsigned int ch = channel - FTDI_CHANNEL_A;
	int i;

	switch (chip) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		if (!ee)
			return NULL;
		for (i = 0; i < ee->num_cbus && i < ARRAY_SIZE(acbus); i++)
			if (i != 7 && ee->cbus[i] == FTDI_CBUSH_TXDEN)
				return acbus[i];
		return NULL;

	case FTDI_CHIP_FT2232H:
	case FTDI_CHIP_FT2232HP:
	case FTDI_CHIP_FT2233HP:
		return ch < ARRAY_SIZE(cbus0) ? cbus0[ch] : NULL;

	case FTDI_CHIP_FT4232H:
	case FTDI_CHIP_FT4232HA:
	case FTDI_CHIP_FT4232HP:
	case FTDI_CHIP_FT4233HP:
		if (!ee || ch >= ARRAY_SIZE(dbus7) ||
		    !ftdi_eeprom_channel_rs485(ee, channel))
			return NULL;
		return dbus7[ch];

	default:
		return NULL;
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

/* Names of ftdi_eeprom (libftdi): its cha_type and cbushN values */
static const char *ftdi_ee_chan_type_name(u8 type)
{
	switch (type) {
	case FTDI_EE_CHAN_UART:		return "UART";
	case FTDI_EE_CHAN_FIFO:		return "FIFO";
	case FTDI_EE_CHAN_OPTO:		return "OPTO";
	case FTDI_EE_CHAN_CPU:		return "CPU";
	case FTDI_EE_CHAN_FT1284:	return "FT1284";
	default:			return NULL;
	}
}

static const char *ftdi_cbush_func_name(u8 func)
{
	static const char * const names[] = {
		[FTDI_CBUSH_TRISTATE]	= "TRISTATE",
		[FTDI_CBUSH_TXLED]	= "TXLED",
		[FTDI_CBUSH_RXLED]	= "RXLED",
		[FTDI_CBUSH_TXRXLED]	= "TXRXLED",
		[FTDI_CBUSH_PWREN]	= "PWREN",
		[FTDI_CBUSH_SLEEP]	= "SLEEP",
		[FTDI_CBUSH_DRIVE_0]	= "DRIVE_0",
		[FTDI_CBUSH_DRIVE_1]	= "DRIVE1",
		[FTDI_CBUSH_IOMODE]	= "IOMODE",
		[FTDI_CBUSH_TXDEN]	= "TXDEN",
		[FTDI_CBUSH_CLK30]	= "CLK30",
		[FTDI_CBUSH_CLK15]	= "CLK15",
		[FTDI_CBUSH_CLK7_5]	= "CLK7_5",
	};

	return func < ARRAY_SIZE(names) ? names[func] : NULL;
}

static ssize_t eeprom_channel_type_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_dev_from_dev(dev);
	struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(fdev);
	u8 channel = ftdi_mpsse_dev_get_channel(fdev);
	const char *name;
	char lower[8];

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	name = ftdi_ee_chan_type_name(ftdi_eeprom_channel_type(ee, channel));
	if (!name)
		return sysfs_emit(buf, "unknown\n");
	string_lower(lower, name);
	return sysfs_emit(buf, "%s\n", lower);
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

static ssize_t eeprom_bus_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(dev);

	if (!ee->valid || ee->empty)
		return sysfs_emit(buf, "n/a\n");

	return sysfs_emit(buf, "%s\n", ee->bus_mode[0] ? ee->bus_mode : "none");
}
static DEVICE_ATTR_RO(eeprom_bus_mode);

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
	&dev_attr_eeprom_bus_mode.attr,
	NULL,
};

static ssize_t eeprom_read(struct file *filp, struct kobject *kobj,
			   const struct bin_attribute *attr,
			   char *buf, loff_t off, size_t count)
{
	struct ftdi_eeprom *ee = ftdi_eeprom_from_dev(kobj_to_dev(kobj));

	if (!ee->valid)
		return -ENODATA;

	return memory_read_from_buffer(buf, count, &off, ee->data,
				       sizeof(ee->data));
}

/* Raw image incl. user area: root only, like in-tree EEPROM drivers */
static const BIN_ATTR_ADMIN_RO(eeprom, FTDI_EEPROM_MAX_BYTES);

static const struct bin_attribute *const ftdi_eeprom_bin_attrs[] = {
	&bin_attr_eeprom,
	NULL,
};

const struct attribute_group ftdi_eeprom_attr_group = {
	.attrs = ftdi_eeprom_text_attrs,
	.bin_attrs = ftdi_eeprom_bin_attrs,
};

/* debugfs: the tables, one file each */

static int ftdi_eeprom_cbus_show(struct seq_file *s, void *unused)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(s->private);
	const char *name;
	int i;

	if (!ee->valid || ee->empty || !ee->num_cbus) {
		seq_puts(s, "n/a\n");
		return 0;
	}

	for (i = 0; i < ee->num_cbus; i++) {
		name = ftdi_cbush_func_name(ee->cbus[i]);
		if (name)
			seq_printf(s, "CBUS%d: %s\n", i, name);
		else
			seq_printf(s, "CBUS%d: 0x%x (unknown)\n", i, ee->cbus[i]);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ftdi_eeprom_cbus);

static int ftdi_eeprom_drive_show(struct seq_file *s, void *unused)
{
	struct ftdi_mpsse_dev *fdev = s->private;
	const struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(fdev);
	const struct ftdi_ee_drive_group *groups;
	unsigned int i, count;

	if (!ee->valid || ee->empty) {
		seq_puts(s, "n/a\n");
		return 0;
	}

	groups = ftdi_ee_drive_groups(ftdi_mpsse_dev_get_chip(fdev), &count);
	for (i = 0; i < count; i++) {
		u8 drv = ee->drive[groups[i].nibble];

		seq_printf(s, "group%u (%s): %umA slew=%s schmitt=%s\n",
			   i, groups[i].pins,
			   (unsigned int)FTDI_EE_DRIVE_MA(drv),
			   (drv & FTDI_EE_DRIVE_SLOW_SLEW) ? "slow" : "fast",
			   str_on_off(drv & FTDI_EE_DRIVE_SCHMITT));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ftdi_eeprom_drive);

/*
 * A string as ftdi_eeprom reads it, on one line: quoted, with what would
 * end or escape the quotes, and control characters, replaced by '?'.
 */
static void ftdi_eeprom_quoted(struct seq_file *s, const char *val)
{
	seq_putc(s, '"');
	for (; *val; val++)
		seq_putc(s, (*val < 0x20 || *val > 0x7e || *val == '"' ||
			     *val == '\\') ? '?' : *val);
	seq_puts(s, "\"\n");
}

static void ftdi_eeprom_config_str(struct seq_file *s, const char *key,
				   const char *val)
{
	seq_printf(s, "%s=", key);
	ftdi_eeprom_quoted(s, val);
}

/* The EEPROM string differs from what the device reports */
static bool ftdi_eeprom_str_differs(const char *ee_str, const char *usb_str)
{
	const char *p;

	if (!usb_str)
		return false;
	/* The decode keeps ASCII only and FTDI_EE_STR_MAX - 1 characters */
	if (strlen(usb_str) >= FTDI_EE_STR_MAX)
		return false;
	for (p = usb_str; *p; p++)
		if (*p < 0x20 || *p > 0x7e)
			return false;
	return strcmp(ee_str, usb_str);
}

static void ftdi_eeprom_config_string(struct seq_file *s, const char *key,
				      const char *ee_str, const char *usb_str)
{
	if (ee_str[0]) {
		ftdi_eeprom_config_str(s, key, ee_str);
		if (ftdi_eeprom_str_differs(ee_str, usb_str)) {
			seq_printf(s, "# WARNING: USB device reports %s ", key);
			ftdi_eeprom_quoted(s, usb_str);
		}
	} else if (usb_str) {
		seq_printf(s, "# %s not in EEPROM, USB device reports ", key);
		ftdi_eeprom_quoted(s, usb_str);
	}
}

static void ftdi_eeprom_config_type(struct seq_file *s, const char *key,
				    u8 type)
{
	const char *name = ftdi_ee_chan_type_name(type);

	if (name)
		seq_printf(s, "%s=%s\n", key, name);
	else
		seq_printf(s, "# %s: unknown value 0x%02x\n", key, type);
}

/* Key names and value syntax of ftdi_eeprom (libftdi) */
static int ftdi_eeprom_config_show(struct seq_file *s, void *unused)
{
	struct ftdi_mpsse_dev *fdev = s->private;
	struct usb_device *udev = ftdi_mpsse_dev_get_udev(fdev);
	enum ftdi_mpsse_chip_type chip = ftdi_mpsse_dev_get_chip(fdev);
	const struct ftdi_eeprom *ee = ftdi_mpsse_dev_get_eeprom_ptr(fdev);
	u8 channel = ftdi_mpsse_dev_get_channel(fdev);
	const struct ftdi_ee_drive_group *groups;
	unsigned int g, count;
	const char *name;
	int i;

	if (!ee->valid || ee->empty) {
		seq_puts(s, "# EEPROM not available or empty\n");
		return 0;
	}
	if (!ee->checksum_ok) {
		seq_puts(s, "# EEPROM checksum bad, data unreliable\n");
		return 0;
	}

	seq_printf(s, "vendor_id=0x%04x\n", ee->vid);
	seq_printf(s, "product_id=0x%04x\n", ee->pid);
	ftdi_eeprom_config_string(s, "manufacturer", ee->manufacturer,
				  udev->manufacturer);
	ftdi_eeprom_config_string(s, "product", ee->product, udev->product);
	ftdi_eeprom_config_string(s, "serial", ee->serial, udev->serial);
	seq_printf(s, "self_powered=%s\n", str_true_false(ee->self_powered));
	seq_printf(s, "remote_wakeup=%s\n", str_true_false(ee->remote_wakeup));
	seq_printf(s, "max_power=%u\n", ee->max_power_ma);
	seq_printf(s, "use_serial=%s\n", str_true_false(ee->use_serial));
	seq_printf(s, "suspend_pull_downs=%s\n", str_true_false(ee->pulldown));

	if (ftdi_chip_is_4232h(chip)) {
		/* No channel type: the VCP and RS485 bits of this channel */
		char pfx = 'a' + channel - FTDI_CHANNEL_A;

		seq_printf(s, "ch%c_vcp=%s\n", pfx,
			   str_true_false(ftdi_eeprom_channel_vcp(ee, channel)));
		seq_printf(s, "ch%c_rs485=%s\n", pfx,
			   str_true_false(ftdi_eeprom_channel_rs485(ee, channel)));
	} else {
		ftdi_eeprom_config_type(s, "cha_type", ee->channel_a_type);
		seq_printf(s, "cha_vcp=%s\n", str_true_false(ee->cha_vcp));
		if (ftdi_chip_is_2232h(chip)) {
			ftdi_eeprom_config_type(s, "chb_type",
						ee->channel_b_type);
			seq_printf(s, "chb_vcp=%s\n",
				   str_true_false(ee->chb_vcp));
		}
	}

	groups = ftdi_ee_drive_groups(chip, &count);
	for (g = 0; g < count; g++) {
		u8 drv = ee->drive[groups[g].nibble];

		seq_printf(s, "group%u_drive=%uMA\n",
			   g, (unsigned int)FTDI_EE_DRIVE_MA(drv));
		seq_printf(s, "group%u_schmitt=%s\n", g,
			   str_true_false(drv & FTDI_EE_DRIVE_SCHMITT));
		seq_printf(s, "group%u_slew=%s\n", g,
			   (drv & FTDI_EE_DRIVE_SLOW_SLEW) ? "slow" : "fast");
	}

	for (i = 0; i < ee->num_cbus; i++) {
		name = ftdi_cbush_func_name(ee->cbus[i]);
		if (name)
			seq_printf(s, "cbush%d=%s\n", i, name);
		else
			seq_printf(s, "# cbush%d: unknown function 0x%x\n",
				   i, ee->cbus[i]);
	}

	/* Written with user_data_addr / user_data_file, not a config key */
	if (ee->bus_mode[0])
		seq_printf(s, "# user area tag: bus_mode=%s\n", ee->bus_mode);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ftdi_eeprom_config);

void ftdi_eeprom_debugfs_init(struct ftdi_mpsse_dev *fdev, struct dentry *dir)
{
	debugfs_create_file("eeprom_cbus", 0444, dir, fdev,
			    &ftdi_eeprom_cbus_fops);
	debugfs_create_file("eeprom_drive", 0444, dir, fdev,
			    &ftdi_eeprom_drive_fops);
	debugfs_create_file("eeprom_config", 0444, dir, fdev,
			    &ftdi_eeprom_config_fops);
}
