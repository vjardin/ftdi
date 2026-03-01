/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE USB driver -- shared header
 *
 * Defines platform data, transport API, and MPSSE opcodes shared between
 * the MPSSE transport core (ftdi_mpsse_core.c) and child drivers (ftdi_uart,
 * ftdi_spi, ftdi_i2c, ftdi_gpio).
 */

#ifndef __LINUX_USB_FTDI_MPSSE_H
#define __LINUX_USB_FTDI_MPSSE_H

#include <linux/types.h>

struct platform_device;
struct usb_device;
struct usb_endpoint_descriptor;

/* Chip types for MPSSE-capable FTDI Hi-Speed devices */
enum ftdi_mpsse_chip_type {
	FTDI_CHIP_FT232H,
	FTDI_CHIP_FT2232H,
	FTDI_CHIP_FT4232H,
	FTDI_CHIP_FT4232HA,
	FTDI_CHIP_FT232HP,
	FTDI_CHIP_FT233HP,
	FTDI_CHIP_FT2232HP,
	FTDI_CHIP_FT2233HP,
	FTDI_CHIP_FT4232HP,
	FTDI_CHIP_FT4233HP,
	FTDI_CHIP_UNKNOWN,
};

/* Function modes for child drivers */
enum ftdi_mpsse_function {
	FTDI_FUNC_UART,
	FTDI_FUNC_SPI,
	FTDI_FUNC_I2C,
	FTDI_FUNC_GPIO,
};

/*
 * Per-device GPIO quirk: pin names and direction overrides.
 * Looked up by usb_device_id.driver_info, same pattern as gpio-mpsse.
 */
struct ftdi_gpio_quirk {
	const char *names[16];
	u16 dir_in;
	u16 dir_out;
};

/* Maximum number of SPI chip-select lines (AD3-AD7) */
#define FTDI_SPI_CS_MAX		5

/*
 * Platform data passed to each child via mfd_cell.platform_data.
 * Children retrieve this with dev_get_platdata(&pdev->dev).
 */
struct ftdi_mpsse_pdata {
	enum ftdi_mpsse_function function;
	u16 gpio_reserved_mask;		/* pins excluded entirely (bus owns) */
	u16 gpio_dir_in;		/* bitmask: pins valid for input */
	u16 gpio_dir_out;		/* bitmask: pins valid for output */
	const struct ftdi_gpio_quirk *gpio_quirk;  /* NULL for generic */
	/* SPI chip-select configuration (FTDI_FUNC_SPI only) */
	u8 spi_cs_pins[FTDI_SPI_CS_MAX];
	u8 spi_num_cs;
};

/*
 * Transport API -- exported by ftdi_mpsse.ko (EXPORT_SYMBOL_GPL).
 * Children pass their own platform_device; the core locates parent state
 * via dev_get_drvdata(pdev->dev.parent).
 */

/* Full MPSSE transfer: send tx_len bytes, then receive rx_len bytes */
int ftdi_mpsse_xfer(struct platform_device *pdev,
		    const u8 *tx, unsigned int tx_len,
		    u8 *rx, unsigned int rx_len);

/* Write-only (no response expected) */
int ftdi_mpsse_write(struct platform_device *pdev,
		     const u8 *buf, unsigned int len);

/* Read-only (e.g. after a prior write that triggers a response) */
int ftdi_mpsse_read(struct platform_device *pdev,
		    u8 *buf, unsigned int len);

/*
 * Bus-level mutual exclusion.  Children must hold bus_lock around entire
 * multi-step MPSSE transactions (e.g. a full SPI message or I2C xfer)
 * so that sibling drivers cannot interleave and corrupt MPSSE state.
 *
 * Lock ordering: bus_lock -> fg->lock -> io_lock -> disconnect_lock.
 */
void ftdi_mpsse_bus_lock(struct platform_device *pdev);
void ftdi_mpsse_bus_unlock(struct platform_device *pdev);

/* USB control message (vendor request to FTDI device) */
int ftdi_mpsse_cfg_msg(struct platform_device *pdev, u8 request, u16 value);

/* Accessors for parent state */
struct usb_device *ftdi_mpsse_get_udev(struct platform_device *pdev);
u8 ftdi_mpsse_get_channel(struct platform_device *pdev);
enum ftdi_mpsse_chip_type ftdi_mpsse_get_chip_type(struct platform_device *pdev);

/* Query MPSSE hardware capability (false for FT4232H channels C/D) */
bool ftdi_mpsse_is_mpsse_capable(struct platform_device *pdev);

/* For UART child: direct access to bulk endpoints for async URBs */
int ftdi_mpsse_get_endpoints(struct platform_device *pdev,
			     struct usb_endpoint_descriptor **ep_in,
			     struct usb_endpoint_descriptor **ep_out);

/* EEPROM accessors for child drivers */
struct ftdi_eeprom;
const struct ftdi_eeprom *ftdi_mpsse_get_eeprom(struct platform_device *pdev);
int ftdi_mpsse_get_cbus_config(struct platform_device *pdev,
			       u8 *buf, unsigned int len);
int ftdi_mpsse_get_eeprom_drive(struct platform_device *pdev,
				u8 *drive_ma, bool *schmitt,
				bool *slow_slew);

/*
 * FTDI USB vendor request codes and hardware register definitions.
 * These are protocol constants defined by FTDI for all their USB devices;
 * values are from FTDI datasheets and the D2XX Programmer's Guide.
 */

/* Channel indices for multi-channel devices (FT2232H, FT4232H) */
#define FTDI_CHANNEL_A			1
#define FTDI_CHANNEL_B			2
#define FTDI_CHANNEL_C			3
#define FTDI_CHANNEL_D			4

/* USB vendor request codes (bRequest in usb_control_msg) */
#define FTDI_SIO_RESET_REQUEST			0x00
#define FTDI_SIO_SET_MODEM_CTRL_REQUEST		0x01
#define FTDI_SIO_SET_FLOW_CTRL_REQUEST		0x02
#define FTDI_SIO_SET_BAUDRATE_REQUEST		0x03
#define FTDI_SIO_SET_DATA_REQUEST		0x04
#define FTDI_SIO_GET_MODEM_STATUS_REQUEST	0x05
#define FTDI_SIO_SET_LATENCY_TIMER_REQUEST	0x09
#define FTDI_SIO_GET_LATENCY_TIMER_REQUEST	0x0a
#define FTDI_SIO_SET_BITMODE_REQUEST		0x0b
#define FTDI_SIO_READ_PINS_REQUEST		0x0c
#define FTDI_SIO_READ_EEPROM_REQUEST		0x90

/* bmRequestType for vendor OUT (host->device) and IN (device->host) */
#define FTDI_SIO_REQTYPE_OUT			0x40
#define FTDI_SIO_REQTYPE_IN			0xc0

/* FTDI_SIO_RESET_REQUEST wValue parameters */
#define FTDI_SIO_RESET_SIO			0
#define FTDI_SIO_RESET_PURGE_RX		1
#define FTDI_SIO_RESET_PURGE_TX		2

/* FTDI_SIO_SET_BITMODE_REQUEST mode values (wValue high byte) */
#define FTDI_SIO_BITMODE_RESET			0x00
#define FTDI_SIO_BITMODE_BITBANG		0x01	/* Async bit-bang */
#define FTDI_SIO_BITMODE_MPSSE			0x02
#define FTDI_SIO_BITMODE_CBUS			0x20

/* FTDI_SIO_SET_MODEM_CTRL_REQUEST wValue encoding */
#define FTDI_SIO_SET_DTR_HIGH			((1 << 8) | 1)
#define FTDI_SIO_SET_DTR_LOW			((1 << 8) | 0)
#define FTDI_SIO_SET_RTS_HIGH			((2 << 8) | 2)
#define FTDI_SIO_SET_RTS_LOW			((2 << 8) | 0)

/* FTDI_SIO_SET_FLOW_CTRL_REQUEST wIndex high byte */
#define FTDI_SIO_DISABLE_FLOW_CTRL		0x0
#define FTDI_SIO_RTS_CTS_HS			(0x1 << 8)
#define FTDI_SIO_DTR_DSR_HS			(0x2 << 8)
#define FTDI_SIO_XON_XOFF_HS			(0x4 << 8)

/* FTDI_SIO_SET_DATA_REQUEST wValue encoding */
#define FTDI_SIO_SET_DATA_PARITY_NONE		(0x0 << 8)
#define FTDI_SIO_SET_DATA_PARITY_ODD		(0x1 << 8)
#define FTDI_SIO_SET_DATA_PARITY_EVEN		(0x2 << 8)
#define FTDI_SIO_SET_DATA_PARITY_MARK		(0x3 << 8)
#define FTDI_SIO_SET_DATA_PARITY_SPACE		(0x4 << 8)
#define FTDI_SIO_SET_DATA_STOP_BITS_1		(0x0 << 11)
#define FTDI_SIO_SET_DATA_STOP_BITS_2		(0x2 << 11)
#define FTDI_SIO_SET_BREAK			(0x1 << 14)

/* Modem status register (byte 0 of bulk-in packet) */
#define FTDI_RS0_CTS				BIT(4)
#define FTDI_RS0_DSR				BIT(5)
#define FTDI_RS0_RI				BIT(6)
#define FTDI_RS0_RLSD				BIT(7)

/* Line status register (byte 1 of bulk-in packet) */
#define FTDI_RS_OE				BIT(1)
#define FTDI_RS_PE				BIT(2)
#define FTDI_RS_FE				BIT(3)
#define FTDI_RS_BI				BIT(4)

/* EEPROM: CBUS pin mux function values */
#define FTDI_FTX_CBUS_MUX_GPIO			0x8
#define FTDI_FTX_CBUS_MUX_TXDEN		0x9

/*
 * MPSSE command opcodes (AN_108)
 */

/* GPIO -- low byte (AD0-AD7) */
#define MPSSE_SET_BITS_LOW	0x80	/* Args: value, direction */
#define MPSSE_GET_BITS_LOW	0x81	/* Returns: 1 byte pin states */

/* GPIO -- high byte (AC0-AC7) */
#define MPSSE_SET_BITS_HIGH	0x82	/* Args: value, direction */
#define MPSSE_GET_BITS_HIGH	0x83	/* Returns: 1 byte pin states */

/* Clock data out (bits/bytes, +ve or -ve clock edge) */
#define MPSSE_CLK_BYTES_OUT_PVE		0x10	/* Byte out on +ve clock        */
#define MPSSE_CLK_BYTES_OUT_NVE		0x11	/* Byte out on -ve clock        */
#define MPSSE_CLK_BITS_OUT_PVE		0x12	/* Bit out on +ve clock         */
#define MPSSE_CLK_BITS_OUT_NVE		0x13	/* Bit out on -ve clock         */

/* Clock data in */
#define MPSSE_CLK_BYTES_IN_PVE		0x20	/* Byte in on +ve clock         */
#define MPSSE_CLK_BYTES_IN_NVE		0x24	/* Byte in on -ve clock         */
#define MPSSE_CLK_BITS_IN_PVE		0x22	/* Bit in on +ve clock          */
#define MPSSE_CLK_BITS_IN_NVE		0x26	/* Bit in on -ve clock          */

/* Bit-mode modifier -- OR with byte-mode clock command for bit granularity */
#define MPSSE_CLK_BIT_MODE		0x02

/* LSB-first modifier -- OR with any clock command for LSB-first bit order */
#define MPSSE_CLK_LSB_FIRST		0x08

/* Clock data in and out simultaneously */
#define MPSSE_CLK_BYTES_INOUT_PVE_NVE	0x31	/* Out -ve, in +ve (SPI mode 0) */
#define MPSSE_CLK_BYTES_INOUT_NVE_PVE	0x34	/* Out +ve, in -ve (SPI mode 2) */
#define MPSSE_CLK_BITS_INOUT_PVE_NVE	0x33	/* Bit out -ve, in +ve          */
#define MPSSE_CLK_BITS_INOUT_NVE_PVE	0x36	/* Bit out +ve, in -ve          */

/* MPSSE configuration commands */
#define MPSSE_LOOPBACK_ON		0x84
#define MPSSE_LOOPBACK_OFF		0x85
#define MPSSE_SET_CLK_DIVISOR		0x86	/* Args: divisorL, divisorH     */
#define MPSSE_SEND_IMMEDIATE		0x87	/* Flush receive buffer to USB  */

/* Hi-Speed exclusive commands (FT232H, FT2232H, FT4232H) */
#define MPSSE_DISABLE_CLK_DIV5		0x8A	/* Use 60 MHz master clock      */
#define MPSSE_ENABLE_CLK_DIV5		0x8B	/* Use 12 MHz master clock      */
#define MPSSE_ENABLE_3PHASE		0x8C	/* Enable 3-phase clocking      */
#define MPSSE_DISABLE_3PHASE		0x8D	/* Disable 3-phase clocking     */
#define MPSSE_ENABLE_ADAPTIVE		0x96	/* Enable adaptive clocking     */
#define MPSSE_DISABLE_ADAPTIVE		0x97	/* Disable adaptive clocking    */
#define MPSSE_DRIVE_ZERO_ONLY		0x9E	/* Drive-only-zero (open-drain) */

/* Bad command response */
#define MPSSE_CMD_BAD			0xFA	/* Followed by the bad cmd byte */

/* FTDI bulk transfer buffer sizes */
#define FTDI_MPSSE_BUF_SIZE		4096
#define FTDI_MPSSE_STATUS_BYTES		2	/* Modem/line status on bulk-in */

/* USB timeouts (ms) */
#define FTDI_MPSSE_WRITE_TIMEOUT	5000
#define FTDI_MPSSE_READ_TIMEOUT		5000
#define FTDI_MPSSE_CTRL_TIMEOUT		USB_CTRL_SET_TIMEOUT

#endif /* __LINUX_USB_FTDI_MPSSE_H */
