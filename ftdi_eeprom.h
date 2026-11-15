/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI EEPROM reader and decoder of ftdi_mpsse.ko
 *
 * Children read struct ftdi_eeprom (ftdi_mpsse_get_eeprom()) and its field
 * encodings from here; the functions are internal to ftdi_mpsse.ko, which
 * passes what depends on chip and channel (the TXDEN pin) in the platform
 * data.
 */

#ifndef __FTDI_EEPROM_H
#define __FTDI_EEPROM_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/types.h>

struct ftdi_mpsse_dev;

#define FTDI_EEPROM_MAX_WORDS		128
#define FTDI_EEPROM_MAX_BYTES		(FTDI_EEPROM_MAX_WORDS * 2)

/* Common byte offsets */
#define FTDI_EE_CHAN_A			0x00
#define FTDI_EE_CHAN_B			0x01
#define FTDI_EE_VID_L			0x02
#define FTDI_EE_PID_L			0x04
#define FTDI_EE_CFG_DESC		0x08
#define FTDI_EE_MAX_POWER		0x09
#define FTDI_EE_CHIP_CFG		0x0a
#define FTDI_EE_INVERT			0x0b
#define FTDI_EE_DRIVE0			0x0c
#define FTDI_EE_DRIVE1			0x0d
#define FTDI_EE_STR_MFR_OFF		0x0e
#define FTDI_EE_STR_MFR_LEN		0x0f
#define FTDI_EE_STR_PROD_OFF		0x10
#define FTDI_EE_STR_PROD_LEN		0x11
#define FTDI_EE_STR_SER_OFF		0x12
#define FTDI_EE_STR_SER_LEN		0x13

#define FTDI_EE_232H_CBUS_BASE		0x18	/* 5 bytes, 10 nibbles */

/* Channel type values (bits [3:0] of byte 0x00 / 0x01) */
#define FTDI_EE_CHAN_UART		0x00
#define FTDI_EE_CHAN_FIFO		0x01
#define FTDI_EE_CHAN_OPTO		0x02
#define FTDI_EE_CHAN_CPU		0x04
#define FTDI_EE_CHAN_FT1284		0x08

/* Config descriptor bits (byte 0x08) */
#define FTDI_EE_CFG_SELF_POWERED	BIT(6)
#define FTDI_EE_CFG_REMOTE_WAKEUP	BIT(5)

/* Chip config bits (byte 0x0a) */
#define FTDI_EE_CFG_PULLDOWN		BIT(2)
#define FTDI_EE_CFG_USE_SERIAL		BIT(3)

/* Drive nibble encoding */
#define FTDI_EE_DRIVE_MA_MASK		GENMASK(1, 0)
#define FTDI_EE_DRIVE_SLOW_SLEW		BIT(2)
#define FTDI_EE_DRIVE_SCHMITT		BIT(3)
#define FTDI_EE_DRIVE_MA(nibble)	((((nibble) & FTDI_EE_DRIVE_MA_MASK) + 1) * 4)

/*
 * The drive nibbles of bytes 0x0c/0x0d and their pins; groups (gN) numbered
 * as in libftdi and ftdi_eeprom. xL = xDBUS of a channel, xH = its xCBUS.
 *
 *	          0x0c low     0x0c high    0x0d low     0x0d high
 *	FT232H    ACBUS (g0)   -            ADBUS (g1)   -
 *	FT2232H   AL (g0)      AH (g1)      BL (g2)      BH (g3)
 *	FT4232H   A (g0)       B (g1)       C (g2)       D (g3)
 */
#define FTDI_EE_DRIVE_NIBBLES		4

/*
 * VCP driver bits (byte 0x00 / 0x01): set = the channel is a virtual COM
 * port, clear = "D2XX direct" (bit-bang, MPSSE). FT4232H has no channel
 * type field: its bytes 0x00/0x01 only hold these bits.
 */
#define FTDI_EE_232H_VCP		BIT(4)	/* byte 0x00, FT232H */
#define FTDI_EE_2232H_VCP		BIT(3)	/* byte 0x00/0x01: A/B */
#define FTDI_EE_4232H_CHC_VCP		BIT(7)	/* byte 0x00, FT4232H chan C */
#define FTDI_EE_4232H_CHD_VCP		BIT(7)	/* byte 0x01, FT4232H chan D */

/* FT4232H byte 0x0b: RS485 (TXDEN on the channel's RI pin), libftdi layout */
#define FTDI_EE_4232H_CHA_RS485	BIT(4)
#define FTDI_EE_4232H_CHB_RS485	BIT(5)
#define FTDI_EE_4232H_CHC_RS485	BIT(6)
#define FTDI_EE_4232H_CHD_RS485	BIT(7)

/*
 * Optional protocol tag in the user area (the EEPROM has no field for SPI
 * or I2C): this text, then a value in the syntax of the bus_mode parameter
 * ("i2c", "spi,uart"), up to the first byte that is not a lower-case
 * letter, a digit or a comma.
 */
#define FTDI_EE_BUS_MODE_TAG		"bus_mode="
#define FTDI_EE_BUS_MODE_MAX		48

#define FTDI_EE_STR_MAX			64

/* CBUSH function values (FT232H ACBUS mux) */
#define FTDI_CBUSH_TRISTATE		0x00
#define FTDI_CBUSH_TXLED		0x01
#define FTDI_CBUSH_RXLED		0x02
#define FTDI_CBUSH_TXRXLED		0x03
#define FTDI_CBUSH_PWREN		0x04
#define FTDI_CBUSH_SLEEP		0x05
#define FTDI_CBUSH_DRIVE_0		0x06
#define FTDI_CBUSH_DRIVE_1		0x07
#define FTDI_CBUSH_IOMODE		0x08
#define FTDI_CBUSH_TXDEN		0x09
#define FTDI_CBUSH_CLK30		0x0a
#define FTDI_CBUSH_CLK15		0x0b
#define FTDI_CBUSH_CLK7_5		0x0c

struct ftdi_eeprom {
	u8 data[FTDI_EEPROM_MAX_BYTES];	/* raw bytes, as-read (LE) */
	u16 size;			/* in words */
	bool valid;			/* read succeeded */
	bool empty;			/* all 0xFF */
	bool checksum_ok;

	u16 vid;
	u16 pid;
	char manufacturer[FTDI_EE_STR_MAX];
	char product[FTDI_EE_STR_MAX];
	char serial[FTDI_EE_STR_MAX];
	u8 channel_a_type;		/* FTDI_EE_CHAN_*: FT232H, FT2232H */
	u8 channel_b_type;		/* FT2232H */
	u8 channel_c_type;		/* FT4232H: no type field, UART */
	u8 channel_d_type;
	bool self_powered;
	bool remote_wakeup;
	bool pulldown;
	bool use_serial;
	u16 max_power_ma;
	u8 cbus[10];			/* FT232H: ACBUS0-9 functions */
	int num_cbus;			/* 10 for FT232H, 0 otherwise */
	/* Raw drive nibbles: 0x0c low, 0x0c high, 0x0d low, 0x0d high */
	u8 drive[FTDI_EE_DRIVE_NIBBLES];

	/* VCP driver bit, see FTDI_EE_*_VCP */
	bool cha_vcp;
	bool chb_vcp;			/* FT2232H / FT4232H */
	bool chc_vcp;			/* FT4232H */
	bool chd_vcp;			/* FT4232H */

	/* FT4232H per-channel RS485 / TXDEN */
	bool cha_rs485;
	bool chb_rs485;
	bool chc_rs485;
	bool chd_rs485;

	/* Value of the "bus_mode=" tag of the user area, "" if none */
	char bus_mode[FTDI_EE_BUS_MODE_MAX];
	bool bus_mode_tag;		/* the tag is there, value or not */
};

enum ftdi_mpsse_chip_type;

void ftdi_eeprom_read(struct ftdi_mpsse_dev *fdev);
u8 ftdi_eeprom_channel_type(const struct ftdi_eeprom *ee, u8 channel);
bool ftdi_eeprom_channel_vcp(const struct ftdi_eeprom *ee, u8 channel);

/* Name of the TXDEN pin of a UART channel or NULL; ee may be NULL */
const char *ftdi_eeprom_txden_pin(const struct ftdi_eeprom *ee,
				  enum ftdi_mpsse_chip_type chip, u8 channel);

/*
 * Drive nibble (FTDI_EE_DRIVE_*) of the data bus (xDBUS) of a channel,
 * or -ENODATA if the chip/channel is not known.
 */
int ftdi_eeprom_dbus_drive(const struct ftdi_eeprom *ee,
			   enum ftdi_mpsse_chip_type chip, u8 channel);

extern const struct attribute_group ftdi_eeprom_attr_group;

/* eeprom_cbus, eeprom_drive, eeprom_config in the debugfs dir of fdev */
struct dentry;
void ftdi_eeprom_debugfs_init(struct ftdi_mpsse_dev *fdev, struct dentry *dir);

/* Accessors of ftdi_mpsse_core.c: struct ftdi_mpsse_dev is private to it */
struct usb_interface;
struct usb_device;

struct usb_interface *ftdi_mpsse_dev_get_intf(struct ftdi_mpsse_dev *fdev);
struct usb_device *ftdi_mpsse_dev_get_udev(struct ftdi_mpsse_dev *fdev);
enum ftdi_mpsse_chip_type ftdi_mpsse_dev_get_chip(struct ftdi_mpsse_dev *fdev);
u8 ftdi_mpsse_dev_get_channel(struct ftdi_mpsse_dev *fdev);
struct ftdi_eeprom *ftdi_mpsse_dev_get_eeprom_ptr(struct ftdi_mpsse_dev *fdev);

#endif /* __FTDI_EEPROM_H */
