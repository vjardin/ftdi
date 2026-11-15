/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI Hi-Speed device emulation (FT232H / FT2232H / FT4232H): USB
 * descriptors, control requests, EEPROM image, MPSSE engine, UART and
 * bit-bang data paths. Behaviour is taken from the FTDI documents
 * listed in ftdi_emu.h.
 *
 *  - FT4232H: MPSSE on channels A/B only, no ACBUS/BCBUS high byte (AN_108
 *    GPIOH table). Opcode 0x9E and CBUS bit-bang exist on FT232H only.
 *  - Bulk IN: every Hi-Speed packet (512 bytes) starts with the modem and
 *    line status bytes; data waits in the RX FIFO for a full packet,
 *    SEND_IMMEDIATE or the latency timer (AN232B-04) and is only discarded
 *    by a purge request.
 *  - UART mode: a loopback plug is attached (TXD->RXD, RTS->CTS,
 *    DTR->DSR+DCD).
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ftdi_emu.h"

#define USB_DT_DEVICE		0x01
#define USB_DT_CONFIG		0x02
#define USB_DT_STRING		0x03
#define USB_DT_INTERFACE	0x04
#define USB_DT_ENDPOINT		0x05

#define USB_REQ_GET_STATUS	0x00
#define USB_REQ_CLEAR_FEATURE	0x01
#define USB_REQ_SET_FEATURE	0x03
#define USB_REQ_SET_ADDRESS	0x05
#define USB_REQ_GET_DESCRIPTOR	0x06
#define USB_REQ_GET_CONFIG	0x08
#define USB_REQ_SET_CONFIG	0x09
#define USB_REQ_GET_INTERFACE	0x0A
#define USB_REQ_SET_INTERFACE	0x0B

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

#define STR_IDX_LANGID		0
#define STR_IDX_MANUFACTURER	1
#define STR_IDX_PRODUCT		2
#define STR_IDX_SERIAL		3

#define PIN_TCK			0x01	/* AD0: TCK/SK/SCL */
#define PIN_TDI			0x02	/* AD1: TDI/DO */
#define PIN_TDO			0x04	/* AD2: TDO/DI */
#define PIN_GPIOL1		0x20	/* AD5: wait-on-I/O / clock-until pin */
#define PIN_GPIOL3		0x80	/* AD7: adaptive clocking input */

static const char *default_manufacturer = "FTDI";
static const char *default_serial = "FT000001";

bool ftdi_emu_ad7_wired;
bool ftdi_emu_spi_echo;
bool ftdi_emu_spi_cs_high;

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

static char chan_name(int idx)
{
	return 'A' + idx;
}

/* Per-channel FIFO size (DS_FT232H, DS_FT2232H, DS_FT4232H) */
static int chip_fifo_size(enum ftdi_chip chip)
{
	switch (chip) {
	case CHIP_FT2232H: return 4096;
	case CHIP_FT4232H: return 2048;
	default:           return 1024;
	}
}

/* RX FIFO: device -> host data, until a bulk IN takes it */
static void rxq_push(struct ftdi_intf_state *st, const uint8_t *data, int len)
{
	if (len <= 0)
		return;

	if (st->rxq_len + len > FTDI_RXQ_LIMIT) {
		fprintf(stderr, "emu: ch%c RX FIFO over %d bytes, host is not reading -- dropping %d bytes\n",
			chan_name(st->index), FTDI_RXQ_LIMIT, len);
		return;
	}

	if (st->rxq_len + len > st->rxq_cap) {
		int cap = st->rxq_cap ? st->rxq_cap : 4096;
		uint8_t *nq;

		while (cap < st->rxq_len + len)
			cap *= 2;
		nq = realloc(st->rxq, cap);
		if (!nq) {
			fprintf(stderr, "emu: out of memory\n");
			exit(1);
		}
		st->rxq = nq;
		st->rxq_cap = cap;
	}
	memcpy(st->rxq + st->rxq_len, data, len);
	st->rxq_len += len;
}

static void rxq_push_byte(struct ftdi_intf_state *st, uint8_t b)
{
	rxq_push(st, &b, 1);
}

static void rxq_consume(struct ftdi_intf_state *st, int n)
{
	int i, k = 0;

	/* The breaks still in the buffer move with the data */
	for (i = 0; i < st->brk_marks; i++)
		if (st->brk_mark[i] >= n)
			st->brk_mark[k++] = st->brk_mark[i] - n;
	st->brk_marks = k;

	if (n >= st->rxq_len) {
		st->rxq_len = 0;
		return;
	}
	memmove(st->rxq, st->rxq + n, st->rxq_len - n);
	st->rxq_len -= n;
}

static void rxq_clear(struct ftdi_intf_state *st)
{
	st->rxq_len = 0;
	st->brk_marks = 0;
}

/*
 * Pin levels: push-pull outputs show their latch, drive-zero-only outputs
 * (0x9E) '0' or the external level, inputs the external level: what the
 * board drives, else the internal pull.
 */
static uint8_t port_level(const struct ftdi_port *p)
{
	uint8_t released = p->od & p->val;		/* '1' on an OD output */
	uint8_t driven = p->dir & ~released;
	uint8_t ext = (p->pull & ~p->ext_drv) | (p->ext_val & p->ext_drv);
	uint8_t lvl = (p->val & driven) | (ext & ~driven);

	return lvl & p->exists;
}

/* What the low byte of an MPSSE channel is wired to, see ftdi_bus.h */
static bool i2c_board(const struct ftdi_intf_state *st)
{
	return st->bitmode == FTDI_BITMODE_MPSSE && st->board_i2c;
}

static bool spi_board(const struct ftdi_intf_state *st)
{
	return st->bitmode == FTDI_BITMODE_MPSSE && !st->board_i2c;
}

/* A pin lets an open-drain line be high unless it drives a '0' */
static bool pin_releases(const struct ftdi_port *p, uint8_t pin)
{
	return !(p->dir & pin) || (p->val & pin);
}

/* A pin that drives a '1' push-pull: an output, not open drain (0x9E) */
static bool pin_pushes(const struct ftdi_port *p, uint8_t pins)
{
	return p->dir & p->val & ~p->od & pins;
}

static bool lo_releases_sda(const struct ftdi_intf_state *st)
{
	/* AD1 (DO) and AD2 (DI) are tied together to SDA */
	return pin_releases(&st->lo, PIN_TDI) && pin_releases(&st->lo, PIN_TDO);
}

/* The MPSSE was reset: every pin of the low byte is an input again */
static void lo_reset(struct ftdi_intf_state *st, long long t)
{
	if (i2c_board(st))
		emu_i2c_detach(st, t);
	else if (spi_board(st))
		emu_spi_pins(st, t, port_level(&st->lo));
}

/* The low byte changed at engine time t: tell what is wired to it */
static void lo_changed(struct ftdi_intf_state *st, long long t)
{
	if (i2c_board(st))
		emu_i2c_master(st, t, pin_releases(&st->lo, PIN_TCK),
			       lo_releases_sda(st),
			       pin_pushes(&st->lo, PIN_TCK),
			       pin_pushes(&st->lo, PIN_TDI | PIN_TDO));
	else if (spi_board(st))
		emu_spi_pins(st, t, port_level(&st->lo));
}

static uint8_t lo_level(struct ftdi_intf_state *st, long long t)
{
	uint8_t lvl = port_level(&st->lo);

	if (i2c_board(st)) {
		/* SCL and SDA are what the bus makes them, slaves included */
		emu_i2c_advance(st, t);
		if (!st->i2c.scl)
			lvl &= ~PIN_TCK;
		lvl &= ~(PIN_TDI | PIN_TDO);
		if (st->i2c.sda)
			lvl |= PIN_TDI | PIN_TDO;
	} else if (spi_board(st) && !(st->lo.dir & PIN_TDO) &&
		   !emu_spi_miso(st)) {
		lvl &= ~PIN_TDO;
	}

	if (ftdi_emu_ad7_wired && !(st->lo.dir & PIN_GPIOL3)) {
		lvl &= ~PIN_GPIOL3;
		if (lvl & PIN_TCK)
			lvl |= PIN_GPIOL3;
	}
	return lvl;
}

/* The chip's side of a port at reset: every pin an input */
static void port_reset(struct ftdi_port *p)
{
	p->val = 0;
	p->dir = 0;
	p->od = 0;
}

/*
 * The engine stops: the command in execution never completes, what the
 * host sent is dropped, no late response
 */
static void stall_abort(struct ftdi_intf_state *st)
{
	if (st->stall_until_ms)
		fprintf(stderr, "emu: ch%c: MPSSE reset during the stall, no late response\n",
			chan_name(st->index));
	st->stall_until_ms = 0;
	st->held_len = 0;
	st->sh_cmd = 0;
	st->cmdq_head = st->cmdq_len = 0;
	st->pend_head = st->pend_len = 0;
	st->pend_bytes = 0;
	st->eng_ns = 0;
	st->eng_wait = true;
	st->eng_hung = false;
	st->eng_rx_full = false;
	st->eng_io_wait = false;
	st->eng_next = -1;
}

/* Engine defaults, at power-on and when MPSSE mode is entered */
static void mpsse_reset(struct ftdi_intf_state *st)
{
	port_reset(&st->lo);
	port_reset(&st->hi);
	st->loopback = false;
	st->clk_div5 = true;
	st->three_phase = false;
	st->adaptive = false;
	st->clk_divisor = 0;
	stall_abort(st);
}

static void intf_power_on(struct ftdi_device *dev, int idx)
{
	struct ftdi_intf_state *st = &dev->intf[idx];
	enum ftdi_error_mode em = st->error_mode;
	int ec = st->error_count, es = st->error_skip;
	/* What the board drives does not depend on the chip's power */
	uint8_t lo_drv = st->lo.ext_drv, lo_val = st->lo.ext_val;
	uint8_t hi_drv = st->hi.ext_drv, hi_val = st->hi.ext_val;
	uint8_t *q = st->rxq, *c = st->cmdq, *tq = st->txq;
	int cap = st->rxq_cap, ccap = st->cmdq_cap;
	int tcap = st->txq_cap, pcap = st->pend_cap;
	struct ftdi_emu_resp *pq = st->pend;

	memset(st, 0, sizeof(*st));
	st->pend = pq;
	st->pend_cap = pcap;
	st->rxq = q;
	st->rxq_cap = cap;
	st->cmdq = c;
	st->cmdq_cap = ccap;
	st->txq = tq;
	st->txq_cap = tcap;
	st->index = idx;
	st->error_mode = em;
	st->error_count = ec;
	st->error_skip = es;
	st->lo.ext_drv = lo_drv;
	st->lo.ext_val = lo_val;
	st->hi.ext_drv = hi_drv;
	st->hi.ext_val = hi_val;
	/* Internal 75 kOhm pull-ups (DS_FT232H 3.1, DS_FT2232H, DS_FT4232H) */
	st->lo.pull = 0xFF;
	st->hi.pull = 0xFF;

	switch (dev->chip) {
	case CHIP_FT232H:
		st->mpsse_capable = true;
		st->has_drive_zero = true;
		st->cbus_capable = true;
		st->lo.exists = 0xFF;	/* ADBUS0-7 */
		st->hi.exists = 0xFF;	/* ACBUS0-7 */
		/* "Default configuration of ACBUS7 - Input pulled down" */
		st->hi.pull = 0x7F;
		break;
	case CHIP_FT2232H:
		st->mpsse_capable = true;
		st->lo.exists = 0xFF;	/* xDBUS0-7 */
		st->hi.exists = 0xFF;	/* xCBUS0-7 */
		break;
	case CHIP_FT4232H:
		st->mpsse_capable = idx < 2;
		st->lo.exists = 0xFF;	/* xDBUS0-7 only, no high byte */
		st->hi.exists = 0x00;
		break;
	}

	if (ftdi_emu_spi_cs_high)
		st->lo.pull &= ~(1u << 4);

	st->bitmode = FTDI_BITMODE_RESET;
	st->latency = 16;
	st->baud = 9600;
	st->ms_last = FTDI_MS_RESERVED;
	mpsse_reset(st);
	emu_spi_init(st);
	emu_spi_attach(st, dev->now_ns, port_level(&st->lo));
}

static uint16_t rol16(uint16_t val, unsigned int n)
{
	return (val << n) | (val >> (16 - n));
}

/* FTDI EEPROM checksum of the first words - 1 words */
static uint16_t eeprom_checksum(const uint8_t *ee, int words)
{
	uint16_t checksum = 0xAAAA;
	int i;

	for (i = 0; i < words - 1; i++) {
		uint16_t w = ee[i * 2] | (ee[i * 2 + 1] << 8);

		checksum ^= w;
		checksum = rol16(checksum, 1);
	}
	return checksum;
}

/*
 * The last word stores the checksum. A 93C46 has 64 words, which the chip
 * reads twice: the image is its lower half repeated.
 */
static void eeprom_fix_checksum_raw(uint8_t *ee, bool c46)
{
	int words = c46 ? FTDI_EEPROM_WORDS / 2 : FTDI_EEPROM_WORDS;
	uint16_t checksum = eeprom_checksum(ee, words);

	ee[words * 2 - 2] = checksum & 0xFF;
	ee[words * 2 - 1] = (checksum >> 8) & 0xFF;
	if (c46)
		memcpy(ee + FTDI_EEPROM_SIZE / 2, ee, FTDI_EEPROM_SIZE / 2);
}

/* Programmed, with a good checksum: what the chip takes its settings from */
static bool eeprom_usable(const uint8_t *ee)
{
	int words = FTDI_EEPROM_WORDS;

	if (!memcmp(ee, ee + FTDI_EEPROM_SIZE / 2, FTDI_EEPROM_SIZE / 2))
		words /= 2;
	return (ee[words * 2 - 2] | (ee[words * 2 - 1] << 8)) ==
	       eeprom_checksum(ee, words);
}

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

int ftdi_emu_mpsse_channels(const struct ftdi_device *dev)
{
	/* FT4232H: A and B only */
	return dev->num_interfaces > 2 ? 2 : dev->num_interfaces;
}

/* Where ftdi_eeprom puts user data: past the fixed part on every chip */
#define EE_USER_DATA_OFF	0x20
#define EE_STRINGS_OFF		0xA0
#define EE_STRINGS_OFF_X232H	0x9A	/* libftdi, FT2232H and FT4232H */

/*
 * Store a string descriptor at off (wrapped into the 128 bytes of a 93C46)
 * and its offset and length at ptr; returns the offset behind it.
 */
static int eeprom_put_string(uint8_t *ee, int off, bool c46, const char *str,
			     int ptr)
{
	int at = c46 ? off & 0x7F : off;
	int room = (c46 ? 0x7E : 0xFE) - at;
	int len;

	if (2 + 2 * (int)strlen(str) > room) {
		fprintf(stderr, "emu: EEPROM string \"%s\" does not fit, left out\n",
			str);
		ee[ptr] = off;
		ee[ptr + 1] = 0;
		return off;
	}
	len = eeprom_write_string(ee, at, str);
	ee[ptr] = off;
	ee[ptr + 1] = len;
	return off + len;
}

void ftdi_emu_build_eeprom(struct ftdi_device *dev,
			   const struct ftdi_ee_opts *opts)
{
	static const struct ftdi_ee_opts defaults;
	uint8_t *ee = dev->eeprom;
	const char *product;
	int off;
	int len;

	if (!opts)
		opts = &defaults;

	memset(ee, 0xFF, FTDI_EEPROM_SIZE);
	memset(ee, 0, 0x20);

	/*
	 * Bytes 0x00/0x01: the channel type stays UART (0), MPSSE is entered
	 * at run time, it is not an EEPROM mode. The VCP driver bit is bit 4
	 * of byte 0 on FT232H; bit 3 of byte 0/1 for channels A/B and bit 7
	 * for C/D on FT2232H/FT4232H (libftdi).
	 */
	if (dev->chip == CHIP_FT232H) {
		if (opts->vcp & 1)
			ee[EE_OFF_CHAN_A] |= 0x10;
	} else {
		if (opts->vcp & 1)
			ee[EE_OFF_CHAN_A] |= 0x08;
		if (opts->vcp & 2)
			ee[EE_OFF_CHAN_B] |= 0x08;
		if (dev->chip == CHIP_FT4232H) {
			if (opts->vcp & 4)
				ee[EE_OFF_CHAN_A] |= 0x80;
			if (opts->vcp & 8)
				ee[EE_OFF_CHAN_B] |= 0x80;
		}
	}

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

	dev->eeprom_c46 = opts->c46;
	if (opts->user_data && !opts->c46) {
		len = strlen(opts->user_data);
		if (len > EE_STRINGS_OFF - EE_USER_DATA_OFF - 1)
			len = EE_STRINGS_OFF - EE_USER_DATA_OFF - 1;
		memcpy(ee + EE_USER_DATA_OFF, opts->user_data, len);
		ee[EE_USER_DATA_OFF + len] = 0;
	}

	/*
	 * The string offsets are those of a 256-byte image; in the 128 bytes
	 * of a 93C46 they wrap, right behind the fixed part (libftdi: 0x9a
	 * on FT2232H/FT4232H, 0xa0 on FT232H), and no user area is left.
	 */
	off = EE_STRINGS_OFF;
	if (opts->c46 && dev->chip != CHIP_FT232H)
		off = EE_STRINGS_OFF_X232H;
	product = opts->product ? opts->product : chip_product_name(dev->chip);
	off = eeprom_put_string(ee, off, opts->c46, default_manufacturer,
				EE_OFF_MFR_OFF);
	off = eeprom_put_string(ee, off, opts->c46, product, EE_OFF_PROD_OFF);
	eeprom_put_string(ee, off, opts->c46, default_serial, EE_OFF_SER_OFF);

	eeprom_fix_checksum_raw(ee, opts->c46);
}

void ftdi_emu_fix_checksum(struct ftdi_device *dev)
{
	eeprom_fix_checksum_raw(dev->eeprom, dev->eeprom_c46);
}

void ftdi_emu_usb_identity(struct ftdi_device *dev)
{
	const uint8_t *ee = dev->eeprom;

	dev->usb_ee = eeprom_usable(ee);
	if (!dev->usb_ee) {
		/* Built-in values, no serial number (DS_FT232H 7.1) */
		dev->usb_vid = dev->vid;
		dev->usb_pid = dev->pid;
		dev->usb_attr = 0x80;		/* bus powered */
		dev->usb_max_power = 45;	/* 90 mA */
		dev->usb_serial = false;
		return;
	}
	dev->usb_vid = ee[EE_OFF_VID_LO] | (ee[EE_OFF_VID_HI] << 8);
	dev->usb_pid = ee[EE_OFF_PID_LO] | (ee[EE_OFF_PID_HI] << 8);
	dev->usb_attr = ee[EE_OFF_CFG_DESC] | 0x80;
	dev->usb_max_power = ee[EE_OFF_MAX_POWER];
	dev->usb_serial = ee[EE_OFF_CHIP_CFG] & 0x08;
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

	for (i = 0; i < dev->num_interfaces; i++)
		intf_power_on(dev, i);

	ftdi_emu_build_eeprom(dev, NULL);
	ftdi_emu_usb_identity(dev);
}

void ftdi_emu_power_cycle(struct ftdi_device *dev)
{
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		rxq_clear(&dev->intf[i]);
		intf_power_on(dev, i);
	}
	fprintf(stderr, "emu: power cycle -- all channels back to power-on state\n");
}

void ftdi_emu_plug(struct ftdi_device *dev)
{
	ftdi_emu_power_cycle(dev);
	/* The chip reads its EEPROM when it starts */
	ftdi_emu_usb_identity(dev);
	/* A new device on the bus starts unconfigured (Default state) */
	dev->configured = false;
	dev->unconfigured_urbs = 0;
}

bool ftdi_emu_configured(struct ftdi_device *dev, const char *what)
{
	if (dev->configured)
		return true;
	if (!dev->unconfigured_urbs++)
		fprintf(stderr, "emu: %s before SET_CONFIGURATION (URB of a previous device instance?) -- no response\n",
			what);
	return false;
}

static void uart_stats(const struct ftdi_device *dev,
		       const struct ftdi_intf_state *st);

void ftdi_emu_dump_stats(const struct ftdi_device *dev)
{
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		const struct ftdi_intf_state *st = &dev->intf[i];

		fprintf(stderr, "emu: stats ch%c bitmode=0x%02x out=%lu in=%lu uart_tx=%lu mpsse_bad=%lu rxq=%d out_held=%lu\n",
			chan_name(i), st->bitmode, st->out_bytes, st->in_bytes,
			st->uart_tx, st->mpsse_bad, st->rxq_len, st->out_held);
		/* Ports: value driven / direction / open-drain mask */
		fprintf(stderr, "emu: state ch%c bitmode=0x%02x lo=%02x/%02x/%02x hi=%02x/%02x/%02x cbus=%x/%x baud=%u latency=%u\n",
			chan_name(i), st->bitmode, st->lo.val & st->lo.dir,
			st->lo.dir, st->lo.od, st->hi.val & st->hi.dir,
			st->hi.dir, st->hi.od, st->cbus_val & st->cbus_dir,
			st->cbus_dir, st->baud, st->latency);
		uart_stats(dev, st);
		emu_i2c_stats(st);
		emu_spi_stats(st);
	}
	fprintf(stderr, "emu: stats configured=%d unconfigured_urbs=%lu\n",
		dev->configured, dev->unconfigured_urbs);
}

void ftdi_emu_set_error(struct ftdi_device *dev, int intf_idx,
			enum ftdi_error_mode mode, int count, int skip)
{
	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return;

	dev->intf[intf_idx].error_mode = mode;
	dev->intf[intf_idx].error_count = count;
	dev->intf[intf_idx].error_skip = skip;

	fprintf(stderr, "emu: error injection set: intf=%d mode=%d count=%d skip=%d\n",
		intf_idx, mode, count, skip);
}

void ftdi_emu_clear_errors(struct ftdi_device *dev)
{
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		dev->intf[i].error_mode = FTDI_ERR_NONE;
		dev->intf[i].error_count = 0;
		dev->intf[i].error_skip = 0;
	}
}

int ftdi_emu_set_pin(struct ftdi_device *dev, int intf_idx, int port,
		     int bit, int level)
{
	struct ftdi_intf_state *st;
	struct ftdi_port *p;
	uint8_t mask;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces || bit < 0 ||
	    bit > 7)
		return -1;
	st = &dev->intf[intf_idx];
	p = port ? &st->hi : &st->lo;
	mask = 1u << bit;
	if (!(p->exists & mask))
		return -1;

	if (level < 0) {
		p->ext_drv &= ~mask;
	} else {
		p->ext_drv |= mask;
		p->ext_val = (p->ext_val & ~mask) | (level ? mask : 0);
	}
	fprintf(stderr, "emu: ch%c: board %s %c%cBUS%d\n", chan_name(intf_idx),
		level < 0 ? "lets go of" : level ? "drives high" : "drives low",
		chan_name(intf_idx), port ? 'C' : 'D', bit);
	ftdi_emu_tick(dev, dev->now_ns);
	return 0;
}

int ftdi_emu_error_from_name(const char *name)
{
	static const struct {
		const char *name;
		enum ftdi_error_mode mode;
	} modes[] = {
		{ "none",        FTDI_ERR_NONE },
		{ "i2c-nak",     FTDI_ERR_I2C_NAK },
		{ "i2c-stuck",   FTDI_ERR_I2C_BUS_STUCK },
		{ "i2c-frozen",  FTDI_ERR_I2C_BUS_STUCK },
		{ "i2c-stretch", FTDI_ERR_I2C_CLK_STRETCH },
		{ "i2c-scl-stuck", FTDI_ERR_I2C_SCL_STUCK },
		{ "i2c-stuck-byte", FTDI_ERR_I2C_STUCK_BYTE },
		{ "i2c-stretch-short", FTDI_ERR_I2C_STRETCH_SHORT },
		{ "usb-stall",   FTDI_ERR_USB_STALL },
		{ "usb-timeout", FTDI_ERR_USB_TIMEOUT },
		{ "mpsse-sync",  FTDI_ERR_MPSSE_SYNC },
		{ "mpsse-stall", FTDI_ERR_MPSSE_STALL },
		{ "mpsse-extra", FTDI_ERR_MPSSE_EXTRA },
		{ "uart-line",   FTDI_ERR_UART_LINE },
	};
	unsigned int i;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (!strcasecmp(name, modes[i].name))
			return modes[i].mode;
	return -1;
}

bool ftdi_emu_error_consume(struct ftdi_intf_state *st, enum ftdi_error_mode m)
{
	if (st->error_mode != m)
		return false;
	if (st->error_skip > 0) {
		st->error_skip--;
		return false;
	}
	if (st->error_count > 0 && --st->error_count == 0)
		st->error_mode = FTDI_ERR_NONE;
	return true;
}

int ftdi_emu_load_eeprom(struct ftdi_device *dev, const char *path)
{
	uint8_t extra;
	FILE *f;
	size_t n;

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -1;
	}
	n = fread(dev->eeprom, 1, FTDI_EEPROM_SIZE, f);
	if (n == FTDI_EEPROM_SIZE && fread(&extra, 1, 1, f) == 1)
		n++;
	fclose(f);

	if (n == FTDI_EEPROM_SIZE / 2) {
		/* 93C46: 64 words, the chip reads them twice */
		memcpy(dev->eeprom + n, dev->eeprom, n);
	} else if (n != FTDI_EEPROM_SIZE) {
		fprintf(stderr, "%s: %s%d bytes, an image is %d bytes (or %d for a 93C46)\n",
			path, n > FTDI_EEPROM_SIZE ? "more than " : "",
			(int)(n > FTDI_EEPROM_SIZE ? FTDI_EEPROM_SIZE : n),
			FTDI_EEPROM_SIZE, FTDI_EEPROM_SIZE / 2);
		return -1;
	}

	dev->eeprom_loaded = true;
	return 0;
}

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
		dev->usb_vid & 0xFF,	/* idVendor */
		(dev->usb_vid >> 8) & 0xFF,
		dev->usb_pid & 0xFF,	/* idProduct */
		(dev->usb_pid >> 8) & 0xFF,
		dev->bcd & 0xFF,	/* bcdDevice: the chip, not the EEPROM */
		(dev->bcd >> 8) & 0xFF,
		STR_IDX_MANUFACTURER,	/* iManufacturer */
		STR_IDX_PRODUCT,	/* iProduct */
		dev->usb_serial ? STR_IDX_SERIAL : 0,	/* iSerialNumber */
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
	/* Config (9) + per interface: interface (9) + 2 endpoints (7) = 23 */
	int full_len = 9 + nintf * (9 + 7 + 7);
	uint8_t tmp[9 + FTDI_MAX_INTERFACES * 23];
	int pos = 0;
	int i;
	uint8_t ep_in, ep_out;

	tmp[pos++] = 9;				/* bLength */
	tmp[pos++] = USB_DT_CONFIG;		/* bDescriptorType */
	tmp[pos++] = full_len & 0xFF;		/* wTotalLength */
	tmp[pos++] = (full_len >> 8) & 0xFF;
	tmp[pos++] = nintf;			/* bNumInterfaces */
	tmp[pos++] = 1;				/* bConfigurationValue */
	tmp[pos++] = 0;				/* iConfiguration */
	tmp[pos++] = dev->usb_attr;		/* bmAttributes */
	tmp[pos++] = dev->usb_max_power;	/* bMaxPower */

	for (i = 0; i < nintf; i++) {
		/* Endpoint numbering of the FTDI chips */
		ep_in  = 0x81 + i * 2;
		ep_out = 0x02 + i * 2;

		tmp[pos++] = 9;
		tmp[pos++] = USB_DT_INTERFACE;
		tmp[pos++] = i;			/* bInterfaceNumber */
		tmp[pos++] = 0;			/* bAlternateSetting */
		tmp[pos++] = 2;			/* bNumEndpoints */
		tmp[pos++] = 0xFF;		/* bInterfaceClass: vendor */
		tmp[pos++] = 0xFF;		/* bInterfaceSubClass: vendor */
		tmp[pos++] = 0xFF;		/* bInterfaceProtocol: vendor */
		tmp[pos++] = STR_IDX_PRODUCT;	/* iInterface */

		tmp[pos++] = 7;
		tmp[pos++] = USB_DT_ENDPOINT;
		tmp[pos++] = ep_in;		/* bEndpointAddress */
		tmp[pos++] = 0x02;		/* bmAttributes: bulk */
		tmp[pos++] = FTDI_HS_MAX_PACKET & 0xFF;
		tmp[pos++] = FTDI_HS_MAX_PACKET >> 8;
		tmp[pos++] = 0;			/* bInterval */

		tmp[pos++] = 7;
		tmp[pos++] = USB_DT_ENDPOINT;
		tmp[pos++] = ep_out;		/* bEndpointAddress */
		tmp[pos++] = 0x02;		/* bmAttributes: bulk */
		tmp[pos++] = FTDI_HS_MAX_PACKET & 0xFF;
		tmp[pos++] = FTDI_HS_MAX_PACKET >> 8;
		tmp[pos++] = 0;			/* bInterval */
	}

	if (maxlen > pos)
		maxlen = pos;
	memcpy(buf, tmp, maxlen);
	return maxlen;
}

/*
 * Strings come from a usable EEPROM when it holds valid string descriptors,
 * otherwise they are the built-in ones of the chip. A descriptor cut short
 * by wLength keeps its bLength.
 */
static int eeprom_string(const struct ftdi_device *dev, int off_byte,
			 int len_byte, uint8_t *buf, int maxlen)
{
	unsigned int off = dev->eeprom[off_byte];
	unsigned int len = dev->eeprom[len_byte];

	if (!dev->usb_ee)
		return -1;
	if (len < 2 || off + len > FTDI_EEPROM_SIZE - 2 ||
	    dev->eeprom[off + 1] != USB_DT_STRING || dev->eeprom[off] != len)
		return -1;
	if ((int)len > maxlen)
		len = maxlen;
	memcpy(buf, &dev->eeprom[off], len);
	return len;
}

int ftdi_emu_string_descriptor(const struct ftdi_device *dev,
			       int index, uint16_t langid,
			       uint8_t *buf, int maxlen)
{
	const char *str;
	int slen, dlen, i, r;

	(void)langid;

	if (maxlen <= 0)
		return 0;

	if (index == STR_IDX_LANGID) {
		uint8_t langs[4] = { 4, USB_DT_STRING, 0x09, 0x04 };

		dlen = maxlen < 4 ? maxlen : 4;
		memcpy(buf, langs, dlen);
		return dlen;
	}

	switch (index) {
	case STR_IDX_MANUFACTURER:
		r = eeprom_string(dev, EE_OFF_MFR_OFF, EE_OFF_MFR_LEN, buf, maxlen);
		if (r > 0)
			return r;
		str = default_manufacturer;
		break;
	case STR_IDX_PRODUCT:
		r = eeprom_string(dev, EE_OFF_PROD_OFF, EE_OFF_PROD_LEN, buf, maxlen);
		if (r > 0)
			return r;
		str = chip_product_name(dev->chip);
		break;
	case STR_IDX_SERIAL:
		if (!dev->usb_serial)
			return -1;
		r = eeprom_string(dev, EE_OFF_SER_OFF, EE_OFF_SER_LEN, buf, maxlen);
		if (r > 0)
			return r;
		str = default_serial;
		break;
	default:
		return -1;
	}

	slen = strlen(str);
	dlen = 2 + slen * 2;
	if (dlen > maxlen)
		dlen = maxlen;

	buf[0] = 2 + slen * 2;
	if (dlen >= 2)
		buf[1] = USB_DT_STRING;
	for (i = 0; i < slen && 2 + i * 2 + 1 < dlen; i++) {
		buf[2 + i * 2] = str[i];
		buf[2 + i * 2 + 1] = 0;
	}

	return dlen;
}

static bool uart_mode(const struct ftdi_intf_state *st)
{
	return st->bitmode == FTDI_BITMODE_RESET ||
	       st->bitmode == FTDI_BITMODE_CBUS;
}

/*
 * RTS# and DTR# as the pins show them: what the host asked for, and with
 * hardware flow control the receiver drops its line when "it cannot receive
 * any more" (AN232B-04 4.3).
 */
static bool uart_rts(const struct ftdi_intf_state *st)
{
	return st->rts && !((st->flow_ctrl & FTDI_FLOW_RTS_CTS) && st->rx_hold);
}

static bool uart_dtr(const struct ftdi_intf_state *st)
{
	return st->dtr && !((st->flow_ctrl & FTDI_FLOW_DTR_DSR) && st->rx_hold);
}

static uint8_t modem_status(const struct ftdi_intf_state *st)
{
	uint8_t ms = FTDI_MS_RESERVED;

	/* Loopback plug wiring is only meaningful in UART/CBUS mode */
	if (uart_mode(st)) {
		if (uart_rts(st))
			ms |= FTDI_MS_CTS;
		if (uart_dtr(st))
			ms |= FTDI_MS_DSR | FTDI_MS_RLSD;
	}
	return ms;
}

/*
 * A change of level on CTS#, DSR#, DCD# or RI# makes the chip "pass back
 * the current buffer even though it may be empty" (AN232B-04 3.1).
 */
static void uart_status_changed(struct ftdi_intf_state *st)
{
	uint8_t ms = modem_status(st);

	if (ms != st->ms_last)
		st->flush = true;
	st->ms_last = ms;
}

/* THRE/TEMT: nothing left in the chip's TX buffer (see --uart-pace) */
static uint8_t line_status(const struct ftdi_intf_state *st)
{
	return st->txq_len ? 0 : FTDI_LS_THRE | FTDI_LS_TEMT;
}

/*
 * Decode the H-type baud rate divisor (see AN_120 and ftdi_sio.c):
 * bits 0-13 integer part, bits 14-16 sub-integer code, bit 17 selects the
 * 120 MHz (10x oversampling, 12 MHz base) clock, otherwise 3 MHz base.
 */
static uint32_t decode_baud(uint32_t div)
{
	static const uint8_t frac_eighths[8] = { 0, 4, 2, 1, 3, 5, 6, 7 };
	uint32_t base = (div & 0x20000) ? 12000000 : 3000000;
	uint32_t ip = div & 0x3FFF;
	uint32_t code = (div >> 14) & 0x7;
	uint32_t eighths;

	if ((div & 0x1FFFF) == 0)
		return base;			/* divisor 1.0 */
	if ((div & 0x1FFFF) == 1)
		return base * 2 / 3;		/* divisor 1.5 */

	eighths = ip * 8 + frac_eighths[code];
	if (!eighths)
		return 0;
	return (uint32_t)((8ULL * base + eighths / 2) / eighths);
}

/* wIndex of a vendor request: 1-based channel, 0 is the first one too */
static int intf_from_windex(const struct ftdi_device *dev, uint16_t wIndex)
{
	int idx;

	if ((wIndex & 0xFF) == 0)
		return 0;

	idx = (wIndex & 0xFF) - 1;
	if (idx < 0 || idx >= dev->num_interfaces)
		return -1;

	return idx;
}

/*
 * CBUS bit-bang (FT232H): bits 0-3 are ACBUS5, 6, 8 and 9, and only those
 * the EEPROM sets to I/O mode take part ("ACBUS Bit Bang", DS_FT232H 3.5;
 * libftdi: function 8 in the nibbles from 0x18, two pins per byte).
 */
static uint8_t cbus_io_mask(const struct ftdi_device *dev)
{
	const uint8_t *ee = dev->eeprom;
	uint8_t mask = 0;

	if (!dev->usb_ee)
		return 0;
	if ((ee[0x1A] >> 4) == 8)
		mask |= 1;	/* ACBUS5 */
	if ((ee[0x1B] & 0x0F) == 8)
		mask |= 2;	/* ACBUS6 */
	if ((ee[0x1C] & 0x0F) == 8)
		mask |= 4;	/* ACBUS8 */
	if ((ee[0x1C] >> 4) == 8)
		mask |= 8;	/* ACBUS9 */
	return mask;
}

static void uart_flow_changed(struct ftdi_device *dev,
			      struct ftdi_intf_state *st);
static void uart_receive_break(struct ftdi_device *dev,
			       struct ftdi_intf_state *st);

static void set_bitmode(struct ftdi_device *dev, struct ftdi_intf_state *st,
			uint8_t mode, uint8_t mask)
{
	char ch = chan_name(st->index);

	switch (mode) {
	case FTDI_BITMODE_RESET:
		/* Leaving MPSSE stops the engine (AN_135 4.2: MPSSE reset) */
		if (st->bitmode == FTDI_BITMODE_MPSSE) {
			stall_abort(st);
			/* ... and the pins stop driving the bus */
			port_reset(&st->lo);
			lo_reset(st, dev->now_ns);
		}
		st->bitmode = FTDI_BITMODE_RESET;
		break;

	case FTDI_BITMODE_BITBANG:
		/* Async bit-bang replaces the UART on this channel */
		st->bitmode = FTDI_BITMODE_BITBANG;
		st->lo.dir = mask;
		st->lo.od = 0;
		break;

	case FTDI_BITMODE_MPSSE:
		if (!st->mpsse_capable) {
			fprintf(stderr, "emu: ch%c: SET_BITMODE MPSSE ignored -- %s channel %c has no MPSSE\n",
				ch, chip_product_name(dev->chip), ch);
			return;
		}
		if (st->bitmode != FTDI_BITMODE_MPSSE)
			mpsse_reset(st);
		st->bitmode = FTDI_BITMODE_MPSSE;
		/* Every pin is an input again: the bus sees them let go */
		lo_reset(st, dev->now_ns);
		break;

	case FTDI_BITMODE_CBUS:
		if (!st->cbus_capable) {
			fprintf(stderr, "emu: ch%c: CBUS bit-bang not available on %s\n",
				ch, chip_product_name(dev->chip));
			return;
		}
		st->bitmode = FTDI_BITMODE_CBUS;
		st->cbus_dir = (mask >> 4) & cbus_io_mask(dev);
		st->cbus_val = mask & 0x0F;
		break;

	default:
		fprintf(stderr, "emu: ch%c: bitmode 0x%02x not emulated, ignored\n",
			ch, mode);
		return;
	}
}

static int handle_vendor_out(struct ftdi_device *dev,
			     uint8_t bRequest, uint16_t wValue,
			     uint16_t wIndex)
{
	int intf;
	struct ftdi_intf_state *st;
	uint32_t baud;

	/* EEPROM requests address the device, wIndex is a word address */
	if (bRequest == FTDI_REQ_WRITE_EEPROM) {
		if (wIndex >= FTDI_EEPROM_WORDS)
			return -1;
		if (dev->eeprom_c46)
			wIndex &= FTDI_EEPROM_WORDS / 2 - 1;
		dev->eeprom[wIndex * 2] = wValue & 0xFF;
		dev->eeprom[wIndex * 2 + 1] = (wValue >> 8) & 0xFF;
		if (dev->eeprom_c46)
			memcpy(dev->eeprom + FTDI_EEPROM_SIZE / 2, dev->eeprom,
			       FTDI_EEPROM_SIZE / 2);
		return 0;
	}
	if (bRequest == FTDI_REQ_ERASE_EEPROM) {
		memset(dev->eeprom, 0xFF, FTDI_EEPROM_SIZE);
		return 0;
	}

	intf = intf_from_windex(dev, wIndex);
	if (intf < 0)
		return -1;
	st = &dev->intf[intf];

	switch (bRequest) {
	case FTDI_REQ_RESET:
		switch (wValue) {
		case FTDI_RESET_SIO:
			/*
			 * Resets the UART and both FIFOs. FTDI documents no
			 * effect on the bit mode (FT_SetBitMode(0, 0) is the
			 * documented way back to the EEPROM-defined mode).
			 */
			rxq_clear(st);
			st->cmdq_head = st->cmdq_len = 0;
			st->pend_head = st->pend_len = 0;
			st->pend_bytes = 0;
			st->txq_len = 0;
			st->flush = false;
			st->pending_ls = 0;
			break;
		case FTDI_RESET_PURGE_RX:
			/*
			 * Host->chip data the engine has not taken yet. The
			 * command in execution goes on and still answers.
			 */
			st->cmdq_head = st->cmdq_len = 0;
			st->txq_len = 0;	/* UART data not sent yet */
			break;
		case FTDI_RESET_PURGE_TX:
			rxq_clear(st);		/* chip->host data */
			st->flush = false;
			break;
		default:
			return -1;
		}
		ftdi_emu_tick(dev, dev->now_ns);
		return 0;

	case FTDI_REQ_SET_MODEM_CTRL:
		if (wValue & 0x0100)
			st->dtr = wValue & 0x01;
		if (wValue & 0x0200)
			st->rts = !!(wValue & 0x02);
		uart_status_changed(st);
		/* CTS or DSR may let the transmitter go on */
		ftdi_emu_tick(dev, dev->now_ns);
		return 0;

	case FTDI_REQ_SET_FLOW_CTRL:
		st->flow_ctrl = wIndex >> 8;
		st->xon = wValue & 0xFF;
		st->xoff = wValue >> 8;
		st->tx_xoff = false;
		uart_flow_changed(dev, st);
		ftdi_emu_tick(dev, dev->now_ns);
		return 0;

	case FTDI_REQ_SET_BAUDRATE:
		baud = decode_baud(wValue | ((uint32_t)(wIndex >> 8) << 16));
		if (baud != st->baud)
			fprintf(stderr, "emu: ch%c baud %u (divisor 0x%05x)\n",
				chan_name(intf), baud,
				wValue | ((uint32_t)(wIndex >> 8) << 16));
		st->baud = baud;
		return 0;

	case FTDI_REQ_SET_DATA:
		/* TXD goes low for longer than a character: a break on RXD */
		if ((wValue & 0x4000) && !st->brk && uart_mode(st))
			uart_receive_break(dev, st);
		st->brk = !!(wValue & 0x4000);
		st->data_cfg = wValue;
		ftdi_emu_tick(dev, dev->now_ns);
		return 0;

	case FTDI_REQ_SET_EVENT_CHAR:
	case FTDI_REQ_SET_ERROR_CHAR:
		return 0;

	case FTDI_REQ_SET_LATENCY_TIMER:
		st->latency = wValue & 0xFF;
		return 0;

	case FTDI_REQ_SET_BITMODE:
		set_bitmode(dev, st, (wValue >> 8) & 0xFF, wValue & 0xFF);
		return 0;

	case FTDI_REQ_SET_ERROR_MODE:
		/* Not a chip request: error injection, count 0 = for ever */
		ftdi_emu_set_error(dev, intf, wValue & 0xFF,
				   (wValue >> 8) & 0xFF, 0);
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
	int intf;
	struct ftdi_intf_state *st;

	(void)wValue;

	/* EEPROM requests address the device, wIndex is a word address */
	if (bRequest == FTDI_REQ_READ_EEPROM) {
		if (wLength < 2 || wIndex >= FTDI_EEPROM_WORDS)
			return -1;
		resp[0] = dev->eeprom[wIndex * 2];
		resp[1] = dev->eeprom[wIndex * 2 + 1];
		return 2;
	}

	intf = intf_from_windex(dev, wIndex);
	if (intf < 0)
		return -1;
	st = &dev->intf[intf];

	switch (bRequest) {
	case FTDI_REQ_GET_MODEM_STATUS:
		/* The data stage stops at wLength: one byte is the modem status */
		if (wLength < 1)
			return -1;
		resp[0] = modem_status(st);
		if (wLength < 2)
			return 1;
		resp[1] = line_status(st);
		return 2;

	case FTDI_REQ_GET_LATENCY_TIMER:
		if (wLength < 1)
			return -1;
		resp[0] = st->latency;
		return 1;

	case FTDI_REQ_READ_PINS:
		if (wLength < 1)
			return -1;
		if (st->bitmode == FTDI_BITMODE_CBUS) {
			/*
			 * CBUS pins in the low nibble: pull-ups on the inputs
			 * and on the pins that are not in I/O mode
			 */
			resp[0] = (st->cbus_val & st->cbus_dir) |
				  (0x0F & ~st->cbus_dir);
		} else {
			resp[0] = lo_level(st, dev->now_ns);
		}
		return 1;

	default:
		return -1;
	}
}

static int handle_get_descriptor(const struct ftdi_device *dev,
				 uint16_t wValue, uint16_t wIndex,
				 uint16_t wLength, uint8_t *resp)
{
	uint8_t type = wValue >> 8;
	uint8_t index = wValue & 0xFF;

	switch (type) {
	case USB_DT_DEVICE:
		return ftdi_emu_device_descriptor(dev, resp, wLength);

	case USB_DT_CONFIG:
		return ftdi_emu_config_descriptor(dev, resp, wLength);

	case USB_DT_STRING:
		return ftdi_emu_string_descriptor(dev, index, wIndex, resp,
						  wLength);

	default:
		return -1;
	}
}

static int ep_to_intf_idx(uint16_t ep_addr)
{
	int ep = ep_addr & 0x0F;

	if (ep == 0)
		return -1;
	return (ep - 1) / 2;
}

int ftdi_emu_control(struct ftdi_device *dev,
		     uint8_t bmRequestType, uint8_t bRequest,
		     uint16_t wValue, uint16_t wIndex, uint16_t wLength,
		     uint8_t *resp_buf)
{
	int idx;

	/* Only standard requests are answered before SET_CONFIGURATION */
	if ((bmRequestType & 0x60) != 0x00 &&
	    !ftdi_emu_configured(dev, "vendor request"))
		return FTDI_EMU_NO_RESPONSE;

	if (bmRequestType == 0x40 || bmRequestType == 0xC0) {
		int r = bmRequestType == 0x40 ?
			handle_vendor_out(dev, bRequest, wValue, wIndex) :
			handle_vendor_in(dev, bRequest, wValue, wIndex,
					 wLength, resp_buf);

		/* What the chip would refuse: a STALL, logged */
		if (r < 0)
			fprintf(stderr, "emu: vendor request STALLed: type=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u\n",
				bmRequestType, bRequest, wValue, wIndex,
				wLength);
		return r;
	}

	switch (bmRequestType) {
	case 0x80:	/* standard, device, IN */
		switch (bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			return handle_get_descriptor(dev, wValue, wIndex,
						     wLength, resp_buf);
		case USB_REQ_GET_STATUS:
			if (wLength < 2)
				return -1;
			resp_buf[0] = 0;	/* bus powered, no remote wakeup */
			resp_buf[1] = 0;
			return 2;
		case USB_REQ_GET_CONFIG:
			if (wLength < 1)
				return -1;
			resp_buf[0] = dev->configured;
			return 1;
		}
		break;

	case 0x81:	/* standard, interface, IN */
		if (bRequest == USB_REQ_GET_STATUS && wLength >= 2) {
			resp_buf[0] = resp_buf[1] = 0;
			return 2;
		}
		if (bRequest == USB_REQ_GET_INTERFACE && wLength >= 1) {
			resp_buf[0] = 0;
			return 1;
		}
		break;

	case 0x82:	/* standard, endpoint, IN */
		if (bRequest == USB_REQ_GET_STATUS && wLength >= 2) {
			idx = ep_to_intf_idx(wIndex);
			resp_buf[0] = (idx >= 0 && idx < dev->num_interfaces &&
				       (wIndex & 0x80) &&
				       dev->intf[idx].ep_in_halted) ? 1 : 0;
			resp_buf[1] = 0;
			return 2;
		}
		break;

	case 0x00:	/* standard, device, OUT */
		if (bRequest == USB_REQ_SET_CONFIG) {
			/* One configuration (1); 0 = back to Address state */
			if ((wValue & 0xff) > 1)
				return FTDI_EMU_STALL;
			dev->configured = (wValue & 0xff) == 1;
			return 0;
		}
		if (bRequest == USB_REQ_SET_ADDRESS ||
		    bRequest == USB_REQ_CLEAR_FEATURE ||
		    bRequest == USB_REQ_SET_FEATURE)
			return 0;
		break;

	case 0x01:	/* standard, interface, OUT */
		if (bRequest == USB_REQ_SET_INTERFACE && wValue == 0)
			return 0;
		break;

	case 0x02:	/* standard, endpoint, OUT */
		if (bRequest == USB_REQ_CLEAR_FEATURE && wValue == 0) {
			/* ENDPOINT_HALT */
			idx = ep_to_intf_idx(wIndex);
			if (idx >= 0 && idx < dev->num_interfaces) {
				if ((wIndex & 0x80) && dev->intf[idx].ep_in_halted) {
					dev->intf[idx].ep_in_halted = false;
					fprintf(stderr, "emu: bulk IN halt of interface %d cleared\n",
						idx);
				}
				return 0;
			}
		}
		break;
	}

	fprintf(stderr, "emu: unhandled control: type=0x%02x req=0x%02x "
		"val=0x%04x idx=0x%04x len=%u\n",
		bmRequestType, bRequest, wValue, wIndex, wLength);
	return -1;
}

/*
 * Bytes of a command the engine needs before it can start it. A byte-mode
 * shift starts with its opcode and length; its data is taken a byte at a
 * time. Unknown opcodes are 1 byte long.
 */
static int mpsse_cmd_len(const struct ftdi_intf_state *st, uint8_t cmd)
{
	if (cmd >= 0x10 && cmd <= 0x3F) {
		if (cmd & 0x02)			/* bit mode: all of it */
			return 2 + ((cmd & 0x10) ? 1 : 0);
		return 3;
	}

	switch (cmd) {
	case 0x4A: case 0x4B:			/* TMS out */
	case 0x6A: case 0x6B: case 0x6E: case 0x6F:	/* TMS out + read */
		return 3;
	case MPSSE_SET_BITS_LOW:
	case MPSSE_SET_BITS_HIGH:
	case MPSSE_SET_CLK_DIVISOR:
	case MPSSE_CLK_N_BYTES:
	case MPSSE_CLK_N_UNTIL_HIGH:
	case MPSSE_CLK_N_UNTIL_LOW:
		return 3;
	case MPSSE_CLK_N_BITS:
		return 2;
	case MPSSE_DRIVE_ZERO_ONLY:
		return st->has_drive_zero ? 3 : 1;
	default:
		return 1;
	}
}

static bool mpsse_cmd_known(const struct ftdi_intf_state *st, uint8_t cmd)
{
	if (cmd >= 0x10 && cmd <= 0x3F)
		return true;

	switch (cmd) {
	case 0x4A: case 0x4B:
	case 0x6A: case 0x6B: case 0x6E: case 0x6F:
	case MPSSE_SET_BITS_LOW:
	case MPSSE_GET_BITS_LOW:
	case MPSSE_SET_BITS_HIGH:
	case MPSSE_GET_BITS_HIGH:
	case MPSSE_LOOPBACK_ON:
	case MPSSE_LOOPBACK_OFF:
	case MPSSE_SET_CLK_DIVISOR:
	case MPSSE_SEND_IMMEDIATE:
	case MPSSE_WAIT_IO_HIGH:
	case MPSSE_WAIT_IO_LOW:
	case MPSSE_DISABLE_CLK_DIV5:
	case MPSSE_ENABLE_CLK_DIV5:
	case MPSSE_ENABLE_3PHASE:
	case MPSSE_DISABLE_3PHASE:
	case MPSSE_CLK_N_BITS:
	case MPSSE_CLK_N_BYTES:
	case MPSSE_CLK_UNTIL_HIGH:
	case MPSSE_CLK_UNTIL_LOW:
	case MPSSE_ENABLE_ADAPTIVE:
	case MPSSE_DISABLE_ADAPTIVE:
	case MPSSE_CLK_N_UNTIL_HIGH:
	case MPSSE_CLK_N_UNTIL_LOW:
		return true;
	case MPSSE_DRIVE_ZERO_ONLY:
		return st->has_drive_zero;
	default:
		return false;
	}
}

/* Response bytes of the next step of a command */
static int mpsse_cmd_emits(const struct ftdi_intf_state *st, uint8_t cmd)
{
	if (cmd >= 0x10 && cmd <= 0x3F)
		return (cmd & 0x20) ? 1 : 0;

	switch (cmd) {
	case 0x6A: case 0x6B: case 0x6E: case 0x6F:
	case MPSSE_GET_BITS_LOW:
	case MPSSE_GET_BITS_HIGH:
		return 1;
	default:
		/* Bad command: 0xFA and the opcode, see mpsse_exec() */
		return !mpsse_cmd_known(st, cmd) && (cmd & 0x80) ? 2 : 0;
	}
}

static void cmdq_take(struct ftdi_intf_state *st, int n)
{
	st->cmdq_head += n;
	st->cmdq_len -= n;
	if (!st->cmdq_len)
		st->cmdq_head = 0;
}

/* Data from the host; the caller asked ftdi_emu_out_room() first */
static void cmdq_push(struct ftdi_device *dev, struct ftdi_intf_state *st,
		      const uint8_t *data, int len)
{
	int cap = chip_fifo_size(dev->chip);

	if (st->cmdq_cap < cap) {
		uint8_t *nb = realloc(st->cmdq, cap);

		if (!nb) {
			fprintf(stderr, "emu: out of memory\n");
			exit(1);
		}
		st->cmdq = nb;
		st->cmdq_cap = cap;
	}
	if (len > cap - st->cmdq_len) {
		fprintf(stderr, "emu: ch%c: buffer from the host overrun, %d byte(s) dropped\n",
			chan_name(st->index), len - (cap - st->cmdq_len));
		len = cap - st->cmdq_len;
	}
	if (st->cmdq_head + st->cmdq_len + len > cap) {
		memmove(st->cmdq, st->cmdq + st->cmdq_head, st->cmdq_len);
		st->cmdq_head = 0;
	}
	memcpy(st->cmdq + st->cmdq_head + st->cmdq_len, data, len);
	st->cmdq_len += len;
}

static void resp_emit(struct ftdi_intf_state *st, uint16_t v)
{
	if (v == FTDI_EMU_RESP_FLUSH)
		st->flush = true;
	else
		rxq_push_byte(st, (uint8_t)v);
}

/* Hand over what the engine has produced by now */
static void resp_drain(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	while (st->pend_head < st->pend_len &&
	       st->pend[st->pend_head].t_ns <= dev->now_ns) {
		uint16_t v = st->pend[st->pend_head++].v;

		if (v != FTDI_EMU_RESP_FLUSH)
			st->pend_bytes--;
		resp_emit(st, v);
	}
	if (st->pend_head == st->pend_len)
		st->pend_head = st->pend_len = 0;
}

/* A response byte (or SEND_IMMEDIATE) leaves the engine at time t */
static void resp_queue(struct ftdi_device *dev, struct ftdi_intf_state *st,
		       long long t, uint16_t v)
{
	if (st->pend_head == st->pend_len && t <= dev->now_ns) {
		resp_emit(st, v);
		return;
	}
	if (st->pend_len == st->pend_cap) {
		int cap = st->pend_cap ? st->pend_cap * 2 : 1024;
		struct ftdi_emu_resp *nb;

		if (cap > FTDI_RXQ_LIMIT) {
			fprintf(stderr, "emu: ch%c: response queue overflow, byte dropped\n",
				chan_name(st->index));
			return;
		}
		nb = realloc(st->pend, cap * sizeof(*nb));
		if (!nb) {
			fprintf(stderr, "emu: out of memory\n");
			exit(1);
		}
		st->pend = nb;
		st->pend_cap = cap;
	}
	st->pend[st->pend_len].t_ns = t;
	st->pend[st->pend_len].v = v;
	st->pend_len++;
	if (v != FTDI_EMU_RESP_FLUSH)
		st->pend_bytes++;
}

/* Room in the buffer to the host, responses on their way counted */
static int rx_room(const struct ftdi_device *dev,
		   const struct ftdi_intf_state *st)
{
	return chip_fifo_size(dev->chip) - st->rxq_len - st->pend_bytes;
}

/*
 * Half a TCK period in ns: TCK = 60 MHz / ((1 + divisor) * 2), from
 * 12 MHz with the divide-by-5 prescaler (AN_108 3.8.2).
 */
static long long clk_half_ns(const struct ftdi_intf_state *st)
{
	long long n = (1LL + st->clk_divisor) * (st->clk_div5 ? 250 : 50);

	return (n + 1) / 3;
}

/*
 * Adaptive clocking (AN_108 6.9): after TCK changed, the engine waits
 * until RTCK, on GPIOL3 (AD7), shows the same level. With AD7 wired to SCL
 * an I2C slave stretching the clock holds the master that way; without the
 * wire AD7 floats high, never follows TCK low and the engine stops for
 * good. Returns the time at which the engine goes on.
 */
static long long rtck_wait(struct ftdi_intf_state *st, long long t)
{
	bool tck = st->lo.val & PIN_TCK;
	long long rel = -1;

	if (!st->adaptive)
		return t;
	if (!!(lo_level(st, t) & PIN_GPIOL3) == tck)
		return t;

	/* Only a slave letting go of SCL makes RTCK follow later */
	if (tck && i2c_board(st) && ftdi_emu_ad7_wired)
		rel = emu_i2c_scl_release(st, t);
	if (rel > 0 && (lo_level(st, rel) & PIN_GPIOL3))
		return rel;

	fprintf(stderr, "emu: ch%c: adaptive clocking: RTCK (AD7) never follows TCK %s, the engine waits for ever\n",
		chan_name(st->index), tck ? "high" : "low");
	st->eng_hung = true;
	return FTDI_EMU_NEVER;
}

/* Align bits read in bit mode the way the MPSSE does (AN_108 3.3.7) */
static uint8_t bit_read_align(uint8_t sent, int nbits, bool lsb_first)
{
	if (nbits >= 8)
		return sent;
	if (lsb_first)
		return (uint8_t)(sent << (8 - nbits));
	return sent >> (8 - nbits);
}

static bool shift_bit(const uint8_t *w, int i, bool lsb_first)
{
	uint8_t b = w[i / 8];

	return lsb_first ? (b >> (i % 8)) & 1 : (b >> (7 - i % 8)) & 1;
}

static void do_set(struct ftdi_intf_state *st, long long t, bool v)
{
	if (v)
		st->lo.val |= PIN_TDI;
	else
		st->lo.val &= ~PIN_TDI;
	lo_changed(st, t);
}

static bool di_level(struct ftdi_intf_state *st, long long t)
{
	if (st->loopback)
		return st->lo.val & PIN_TDI;	/* 0x84: DO looped to DI */
	return lo_level(st, t) & PIN_TDO;
}

/*
 * Data shifting, 0x10-0x3F, edge by edge as AN_108 describes it.
 * 2.2: a clock pulse is the CLK pin toggled twice from the level it has,
 * so it idles where SET_BITS_LOW left it.
 * 3.3: the first bit is put on DO, then the pin is clocked; DO changes on
 * the edge the opcode names (bit 0: 0 = rising, 1 = falling) and DI is
 * sampled just before the edge it names (bit 2, same coding).
 * 6.3/6.4: a bit is half a period of data setup, then the pulse; 3-phase
 * clocking adds half a period of hold, and DO then only changes when the
 * next bit starts.
 *
 * This shifts total bits of the command: all of a bit-mode one, a byte of
 * a byte-mode one. wdata is what goes out (NULL: a read only); nextbit the
 * bit that follows in the command, which DO takes on the last edge that
 * changes it (-1: none, or the host has not sent it yet). t is when it
 * starts; returns when it is over.
 */
static long long mpsse_shift(struct ftdi_device *dev,
			     struct ftdi_intf_state *st, uint8_t cmd,
			     const uint8_t *wdata, int total, int nextbit,
			     long long t)
{
	bool write = cmd & 0x10;
	bool read = cmd & 0x20;
	bool lsb_first = cmd & 0x08;
	bool out_rising = !(cmd & 0x01);
	bool in_rising = !(cmd & 0x04);
	long long half = clk_half_ns(st);
	int next = 1, i, e;
	uint8_t acc = 0;

	/* DI is what the board puts on it: a read with no slave selected */
	if (read && !st->loopback && spi_board(st) &&
	    !emu_spi_cs_asserted(st) && !st->warned_cs) {
		fprintf(stderr, "emu: ch%c: SPI read with no chip select asserted\n",
			chan_name(st->index));
		st->warned_cs = true;
	}

	if (write && !st->sh_do_loaded)
		do_set(st, t, shift_bit(wdata, 0, lsb_first));
	st->sh_do_loaded = false;

	for (i = 0; i < total; i++) {
		if (write && st->three_phase && i)
			do_set(st, t, shift_bit(wdata, i, lsb_first));
		t += half;				/* data setup */

		for (e = 0; e < 2; e++) {
			bool rising = !(st->lo.val & PIN_TCK);

			if (read && rising == in_rising) {
				bool v = di_level(st, t);

				acc = lsb_first ? (acc >> 1) | (v << 7)
						: (acc << 1) | v;
			}
			st->lo.val ^= PIN_TCK;
			lo_changed(st, t);
			t = rtck_wait(st, t);
			if (t >= FTDI_EMU_NEVER)
				return t;
			if (write && !st->three_phase && rising == out_rising) {
				if (next < total) {
					do_set(st, t, shift_bit(wdata, next,
								lsb_first));
				} else if (next == total && nextbit >= 0) {
					do_set(st, t, nextbit);
					st->sh_do_loaded = true;
				}
				next++;
			}
			if (!e)
				t += half;		/* the pulse */
		}
		if (st->three_phase)
			t += half;			/* data hold */

		if (read && (i % 8 == 7 || i == total - 1)) {
			resp_queue(dev, st, t, acc);
			acc = 0;
		}
	}
	return t;
}

/* n pulses of TCK with nothing on the data pins (0x8E, 0x8F, 0x94...) */
static long long mpsse_clock(struct ftdi_intf_state *st, long long t, int n)
{
	long long half = clk_half_ns(st);
	int e;

	while (n-- > 0) {
		for (e = 0; e < 2; e++) {
			t += half;
			st->lo.val ^= PIN_TCK;
			lo_changed(st, t);
			t = rtck_wait(st, t);
			if (t >= FTDI_EMU_NEVER)
				return t;
		}
	}
	return t;
}

static void mpsse_clocking(struct ftdi_intf_state *st, bool on)
{
	emu_i2c_clocking(st, on);
	emu_spi_clocking(st, on);
}

/*
 * When the engine goes on: where it got to, or now if it had to wait for
 * something (data from the host, room for a response, a pin).
 */
static long long mpsse_now(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	if (st->eng_wait && st->eng_ns < dev->now_ns)
		st->eng_ns = dev->now_ns;
	st->eng_wait = false;
	return st->eng_ns;
}

/*
 * A command without data shifting, at p, taking effect at t. False: it
 * waits (wait-on-I/O), nothing is done.
 */
static bool mpsse_exec(struct ftdi_device *dev, struct ftdi_intf_state *st,
		       const uint8_t *p, long long t)
{
	uint8_t cmd = p[0];
	char ch = chan_name(st->index);

	switch (cmd) {
	case 0x4A: case 0x4B:
		return true;
	case 0x6A: case 0x6B: case 0x6E: case 0x6F:
		/* TMS with read: TDO sampled -> loopback of TDI, else high */
		resp_queue(dev, st, t, st->loopback ?
			   bit_read_align((p[2] & 0x80) ? 0xFF : 0x00,
					  (p[1] & 7) + 1, false) :
			   0xFF);
		return true;

	case MPSSE_SET_BITS_LOW:
		st->lo.val = p[1];
		st->lo.dir = p[2];
		lo_changed(st, t);
		return true;

	case MPSSE_GET_BITS_LOW:
		resp_queue(dev, st, t, lo_level(st, t));
		return true;

	case MPSSE_SET_BITS_HIGH:
		if (!st->hi.exists && !st->warned_gpioh) {
			fprintf(stderr, "emu: ch%c: SET_BITS_HIGH on %s -- no high-byte pins on this chip\n",
				ch, chip_product_name(dev->chip));
			st->warned_gpioh = true;
		}
		st->hi.val = p[1];
		st->hi.dir = p[2];
		return true;

	case MPSSE_GET_BITS_HIGH:
		if (!st->hi.exists && !st->warned_gpioh) {
			fprintf(stderr, "emu: ch%c: GET_BITS_HIGH on %s -- no high-byte pins on this chip\n",
				ch, chip_product_name(dev->chip));
			st->warned_gpioh = true;
		}
		resp_queue(dev, st, t, port_level(&st->hi));
		return true;

	case MPSSE_LOOPBACK_ON:
		st->loopback = true;
		return true;
	case MPSSE_LOOPBACK_OFF:
		st->loopback = false;
		return true;
	case MPSSE_SET_CLK_DIVISOR:
		st->clk_divisor = p[1] | (p[2] << 8);
		return true;
	case MPSSE_SEND_IMMEDIATE:
		resp_queue(dev, st, t, FTDI_EMU_RESP_FLUSH);
		return true;
	case MPSSE_WAIT_IO_HIGH:
	case MPSSE_WAIT_IO_LOW: {
		/* The engine goes on once GPIOL1 is at that level (AN_108 6.5) */
		bool high = lo_level(st, t) & PIN_GPIOL1;

		if (high == (cmd == MPSSE_WAIT_IO_HIGH))
			return true;
		if (!st->eng_io_wait)
			fprintf(stderr, "emu: ch%c: wait-on-I/O 0x%02x: the engine stops until GPIOL1 is %s\n",
				ch, cmd, high ? "low" : "high");
		st->eng_io_wait = true;
		return false;
	}
	case MPSSE_DISABLE_CLK_DIV5:
		st->clk_div5 = false;
		return true;
	case MPSSE_ENABLE_CLK_DIV5:
		st->clk_div5 = true;
		return true;
	case MPSSE_ENABLE_3PHASE:
		/*
		 * The board the pins go to follows who uses the channel:
		 * an I2C bus from this command on, until the command below
		 * (an MPSSE reset does not rewire anything).
		 */
		st->three_phase = true;
		if (!st->board_i2c) {
			st->board_i2c = true;
			emu_i2c_attach(st, t, pin_releases(&st->lo, PIN_TCK),
				       lo_releases_sda(st),
				       pin_pushes(&st->lo, PIN_TCK),
				       pin_pushes(&st->lo, PIN_TDI | PIN_TDO));
		}
		return true;
	case MPSSE_DISABLE_3PHASE:
		st->three_phase = false;
		if (st->board_i2c) {
			st->board_i2c = false;
			emu_spi_attach(st, t, port_level(&st->lo));
		}
		return true;
	case MPSSE_CLK_N_BITS:
		/* Clock pulses without data (AN_108 6.6) */
		st->eng_ns = mpsse_clock(st, t, (p[1] & 7) + 1);
		return true;
	case MPSSE_CLK_N_BYTES:
	case MPSSE_CLK_N_UNTIL_HIGH:
	case MPSSE_CLK_N_UNTIL_LOW:
		/* ... a byte, or a pulse, per step: see mpsse_clock_step() */
		st->sh_cmd = cmd;
		st->sh_left = ((p[1] | (p[2] << 8)) + 1) * 8;
		return true;
	case MPSSE_CLK_UNTIL_HIGH:
	case MPSSE_CLK_UNTIL_LOW:
		st->sh_cmd = cmd;
		st->sh_left = 0;
		return true;
	case MPSSE_ENABLE_ADAPTIVE:
		st->adaptive = true;
		return true;
	case MPSSE_DISABLE_ADAPTIVE:
		st->adaptive = false;
		return true;
	case MPSSE_DRIVE_ZERO_ONLY:
		if (st->has_drive_zero) {
			st->lo.od = p[1];
			st->hi.od = p[2];
			lo_changed(st, t);
			return true;
		}
		break;		/* FT2232H/FT4232H: bad command */
	}

	/*
	 * Bad command: the MPSSE answers 0xFA followed by the opcode and
	 * carries on with the next byte as a new command, which it "will only
	 * trigger when bit 7 of the rogue command is high" (AN_108 3.1).
	 * Below 0x80 a real chip decodes a shifting command from the bits:
	 * not modelled, reported all the same.
	 */
	st->mpsse_bad++;
	if (cmd & 0x80) {
		resp_queue(dev, st, t, MPSSE_BAD_CMD);
		/* mpsse-sync injection: a wrong echo */
		resp_queue(dev, st, t, st->error_mode == FTDI_ERR_MPSSE_SYNC ?
			   (uint8_t)(cmd ^ 0xFF) : cmd);
	}
	if (cmd != 0xAA && cmd != 0xAB)
		fprintf(stderr, "emu: ch%c: MPSSE bad command 0x%02x%s\n", ch,
			cmd, cmd & 0x80 ? "" : " (bit 7 clear: no 0xFA answer)");
	return true;
}

/* Commands that sample TDO/DI and answer on bulk IN */
static bool mpsse_cmd_reads(uint8_t cmd)
{
	return cmd >= 0x10 && cmd <= 0x3F && (cmd & 0x20);
}

/*
 * Clock commands in progress: 0x8F a byte of pulses per step; 0x94/0x95 a
 * pulse per step until GPIOL1 is at the level asked for (AN_108 6.7, 6.8);
 * 0x9C/0x9D the same, for the number of pulses given at most (6.11, 6.12).
 */
static bool mpsse_clock_step(struct ftdi_device *dev,
			     struct ftdi_intf_state *st)
{
	uint8_t cmd = st->sh_cmd;
	long long t = mpsse_now(dev, st);
	int n = 8;

	if (cmd != MPSSE_CLK_N_BYTES) {
		bool high = lo_level(st, t) & PIN_GPIOL1;
		bool want = cmd == MPSSE_CLK_UNTIL_HIGH ||
			    cmd == MPSSE_CLK_N_UNTIL_HIGH;

		if (high == want) {
			st->sh_cmd = 0;
			return true;
		}
		n = 1;
	}
	st->eng_ns = mpsse_clock(st, t, n);
	if (cmd == MPSSE_CLK_UNTIL_HIGH || cmd == MPSSE_CLK_UNTIL_LOW)
		return true;
	st->sh_left -= n;
	if (!st->sh_left)
		st->sh_cmd = 0;
	return true;
}

/* The next byte of a byte-mode shift, when its data and its room are there */
static bool mpsse_shift_step(struct ftdi_device *dev,
			     struct ftdi_intf_state *st)
{
	const uint8_t *p = st->cmdq + st->cmdq_head;
	uint8_t cmd = st->sh_cmd;
	bool write = cmd & 0x10;
	bool waited = st->eng_wait;
	int nextbit = -1;
	long long t;

	if (write && !st->cmdq_len)
		return false;
	if ((cmd & 0x20) && rx_room(dev, st) < 1) {
		st->eng_rx_full = true;
		return false;
	}

	t = mpsse_now(dev, st);
	/* The clock stood still: no bit time across that */
	if (waited && !st->sh_first)
		mpsse_clocking(st, true);
	st->sh_first = false;
	if (write && st->sh_left > 1 && st->cmdq_len > 1)
		nextbit = shift_bit(&p[1], 0, cmd & 0x08);
	st->eng_ns = mpsse_shift(dev, st, cmd, write ? p : NULL, 8, nextbit, t);
	if (write)
		cmdq_take(st, 1);
	if (!--st->sh_left) {
		st->sh_cmd = 0;
		mpsse_clocking(st, false);
	}
	return true;
}

/*
 * One step of the engine: the next command of the buffer, or the next part
 * of the one in progress. False: it waits (for more bytes from the host,
 * for room in the buffer to the host, for a pin).
 */
static bool mpsse_step(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	const uint8_t *p = st->cmdq + st->cmdq_head;
	long long t;
	uint8_t cmd;
	int clen;

	if (st->sh_cmd >= 0x10 && st->sh_cmd <= 0x3F)
		return mpsse_shift_step(dev, st);
	if (st->sh_cmd)
		return mpsse_clock_step(dev, st);

	if (!st->cmdq_len)
		return false;
	cmd = p[0];
	clen = mpsse_cmd_len(st, cmd);
	if (st->cmdq_len < clen)
		return false;
	if (mpsse_cmd_emits(st, cmd) > rx_room(dev, st)) {
		st->eng_rx_full = true;
		return false;
	}

	/* It starts when the engine is free, and takes effect once decoded */
	t = mpsse_now(dev, st) + FTDI_EMU_CMD_NS;

	if (mpsse_cmd_reads(cmd)) {
		/* Injected desync: a stray byte ahead of the response */
		if (ftdi_emu_error_consume(st, FTDI_ERR_MPSSE_EXTRA)) {
			fprintf(stderr, "emu: ch%c: stray response byte injected ahead of command 0x%02x\n",
				chan_name(st->index), cmd);
			resp_queue(dev, st, t, 0x5E);
		}
		/*
		 * Injected stall: this read command hangs (as on RTCK with a
		 * slave stretching the clock) and answers when it is over.
		 */
		if (ftdi_emu_error_consume(st, FTDI_ERR_MPSSE_STALL)) {
			fprintf(stderr, "emu: ch%c: MPSSE stall injected on command 0x%02x for %d ms\n",
				chan_name(st->index), cmd, FTDI_EMU_STALL_MS);
			st->stall_until_ms = dev->now_ms + FTDI_EMU_STALL_MS;
			if (cmd & 0x02) {
				memcpy(st->held, p, clen);
				st->held_len = clen;
				cmdq_take(st, clen);
				return true;
			}
		}
	}

	if (cmd >= 0x10 && cmd <= 0x3F) {
		st->sh_do_loaded = false;
		mpsse_clocking(st, true);
		if (cmd & 0x02) {
			st->eng_ns = mpsse_shift(dev, st, cmd,
						 (cmd & 0x10) ? &p[2] : NULL,
						 (p[1] & 7) + 1, -1, t);
			mpsse_clocking(st, false);
		} else {
			st->sh_cmd = cmd;
			st->sh_left = (p[1] | (p[2] << 8)) + 1;
			st->sh_first = true;
			st->eng_ns = t;
		}
		cmdq_take(st, clen);
		return true;
	}

	st->eng_ns = t;
	if (!mpsse_exec(dev, st, p, t)) {
		st->eng_ns = t - FTDI_EMU_CMD_NS;
		return false;
	}
	cmdq_take(st, clen);
	return true;
}

/*
 * Run the engine as far as it gets: until it has to wait, or is far enough
 * ahead of real time (eng_next then says when to call again).
 */
static void mpsse_run(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	st->eng_next = -1;
	if (st->bitmode != FTDI_BITMODE_MPSSE)
		return;

	for (;;) {
		if (st->eng_hung || st->stall_until_ms)
			break;
		if (!st->eng_wait &&
		    st->eng_ns > dev->now_ns + FTDI_EMU_RUN_AHEAD_NS) {
			st->eng_next = st->eng_ns - FTDI_EMU_RUN_AHEAD_NS;
			return;
		}
		st->eng_rx_full = false;
		if (!mpsse_step(dev, st))
			break;
		st->eng_io_wait = false;
		st->warned_deadlock = false;
	}
	st->eng_wait = true;
}

/* The injected stall is over: the command it held answers */
static void mpsse_stall_end(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	uint8_t cmd = st->held_len ? st->held[0] : st->sh_cmd;

	st->stall_until_ms = 0;
	fprintf(stderr, "emu: ch%c: MPSSE stall over, command 0x%02x answers now (%d byte(s) queued behind)\n",
		chan_name(st->index), cmd, st->cmdq_len);
	st->eng_wait = true;
	if (st->held_len && st->bitmode == FTDI_BITMODE_MPSSE) {
		long long t = mpsse_now(dev, st) + FTDI_EMU_CMD_NS;

		st->sh_do_loaded = false;
		mpsse_clocking(st, true);
		st->eng_ns = mpsse_shift(dev, st, cmd,
					 (cmd & 0x10) ? &st->held[2] : NULL,
					 (st->held[1] & 7) + 1, -1, t);
		mpsse_clocking(st, false);
	}
	st->held_len = 0;
	mpsse_run(dev, st);
}

bool ftdi_emu_uart_pace;

static int uart_data_bits(const struct ftdi_intf_state *st)
{
	int data = st->data_cfg & 0xFF;

	return data == 7 ? 7 : 8;
}

static int uart_frame_bits(const struct ftdi_intf_state *st)
{
	int parity = (st->data_cfg >> 8) & 0x7;
	int stop = (st->data_cfg >> 11) & 0x3;

	return 1 + uart_data_bits(st) + (parity ? 1 : 0) + (stop ? 2 : 1);
}

/*
 * The receiver holds its sender when its buffer is nearly full and lets it
 * go on once half of it is free. RTS/CTS and DTR/DSR: the line drops (see
 * uart_rts()). XON/XOFF: it sends XOFF, then XON, on TXD ahead of the
 * data; the plug brings them back to RXD, where they stop and restart the
 * transmitter (AN232B-04 4.3).
 */
static void uart_flow_changed(struct ftdi_device *dev,
			      struct ftdi_intf_state *st)
{
	int cap = chip_fifo_size(dev->chip);
	int room = cap - st->rxq_len;
	bool hold = st->rx_hold;

	if (!st->flow_ctrl || !uart_mode(st))
		hold = false;
	else if (room < FTDI_EMU_RX_HOLD_ROOM)
		hold = true;
	else if (room >= cap / 2)
		hold = false;

	if (hold != st->rx_hold) {
		st->rx_hold = hold;
		if (hold)
			st->rx_holds++;
		if (st->flow_ctrl & FTDI_FLOW_XON_XOFF)
			st->tx_xoff = hold;
	}
	uart_status_changed(st);
}

/* A character arrives on RXD */
static void uart_receive(struct ftdi_device *dev, struct ftdi_intf_state *st,
			 uint8_t b)
{
	/*
	 * In-band flow control acts on the transmitter. Whether the chip
	 * also hands these two to the host is not documented: taken out.
	 */
	if (st->flow_ctrl & FTDI_FLOW_XON_XOFF) {
		if (b == st->xoff) {
			st->tx_xoff = true;
			return;
		}
		if (b == st->xon) {
			st->tx_xoff = false;
			return;
		}
	}

	/* Nobody held the sender: "this may result in data loss" */
	if (st->rxq_len >= chip_fifo_size(dev->chip)) {
		if (!st->rx_overruns)
			fprintf(stderr, "emu: ch%c: UART RX buffer full (%d bytes), no flow control: overrun\n",
				chan_name(st->index), st->rxq_len);
		st->rx_overruns++;
		st->pending_ls |= FTDI_LS_OE;
		return;
	}
	rxq_push_byte(st, b);
	uart_flow_changed(dev, st);
}

/*
 * A break on RXD: a NUL with the break bit of the line status. The bit
 * belongs to that character: it is the last one of its packet (the host
 * drivers rely on it, see ftdi_emu_bulk_in()).
 */
static void uart_receive_break(struct ftdi_device *dev,
			       struct ftdi_intf_state *st)
{
	int max = sizeof(st->brk_mark) / sizeof(st->brk_mark[0]);

	st->breaks++;
	if (st->rxq_len >= chip_fifo_size(dev->chip) || st->brk_marks == max) {
		st->rx_overruns++;
		st->pending_ls |= FTDI_LS_OE;
		return;
	}
	st->brk_mark[st->brk_marks++] = st->rxq_len;
	rxq_push_byte(st, 0);
	uart_flow_changed(dev, st);
}

/* CTS#, DSR# (the plug: RTS#, DTR#) and XOFF gate the transmitter */
static bool uart_tx_allowed(const struct ftdi_intf_state *st)
{
	if ((st->flow_ctrl & FTDI_FLOW_RTS_CTS) && !uart_rts(st))
		return false;
	if ((st->flow_ctrl & FTDI_FLOW_DTR_DSR) && !uart_dtr(st))
		return false;
	if ((st->flow_ctrl & FTDI_FLOW_XON_XOFF) && st->tx_xoff)
		return false;
	/* A break holds TXD low: nothing else gets out */
	return !st->brk;
}

/*
 * The pin the chip enables an RS-485 driver with while it transmits:
 * FT232H, the ACBUS pin given the TXDEN function (9) in the EEPROM, not
 * ACBUS7 (DS_FT232H 3.4); FT2232H, xCBUS0 of a channel in its RS232
 * configuration (DS_FT2232H table 3.5); FT4232H, xDBUS7 when the EEPROM
 * selects TXDEN there instead of RI# (DS_FT4232H 4.6).
 */
static const char *uart_txden_pin(const struct ftdi_device *dev,
				  const struct ftdi_intf_state *st, char *buf,
				  size_t len)
{
	const uint8_t *ee = dev->eeprom;
	int i;

	switch (dev->chip) {
	case CHIP_FT2232H:
		snprintf(buf, len, "%cCBUS0", chan_name(st->index));
		return buf;
	case CHIP_FT4232H:
		if (!dev->usb_ee || !(ee[0x0B] & (0x10 << st->index)))
			return "none";
		snprintf(buf, len, "%cDBUS7", chan_name(st->index));
		return buf;
	default:
		if (!dev->usb_ee)
			return "none";
		for (i = 0; i < 10; i++) {
			uint8_t fn = ee[0x18 + i / 2];

			fn = (i & 1) ? fn >> 4 : fn & 0x0F;
			if (i != 7 && fn == 9) {
				snprintf(buf, len, "ACBUS%d", i);
				return buf;
			}
		}
		return "none";
	}
}

static void uart_stats(const struct ftdi_device *dev,
		       const struct ftdi_intf_state *st)
{
	char pin[16];

	fprintf(stderr, "emu: uart ch%c tx=%lu rx_overruns=%lu rx_holds=%lu breaks=%lu flow=0x%02x txden=%s txden_pulses=%lu\n",
		chan_name(st->index), st->uart_tx, st->rx_overruns,
		st->rx_holds, st->breaks, st->flow_ctrl,
		uart_txden_pin(dev, st, pin, sizeof(pin)), st->txden_pulses);
}

int ftdi_emu_out_room(const struct ftdi_device *dev, int intf_idx)
{
	const struct ftdi_intf_state *st;
	int room;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return 0;
	st = &dev->intf[intf_idx];
	if (st->bitmode == FTDI_BITMODE_MPSSE)
		room = chip_fifo_size(dev->chip) - st->cmdq_len;
	else if (uart_mode(st))
		room = chip_fifo_size(dev->chip) - st->txq_len;
	else
		return 1 << 30;
	return room > 0 ? room : 0;
}

void ftdi_emu_out_held(struct ftdi_device *dev, int intf_idx)
{
	if (intf_idx >= 0 && intf_idx < dev->num_interfaces)
		dev->intf[intf_idx].out_held++;
}

void ftdi_emu_out_blocked(struct ftdi_device *dev, int intf_idx)
{
	struct ftdi_intf_state *st;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return;
	st = &dev->intf[intf_idx];
	if (st->bitmode != FTDI_BITMODE_MPSSE || !st->eng_rx_full ||
	    st->warned_deadlock)
		return;
	st->warned_deadlock = true;
	fprintf(stderr, "emu: ch%c: RX FIFO full (%d bytes) and the buffer from the host too: the write cannot go on until the host reads -- a real chip stalls\n",
		chan_name(intf_idx), st->rxq_len + st->pend_bytes);
}

static void uart_txq_push(struct ftdi_device *dev, struct ftdi_intf_state *st,
			  const uint8_t *data, int len)
{
	int cap = chip_fifo_size(dev->chip);

	if (st->txq_cap < cap) {
		uint8_t *nb = realloc(st->txq, cap);

		if (!nb) {
			fprintf(stderr, "emu: out of memory\n");
			exit(1);
		}
		st->txq = nb;
		st->txq_cap = cap;
	}
	if (len > cap - st->txq_len) {
		fprintf(stderr, "emu: ch%c: UART TX buffer overrun, %d byte(s) dropped\n",
			chan_name(st->index), len - (cap - st->txq_len));
		len = cap - st->txq_len;
	}
	if (!st->txq_len) {
		st->tx_last_ms = dev->now_ms;
		st->tx_acc = 0;
	}
	memcpy(st->txq + st->txq_len, data, len);
	st->txq_len += len;
}

/*
 * The transmitter: what the baud rate allowed since the last call with
 * --uart-pace, else all the receiver has room for. TXD loops to RXD.
 */
static void uart_transmit(struct ftdi_device *dev, struct ftdi_intf_state *st)
{
	long long per_char = (long long)uart_frame_bits(st) * 1000;
	uint8_t mask = uart_data_bits(st) == 7 ? 0x7F : 0xFF;
	long long n = st->txq_len;
	int sent = 0;

	if (!st->txq_len) {
		st->tx_active = false;
		return;
	}
	if (!uart_mode(st)) {
		st->txq_len = 0;
		st->tx_active = false;
		return;
	}

	if (ftdi_emu_uart_pace) {
		st->tx_acc += (dev->now_ms - st->tx_last_ms) *
			      (st->baud ? st->baud : 9600);
		st->tx_last_ms = dev->now_ms;
		n = st->tx_acc / per_char;
		if (n > st->txq_len)
			n = st->txq_len;
	}

	while (sent < n && uart_tx_allowed(st)) {
		if (!ftdi_emu_uart_pace &&
		    st->rxq_len >= chip_fifo_size(dev->chip))
			break;
		if (!st->tx_active) {
			st->tx_active = true;
			st->txden_pulses++;
		}
		uart_receive(dev, st, st->txq[sent++] & mask);
	}

	if (!uart_tx_allowed(st)) {
		/* Held: the time it waits sends nothing later */
		st->tx_acc = 0;
		st->tx_active = false;
	} else if (ftdi_emu_uart_pace) {
		st->tx_acc -= sent * per_char;
	}
	if (!sent)
		return;

	st->uart_tx += sent;
	memmove(st->txq, st->txq + sent, st->txq_len - sent);
	st->txq_len -= sent;
	if (!st->txq_len) {
		st->tx_acc = 0;
		st->tx_active = false;
	}
}

void ftdi_emu_tick(struct ftdi_device *dev, long long now_ns)
{
	long long now_ms = now_ns / 1000000;
	int i;

	dev->now_ns = now_ns;
	dev->now_ms = now_ms;

	for (i = 0; i < dev->num_interfaces; i++) {
		uart_transmit(dev, &dev->intf[i]);
		resp_drain(dev, &dev->intf[i]);
	}

	for (i = 0; i < dev->num_interfaces; i++) {
		struct ftdi_intf_state *st = &dev->intf[i];

		if (st->stall_until_ms && now_ms >= st->stall_until_ms)
			mpsse_stall_end(dev, st);
		else
			mpsse_run(dev, st);
	}
}

long long ftdi_emu_next_event(const struct ftdi_device *dev)
{
	const long long ms = 1000000;
	long long best = -1;
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		const struct ftdi_intf_state *st = &dev->intf[i];
		long long dl;

		/* A transmitting UART moves bytes every couple of ms */
		if (st->txq_len && (best < 0 || best > 2 * ms))
			best = 2 * ms;

		if (st->pend_head < st->pend_len &&
		    st->pend[st->pend_head].t_ns < FTDI_EMU_NEVER) {
			dl = st->pend[st->pend_head].t_ns - dev->now_ns;
			if (dl < 0)
				dl = 0;
			if (best < 0 || dl < best)
				best = dl;
		}

		/* The engine has work it could not do yet */
		if (st->bitmode == FTDI_BITMODE_MPSSE && st->eng_next >= 0) {
			dl = st->eng_next - dev->now_ns;
			if (dl < 0)
				dl = 0;
			if (best < 0 || dl < best)
				best = dl;
		}

		if (!st->stall_until_ms)
			continue;
		dl = (st->stall_until_ms - dev->now_ms) * ms;
		if (dl < 0)
			dl = 0;
		if (best < 0 || dl < best)
			best = dl;
	}
	return best;
}

void ftdi_emu_bulk_out(struct ftdi_device *dev, int intf_idx,
		       const uint8_t *data, int len)
{
	struct ftdi_intf_state *st;
	int j;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces || len <= 0)
		return;

	st = &dev->intf[intf_idx];
	st->out_bytes += len;

	switch (st->bitmode) {
	case FTDI_BITMODE_RESET:
	case FTDI_BITMODE_CBUS:
		/* UART (CBUS bit-bang leaves it on): TXD is looped to RXD */
		uart_txq_push(dev, st, data, len);
		uart_transmit(dev, st);
		return;

	case FTDI_BITMODE_BITBANG:
		/*
		 * Async bit-bang: each byte is clocked onto the output pins;
		 * nothing is returned on bulk IN (AN2232C-02).
		 */
		for (j = 0; j < len; j++)
			st->lo.val = data[j];
		return;

	case FTDI_BITMODE_MPSSE:
		cmdq_push(dev, st, data, len);
		mpsse_run(dev, st);
		return;
	}
}

int ftdi_emu_latency(const struct ftdi_device *dev, int intf_idx)
{
	int l;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return 16;
	l = dev->intf[intf_idx].latency;
	return l ? l : 1;
}

bool ftdi_emu_in_ready(const struct ftdi_device *dev, int intf_idx,
		       long ms_since_last_packet)
{
	const struct ftdi_intf_state *st;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return true;
	st = &dev->intf[intf_idx];

	if (st->error_mode == FTDI_ERR_USB_STALL || st->ep_in_halted)
		return true;
	if (st->rxq_len >= FTDI_HS_MAX_PACKET - FTDI_STATUS_BYTES)
		return true;
	if (st->flush)
		return true;
	return ms_since_last_packet >= ftdi_emu_latency(dev, intf_idx);
}

bool ftdi_emu_in_blackhole(struct ftdi_device *dev, int intf_idx)
{
	struct ftdi_intf_state *st;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return false;
	st = &dev->intf[intf_idx];
	if (ftdi_emu_error_consume(st, FTDI_ERR_USB_TIMEOUT)) {
		fprintf(stderr, "emu: USB timeout injected on bulk IN\n");
		return true;
	}
	return false;
}

int ftdi_emu_bulk_in(struct ftdi_device *dev, int intf_idx,
		     uint8_t *buf, int maxlen, bool *babble)
{
	struct ftdi_intf_state *st;
	int pos = 0;

	*babble = false;

	if (intf_idx < 0 || intf_idx >= dev->num_interfaces)
		return -1;
	st = &dev->intf[intf_idx];

	if (st->ep_in_halted)
		return -1;
	if (ftdi_emu_error_consume(st, FTDI_ERR_USB_STALL)) {
		/* It stays halted until CLEAR_FEATURE (USB 2.0 8.4.5) */
		st->ep_in_halted = true;
		fprintf(stderr, "emu: USB STALL injected on bulk IN of interface %d, halted\n",
			intf_idx);
		return -1;
	}

	for (;;) {
		int room = maxlen - pos;
		int n = FTDI_HS_MAX_PACKET - FTDI_STATUS_BYTES;
		uint8_t ls = line_status(st);
		uint8_t brk = 0;

		/* The chip fills its packet with what it has */
		if (n > st->rxq_len)
			n = st->rxq_len;
		/* ... up to a break, which its line status goes with */
		if (st->brk_marks && st->brk_mark[0] < n) {
			n = st->brk_mark[0] + 1;
			brk = FTDI_LS_BI;
		}
		if (FTDI_STATUS_BYTES + n > room) {
			fprintf(stderr, "emu: ch%c: bulk IN packet of %d bytes for a transfer with %d left: babble, %d byte(s) dropped\n",
				chan_name(intf_idx), FTDI_STATUS_BYTES + n, room,
				n);
			rxq_consume(st, n);
			*babble = true;
			break;
		}

		ls |= brk;
		if (n > 0 && st->pending_ls) {
			ls |= st->pending_ls;
			st->pending_ls = 0;
		}
		if (n > 0 && (st->bitmode == FTDI_BITMODE_RESET ||
			      st->bitmode == FTDI_BITMODE_CBUS) &&
		    ftdi_emu_error_consume(st, FTDI_ERR_UART_LINE)) {
			static const uint8_t errs[4] = {
				FTDI_LS_OE, FTDI_LS_PE, FTDI_LS_FE, FTDI_LS_BI
			};
			static int rot;

			ls |= errs[rot++ & 3];
			fprintf(stderr, "emu: UART line status 0x%02x injected\n", ls);
		}

		buf[pos] = modem_status(st);
		buf[pos + 1] = ls;
		if (n > 0)
			memcpy(buf + pos + FTDI_STATUS_BYTES, st->rxq, n);
		rxq_consume(st, n);
		pos += FTDI_STATUS_BYTES + n;
		st->in_bytes += n;

		/* A short packet ends the transfer, as a full buffer does */
		if (FTDI_STATUS_BYTES + n < FTDI_HS_MAX_PACKET || !st->rxq_len ||
		    pos == maxlen)
			break;
	}

	st->flush = false;
	/* Room for responses again */
	if (st->eng_rx_full)
		mpsse_run(dev, st);
	/* ... and in the UART receiver: its sender may go on */
	if (uart_mode(st)) {
		uart_flow_changed(dev, st);
		uart_transmit(dev, st);
	}
	return pos;
}
