/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE USB driver -- shared header
 *
 * Platform data, transport API, vendor requests and MPSSE opcodes shared by
 * the core (ftdi_mpsse_core.c) and its child drivers.
 */

#ifndef __LINUX_USB_FTDI_MPSSE_H
#define __LINUX_USB_FTDI_MPSSE_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/usb.h>

struct platform_device;
struct usb_device;
struct usb_endpoint_descriptor;

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

/* Chip families: the -HP/-HA chips follow their base chip */
static inline bool ftdi_chip_is_232h(enum ftdi_mpsse_chip_type chip)
{
	return chip == FTDI_CHIP_FT232H || chip == FTDI_CHIP_FT232HP ||
	       chip == FTDI_CHIP_FT233HP;
}

static inline bool ftdi_chip_is_2232h(enum ftdi_mpsse_chip_type chip)
{
	return chip == FTDI_CHIP_FT2232H || chip == FTDI_CHIP_FT2232HP ||
	       chip == FTDI_CHIP_FT2233HP;
}

static inline bool ftdi_chip_is_4232h(enum ftdi_mpsse_chip_type chip)
{
	return chip == FTDI_CHIP_FT4232H || chip == FTDI_CHIP_FT4232HA ||
	       chip == FTDI_CHIP_FT4232HP || chip == FTDI_CHIP_FT4233HP;
}

/* SPI chip selects: AD3-AD7 */
#define FTDI_SPI_CS_MAX		5

/* Platform data of a child (mfd_cell.platform_data, dev_get_platdata()) */
struct ftdi_mpsse_pdata {
	/* The channel, fixed at probe */
	enum ftdi_mpsse_chip_type chip_type;
	u8 channel;			/* FTDI_CHANNEL_A..D, wIndex of requests */
	bool has_gpioh;			/* MPSSE high byte (xCBUS), not FT4232H */
	bool has_drive_zero;		/* 0x9E open drain (AN_108 7.1): FT232H */
	unsigned int fifo_size;		/* chip buffer of the channel, bytes */
	const char *txden_pin;		/* UART: RS-485 TXDEN pin ("ACBUS4"), or NULL */
	/* GPIO lines */
	u16 gpio_reserved_mask;		/* pins of the bus, never GPIO lines */
	u16 gpio_dir_in;		/* pins valid for input */
	u16 gpio_dir_out;		/* pins valid for output */
	/* ftdi-spi only */
	u8 spi_cs_pins[FTDI_SPI_CS_MAX];
	u8 spi_num_cs;
};

/*
 * Transport API, exported by ftdi_mpsse.ko. Children pass their own
 * platform_device; the core finds its state through pdev->dev.parent.
 */

/*
 * Send tx_len bytes, then receive exactly rx_len bytes: what the commands in
 * tx produce, more is a protocol error. Neither buffer has to be DMA-safe.
 * MPSSE channels: bus lock held. A failed transfer makes the core reset the
 * engine before anything else is sent, as resume does: clock divisor,
 * divide-by-5, 3-phase, adaptive clocking and loopback are back at their
 * defaults, the pins and the open-drain mask are replayed from the shadow.
 * Children send their engine setup with every transaction. The response
 * timeout runs while no response data comes, not over the transfer.
 */
int ftdi_mpsse_xfer(struct platform_device *pdev,
		    const u8 *tx, unsigned int tx_len,
		    u8 *rx, unsigned int rx_len);

/*
 * Same, for slow clocks: busy_ms is the time to clock out everything queued
 * and not yet answered, this transfer included (bits / bit rate, AN_108
 * 3.8). The chip only takes data as it executes it, so busy_ms is added
 * to the write and to the response timeout.
 */
int ftdi_mpsse_xfer_busy(struct platform_device *pdev,
			 const u8 *tx, unsigned int tx_len,
			 u8 *rx, unsigned int rx_len, unsigned int busy_ms);

/* Write only, no response expected */
int ftdi_mpsse_write(struct platform_device *pdev,
		     const u8 *buf, unsigned int len);

/*
 * Bus lock, to hold around every MPSSE transfer and around whole
 * transactions (an SPI message, an I2C xfer): siblings on a channel must
 * not interleave.
 * Lock ordering: bus_lock -> fg->lock -> io_lock -> disconnect_lock.
 */
void ftdi_mpsse_bus_lock(struct platform_device *pdev);
void ftdi_mpsse_bus_unlock(struct platform_device *pdev);

/* Vendor control OUT request to the channel (wIndex) */
int ftdi_mpsse_cfg_msg(struct platform_device *pdev, u8 request, u16 value);

/* READ_PINS vendor request: instantaneous xDBUS pin levels */
int ftdi_mpsse_read_pins(struct platform_device *pdev, u8 *val);

/*
 * Raw handles: what a child does with them bypasses the disconnect/suspend
 * gate and io_lock of the transport above. ftdi-uart is the one user that
 * submits with them: alone on its channel, it stops its URBs in shutdown and
 * poisons them in remove.
 */
struct usb_device *ftdi_mpsse_get_udev(struct platform_device *pdev);

/*
 * Pin shadow of the channel (bank 0 = xDBUS low byte, 1 = xCBUS high byte).
 * With the bus lock held, a driver updates its own pins (mask), then sends
 * SET_BITS_LOW/HIGH or 0x9E with the merged values, so that siblings keep
 * theirs. The core replays the shadow when it resets the engine.
 */
void ftdi_mpsse_pins_set(struct platform_device *pdev, unsigned int bank,
			 u8 mask, u8 val, u8 dir);
void ftdi_mpsse_pins_get(struct platform_device *pdev, unsigned int bank,
			 u8 *val, u8 *dir);
void ftdi_mpsse_od_set(struct platform_device *pdev, unsigned int bank,
		       u8 mask, u8 od);
void ftdi_mpsse_od_get(struct platform_device *pdev, u8 *od_lo, u8 *od_hi);

/*
 * Pins outside gpio_reserved_mask are GPIO lines. A bus driver claims the
 * one it needs for an option decided at probe (I2C: AD7 as SCL sense with
 * adaptive clocking); the GPIO driver asks before it hands a line to a
 * consumer. The second one gets -EBUSY. All with the bus lock held.
 */
int ftdi_mpsse_pins_claim(struct platform_device *pdev, unsigned int bank,
			  u8 mask);
void ftdi_mpsse_pins_unclaim(struct platform_device *pdev, unsigned int bank,
			     u8 mask);
/* Pins of a bank the bus driver holds: the GPIO driver leaves them alone */
u8 ftdi_mpsse_pins_claimed(struct platform_device *pdev, unsigned int bank);
int ftdi_mpsse_pins_gpio_request(struct platform_device *pdev,
				 unsigned int bank, u8 mask);
void ftdi_mpsse_pins_gpio_free(struct platform_device *pdev,
			       unsigned int bank, u8 mask);

/* Bulk endpoints, for a child that runs its own URBs (UART) */
int ftdi_mpsse_get_endpoints(struct platform_device *pdev,
			     struct usb_endpoint_descriptor **ep_in,
			     struct usb_endpoint_descriptor **ep_out);

/*
 * Decoded EEPROM (ftdi_eeprom.h), NULL unless it was read, is programmed
 * and has a good checksum.
 */
struct ftdi_eeprom;
const struct ftdi_eeprom *ftdi_mpsse_get_eeprom(struct platform_device *pdev);

/* FTDI vendor requests (FTDI datasheets, D2XX Programmer's Guide) */

/* Channel of an interface, the wIndex of its vendor requests */
#define FTDI_CHANNEL_A			1
#define FTDI_CHANNEL_B			2
#define FTDI_CHANNEL_C			3
#define FTDI_CHANNEL_D			4

/* bRequest */
#define FTDI_SIO_RESET_REQUEST			0x00
#define FTDI_SIO_SET_MODEM_CTRL_REQUEST		0x01
#define FTDI_SIO_SET_FLOW_CTRL_REQUEST		0x02
#define FTDI_SIO_SET_BAUDRATE_REQUEST		0x03
#define FTDI_SIO_SET_DATA_REQUEST		0x04
#define FTDI_SIO_GET_MODEM_STATUS_REQUEST	0x05
#define FTDI_SIO_SET_LATENCY_TIMER_REQUEST	0x09
#define FTDI_SIO_SET_BITMODE_REQUEST		0x0b
#define FTDI_SIO_READ_PINS_REQUEST		0x0c
#define FTDI_SIO_READ_EEPROM_REQUEST		0x90

/* bmRequestType */
#define FTDI_SIO_REQTYPE_OUT			0x40
#define FTDI_SIO_REQTYPE_IN			0xc0

/* FTDI_SIO_RESET_REQUEST wValue */
#define FTDI_SIO_RESET_SIO			0
#define FTDI_SIO_RESET_PURGE_RX		1
#define FTDI_SIO_RESET_PURGE_TX		2

/* FTDI_SIO_SET_BITMODE_REQUEST mode (wValue high byte) */
#define FTDI_SIO_BITMODE_RESET			0x00
#define FTDI_SIO_BITMODE_BITBANG		0x01	/* Async bit-bang */
#define FTDI_SIO_BITMODE_MPSSE			0x02
#define FTDI_SIO_BITMODE_CBUS			0x20

/* FTDI_SIO_SET_MODEM_CTRL_REQUEST wValue */
#define FTDI_SIO_SET_DTR_HIGH			((1 << 8) | 1)
#define FTDI_SIO_SET_DTR_LOW			((1 << 8) | 0)
#define FTDI_SIO_SET_RTS_HIGH			((2 << 8) | 2)
#define FTDI_SIO_SET_RTS_LOW			((2 << 8) | 0)

/* FTDI_SIO_SET_FLOW_CTRL_REQUEST wIndex high byte */
#define FTDI_SIO_DISABLE_FLOW_CTRL		0x0
#define FTDI_SIO_RTS_CTS_HS			(0x1 << 8)
#define FTDI_SIO_DTR_DSR_HS			(0x2 << 8)
#define FTDI_SIO_XON_XOFF_HS			(0x4 << 8)

/* FTDI_SIO_SET_DATA_REQUEST wValue */
#define FTDI_SIO_SET_DATA_PARITY_NONE		(0x0 << 8)
#define FTDI_SIO_SET_DATA_PARITY_ODD		(0x1 << 8)
#define FTDI_SIO_SET_DATA_PARITY_EVEN		(0x2 << 8)
#define FTDI_SIO_SET_DATA_PARITY_MARK		(0x3 << 8)
#define FTDI_SIO_SET_DATA_PARITY_SPACE		(0x4 << 8)
#define FTDI_SIO_SET_DATA_STOP_BITS_1		(0x0 << 11)
#define FTDI_SIO_SET_DATA_STOP_BITS_2		(0x2 << 11)
#define FTDI_SIO_SET_BREAK			(0x1 << 14)

/* Modem status (byte 0 of every bulk-in packet) */
#define FTDI_RS0_CTS				BIT(4)
#define FTDI_RS0_DSR				BIT(5)
#define FTDI_RS0_RI				BIT(6)
#define FTDI_RS0_RLSD				BIT(7)

/* Line status (byte 1 of every bulk-in packet) */
#define FTDI_RS_OE				BIT(1)
#define FTDI_RS_PE				BIT(2)
#define FTDI_RS_FE				BIT(3)
#define FTDI_RS_BI				BIT(4)
#define FTDI_RS_THRE				BIT(5)
#define FTDI_RS_TEMT				BIT(6)	/* TX shifter empty */

/* MPSSE command opcodes (AN_108) */

/* Low byte (AD0-AD7) */
#define MPSSE_SET_BITS_LOW	0x80	/* Args: value, direction */
#define MPSSE_GET_BITS_LOW	0x81	/* Returns: 1 byte pin states */

/* High byte (AC0-AC7) */
#define MPSSE_SET_BITS_HIGH	0x82	/* Args: value, direction */
#define MPSSE_GET_BITS_HIGH	0x83	/* Returns: 1 byte pin states */

/* Clock data out on the +ve (PVE) or -ve (NVE) clock edge */
#define MPSSE_CLK_BYTES_OUT_PVE		0x10
#define MPSSE_CLK_BYTES_OUT_NVE		0x11
#define MPSSE_CLK_BITS_OUT_PVE		0x12
#define MPSSE_CLK_BITS_OUT_NVE		0x13

/* Clock data in */
#define MPSSE_CLK_BYTES_IN_PVE		0x20
#define MPSSE_CLK_BYTES_IN_NVE		0x24
#define MPSSE_CLK_BITS_IN_PVE		0x22
#define MPSSE_CLK_BITS_IN_NVE		0x26

/* ORed into a byte clock command: bits instead of bytes */
#define MPSSE_CLK_BIT_MODE		0x02

/* ORed into any clock command: LSB first */
#define MPSSE_CLK_LSB_FIRST		0x08

/* Clock data in and out simultaneously */
#define MPSSE_CLK_BYTES_INOUT_PVE_NVE	0x31	/* Out -ve, in +ve (SPI mode 0) */
#define MPSSE_CLK_BYTES_INOUT_NVE_PVE	0x34	/* Out +ve, in -ve (SPI mode 2) */
#define MPSSE_CLK_BITS_INOUT_PVE_NVE	0x33
#define MPSSE_CLK_BITS_INOUT_NVE_PVE	0x36

/* Engine configuration */
#define MPSSE_LOOPBACK_ON		0x84
#define MPSSE_LOOPBACK_OFF		0x85
#define MPSSE_SET_CLK_DIVISOR		0x86	/* Args: divisorL, divisorH */
#define MPSSE_SEND_IMMEDIATE		0x87	/* Flush RX buffer to USB */

/* Hi-Speed only (FT232H, FT2232H, FT4232H) */
#define MPSSE_DISABLE_CLK_DIV5		0x8A	/* 60 MHz master clock */
#define MPSSE_ENABLE_CLK_DIV5		0x8B	/* 12 MHz master clock */
#define MPSSE_ENABLE_3PHASE		0x8C
#define MPSSE_DISABLE_3PHASE		0x8D
#define MPSSE_ENABLE_ADAPTIVE		0x96
#define MPSSE_DISABLE_ADAPTIVE		0x97
#define MPSSE_DRIVE_ZERO_ONLY		0x9E	/* Open drain, FT232H only */

/* Response to a bad command, followed by its opcode */
#define MPSSE_CMD_BAD			0xFA

/* Bulk transfer buffers */
#define FTDI_MPSSE_BUF_SIZE		4096
#define FTDI_MPSSE_STATUS_BYTES		2	/* Modem/line status on bulk-in */

/* USB timeouts (ms); the response timeout is private to the core */
#define FTDI_MPSSE_WRITE_TIMEOUT	5000
#define FTDI_MPSSE_CTRL_TIMEOUT		USB_CTRL_SET_TIMEOUT

#endif /* __LINUX_USB_FTDI_MPSSE_H */
