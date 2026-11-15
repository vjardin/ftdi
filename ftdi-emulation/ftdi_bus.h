/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI device emulation -- what is wired to the MPSSE pins
 *
 * The board of a channel follows its last 3-phase clocking command:
 *  - enabled (0x8C, an I2C master): an I2C bus. AD0 = SCL, AD1 and AD2
 *    tied to SDA (AN_113, AN_255), both lines pulled up and the wired-AND
 *    of master and slaves. With --wire-ad7, AD7 (GPIOL3) senses SCL.
 *  - disabled (0x8D, an SPI master) and at power-on: AD0 = SCK,
 *    AD1 = MOSI, AD2 = MISO, chip selects on AD3-AD7.
 * An MPSSE reset does not change the board. Times are those of the MPSSE
 * engine, in ns (ftdi_emu.h).
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ftdi_intf_state;

/*
 * I2C slaves; any other address is not acknowledged.
 * Counter devices: every byte read is the next value of one counter
 * (0xA0, 0xA1, ...), whatever the address; written bytes are accepted.
 * Memory device: 256 bytes. The first byte written is the offset, the
 * rest is stored at the STOP (discarded and reported if the write ends
 * any other way); reads return the memory from the offset on.
 */
#define EMU_I2C_COUNTER_ADDRS	{ 0x20, 0x22, 0x50, 0x51 }
#define EMU_I2C_ADDR_MEM	0x57

/* How long the slave stretches SCL for one "i2c-stretch" injection */
#define EMU_I2C_STRETCH_NS	(20 * 1000 * 1000LL)

/*
 * "i2c-stretch-short": the slave lets SCL go this long after the master
 * does, inside tSU;STA and tSU;STO of standard mode
 */
#define EMU_I2C_STRETCH_SHORT_NS	2000LL

/*
 * Minimum times of the I2C-bus specification (UM10204 rev. 7, table 11),
 * in ns: standard mode up to 100 kHz, fast mode up to 400 kHz.
 */
struct emu_i2c_spec {
	const char *name;
	int hd_sta;	/* tHD;STA: START hold */
	int su_sta;	/* tSU;STA: repeated START setup */
	int su_sto;	/* tSU;STO: STOP setup */
	int buf;	/* tBUF: bus free between STOP and START */
	int low;
	int high;
	int su_dat;	/* tSU;DAT: data setup */
};

enum emu_i2c_state {
	EMU_I2C_IDLE,		/* no START seen, or after a STOP */
	EMU_I2C_ADDR,
	EMU_I2C_RX,		/* addressed for write */
	EMU_I2C_TX,		/* addressed for read */
	EMU_I2C_DEAF,		/* not addressed, or done: waits for START */
};

enum emu_i2c_dev {
	EMU_I2C_DEV_NONE,
	EMU_I2C_DEV_COUNTER,
	EMU_I2C_DEV_MEM,
};

struct emu_i2c {
	/* Lines: true = high */
	bool m_scl, m_sda;	/* what the master lets them be */
	bool m_scl_push, m_sda_push;	/* driven high push-pull, no 0x9E */
	bool scl, sda;		/* resulting levels */
	bool s_sda_low;		/* the slave pulls SDA low */
	/* i2c-stuck-byte: SDA held until recovery_clocks reaches stuck_until */
	bool stuck, stuck_released;
	unsigned long stuck_until;
	long long hold_until;	/* the slave holds SCL low until then, 0 = no */
	bool stretch_short;	/* ... until a moment after the master lets go */
	bool slave_release;	/* SCL rising now because a slave lets go */
	bool rise_by_slave;	/* the last SCL rise was a slave letting go */
	bool late_start;	/* Sr below tSU;STA after such a rise */

	/* Slave */
	enum emu_i2c_state state;
	enum emu_i2c_dev dev;
	int nbit;		/* bits done in the current byte, 0..8 */
	bool ack_phase;		/* the 9th clock is next / in progress */
	bool master_ack;
	uint8_t shift, tx;
	uint8_t mem[256];
	uint8_t ptr;
	bool got_ptr;
	uint8_t wbuf[256];
	int wlen;

	/* Timing, in engine ns */
	bool in_xfer;		/* between START and STOP */
	bool start_pending;	/* START seen, SCL not yet low */
	bool untimed;		/* edges of an MPSSE reset: not checked */
	long long t_scl_rise, t_scl_fall, t_sda, t_start, t_stop;

	/* SCL rate: rise to rise within one clocking command */
	bool clocking;
	long long t_clk_rise, bit_ns_min, bit_ns_max;

	unsigned long starts, rstarts, stops, rx_bytes, tx_bytes;
	unsigned long naks, stretches, timing_errs, proto_errs;
	/* SCL pulsed by pin writes while SDA is held low: a bus recovery */
	unsigned long recovery_clocks;
	/* SCL clocked by the engine while a fault holds SDA low */
	unsigned long busy_clocks;
};

/* The board becomes an I2C bus: take the lines as the pins are now */
void emu_i2c_attach(struct ftdi_intf_state *st, long long t,
		    bool scl, bool sda, bool scl_push, bool sda_push);

/*
 * The master's pins changed: true = line released (or driven high);
 * *_push: driven high push-pull, a short against a slave pulling it low
 */
void emu_i2c_master(struct ftdi_intf_state *st, long long t,
		    bool scl, bool sda, bool scl_push, bool sda_push);

/* The MPSSE was reset: its pins are inputs, the lines are let go */
void emu_i2c_detach(struct ftdi_intf_state *st, long long t);

/* Time went by: a slave that was stretching may have released SCL */
void emu_i2c_advance(struct ftdi_intf_state *st, long long t);

/* When the slave stops holding SCL low: 0 = not held, -1 = never */
long long emu_i2c_scl_release(const struct ftdi_intf_state *st, long long t);

/* A clocking command (0x10-0x3F) starts or ends: its edges set the bit rate */
void emu_i2c_clocking(struct ftdi_intf_state *st, bool on);

void emu_i2c_stats(const struct ftdi_intf_state *st);

/*
 * The SPI slaves of the board, each behind its own chip select:
 *  - AD3, active low: the counter device, a mode 0 / mode 3 slave. MISO
 *    changes on the falling edge of SCK that follows a rising one: MSB
 *    first, one counter value per 8 clocks (0xA0, 0xA1, ...).
 *  - with --spi-cs-high, a second counter device on AD4 whose chip select
 *    is active high, with a pull-down on that line.
 *  - with --spi-echo, four shift-register slaves, one per SPI mode,
 *    selected (active low) by AD4-AD7: AD4 = mode 0 ... AD7 = mode 3. Each
 *    is an 8-bit delay line from MOSI to MISO clocked as a device of its
 *    mode is: a byte comes back as the next byte only if the master
 *    changes and samples the data on the right edges.
 * Nothing answers behind the other pins: MISO stays high.
 */
#define EMU_SPI_SLAVES		5
#define EMU_SPI_ECHO_FIRST	1	/* slaves 1-4, with --spi-echo */
#define EMU_SPI_ECHO_FIRST_PIN	4
#define EMU_SPI_CS_PINS		0xF8	/* AD3-AD7 */

enum emu_spi_kind {
	EMU_SPI_NONE,
	EMU_SPI_COUNTER,
	EMU_SPI_ECHO,
};

struct emu_spi_slave {
	enum emu_spi_kind kind;
	uint8_t cs;			/* chip select pin (mask) */
	bool cs_high;			/* selected by a high level */
	int mode;			/* echo: the SPI mode it works in */
	bool sel;
	bool miso, in;
	bool rose;			/* counter: a rising edge since it shifted */
	uint8_t sr;
	unsigned int nbits;		/* bits sampled in this frame */
	long long t_assert, t_first, t_last, t_sample, t_desel;
	long long inactive;		/* CS high time before this frame */
	long long wgap;			/* shortest gap between two words */
};

struct emu_spi {
	uint8_t lvl;			/* pin levels last seen */
	struct emu_spi_slave dev[EMU_SPI_SLAVES];
	unsigned long frames, proto_errs;
	/* SCK rate: rise to rise within one clocking command */
	bool clocking;
	long long t_clk_rise, bit_ns_min, bit_ns_max;
};

/* The slaves the board has, at power-on */
void emu_spi_init(struct ftdi_intf_state *st);

/* The board becomes an SPI bus: take the pins as they are now */
void emu_spi_attach(struct ftdi_intf_state *st, long long t, uint8_t lvl);

/* Levels of the output pins changed */
void emu_spi_pins(struct ftdi_intf_state *st, long long t, uint8_t lvl);

/* What the selected slaves put on MISO (pulled up when none does) */
bool emu_spi_miso(const struct ftdi_intf_state *st);

/* Is a chip select pin at a level that selects, whatever is behind it */
bool emu_spi_cs_asserted(const struct ftdi_intf_state *st);

/* A clocking command (0x10-0x3F) starts or ends: its edges set the bit rate */
void emu_spi_clocking(struct ftdi_intf_state *st, bool on);

void emu_spi_stats(const struct ftdi_intf_state *st);
