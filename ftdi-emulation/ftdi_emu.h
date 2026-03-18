/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI device emulation -- state machine and USB descriptor generation
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* FTDI vendor ID -- same for all chips */
#define FTDI_VID		0x0403

/* Product IDs */
#define FTDI_PID_FT232H		0x6014
#define FTDI_PID_FT2232H	0x6010
#define FTDI_PID_FT4232H	0x6011

/* bcdDevice values used by the kernel driver to detect chip type */
#define FTDI_BCD_FT232H		0x0900
#define FTDI_BCD_FT2232H	0x0700
#define FTDI_BCD_FT4232H	0x0800

/* FTDI vendor-specific control requests */
#define FTDI_REQ_RESET			0x00
#define FTDI_REQ_SET_MODEM_CTRL		0x01
#define FTDI_REQ_SET_FLOW_CTRL		0x02
#define FTDI_REQ_SET_BAUDRATE		0x03
#define FTDI_REQ_SET_DATA		0x04
#define FTDI_REQ_GET_MODEM_STATUS	0x05
#define FTDI_REQ_SET_EVENT_CHAR		0x06
#define FTDI_REQ_SET_ERROR_CHAR		0x07
#define FTDI_REQ_SET_LATENCY_TIMER	0x09
#define FTDI_REQ_GET_LATENCY_TIMER	0x0A
#define FTDI_REQ_SET_BITMODE		0x0B
#define FTDI_REQ_READ_PINS		0x0C
#define FTDI_REQ_READ_EEPROM		0x90
#define FTDI_REQ_WRITE_EEPROM		0x91
#define FTDI_REQ_ERASE_EEPROM		0x92

/* Error injection control (emulator-only, not real FTDI request) */
#define FTDI_REQ_SET_ERROR_MODE		0xE0

/* Bitmode values */
#define FTDI_BITMODE_RESET	0x00
#define FTDI_BITMODE_BITBANG	0x01
#define FTDI_BITMODE_MPSSE	0x02
#define FTDI_BITMODE_CBUS	0x20

/* MPSSE opcodes the emulator understands */
#define MPSSE_SET_BITS_LOW	0x80
#define MPSSE_GET_BITS_LOW	0x81
#define MPSSE_SET_BITS_HIGH	0x82
#define MPSSE_GET_BITS_HIGH	0x83
#define MPSSE_LOOPBACK_ON	0x84
#define MPSSE_LOOPBACK_OFF	0x85
#define MPSSE_SET_CLK_DIVISOR	0x86
#define MPSSE_SEND_IMMEDIATE	0x87
#define MPSSE_DISABLE_CLK_DIV5	0x8A
#define MPSSE_ENABLE_CLK_DIV5	0x8B
#define MPSSE_ENABLE_3PHASE	0x8C
#define MPSSE_DISABLE_3PHASE	0x8D
#define MPSSE_ENABLE_ADAPTIVE	0x96
#define MPSSE_DISABLE_ADAPTIVE	0x97
#define MPSSE_DRIVE_ZERO_ONLY	0x9E
#define MPSSE_BAD_CMD		0xFA

/* Bulk-IN status bytes prepended to every FTDI response */
#define FTDI_MODEM_STATUS_DEFAULT	0x60	/* CTS + DSR asserted */
#define FTDI_LINE_STATUS_DEFAULT	0x00	/* No errors */
#define FTDI_STATUS_BYTES		2

/* EEPROM */
#define FTDI_EEPROM_SIZE	256	/* bytes (128 x 16-bit words) */
#define FTDI_EEPROM_WORDS	128

/* Max interfaces per device */
#define FTDI_MAX_INTERFACES	4

/* Chip types we emulate */
enum ftdi_chip {
	CHIP_FT232H,
	CHIP_FT2232H,
	CHIP_FT4232H,
};

/*
 * Error injection modes for testing driver error handling
 */
enum ftdi_error_mode {
	FTDI_ERR_NONE = 0,		/* Normal operation */
	FTDI_ERR_I2C_NAK,		/* I2C NAK on address/data byte */
	FTDI_ERR_I2C_BUS_STUCK,		/* SDA stuck low (frozen bus) */
	FTDI_ERR_USB_STALL,		/* USB endpoint stall */
	FTDI_ERR_USB_TIMEOUT,		/* USB timeout (no response) */
	FTDI_ERR_MPSSE_SYNC,		/* MPSSE sync: wrong echo for 0xAA */
	FTDI_ERR_I2C_CLK_STRETCH,	/* I2C clock stretch: SCL held low */
};

/* Per-interface state */
struct ftdi_intf_state {
	uint8_t  bitmode;
	uint8_t  latency;
	uint8_t  gpio_low;		/* AD0-AD7 pin values */
	uint8_t  gpio_high;		/* AC0-AC7 pin values */
	uint8_t  gpio_dir_low;		/* AD0-AD7 direction */
	uint8_t  gpio_dir_high;		/* AC0-AC7 direction */
	uint16_t modem_ctrl;
	uint16_t flow_ctrl;
	uint16_t baudrate;
	uint16_t data_cfg;
	bool     loopback;
	bool     clk_div5;
	bool     three_phase;
	bool     adaptive;
	bool     drive_zero;
	uint8_t  read_counter;		/* Counter for test pattern generation */

	/* Error injection state */
	enum ftdi_error_mode error_mode;
	int      error_count;		/* Remaining errors to inject (0 = infinite) */
	int      i2c_byte_count;	/* Track I2C bytes for targeted NAK */
	int      clk_stretch_left;	/* GET_BITS_LOW calls to return SCL=0 */

	/* Per-interface bulk-IN response accumulator */
	uint8_t  resp_buf[512];
	int      resp_len;
};

/* Top-level emulated device */
struct ftdi_device {
	enum ftdi_chip    chip;
	uint16_t          vid;
	uint16_t          pid;
	uint16_t          bcd;
	int               num_interfaces;
	struct ftdi_intf_state intf[FTDI_MAX_INTERFACES];
	uint8_t           eeprom[FTDI_EEPROM_SIZE];
	bool              eeprom_loaded;	/* true if loaded from file */
};

/* Initialise device state for the given chip type */
void ftdi_emu_init(struct ftdi_device *dev, enum ftdi_chip chip);

/* Load EEPROM binary from file (256 bytes), returns 0 on success */
int ftdi_emu_load_eeprom(struct ftdi_device *dev, const char *path);

/* Generate a default EEPROM image with valid checksum */
void ftdi_emu_default_eeprom(struct ftdi_device *dev);

/* Recalculate EEPROM checksum after modification */
void ftdi_emu_fix_checksum(struct ftdi_device *dev);

/*
 * Handle a USB control request.
 * Returns the number of response bytes written to resp_buf, or
 * negative on stall (unsupported request).
 */
int ftdi_emu_control(struct ftdi_device *dev,
		     uint8_t bmRequestType, uint8_t bRequest,
		     uint16_t wValue, uint16_t wIndex, uint16_t wLength,
		     uint8_t *resp_buf);

/*
 * Handle bulk OUT data (MPSSE commands or UART TX).
 * Generates a response in dev->resp_buf / dev->resp_len.
 * The caller must prepend the 2-byte FTDI status header.
 */
void ftdi_emu_bulk_out(struct ftdi_device *dev, int intf_idx,
		       const uint8_t *data, int len);

/*
 * Produce a bulk IN packet.
 * Copies the pending response (with FTDI status header) into buf.
 * Returns the number of bytes (always >= 2 for the status header).
 */
int ftdi_emu_bulk_in(struct ftdi_device *dev, int intf_idx,
		     uint8_t *buf, int maxlen);

/* Build standard USB descriptors for this device */
int ftdi_emu_device_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen);
int ftdi_emu_config_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen);
int ftdi_emu_string_descriptor(const struct ftdi_device *dev,
			       int index, uint16_t langid,
			       uint8_t *buf, int maxlen);

/*
 * Set error injection mode for an interface.
 * mode: FTDI_ERR_* constant
 * count: number of errors to inject (0 = infinite until cleared)
 */
void ftdi_emu_set_error(struct ftdi_device *dev, int intf_idx,
			enum ftdi_error_mode mode, int count);

/* Clear all error injection state */
void ftdi_emu_clear_errors(struct ftdi_device *dev);
