/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI device emulation -- chip model, MPSSE engine, UART/bit-bang data paths
 *
 * The model follows the FTDI documentation:
 *   DS_FT232H, DS_FT2232H, DS_FT4232H  (channels, buffers, pins, EEPROM)
 *   AN_108                              (MPSSE command set)
 *   AN2232C-02                          (bit-bang modes)
 *   AN232B-04                           (latency timer, status bytes)
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "ftdi_bus.h"

#define FTDI_VID		0x0403

#define FTDI_PID_FT232H		0x6014
#define FTDI_PID_FT2232H	0x6010
#define FTDI_PID_FT4232H	0x6011

/* bcdDevice tells the chip types apart */
#define FTDI_BCD_FT232H		0x0900
#define FTDI_BCD_FT2232H	0x0700
#define FTDI_BCD_FT4232H	0x0800

/* Vendor requests (bRequest) */
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

/* Emulator only, not an FTDI request: error injection */
#define FTDI_REQ_SET_ERROR_MODE		0xE0

/*
 * FTDI_REQ_RESET wValue, named from the chip's side (see libftdi's ftdi.h):
 * PURGE_RX empties the FIFO of data from the host (bulk OUT), PURGE_TX the
 * FIFO of data for the host (bulk IN).
 */
#define FTDI_RESET_SIO		0
#define FTDI_RESET_PURGE_RX	1
#define FTDI_RESET_PURGE_TX	2

/* Bitmode values (D2XX FT_SetBitMode) */
#define FTDI_BITMODE_RESET	0x00
#define FTDI_BITMODE_BITBANG	0x01
#define FTDI_BITMODE_MPSSE	0x02
#define FTDI_BITMODE_SYNCBB	0x04
#define FTDI_BITMODE_MCU	0x08
#define FTDI_BITMODE_OPTO	0x10
#define FTDI_BITMODE_CBUS	0x20
#define FTDI_BITMODE_SYNCFF	0x40

/* MPSSE opcodes (AN_108) */
#define MPSSE_SET_BITS_LOW	0x80
#define MPSSE_GET_BITS_LOW	0x81
#define MPSSE_SET_BITS_HIGH	0x82
#define MPSSE_GET_BITS_HIGH	0x83
#define MPSSE_LOOPBACK_ON	0x84
#define MPSSE_LOOPBACK_OFF	0x85
#define MPSSE_SET_CLK_DIVISOR	0x86
#define MPSSE_SEND_IMMEDIATE	0x87
#define MPSSE_WAIT_IO_HIGH	0x88
#define MPSSE_WAIT_IO_LOW	0x89
#define MPSSE_DISABLE_CLK_DIV5	0x8A
#define MPSSE_ENABLE_CLK_DIV5	0x8B
#define MPSSE_ENABLE_3PHASE	0x8C
#define MPSSE_DISABLE_3PHASE	0x8D
#define MPSSE_CLK_N_BITS	0x8E
#define MPSSE_CLK_N_BYTES	0x8F
#define MPSSE_CLK_UNTIL_HIGH	0x94
#define MPSSE_CLK_UNTIL_LOW	0x95
#define MPSSE_ENABLE_ADAPTIVE	0x96
#define MPSSE_DISABLE_ADAPTIVE	0x97
#define MPSSE_CLK_N_UNTIL_HIGH	0x9C
#define MPSSE_CLK_N_UNTIL_LOW	0x9D
#define MPSSE_DRIVE_ZERO_ONLY	0x9E
#define MPSSE_BAD_CMD		0xFA

/* Byte 0 of every bulk-IN packet and of GET_MODEM_STATUS */
#define FTDI_MS_RESERVED	0x01	/* always 1 */
#define FTDI_MS_CTS		0x10
#define FTDI_MS_DSR		0x20
#define FTDI_MS_RI		0x40
#define FTDI_MS_RLSD		0x80

/* SET_FLOW_CTRL, high byte of wIndex; wValue = XOFF << 8 | XON */
#define FTDI_FLOW_RTS_CTS	0x01
#define FTDI_FLOW_DTR_DSR	0x02
#define FTDI_FLOW_XON_XOFF	0x04

/*
 * Room left in the receive buffer at which the receiver holds its sender;
 * released with half of the buffer free. No figures in the documents.
 */
#define FTDI_EMU_RX_HOLD_ROOM	64

/* Byte 1: line status */
#define FTDI_LS_DR		0x01
#define FTDI_LS_OE		0x02
#define FTDI_LS_PE		0x04
#define FTDI_LS_FE		0x08
#define FTDI_LS_BI		0x10
#define FTDI_LS_THRE		0x20
#define FTDI_LS_TEMT		0x40
#define FTDI_LS_FIFO_ERR	0x80

#define FTDI_STATUS_BYTES	2
#define FTDI_HS_MAX_PACKET	512	/* Hi-Speed bulk wMaxPacketSize */

#define FTDI_EEPROM_SIZE	256	/* bytes */
#define FTDI_EEPROM_WORDS	128	/* 16-bit words */

#define FTDI_MAX_INTERFACES	4

/*
 * Sanity cap of the receive queue. The chip's buffers are 1 KiB (FT232H),
 * 4 KiB (FT2232H) or 2 KiB (FT4232H) in each direction.
 */
#define FTDI_RXQ_LIMIT		(1024 * 1024)

enum ftdi_chip {
	CHIP_FT232H,
	CHIP_FT2232H,
	CHIP_FT4232H,
};

enum ftdi_error_mode {
	FTDI_ERR_NONE = 0,
	FTDI_ERR_I2C_NAK,		/* a slave does not ACK its address */
	FTDI_ERR_I2C_BUS_STUCK,		/* SDA stuck low */
	FTDI_ERR_USB_STALL,		/* bulk IN stalls */
	FTDI_ERR_USB_TIMEOUT,		/* bulk IN never completes */
	FTDI_ERR_MPSSE_SYNC,		/* wrong echo for 0xAA */
	FTDI_ERR_I2C_CLK_STRETCH,	/* a slave stretches SCL after a byte */
	FTDI_ERR_UART_LINE,		/* OE/PE/FE/BI */
	FTDI_ERR_MPSSE_STALL,		/* a read stalls, answers late */
	FTDI_ERR_MPSSE_EXTRA,		/* stray byte ahead of a response */
	FTDI_ERR_I2C_SCL_STUCK,		/* a slave holds SCL low for good */
	FTDI_ERR_I2C_STUCK_BYTE,	/* a slave stuck in a byte holds SDA */
	FTDI_ERR_I2C_STRETCH_SHORT,	/* a slave lets SCL go just after the master */
};

/*
 * Injected MPSSE stall, in ms: longer than a driver's response timeout, as
 * a slave stretching the clock under adaptive clocking can be.
 */
#define FTDI_EMU_STALL_MS	3000

/*
 * MPSSE engine clock, in ns (eng_ns): a command starts when the previous
 * one is over, or when what it waited for (data from the host, room for a
 * response, a pin) is there. A command without clocking takes
 * FTDI_EMU_CMD_NS (SET_BITS_LOW on an FT232H: 80 to 100 ns,
 * doc/hw-design.md); clocked bits take the bit time of the divisor (AN_108
 * 3.8), 1.5 times that with 3-phase clocking (6.3), and with adaptive
 * clocking every edge waits for RTCK on GPIOL3 (6.9). A response enters
 * the RX FIFO when the engine reaches its command.
 *
 * The engine runs at most FTDI_EMU_RUN_AHEAD_NS ahead of real time; the
 * commands beyond that stay in the buffer from the host.
 */
#define FTDI_EMU_CMD_NS		80
#define FTDI_EMU_RUN_AHEAD_NS	1000000
#define FTDI_EMU_NEVER		(INT64_MAX / 4)

/* One response byte, or a SEND_IMMEDIATE, and when the engine emits it */
struct ftdi_emu_resp {
	long long t_ns;
	uint16_t  v;			/* byte, or FTDI_EMU_RESP_FLUSH */
};
#define FTDI_EMU_RESP_FLUSH	0x100

/*
 * One 8-bit I/O port (ADBUS = low byte, ACBUS = high byte).
 * level = driven value for push-pull outputs, external level otherwise:
 * what the board drives ("pin" control command), else the internal pull.
 */
struct ftdi_port {
	uint8_t val;		/* output latch */
	uint8_t dir;		/* 1 = output */
	uint8_t od;		/* 1 = drive-zero-only (0x9E, FT232H) */
	uint8_t pull;		/* internal pull resistors: 1 = up, 0 = down */
	uint8_t ext_drv;	/* pins driven by the board */
	uint8_t ext_val;	/* ... and their levels */
	uint8_t exists;		/* pins physically present on this channel */
};

/* One channel = one USB interface */
struct ftdi_intf_state {
	int      index;			/* 0 = A ... 3 = D */
	bool     mpsse_capable;		/* FT232H A, FT2232H A/B, FT4232H A/B */
	bool     has_drive_zero;	/* opcode 0x9E: FT232H only */
	bool     cbus_capable;		/* CBUS bit-bang: FT232H only */

	uint8_t  bitmode;
	uint8_t  latency;

	/* MPSSE engine */
	struct ftdi_port lo;		/* xDBUS0-7 */
	struct ftdi_port hi;		/* xCBUS0-7 (not on FT4232H) */
	bool     loopback;
	bool     clk_div5;
	bool     three_phase;
	bool     adaptive;
	uint16_t clk_divisor;
	/* Buffer from the host: what the engine has not taken yet */
	uint8_t *cmdq;
	int      cmdq_head, cmdq_len, cmdq_cap;
	/*
	 * Command in execution over several steps: a byte-mode shift
	 * (0x10-0x3F) or a clock command, and what is left of it
	 */
	uint8_t  sh_cmd;		/* 0 = none */
	uint32_t sh_left;		/* data bytes, or clock pulses */
	bool     sh_first;		/* nothing shifted yet */
	bool     sh_do_loaded;		/* DO shows the next byte's first bit */
	long long eng_ns;		/* when the engine is done, see above */
	bool     eng_wait;		/* it had to wait: goes on from now */
	bool     eng_hung;		/* waiting for RTCK that never comes */
	bool     eng_rx_full;		/* stopped: no room for a response */
	bool     eng_io_wait;		/* stopped: wait-on-I/O (0x88/0x89) */
	long long eng_next;		/* when to run it again, -1: on events */
	struct ftdi_emu_resp *pend;	/* responses not emitted yet */
	int      pend_head, pend_len, pend_cap;
	int      pend_bytes;		/* ... of which response bytes */

	/* What the low byte is wired to, see ftdi_bus.h */
	bool     board_i2c;		/* an I2C bus, else SPI slaves */
	struct emu_i2c i2c;
	struct emu_spi spi;

	/*
	 * Injected stall: the read command the engine has taken (a bit-mode
	 * one is kept in held[]) is executed at stall_until_ms.
	 */
	uint8_t  held[3];
	int      held_len;
	long long stall_until_ms;	/* 0 = engine running */
	bool     warned_deadlock;
	bool     warned_gpioh;
	bool     warned_cs;

	/* UART transmitter: the chip's TX buffer; tx_acc: bit-times x 1000 */
	uint8_t *txq;
	int      txq_len;
	int      txq_cap;
	long long tx_last_ms;
	long long tx_acc;
	bool     tx_active;		/* a burst is going out: TXDEN asserted */
	unsigned long txden_pulses;	/* bursts sent */
	unsigned long rx_overruns;	/* characters lost, RX buffer full */
	unsigned long rx_holds;		/* times the receiver held its sender */
	unsigned long breaks;		/* breaks received */

	/* UART (bitmode RESET) -- a loopback plug is attached */
	uint8_t  dtr, rts;		/* as the host set them */
	uint8_t  flow_ctrl;		/* FTDI_FLOW_* */
	uint8_t  xon, xoff;
	bool     rx_hold;		/* RX buffer nearly full: sender held */
	bool     tx_xoff;		/* XOFF received: transmitter stopped */
	uint8_t  ms_last;		/* modem status last reported */
	uint32_t baud;
	uint16_t data_cfg;
	bool     brk;
	uint8_t  pending_ls;		/* one-shot line status bits */
	/* Offsets in rxq of the NULs received as breaks (BI) */
	int      brk_mark[8];
	int      brk_marks;

	/* CBUS bit-bang (FT232H, mode 0x20) */
	uint8_t  cbus_val;
	uint8_t  cbus_dir;

	/* RX FIFO: device -> host (bulk IN) */
	uint8_t *rxq;
	int      rxq_len;
	int      rxq_cap;
	bool     flush;			/* SEND_IMMEDIATE seen */

	/* Statistics */
	unsigned long out_bytes, in_bytes, uart_tx, mpsse_bad;
	unsigned long out_held;		/* bulk OUT transfers it made wait */

	uint8_t  read_counter;		/* data pattern of the counter slaves */

	enum ftdi_error_mode error_mode;
	int      error_count;		/* injections left, 0 = no limit */
	int      error_skip;		/* occasions to let pass first */
	bool     ep_in_halted;		/* bulk IN stalled until CLEAR_FEATURE */
};

struct ftdi_device {
	enum ftdi_chip    chip;
	uint16_t          vid;
	uint16_t          pid;
	uint16_t          bcd;
	int               num_interfaces;
	struct ftdi_intf_state intf[FTDI_MAX_INTERFACES];
	uint8_t           eeprom[FTDI_EEPROM_SIZE];
	bool              eeprom_loaded;	/* from a file */
	bool              eeprom_c46;		/* 64 words, read back mirrored */
	/*
	 * What the USB descriptors say: from a programmed EEPROM with a good
	 * checksum, else the chip's built-in values and no serial number
	 * (DS_FT232H 7.1). Taken when the device is plugged in.
	 */
	uint16_t          usb_vid, usb_pid;
	uint8_t           usb_attr;		/* bmAttributes */
	uint8_t           usb_max_power;	/* bMaxPower, 2 mA units */
	bool              usb_serial;		/* has a serial number string */
	bool              usb_ee;		/* strings from the EEPROM */
	/*
	 * USB device state (USB 2.0 ch. 9.1): cleared by a plug (every
	 * import), set by SET_CONFIGURATION, kept across a power cycle of
	 * the chip. ftdi_emu_configured() counts the URBs it refuses.
	 */
	bool              configured;
	unsigned long     unconfigured_urbs;
	bool              verbose;
	long long         now_ns;	/* time base, see ftdi_emu_tick() */
	long long         now_ms;
};

/* Board option: AD7 (GPIOL3) wired to AD0 (SCL) for I2C clock stretching */
extern bool ftdi_emu_ad7_wired;

/* Board option: shift-register SPI slaves on AD4-AD7, see ftdi_bus.h */
extern bool ftdi_emu_spi_echo;

/*
 * Board option: a counter device on AD4 whose chip select is active high,
 * with a pull-down on that line that wins over the chip's pull-up
 */
extern bool ftdi_emu_spi_cs_high;

/*
 * --uart-pace. Off: UART data is sent and looped back the instant it
 * arrives. On: it leaves the chip's TX buffer (1/4/2 KiB on FT232H/
 * FT2232H/FT4232H) at the baud rate, a bulk OUT completes as the buffer
 * takes its data and THRE/TEMT are reported only when the buffer is empty.
 */
extern bool ftdi_emu_uart_pace;

/* Power-on state of the chip, with a generated default EEPROM */
void ftdi_emu_init(struct ftdi_device *dev, enum ftdi_chip chip);

/* Power-on reset of every channel; the USB configuration is kept */
void ftdi_emu_power_cycle(struct ftdi_device *dev);

/* Plugging in: power cycle plus USB Default state (unconfigured) */
void ftdi_emu_plug(struct ftdi_device *dev);

void ftdi_emu_dump_stats(const struct ftdi_device *dev);

/*
 * Time base of the chip model: the server calls ftdi_emu_tick() with a
 * monotonic time in ns before it handles anything, and what is due by then
 * happens there. ftdi_emu_next_event(): ns until the next event, -1 if
 * none.
 */
void ftdi_emu_tick(struct ftdi_device *dev, long long now_ns);
long long ftdi_emu_next_event(const struct ftdi_device *dev);

/*
 * Load an EEPROM image from a file, used as it is: 256 bytes, or 128
 * bytes for a 93C46, which reads back mirrored. Returns 0 on success.
 */
int ftdi_emu_load_eeprom(struct ftdi_device *dev, const char *path);

/*
 * Contents of a generated EEPROM image. It is laid out like one written
 * by ftdi_eeprom/FT_PROG: channel type UART, a VCP driver bit per
 * channel, strings at 0xa0 and optional user data in the unused area.
 */
struct ftdi_ee_opts {
	const char *user_data;	/* ASCII text stored at 0x20, NULL = none */
	const char *product;	/* product string, NULL = chip default */
	unsigned int vcp;	/* bit n: VCP driver bit of channel n */
	bool c46;		/* 93C46: 64 words, no room for user data */
};

/* Generate an EEPROM image with valid checksum; opts NULL = defaults */
void ftdi_emu_build_eeprom(struct ftdi_device *dev,
			   const struct ftdi_ee_opts *opts);

/* Number of channels with an MPSSE engine */
int ftdi_emu_mpsse_channels(const struct ftdi_device *dev);

void ftdi_emu_fix_checksum(struct ftdi_device *dev);

/* Take the USB identity from the EEPROM as it is now (a plug does it too) */
void ftdi_emu_usb_identity(struct ftdi_device *dev);

/*
 * The board drives a pin of a channel: port 0 = xDBUS, 1 = xCBUS; level
 * 0 or 1, or -1 to let go of it (the internal pull decides again).
 * Returns -1 if there is no such pin.
 */
int ftdi_emu_set_pin(struct ftdi_device *dev, int intf_idx, int port,
		     int bit, int level);

/*
 * Handle a control request. Returns the number of bytes written to
 * resp_buf, FTDI_EMU_STALL for an unsupported request, FTDI_EMU_NO_RESPONSE
 * while the device is not configured (see ftdi_emu_configured()).
 */
#define FTDI_EMU_STALL		(-1)
#define FTDI_EMU_NO_RESPONSE	(-2)
int ftdi_emu_control(struct ftdi_device *dev,
		     uint8_t bmRequestType, uint8_t bRequest,
		     uint16_t wValue, uint16_t wIndex, uint16_t wLength,
		     uint8_t *resp_buf);

/*
 * May a non-standard request or a bulk transfer ("what", for the log)
 * reach the chip? Not before SET_CONFIGURATION: vhci_hcd can deliver URBs
 * of the previous device instance to a new import, which on a real bus end
 * in a transaction error.
 */
bool ftdi_emu_configured(struct ftdi_device *dev, const char *what);

/* Handle bulk OUT data (MPSSE commands, UART TX or bit-bang bytes) */
void ftdi_emu_bulk_out(struct ftdi_device *dev, int intf_idx,
		       const uint8_t *data, int len);

/*
 * How many bytes of a bulk OUT transfer the channel takes right now: what
 * is free in the chip's buffer from the host (MPSSE, UART), no limit
 * otherwise.
 */
int ftdi_emu_out_room(const struct ftdi_device *dev, int intf_idx);

/*
 * The server holds a bulk OUT transfer for lack of room: logged once if the
 * MPSSE is itself stopped by a full buffer to the host.
 */
void ftdi_emu_out_blocked(struct ftdi_device *dev, int intf_idx);

/* The server could not hand a bulk OUT transfer over at once: counted */
void ftdi_emu_out_held(struct ftdi_device *dev, int intf_idx);

/*
 * True when a bulk IN transfer completes now: data for a full packet, a
 * SEND_IMMEDIATE, or the latency timer expired (ms since the last packet).
 */
bool ftdi_emu_in_ready(const struct ftdi_device *dev, int intf_idx,
		       long ms_since_last_packet);

/* Latency timer of a channel in ms */
int ftdi_emu_latency(const struct ftdi_device *dev, int intf_idx);

/*
 * Produce a bulk IN transfer of at most maxlen bytes: every 512-byte packet
 * starts with the two status bytes, the first short packet ends it.
 * Returns the number of bytes, or -1 to stall the endpoint. *babble: the
 * chip sent a packet the transfer had no room for, which is lost: the host
 * controller ends the transfer with an error (USB 2.0 8.5.1, -EOVERFLOW).
 */
int ftdi_emu_bulk_in(struct ftdi_device *dev, int intf_idx,
		     uint8_t *buf, int maxlen, bool *babble);

/* True when the bulk IN endpoint must never complete (timeout injection) */
bool ftdi_emu_in_blackhole(struct ftdi_device *dev, int intf_idx);

/* Return the bytes written to buf, at most maxlen; -1: no such string */
int ftdi_emu_device_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen);
int ftdi_emu_config_descriptor(const struct ftdi_device *dev,
			       uint8_t *buf, int maxlen);
int ftdi_emu_string_descriptor(const struct ftdi_device *dev,
			       int index, uint16_t langid,
			       uint8_t *buf, int maxlen);

/*
 * Arm count injections of mode on one channel, 0 = until cleared, after
 * skip occasions for it went by untouched (the faults that are states,
 * i2c-stuck, i2c-scl-stuck and mpsse-sync, have no occasions: no skip).
 */
void ftdi_emu_set_error(struct ftdi_device *dev, int intf_idx,
			enum ftdi_error_mode mode, int count, int skip);

void ftdi_emu_clear_errors(struct ftdi_device *dev);

/* Error mode from its --error name, or -1 if unknown */
int ftdi_emu_error_from_name(const char *name);

/* Use up one injection of mode m: true if the fault must happen now */
bool ftdi_emu_error_consume(struct ftdi_intf_state *st, enum ftdi_error_mode m);
