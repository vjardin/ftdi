/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI EEPROM reader and decoder -- internal header
 *
 * Constants, structures, and function declarations for EEPROM support
 * compiled into ftdi_mpsse.ko.  Not exported to child modules; children
 * access EEPROM data via ftdi_mpsse_get_eeprom() / ftdi_mpsse_get_cbus_config().
 */

#ifndef __FTDI_EEPROM_H
#define __FTDI_EEPROM_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/types.h>

/* Forward declarations */
struct ftdi_mpsse_dev;

/* EEPROM geometry */
#define FTDI_EEPROM_MAX_WORDS		128
#define FTDI_EEPROM_MAX_BYTES		(FTDI_EEPROM_MAX_WORDS * 2)

/* Common byte offsets */
#define FTDI_EE_CHAN_A			0x00
#define FTDI_EE_CHAN_B			0x01
#define FTDI_EE_VID_L			0x02
#define FTDI_EE_PID_L			0x04
#define FTDI_EE_RELEASE_L		0x06
#define FTDI_EE_CFG_DESC		0x08
#define FTDI_EE_MAX_POWER		0x09
#define FTDI_EE_CHIP_CFG		0x0a
#define FTDI_EE_INVERT			0x0b
#define FTDI_EE_DRIVE0			0x0c
#define FTDI_EE_DRIVE1			0x0d
#define FTDI_EE_STR_MFR_OFF		0x0e	/* manufacturer string offset */
#define FTDI_EE_STR_MFR_LEN		0x0f	/* manufacturer string length */
#define FTDI_EE_STR_PROD_OFF		0x10	/* product string offset */
#define FTDI_EE_STR_PROD_LEN		0x11	/* product string length */
#define FTDI_EE_STR_SER_OFF		0x12	/* serial string offset */
#define FTDI_EE_STR_SER_LEN		0x13	/* serial string length */

/* Per-chip CBUS offsets */
#define FTDI_EE_232H_CBUS_BASE		0x18	/* 5 bytes, 10 nibbles */
#define FTDI_EE_2232H_CBUS_BASE	0x14	/* 2 bytes */

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

/* VCP driver bits (byte 0x00 / 0x01) */
#define FTDI_EE_232H_VCP		BIT(4)	/* byte 0x00, FT232H */
#define FTDI_EE_2232H_VCP		BIT(3)	/* byte 0x00/0x01, FT2232H */
#define FTDI_EE_4232H_CHC_VCP		BIT(4)	/* byte 0x00, FT4232H chan C */
#define FTDI_EE_4232H_CHD_VCP		BIT(4)	/* byte 0x01, FT4232H chan D */

/* FT4232H invert byte (0x0B) -- RS485 and TXDEN bits for C/D */
#define FTDI_EE_4232H_CHA_RS485	BIT(0)
#define FTDI_EE_4232H_CHB_RS485	BIT(1)
#define FTDI_EE_4232H_CHC_RS485	BIT(2)
#define FTDI_EE_4232H_CHD_RS485	BIT(3)
#define FTDI_EE_4232H_CHA_TXDEN	BIT(4)
#define FTDI_EE_4232H_CHB_TXDEN	BIT(5)

/*
 * User area protocol hint (optional, at offset 0x1A)
 *
 * The FTDI EEPROM has no native field for SPI vs I2C mode.  Users can
 * program a single byte at offset 0x1A to hint the driver:
 *
 *   'S' (0x53) = SPI mode
 *   'I' (0x49) = I2C mode
 *   'U' (0x55) = UART mode
 *   other      = auto-detect via heuristics
 *
 * Note: A single MPSSE channel can only use one protocol at a time
 * (SPI or I2C), as they use incompatible clocking modes.
 *
 * This offset is chosen to avoid conflict with FT232H CBUS config
 * (0x18-0x1C) and string descriptors (typically starting at 0x1E+).
 * It lies in the "reserved" area for FT2232H/FT4232H.
 *
 * Program with ftdi_eeprom using user_data_addr=0x1a and a 1-byte file.
 */
#define FTDI_EE_PROTOCOL_HINT		0x1a
#define FTDI_EE_PROTO_SPI		'S'	/* 0x53 */
#define FTDI_EE_PROTO_I2C		'I'	/* 0x49 */
#define FTDI_EE_PROTO_UART		'U'	/* 0x55 */

/* Maximum decoded string length (UTF-16LE -> ASCII) */
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
	u16 size;			/* actual size in words */
	bool valid;			/* read succeeded */
	bool empty;			/* all 0xFF */
	bool checksum_ok;

	/* Decoded common fields */
	u16 vid;
	u16 pid;
	u16 release;
	char manufacturer[FTDI_EE_STR_MAX];
	char product[FTDI_EE_STR_MAX];
	char serial[FTDI_EE_STR_MAX];
	u8 channel_a_type;		/* FTDI_EE_CHAN_* */
	u8 channel_b_type;		/* FT2232H / FT4232H */
	u8 channel_c_type;		/* FT4232H: always UART (hardware-fixed) */
	u8 channel_d_type;		/* FT4232H: always UART (hardware-fixed) */
	bool self_powered;
	bool remote_wakeup;
	bool pulldown;
	bool use_serial;
	u16 max_power_ma;
	u8 cbus[10];			/* FT232H: ACBUS0-9 functions */
	int num_cbus;			/* 10 for FT232H, 0 otherwise */
	u8 group0_drive;		/* raw nibble */
	u8 group1_drive;		/* raw nibble */

	/* Decoded drive per group */
	u8 group0_drive_ma;		/* 4, 8, 12, or 16 */
	bool group0_schmitt;
	bool group0_slow_slew;
	u8 group1_drive_ma;
	bool group1_schmitt;
	bool group1_slow_slew;

	/* VCP driver bit */
	bool cha_vcp;			/* FT232H: byte 0x00 bit 4 */
	bool chb_vcp;			/* FT2232H / FT4232H: byte 0x01 bit 3 */
	bool chc_vcp;			/* FT4232H: byte 0x00 bit 4 */
	bool chd_vcp;			/* FT4232H: byte 0x01 bit 4 */

	/* FT4232H per-channel RS485 / TXDEN */
	bool cha_rs485;
	bool chb_rs485;
	bool chc_rs485;
	bool chd_rs485;
	bool cha_txden;
	bool chb_txden;

	/* User area protocol hint (0x1A) */
	u8 protocol_hint;		/* 'S', 'I', 'U', 'B', or other */
};

/* EEPROM functions (ftdi_eeprom.c) */
int ftdi_eeprom_read(struct ftdi_mpsse_dev *fdev);
u8 ftdi_eeprom_channel_type(const struct ftdi_eeprom *ee, u8 channel);

extern const struct attribute_group ftdi_eeprom_attr_group;

/*
 * Internal accessors (ftdi_mpsse_core.c) -- allow ftdi_eeprom.c to reach
 * fdev internals without exposing struct ftdi_mpsse_dev in a header.
 */
struct usb_interface;
struct usb_device;
enum ftdi_mpsse_chip_type;

struct usb_interface *ftdi_mpsse_dev_get_intf(struct ftdi_mpsse_dev *fdev);
struct usb_device *ftdi_mpsse_dev_get_udev(struct ftdi_mpsse_dev *fdev);
enum ftdi_mpsse_chip_type ftdi_mpsse_dev_get_chip(struct ftdi_mpsse_dev *fdev);
u8 ftdi_mpsse_dev_get_channel(struct ftdi_mpsse_dev *fdev);
struct ftdi_eeprom *ftdi_mpsse_dev_get_eeprom_ptr(struct ftdi_mpsse_dev *fdev);

#endif /* __FTDI_EEPROM_H */
