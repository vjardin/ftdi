/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI device emulation -- USB descriptors, control requests, MPSSE engine
 *
 * Emulates an FTDI Hi-Speed device (FT232H / FT2232H / FT4232H) well enough
 * for the ftdi_mpsse kernel driver to probe successfully over USB/IP.
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftdi_emu.h"

/* USB descriptor types */
#define USB_DT_DEVICE		0x01
#define USB_DT_CONFIG		0x02
#define USB_DT_STRING		0x03
#define USB_DT_INTERFACE	0x04
#define USB_DT_ENDPOINT		0x05

/* USB request types */
#define USB_DIR_IN		0x80
#define USB_TYPE_STANDARD	0x00
#define USB_TYPE_VENDOR		0x40
#define USB_RECIP_DEVICE	0x00

/* Standard USB requests */
#define USB_REQ_GET_STATUS	0x00
#define USB_REQ_SET_ADDRESS	0x05
#define USB_REQ_GET_DESCRIPTOR	0x06
#define USB_REQ_SET_CONFIG	0x09
#define USB_REQ_SET_INTERFACE	0x0B

/* EEPROM layout offsets */
#define EE_OFF_CHAN_A		0x00
#define EE_OFF_CHAN_B		0x01
#define EE_OFF_VID_LO		0x02
#define EE_OFF_VID_HI		0x03
#define EE_OFF_PID_LO		0x04
#define EE_OFF_PID_HI		0x05
#define EE_OFF_REL_LO		0x06
#define EE_OFF_REL_HI		0x07
#define EE_OFF_CFG_DESC		0x08
#define EE_OFF_MAX_POWER	0x09
#define EE_OFF_CHIP_CFG		0x0A
#define EE_OFF_MFR_OFF		0x0E
#define EE_OFF_MFR_LEN		0x0F
#define EE_OFF_PROD_OFF		0x10
#define EE_OFF_PROD_LEN		0x11
#define EE_OFF_SER_OFF		0x12
#define EE_OFF_SER_LEN		0x13

/* Predefined string descriptors (indices into our string table) */
#define STR_IDX_LANGID		0
#define STR_IDX_MANUFACTURER	1
#define STR_IDX_PRODUCT		2
#define STR_IDX_SERIAL		3

static const char *default_manufacturer = "FTDI";
static const char *default_serial = "FT000001";

static const char *chip_product_name(enum ftdi_chip chip)
{
	switch (chip) {
	case CHIP_FT232H:  return "FT232H";
	case CHIP_FT2232H: return "FT2232H";
	case CHIP_FT4232H: return "FT4232H";
	}
	return "FTDI";
}

static int chip_num_interfaces(enum ftdi_chip chip)
{
	switch (chip) {
	case CHIP_FT232H:  return 1;
	case CHIP_FT2232H: return 2;
	case CHIP_FT4232H: return 4;
	}
	return 1;
}

static uint16_t chip_pid(enum ftdi_chip chip)
{
	switch (chip) {
	case CHIP_FT232H:  return FTDI_PID_FT232H;
	case CHIP_FT2232H: return FTDI_PID_FT2232H;
	case CHIP_FT4232H: return FTDI_PID_FT4232H;
	}
	return FTDI_PID_FT232H;
}

static uint16_t chip_bcd(enum ftdi_chip chip)
{
	switch (chip) {
	case CHIP_FT232H:  return FTDI_BCD_FT232H;
	case CHIP_FT2232H: return FTDI_BCD_FT2232H;
	case CHIP_FT4232H: return FTDI_BCD_FT4232H;
	}
	return FTDI_BCD_FT232H;
}

/* Rotate a 16-bit value left by n bits */
static uint16_t rol16(uint16_t val, unsigned int n)
{
	return (val << n) | (val >> (16 - n));
}

/*
 * Compute the FTDI EEPROM checksum and store it in the last word.
 * Algorithm: start with 0xAAAA, XOR each 16-bit LE word, rotate left by 1.
 * The stored checksum makes the full-pass result zero.
 */
static void eeprom_fix_checksum_raw(uint8_t *ee)
{
	uint16_t checksum = 0xAAAA;
	int i;

	for (i = 0; i < FTDI_EEPROM_WORDS - 1; i++) {
		uint16_t w = ee[i * 2] | (ee[i * 2 + 1] << 8);

		checksum ^= w;
		checksum = rol16(checksum, 1);
	}
	/* Store so full pass yields zero */
	ee[254] = checksum & 0xFF;
	ee[255] = (checksum >> 8) & 0xFF;
}

/*
 * Write a USB string descriptor (UTF-16LE) into the EEPROM at the given
 * byte offset.  Returns the total descriptor length (including header).
 */
static int eeprom_write_string(uint8_t *ee, int offset,
			       const char *str)
{
	int slen = strlen(str);
	int desc_len = 2 + slen * 2;
	int i;

	ee[offset] = desc_len;
	ee[offset + 1] = USB_DT_STRING;
	for (i = 0; i < slen; i++) {
		ee[offset + 2 + i * 2] = str[i];
		ee[offset + 2 + i * 2 + 1] = 0;
	}
	return desc_len;
}

void ftdi_emu_default_eeprom(struct ftdi_device *dev)
{
	uint8_t *ee = dev->eeprom;
	int off;
	int len;

	memset(ee, 0xFF, FTDI_EEPROM_SIZE);
	memset(ee, 0, 0x18);

	/*
	 * Channel type determines operating mode:
	 * 0x00 = UART mode (bitmode RESET, no MPSSE)
	 * 0x0F = unknown type (driver uses auto-detection)
	 *
	 * Byte 0x00 layout for FT232H:
	 *   Bits [3:0] = channel type
	 *   Bit 4 = VCP driver (if set, device appears as COM port)
	 *
	 * Set to 0x0F (unknown type, VCP=0) to trigger MPSSE auto mode.
	 * Do NOT set 0xFF (which sets VCP=1 and forces UART mode).
	 */
	ee[EE_OFF_CHAN_A] = 0x0F;	/* Unknown type, VCP disabled */
	ee[EE_OFF_CHAN_B] = 0x0F;

	ee[EE_OFF_VID_LO] = dev->vid & 0xFF;
	ee[EE_OFF_VID_HI] = (dev->vid >> 8) & 0xFF;
	ee[EE_OFF_PID_LO] = dev->pid & 0xFF;
	ee[EE_OFF_PID_HI] = (dev->pid >> 8) & 0xFF;
	ee[EE_OFF_REL_LO] = dev->bcd & 0xFF;
	ee[EE_OFF_REL_HI] = (dev->bcd >> 8) & 0xFF;

	ee[EE_OFF_CFG_DESC] = 0x80;	/* Bus-powered */
	ee[EE_OFF_MAX_POWER] = 0x32;	/* 100 mA (50 * 2) */
	ee[EE_OFF_CHIP_CFG] = 0x08;	/* use_serial = 1 */

	/* Drive strength defaults (4mA, fast slew, no Schmitt) */
	ee[0x0C] = 0x00;
	ee[0x0D] = 0x00;

	/* Protocol hint at offset 0x1A - set by main.c via --mode option */

	/* Strings: manufacturer at 0xA0, product after, serial after that */
	off = 0xA0;
	len = eeprom_write_string(ee, off, default_manufacturer);
	ee[EE_OFF_MFR_OFF] = off;
	ee[EE_OFF_MFR_LEN] = len;
	off += len;

	len = eeprom_write_string(ee, off, chip_product_name(dev->chip));
	ee[EE_OFF_PROD_OFF] = off;
	ee[EE_OFF_PROD_LEN] = len;
	off += len;

	len = eeprom_write_string(ee, off, default_serial);
	ee[EE_OFF_SER_OFF] = off;
	ee[EE_OFF_SER_LEN] = len;

	eeprom_fix_checksum_raw(ee);
}

void ftdi_emu_fix_checksum(struct ftdi_device *dev)
{
	eeprom_fix_checksum_raw(dev->eeprom);
}

void ftdi_emu_init(struct ftdi_device *dev, enum ftdi_chip chip)
{
	int i;

	memset(dev, 0, sizeof(*dev));
	dev->chip = chip;
	dev->vid = FTDI_VID;
	dev->pid = chip_pid(chip);
	dev->bcd = chip_bcd(chip);
	dev->num_interfaces = chip_num_interfaces(chip);

	for (i = 0; i < dev->num_interfaces; i++) {
		dev->intf[i].latency = 16;
		dev->intf[i].bitmode = FTDI_BITMODE_RESET;
		dev->intf[i].clk_div5 = true;
		dev->intf[i].error_mode = FTDI_ERR_NONE;
		dev->intf[i].error_count = 0;
		dev->intf[i].i2c_byte_count = 0;
	}

	ftdi_emu_default_eeprom(dev);
}

void ftdi_emu_set_error(struct ftdi_device *dev, int intf_idx,
			enum ftdi_error_mode mode, int count)
{
	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return;

	dev->intf[intf_idx].error_mode = mode;
	dev->intf[intf_idx].error_count = count;
	dev->intf[intf_idx].i2c_byte_count = 0;

	fprintf(stderr, "emu: error injection set: intf=%d mode=%d count=%d\n",
		intf_idx, mode, count);
}

void ftdi_emu_clear_errors(struct ftdi_device *dev)
{
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		dev->intf[i].error_mode = FTDI_ERR_NONE;
		dev->intf[i].error_count = 0;
		dev->intf[i].i2c_byte_count = 0;
	}
}

int ftdi_emu_load_eeprom(struct ftdi_device *dev, const char *path)
{
	FILE *f;
	size_t n;

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -1;
	}
	n = fread(dev->eeprom, 1, FTDI_EEPROM_SIZE, f);
	fclose(f);

	if (n < FTDI_EEPROM_SIZE) {
		fprintf(stderr, "%s: short read (%zu bytes, need %d)\n",
			path, n, FTDI_EEPROM_SIZE);
		return -1;
	}

	dev->eeprom_loaded = true;
	return 0;
}

/* --- USB Descriptors ----------------------------------------------- */

int ftdi_emu_device_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen)
{
	uint8_t desc[18] = {
		18,			/* bLength */
		USB_DT_DEVICE,		/* bDescriptorType */
		0x00, 0x02,		/* bcdUSB 2.00 */
		0x00,			/* bDeviceClass (per-interface) */
		0x00,			/* bDeviceSubClass */
		0x00,			/* bDeviceProtocol */
		64,			/* bMaxPacketSize0 */
		dev->vid & 0xFF,	/* idVendor */
		(dev->vid >> 8) & 0xFF,
		dev->pid & 0xFF,	/* idProduct */
		(dev->pid >> 8) & 0xFF,
		dev->bcd & 0xFF,	/* bcdDevice */
		(dev->bcd >> 8) & 0xFF,
		STR_IDX_MANUFACTURER,	/* iManufacturer */
		STR_IDX_PRODUCT,	/* iProduct */
		STR_IDX_SERIAL,		/* iSerialNumber */
		1,			/* bNumConfigurations */
	};

	int len = maxlen < 18 ? maxlen : 18;

	memcpy(buf, desc, len);
	return len;
}

int ftdi_emu_config_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen)
{
	int nintf = dev->num_interfaces;
	/* Config (9) + per-interface: Interface (9) + EP-IN (7) + EP-OUT (7) = 23 */
	int full_len = 9 + nintf * (9 + 7 + 7);
	int return_len = (maxlen < full_len) ? maxlen : full_len;
	int pos = 0;
	int i;
	uint8_t ep_in, ep_out;

	memset(buf, 0, return_len);

	/* Configuration descriptor */
	buf[pos++] = 9;			/* bLength */
	buf[pos++] = USB_DT_CONFIG;		/* bDescriptorType */
	buf[pos++] = full_len & 0xFF;		/* wTotalLength (actual full length) */
	buf[pos++] = (full_len >> 8) & 0xFF;
	buf[pos++] = nintf;			/* bNumInterfaces */
	buf[pos++] = 1;			/* bConfigurationValue */
	buf[pos++] = 0;			/* iConfiguration */
	buf[pos++] = 0x80;			/* bmAttributes: bus-powered */
	buf[pos++] = 50;			/* bMaxPower: 100 mA */

	for (i = 0; i < nintf && pos + 23 <= return_len; i++) {
		/*
		 * FTDI convention: interface N uses
		 * bulk-IN = 0x81 + 2*N, bulk-OUT = 0x02 + 2*N
		 */
		ep_in  = 0x81 + i * 2;
		ep_out = 0x02 + i * 2;

		/* Interface descriptor */
		buf[pos++] = 9;
		buf[pos++] = USB_DT_INTERFACE;
		buf[pos++] = i;		/* bInterfaceNumber */
		buf[pos++] = 0;		/* bAlternateSetting */
		buf[pos++] = 2;		/* bNumEndpoints */
		buf[pos++] = 0xFF;		/* bInterfaceClass: vendor */
		buf[pos++] = 0xFF;		/* bInterfaceSubClass: vendor */
		buf[pos++] = 0xFF;		/* bInterfaceProtocol: vendor */
		buf[pos++] = 0;		/* iInterface */

		/* Bulk IN endpoint */
		buf[pos++] = 7;
		buf[pos++] = USB_DT_ENDPOINT;
		buf[pos++] = ep_in;		/* bEndpointAddress */
		buf[pos++] = 0x02;		/* bmAttributes: bulk */
		buf[pos++] = 0x00;		/* wMaxPacketSize = 512 */
		buf[pos++] = 0x02;
		buf[pos++] = 0;		/* bInterval */

		/* Bulk OUT endpoint */
		buf[pos++] = 7;
		buf[pos++] = USB_DT_ENDPOINT;
		buf[pos++] = ep_out;		/* bEndpointAddress */
		buf[pos++] = 0x02;		/* bmAttributes: bulk */
		buf[pos++] = 0x00;		/* wMaxPacketSize = 512 */
		buf[pos++] = 0x02;
		buf[pos++] = 0;		/* bInterval */
	}

	return pos;
}

int ftdi_emu_string_descriptor(const struct ftdi_device *dev,
			       int index, uint16_t langid,
			       uint8_t *buf, int maxlen)
{
	const char *str;
	int slen, dlen, i;

	(void)langid;

	if (index == STR_IDX_LANGID) {
		/* Language ID descriptor: English (US) */
		if (maxlen < 4)
			return maxlen;
		buf[0] = 4;
		buf[1] = USB_DT_STRING;
		buf[2] = 0x09;
		buf[3] = 0x04;
		return 4;
	}

	switch (index) {
	case STR_IDX_MANUFACTURER:
		str = default_manufacturer;
		break;
	case STR_IDX_PRODUCT:
		str = chip_product_name(dev->chip);
		break;
	case STR_IDX_SERIAL:
		str = default_serial;
		break;
	default:
		return -1;
	}

	slen = strlen(str);
	dlen = 2 + slen * 2;
	if (dlen > maxlen)
		dlen = maxlen;

	buf[0] = dlen;
	buf[1] = USB_DT_STRING;
	for (i = 0; i < slen && 2 + i * 2 + 1 < dlen; i++) {
		buf[2 + i * 2] = str[i];
		buf[2 + i * 2 + 1] = 0;
	}

	return dlen;
}

/* --- Control Requests ---------------------------------------------- */

/*
 * Map wIndex to a 0-based interface index.
 * The driver sends 1-based channel numbers in wIndex for vendor requests.
 */
static int intf_from_windex(const struct ftdi_device *dev, uint16_t wIndex)
{
	int idx;

	if (wIndex == 0)
		return 0;

	idx = (wIndex & 0xFF) - 1;
	if (idx < 0 || idx >= dev->num_interfaces)
		return 0;

	return idx;
}

static int handle_vendor_out(struct ftdi_device *dev,
			     uint8_t bRequest, uint16_t wValue,
			     uint16_t wIndex)
{
	int intf = intf_from_windex(dev, wIndex);
	struct ftdi_intf_state *st = &dev->intf[intf];

	switch (bRequest) {
	case FTDI_REQ_RESET:
		if (wValue == 0) {
			st->bitmode = FTDI_BITMODE_RESET;
		}
		/* wValue 1/2 = purge RX/TX -- nothing to do in emulation */
		return 0;

	case FTDI_REQ_SET_MODEM_CTRL:
		st->modem_ctrl = wValue;
		return 0;

	case FTDI_REQ_SET_FLOW_CTRL:
		st->flow_ctrl = wIndex >> 8;
		return 0;

	case FTDI_REQ_SET_BAUDRATE:
		st->baudrate = wValue;
		return 0;

	case FTDI_REQ_SET_DATA:
		st->data_cfg = wValue;
		return 0;

	case FTDI_REQ_SET_EVENT_CHAR:
	case FTDI_REQ_SET_ERROR_CHAR:
		return 0;

	case FTDI_REQ_SET_LATENCY_TIMER:
		st->latency = wValue & 0xFF;
		return 0;

	case FTDI_REQ_SET_BITMODE:
		st->bitmode = (wValue >> 8) & 0xFF;
		st->gpio_dir_low = wValue & 0xFF;
		st->i2c_byte_count = 0;  /* Reset I2C byte counter */
		return 0;

	case FTDI_REQ_WRITE_EEPROM:
		if (wIndex < FTDI_EEPROM_WORDS) {
			dev->eeprom[wIndex * 2] = wValue & 0xFF;
			dev->eeprom[wIndex * 2 + 1] = (wValue >> 8) & 0xFF;
		}
		return 0;

	case FTDI_REQ_ERASE_EEPROM:
		memset(dev->eeprom, 0xFF, FTDI_EEPROM_SIZE);
		return 0;

	case FTDI_REQ_SET_ERROR_MODE:
		/*
		 * Error injection control for testing.
		 * wValue low byte: error mode (FTDI_ERR_*)
		 * wValue high byte: error count (0 = infinite)
		 * wIndex: interface number (1-based like other FTDI requests)
		 */
		ftdi_emu_set_error(dev, intf,
				   wValue & 0xFF,
				   (wValue >> 8) & 0xFF);
		return 0;

	default:
		return -1;
	}
}

static int handle_vendor_in(struct ftdi_device *dev,
			    uint8_t bRequest, uint16_t wValue,
			    uint16_t wIndex, uint16_t wLength,
			    uint8_t *resp)
{
	int intf = intf_from_windex(dev, wIndex);
	struct ftdi_intf_state *st = &dev->intf[intf];

	(void)wValue;

	switch (bRequest) {
	case FTDI_REQ_GET_MODEM_STATUS:
		if (wLength < 2)
			return -1;
		resp[0] = FTDI_MODEM_STATUS_DEFAULT;
		resp[1] = FTDI_LINE_STATUS_DEFAULT;
		return 2;

	case FTDI_REQ_GET_LATENCY_TIMER:
		if (wLength < 1)
			return -1;
		resp[0] = st->latency;
		return 1;

	case FTDI_REQ_READ_PINS:
		if (wLength < 1)
			return -1;
		resp[0] = st->gpio_low;
		return 1;

	case FTDI_REQ_READ_EEPROM:
		if (wLength < 2)
			return -1;
		if (wIndex >= FTDI_EEPROM_WORDS)
			return -1;
		resp[0] = dev->eeprom[wIndex * 2];
		resp[1] = dev->eeprom[wIndex * 2 + 1];
		return 2;

	default:
		return -1;
	}
}

static int handle_get_descriptor(const struct ftdi_device *dev,
				 uint16_t wValue, uint16_t wLength,
				 uint8_t *resp)
{
	uint8_t type = wValue >> 8;
	uint8_t index = wValue & 0xFF;
	int len;

	switch (type) {
	case USB_DT_DEVICE:
		len = ftdi_emu_device_descriptor(dev, resp, wLength);
		return len;

	case USB_DT_CONFIG:
		len = ftdi_emu_config_descriptor(dev, resp, wLength);
		return len;

	case USB_DT_STRING:
		len = ftdi_emu_string_descriptor(dev, index, 0, resp, wLength);
		return len;

	default:
		return -1;
	}
}

int ftdi_emu_control(struct ftdi_device *dev,
		     uint8_t bmRequestType, uint8_t bRequest,
		     uint16_t wValue, uint16_t wIndex, uint16_t wLength,
		     uint8_t *resp_buf)
{
	/* Vendor OUT (host -> device, no data returned) */
	if (bmRequestType == 0x40)
		return handle_vendor_out(dev, bRequest, wValue, wIndex);

	/* Vendor IN (device -> host) */
	if (bmRequestType == 0xC0)
		return handle_vendor_in(dev, bRequest, wValue, wIndex,
					wLength, resp_buf);

	/* Standard GET_DESCRIPTOR */
	if (bmRequestType == 0x80 && bRequest == USB_REQ_GET_DESCRIPTOR)
		return handle_get_descriptor(dev, wValue, wLength, resp_buf);

	/* Standard SET_CONFIGURATION -- accept silently */
	if (bmRequestType == 0x00 && bRequest == USB_REQ_SET_CONFIG)
		return 0;

	/* Standard SET_INTERFACE -- accept silently */
	if (bmRequestType == 0x01 && bRequest == USB_REQ_SET_INTERFACE)
		return 0;

	/* Standard SET_ADDRESS -- handled by USB/IP layer */
	if (bmRequestType == 0x00 && bRequest == USB_REQ_SET_ADDRESS)
		return 0;

	/* Standard GET_STATUS -- 2 bytes of zeros (not self-powered) */
	if (bmRequestType == 0x80 && bRequest == USB_REQ_GET_STATUS) {
		if (wLength >= 2) {
			resp_buf[0] = 0;
			resp_buf[1] = 0;
			return 2;
		}
		return 0;
	}

	fprintf(stderr, "emu: unhandled control: type=0x%02x req=0x%02x "
		"val=0x%04x idx=0x%04x len=%u\n",
		bmRequestType, bRequest, wValue, wIndex, wLength);
	return -1;
}

/* --- MPSSE Engine -------------------------------------------------- */

/*
 * Process MPSSE commands from bulk OUT.
 * We only need to handle the subset the ftdi_mpsse driver sends
 * during probe and normal operation.
 */
void ftdi_emu_bulk_out(struct ftdi_device *dev, int intf_idx,
		       const uint8_t *data, int len)
{
	struct ftdi_intf_state *st;
	int i = 0;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return;

	st = &dev->intf[intf_idx];
	st->resp_len = 0;

	/*
	 * In UART mode, bulk OUT is serial TX data.
	 * Generate counter pattern response for testing.
	 */
	if (st->bitmode == FTDI_BITMODE_RESET) {
		int room = sizeof(st->resp_buf) - st->resp_len;
		int n = len < room ? len : room;
		int j;

		/*
		 * For each TX byte, generate one RX byte as a counter:
		 * 0xA0, 0xA1, 0xA2, ... (matches SPI/I2C test pattern)
		 */
		for (j = 0; j < n; j++)
			st->resp_buf[st->resp_len++] = 0xA0 + st->read_counter++;
		return;
	}

	/*
	 * In async bit-bang mode, each bulk OUT byte sets pin values.
	 * Only output pins (set by direction mask in SET_BITMODE) are
	 * driven; input pins retain their previous state.
	 */
	if (st->bitmode == FTDI_BITMODE_BITBANG) {
		if (len > 0) {
			uint8_t last = data[len - 1];
			st->gpio_low = (st->gpio_low & ~st->gpio_dir_low) |
					(last & st->gpio_dir_low);
		}
		return;
	}

	while (i < len) {
		uint8_t cmd = data[i++];
		int nbytes;

		switch (cmd) {
		case MPSSE_SET_BITS_LOW:
			if (i + 2 > len)
				goto done;
			st->gpio_low = data[i++];
			st->gpio_dir_low = data[i++];
			break;

		case MPSSE_GET_BITS_LOW:
			if (st->resp_len < (int)sizeof(st->resp_buf)) {
				uint8_t val = st->gpio_low;

				/*
				 * Frozen bus simulation: SDA (AD1/AD2) stuck low.
				 * This causes bus recovery to be triggered.
				 */
				if (st->error_mode == FTDI_ERR_I2C_BUS_STUCK) {
					/* AD1 (SDA_OUT) and AD2 (SDA_IN) stuck low */
					val &= ~0x06;
				}
				st->resp_buf[st->resp_len++] = val;
			}
			break;

		case MPSSE_SET_BITS_HIGH:
			if (i + 2 > len)
				goto done;
			st->gpio_high = data[i++];
			st->gpio_dir_high = data[i++];
			break;

		case MPSSE_GET_BITS_HIGH:
			if (st->resp_len < (int)sizeof(st->resp_buf))
				st->resp_buf[st->resp_len++] = st->gpio_high;
			break;

		case MPSSE_LOOPBACK_ON:
			st->loopback = true;
			break;

		case MPSSE_LOOPBACK_OFF:
			st->loopback = false;
			break;

		case MPSSE_SET_CLK_DIVISOR:
			if (i + 2 > len)
				goto done;
			i += 2; /* consume divisorL, divisorH */
			break;

		case MPSSE_SEND_IMMEDIATE:
			/* Flush -- our responses are already buffered */
			break;

		case MPSSE_DISABLE_CLK_DIV5:
			st->clk_div5 = false;
			break;

		case MPSSE_ENABLE_CLK_DIV5:
			st->clk_div5 = true;
			break;

		case MPSSE_ENABLE_3PHASE:
			st->three_phase = true;
			break;

		case MPSSE_DISABLE_3PHASE:
			st->three_phase = false;
			break;

		case MPSSE_ENABLE_ADAPTIVE:
			st->adaptive = true;
			break;

		case MPSSE_DISABLE_ADAPTIVE:
			st->adaptive = false;
			break;

		case MPSSE_DRIVE_ZERO_ONLY:
			if (i + 2 > len)
				goto done;
			st->drive_zero = true;
			i += 2; /* consume low/high byte mask */
			break;

		default:
			/*
			 * Data clocking commands (0x10-0x3F) have a length
			 * field.  Handle the common byte-granularity cases.
			 *
			 * For testing, read data returns a counter-based pattern
			 * (0xA0, 0xA1, 0xA2, ...) which tests can verify.
			 * In 3-phase I2C mode (bit reads), return ACK (0x00).
			 */
			if ((cmd & 0xF0) >= 0x10 && (cmd & 0xF0) <= 0x30) {
				bool bit_mode = (cmd & 0x02);
				bool read = (cmd & 0x20);
				bool write = (cmd & 0x10);

				if (bit_mode) {
					/* Bit mode: 1 byte count */
					if (i + 1 > len)
						goto done;
					nbytes = 1;
					int nbits = data[i++] + 1;

					if (write) {
						if (i + 1 > len)
							goto done;
						i += 1;
						/* Track I2C write bytes for targeted NAK */
						if (st->three_phase && nbits == 8)
							st->i2c_byte_count++;
					}
					if (read) {
						if (st->resp_len < (int)sizeof(st->resp_buf)) {
							if (nbits == 8) {
								/*
								 * 8-bit data read (I2C data):
								 * Return counter pattern for testing.
								 */
								st->resp_buf[st->resp_len++] =
									0xA0 + st->read_counter++;
							} else {
								/*
								 * 1-bit ACK read (I2C ACK/NACK):
								 * Return 0x00 (ACK) in 3-phase mode,
								 * or inject NAK for error testing.
								 */
								uint8_t ack = st->three_phase ? 0x00 : 0xFF;

								/* I2C NAK injection */
								if (st->error_mode == FTDI_ERR_I2C_NAK &&
								    st->three_phase) {
									ack = 0x01; /* NAK: bit 0 set */
									if (st->error_count > 0) {
										st->error_count--;
										if (st->error_count == 0)
											st->error_mode = FTDI_ERR_NONE;
									}
									fprintf(stderr,
										"emu: I2C NAK injected at byte %d\n",
										st->i2c_byte_count);
								}
								st->resp_buf[st->resp_len++] = ack;
							}
						}
					}
				} else {
					/* Byte mode: 2 byte count (LE) + 1 */
					if (i + 2 > len)
						goto done;
					nbytes = data[i] | (data[i + 1] << 8);
					nbytes += 1;
					i += 2;

					if (write) {
						if (i + nbytes > len)
							goto done;
						i += nbytes;
					}
					if (read) {
						int room = sizeof(st->resp_buf) - st->resp_len;
						int n = nbytes < room ? nbytes : room;
						int j;

						/*
						 * Return counter-based pattern for testing:
						 * 0xA0, 0xA1, 0xA2, ... (wraps at 0xFF)
						 */
						for (j = 0; j < n; j++) {
							st->resp_buf[st->resp_len++] =
								0xA0 + st->read_counter++;
						}
					}
				}
				break;
			}

			/* Unknown command -- reply with bad-command marker */
			if (st->resp_len + 2 <= (int)sizeof(st->resp_buf)) {
				st->resp_buf[st->resp_len++] = MPSSE_BAD_CMD;
				st->resp_buf[st->resp_len++] = cmd;
			}
			break;
		}
	}
done:
	return;
}

int ftdi_emu_bulk_in(struct ftdi_device *dev, int intf_idx,
		     uint8_t *buf, int maxlen)
{
	struct ftdi_intf_state *st;
	int payload;
	int total;

	if (maxlen < FTDI_STATUS_BYTES)
		return 0;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces) {
		/* Invalid interface -- return minimal status */
		buf[0] = FTDI_MODEM_STATUS_DEFAULT;
		buf[1] = FTDI_LINE_STATUS_DEFAULT;
		return FTDI_STATUS_BYTES;
	}

	st = &dev->intf[intf_idx];

	/*
	 * USB stall simulation: return -1 to indicate stall.
	 * Caller should translate to USB STALL response.
	 */
	if (st->error_mode == FTDI_ERR_USB_STALL) {
		if (st->error_count > 0) {
			st->error_count--;
			if (st->error_count == 0)
				st->error_mode = FTDI_ERR_NONE;
		}
		fprintf(stderr, "emu: USB STALL injected on bulk IN\n");
		return -1;
	}

	/*
	 * USB timeout simulation: return 0 to indicate no data.
	 * This causes the host to timeout waiting for a response.
	 */
	if (st->error_mode == FTDI_ERR_USB_TIMEOUT) {
		if (st->error_count > 0) {
			st->error_count--;
			if (st->error_count == 0)
				st->error_mode = FTDI_ERR_NONE;
		}
		fprintf(stderr, "emu: USB timeout injected on bulk IN\n");
		return 0;
	}

	/* Every bulk-IN packet starts with the 2-byte FTDI status header */
	buf[0] = FTDI_MODEM_STATUS_DEFAULT;
	buf[1] = FTDI_LINE_STATUS_DEFAULT;

	payload = st->resp_len;
	if (payload > maxlen - FTDI_STATUS_BYTES)
		payload = maxlen - FTDI_STATUS_BYTES;

	if (payload > 0) {
		memcpy(buf + FTDI_STATUS_BYTES, st->resp_buf, payload);
		/* Shift any remaining data forward */
		st->resp_len -= payload;
		if (st->resp_len > 0) {
			memmove(st->resp_buf,
				st->resp_buf + payload,
				st->resp_len);
		}
	}

	total = FTDI_STATUS_BYTES + payload;
	return total;
}
