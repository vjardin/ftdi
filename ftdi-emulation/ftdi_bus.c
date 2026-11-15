/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTDI device emulation -- I2C bus and SPI slaves behind the MPSSE pins
 *
 * See ftdi_bus.h. The slaves only see the lines: START, STOP, address,
 * direction, ACK and data are decoded from the edges the engine produces.
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <string.h>

#include "ftdi_emu.h"

/* Reports of each kind that are logged */
#define EMU_BUS_LOG_MAX		8

static char bus_chan(const struct ftdi_intf_state *st)
{
	return 'A' + st->index;
}

static const struct emu_i2c_spec i2c_standard = {
	.name = "standard", .hd_sta = 4000, .su_sta = 4700, .su_sto = 4000,
	.buf = 4700, .low = 4700, .high = 4000, .su_dat = 250,
};

static const struct emu_i2c_spec i2c_fast = {
	.name = "fast", .hd_sta = 600, .su_sta = 600, .su_sto = 600,
	.buf = 1300, .low = 1300, .high = 600, .su_dat = 100,
};

/*
 * The limits that apply follow the bit rate the master is set up for:
 * with 3-phase clocking SCL = 60 MHz (12 MHz with divide-by-5) /
 * ((1 + divisor) * 3), standard mode up to 100 kHz.
 */
static const struct emu_i2c_spec *i2c_spec(const struct ftdi_intf_state *st)
{
	unsigned int n = (1u + st->clk_divisor) * (st->clk_div5 ? 5 : 1);

	return n >= 200 ? &i2c_standard : &i2c_fast;
}

static void i2c_timing(struct ftdi_intf_state *st, const char *what,
		       long long got, int min)
{
	struct emu_i2c *b = &st->i2c;

	/* got < 0: the engine was reset, its clock started again */
	if (got >= min || got < 0 || b->untimed)
		return;
	if (b->timing_errs++ < EMU_BUS_LOG_MAX)
		fprintf(stderr, "emu: ch%c: I2C timing: %s %lld ns < %d ns (%s mode)\n",
			bus_chan(st), what, got, min, i2c_spec(st)->name);
}

static void i2c_proto(struct ftdi_intf_state *st, const char *what, int arg)
{
	struct emu_i2c *b = &st->i2c;

	if (b->proto_errs++ < EMU_BUS_LOG_MAX)
		fprintf(stderr, "emu: ch%c: I2C protocol: %s (%d)\n",
			bus_chan(st), what, arg);
}

static bool i2c_scl_held(const struct ftdi_intf_state *st, long long t)
{
	return st->error_mode == FTDI_ERR_I2C_SCL_STUCK ||
	       st->i2c.hold_until > t;
}

long long emu_i2c_scl_release(const struct ftdi_intf_state *st, long long t)
{
	if (st->error_mode == FTDI_ERR_I2C_SCL_STUCK)
		return -1;
	return st->i2c.hold_until > t ? st->i2c.hold_until : 0;
}

static enum emu_i2c_dev i2c_match(uint8_t addr)
{
	static const uint8_t counters[] = EMU_I2C_COUNTER_ADDRS;
	unsigned int i;

	if (addr == EMU_I2C_ADDR_MEM)
		return EMU_I2C_DEV_MEM;
	for (i = 0; i < sizeof(counters); i++)
		if (addr == counters[i])
			return EMU_I2C_DEV_COUNTER;
	return EMU_I2C_DEV_NONE;
}

static void i2c_tx_load(struct ftdi_intf_state *st)
{
	struct emu_i2c *b = &st->i2c;

	if (b->dev == EMU_I2C_DEV_MEM)
		b->tx = b->mem[b->ptr++];
	else
		b->tx = 0xA0 + st->read_counter++;
	b->tx_bytes++;
}

static void i2c_rx_byte(struct ftdi_intf_state *st, uint8_t v)
{
	struct emu_i2c *b = &st->i2c;

	b->rx_bytes++;
	if (b->dev != EMU_I2C_DEV_MEM)
		return;
	if (!b->got_ptr) {
		b->ptr = v;
		b->got_ptr = true;
	} else if (b->wlen < (int)sizeof(b->wbuf)) {
		b->wbuf[b->wlen++] = v;
	}
}

static void i2c_write_end(struct ftdi_intf_state *st, bool stop)
{
	struct emu_i2c *b = &st->i2c;
	int i;

	if (b->state != EMU_I2C_RX || b->dev != EMU_I2C_DEV_MEM || !b->wlen)
		return;
	if (stop) {
		for (i = 0; i < b->wlen; i++)
			b->mem[b->ptr++] = b->wbuf[i];
	} else {
		i2c_proto(st, "write to the memory device not ended by a STOP, bytes discarded",
			  b->wlen);
	}
	b->wlen = 0;
}

/* The slave may hold SCL low once the 9th clock of a byte is over */
static void i2c_stretch(struct ftdi_intf_state *st, long long t)
{
	struct emu_i2c *b = &st->i2c;

	if (ftdi_emu_error_consume(st, FTDI_ERR_I2C_STRETCH_SHORT)) {
		b->stretch_short = true;
		if (b->stretches++ < EMU_BUS_LOG_MAX)
			fprintf(stderr, "emu: ch%c: I2C slave lets SCL go %lld ns after the master\n",
				bus_chan(st), EMU_I2C_STRETCH_SHORT_NS);
		return;
	}
	if (!ftdi_emu_error_consume(st, FTDI_ERR_I2C_CLK_STRETCH))
		return;
	b->hold_until = t + EMU_I2C_STRETCH_NS;
	if (b->stretches++ < EMU_BUS_LOG_MAX)
		fprintf(stderr, "emu: ch%c: I2C slave stretches SCL for %lld ms\n",
			bus_chan(st), EMU_I2C_STRETCH_NS / 1000000);
}

static void i2c_start(struct ftdi_intf_state *st, long long t)
{
	const struct emu_i2c_spec *spec = i2c_spec(st);
	struct emu_i2c *b = &st->i2c;

	/*
	 * A repeated START too soon after a slave let SCL go: the master did
	 * not wait for SCL, a slave may miss the condition
	 */
	b->late_start = b->in_xfer && b->rise_by_slave &&
			t - b->t_scl_rise < spec->su_sta;

	if (b->stuck_released) {
		b->stuck_released = false;
		i2c_proto(st, "START with no STOP after a stuck slave let go of SDA",
			  0);
	}

	if (b->in_xfer) {
		i2c_timing(st, "tSU;STA", t - b->t_scl_rise, spec->su_sta);
		b->rstarts++;
	} else {
		if (b->t_stop)
			i2c_timing(st, "tBUF", t - b->t_stop, spec->buf);
		b->starts++;
	}
	i2c_write_end(st, false);

	b->in_xfer = true;
	b->start_pending = true;
	b->t_start = t;
	b->state = EMU_I2C_ADDR;
	b->nbit = 0;
	b->shift = 0;
	b->ack_phase = false;
	b->s_sda_low = false;
}

static void i2c_stop(struct ftdi_intf_state *st, long long t)
{
	struct emu_i2c *b = &st->i2c;

	if (b->in_xfer)
		i2c_timing(st, "tSU;STO", t - b->t_scl_rise, i2c_spec(st)->su_sto);
	i2c_write_end(st, true);

	b->stops++;
	b->stuck_released = false;
	b->in_xfer = false;
	b->start_pending = false;
	b->t_stop = t;
	b->state = EMU_I2C_IDLE;
	b->s_sda_low = false;
}

static void i2c_scl_rise(struct ftdi_intf_state *st, long long t)
{
	const struct emu_i2c_spec *spec = i2c_spec(st);
	struct emu_i2c *b = &st->i2c;

	/*
	 * The bit is sampled now: SDA pulled low by a slave (or the stuck
	 * fault) and driven high by the master. Earlier in the low phase
	 * the two may overlap for a moment, as when a slave starts its ACK.
	 */
	if (b->m_sda_push && (b->s_sda_low || b->stuck ||
			      st->error_mode == FTDI_ERR_I2C_BUS_STUCK))
		i2c_proto(st, "contention: SDA driven high by the master, pulled low by a slave",
			  0);

	if (b->in_xfer) {
		i2c_timing(st, "tLOW", t - b->t_scl_fall, spec->low);
		i2c_timing(st, "tSU;DAT", t - b->t_sda, spec->su_dat);
	}
	b->t_scl_rise = t;
	b->rise_by_slave = b->slave_release;

	if (b->clocking) {
		if (b->stuck || st->error_mode == FTDI_ERR_I2C_BUS_STUCK)
			b->busy_clocks++;
		if (b->t_clk_rise) {
			long long bit = t - b->t_clk_rise;

			if (!b->bit_ns_min || bit < b->bit_ns_min)
				b->bit_ns_min = bit;
			if (bit > b->bit_ns_max)
				b->bit_ns_max = bit;
		}
		b->t_clk_rise = t;
	} else if (!b->untimed && (b->s_sda_low || b->stuck ||
				   st->error_mode == FTDI_ERR_I2C_BUS_STUCK)) {
		/* A pin write pulses SCL while a slave holds SDA */
		b->recovery_clocks++;
	}

	switch (b->state) {
	case EMU_I2C_ADDR:
	case EMU_I2C_RX:
		if (!b->ack_phase && b->nbit < 8) {
			b->shift = (b->shift << 1) | b->sda;
			b->nbit++;
		}
		break;
	case EMU_I2C_TX:
		if (b->ack_phase)
			b->master_ack = !b->sda;
		else if (b->nbit < 8)
			b->nbit++;
		break;
	default:
		break;
	}
}

static void i2c_scl_fall(struct ftdi_intf_state *st, long long t)
{
	const struct emu_i2c_spec *spec = i2c_spec(st);
	struct emu_i2c *b = &st->i2c;

	/*
	 * i2c-stuck-byte: a slave left in the middle of a byte holds SDA low
	 * from this clock on, and lets go after three recovery pulses
	 */
	if (!b->stuck && ftdi_emu_error_consume(st, FTDI_ERR_I2C_STUCK_BYTE)) {
		b->stuck = true;
		b->stuck_until = b->recovery_clocks + 3;
		fprintf(stderr, "emu: ch%c: I2C slave stuck in a byte, SDA held low\n",
			bus_chan(st));
	} else if (b->stuck && b->recovery_clocks >= b->stuck_until) {
		b->stuck = false;
		b->stuck_released = true;
		fprintf(stderr, "emu: ch%c: I2C slave lets go of SDA\n",
			bus_chan(st));
	}

	if (b->in_xfer) {
		if (b->start_pending)
			i2c_timing(st, "tHD;STA", t - b->t_start, spec->hd_sta);
		else
			i2c_timing(st, "tHIGH", t - b->t_scl_rise, spec->high);
	}
	b->start_pending = false;
	b->t_scl_fall = t;

	switch (b->state) {
	case EMU_I2C_ADDR:
		if (!b->ack_phase) {
			if (b->nbit < 8)
				break;
			if (b->late_start)
				i2c_proto(st, "address after a repeated START below tSU;STA, SCL let go by a slave just before",
					  b->shift >> 1);
			b->dev = i2c_match(b->shift >> 1);
			if (b->dev != EMU_I2C_DEV_NONE &&
			    ftdi_emu_error_consume(st, FTDI_ERR_I2C_NAK)) {
				fprintf(stderr, "emu: ch%c: I2C NAK injected on address 0x%02x (%s)\n",
					bus_chan(st), b->shift >> 1,
					b->shift & 1 ? "read" : "write");
				b->dev = EMU_I2C_DEV_NONE;
			}
			if (b->dev == EMU_I2C_DEV_NONE) {
				b->naks++;
				b->state = EMU_I2C_DEAF;
				break;
			}
			b->s_sda_low = true;		/* ACK */
			b->ack_phase = true;
			break;
		}
		b->ack_phase = false;
		b->nbit = 0;
		if (b->shift & 1) {
			b->state = EMU_I2C_TX;
			i2c_tx_load(st);
			b->s_sda_low = !(b->tx & 0x80);
		} else {
			b->state = EMU_I2C_RX;
			b->s_sda_low = false;
			b->got_ptr = false;
			b->wlen = 0;
		}
		b->shift = 0;
		i2c_stretch(st, t);
		break;

	case EMU_I2C_RX:
		if (!b->ack_phase) {
			if (b->nbit < 8)
				break;
			i2c_rx_byte(st, b->shift);
			b->s_sda_low = true;		/* ACK */
			b->ack_phase = true;
			break;
		}
		b->ack_phase = false;
		b->nbit = 0;
		b->shift = 0;
		b->s_sda_low = false;
		i2c_stretch(st, t);
		break;

	case EMU_I2C_TX:
		if (!b->ack_phase) {
			if (b->nbit < 8) {
				b->s_sda_low = !((b->tx << b->nbit) & 0x80);
				break;
			}
			b->s_sda_low = false;	/* the master's ACK bit */
			b->ack_phase = true;
			break;
		}
		b->ack_phase = false;
		b->nbit = 0;
		if (b->master_ack) {
			i2c_tx_load(st);
			b->s_sda_low = !(b->tx & 0x80);
		} else {
			b->state = EMU_I2C_DEAF;
			b->s_sda_low = false;
		}
		i2c_stretch(st, t);
		break;

	default:
		break;
	}
}

/* Work out the lines and tell the slave about every edge */
static void i2c_eval(struct ftdi_intf_state *st, long long t)
{
	struct emu_i2c *b = &st->i2c;
	int guard;

	/* The slave reacts to an edge and may move a line again */
	for (guard = 0; guard < 8; guard++) {
		bool scl = b->m_scl && !i2c_scl_held(st, t);
		bool sda = b->m_sda && !b->s_sda_low && !b->stuck &&
			   st->error_mode != FTDI_ERR_I2C_BUS_STUCK;

		if (scl != b->scl) {
			/* Both lines at once: the clock edge goes first */
			b->scl = scl;
			if (scl)
				i2c_scl_rise(st, t);
			else
				i2c_scl_fall(st, t);
			continue;
		}
		if (sda == b->sda)
			return;
		b->sda = sda;
		b->t_sda = t;
		if (!b->scl)
			continue;
		if (sda)
			i2c_stop(st, t);
		else
			i2c_start(st, t);
	}
}

void emu_i2c_attach(struct ftdi_intf_state *st, long long t,
		    bool scl, bool sda, bool scl_push, bool sda_push)
{
	struct emu_i2c *b = &st->i2c;

	/* No edge: the lines are simply what they are from now on */
	b->m_scl = scl;
	b->m_sda = sda;
	b->m_scl_push = scl_push;
	b->m_sda_push = sda_push;
	b->scl = b->m_scl && !i2c_scl_held(st, t);
	b->sda = b->m_sda && !b->s_sda_low && !b->stuck &&
		 st->error_mode != FTDI_ERR_I2C_BUS_STUCK;
}

void emu_i2c_advance(struct ftdi_intf_state *st, long long t)
{
	struct emu_i2c *b = &st->i2c;

	if (b->hold_until && t >= b->hold_until) {
		long long rel = b->hold_until;

		b->hold_until = 0;
		b->slave_release = true;
		i2c_eval(st, rel);
		b->slave_release = false;
	}
	/* An injection armed or cleared meanwhile moves the lines too */
	i2c_eval(st, t);
}

void emu_i2c_clocking(struct ftdi_intf_state *st, bool on)
{
	st->i2c.clocking = on;
	st->i2c.t_clk_rise = 0;
}

void emu_i2c_master(struct ftdi_intf_state *st, long long t,
		    bool scl, bool sda, bool scl_push, bool sda_push)
{
	struct emu_i2c *b = &st->i2c;

	emu_i2c_advance(st, t);
	if (b->stretch_short && scl && !b->m_scl) {
		b->stretch_short = false;
		b->hold_until = t + EMU_I2C_STRETCH_SHORT_NS;
	}
	b->m_scl = scl;
	b->m_sda = sda;
	b->m_scl_push = scl_push;
	b->m_sda_push = sda_push;
	i2c_eval(st, t);
	if (scl_push && i2c_scl_held(st, t))
		i2c_proto(st, "contention: SCL driven high by the master, held low by a slave",
			  0);
}

void emu_i2c_detach(struct ftdi_intf_state *st, long long t)
{
	struct emu_i2c *b = &st->i2c;

	/*
	 * The pull-ups take both lines up together: the slave may see a
	 * STOP, but not one whose timing the master chose.
	 */
	b->untimed = true;
	emu_i2c_master(st, t, true, true, false, false);
	b->untimed = false;
}

static long long bit_hz(long long bit_ns)
{
	return bit_ns ? (1000000000LL + bit_ns / 2) / bit_ns : 0;
}

void emu_i2c_stats(const struct ftdi_intf_state *st)
{
	const struct emu_i2c *b = &st->i2c;

	if (!b->starts && !b->stops && !b->timing_errs && !b->recovery_clocks &&
	    !b->busy_clocks)
		return;
	fprintf(stderr, "emu: i2c ch%c starts=%lu rstarts=%lu stops=%lu rx=%lu tx=%lu naks=%lu stretches=%lu timing_errs=%lu proto_errs=%lu scl_hz_max=%lld scl_hz_min=%lld recovery_clocks=%lu busy_clocks=%lu\n",
		bus_chan(st), b->starts, b->rstarts, b->stops, b->rx_bytes,
		b->tx_bytes, b->naks, b->stretches, b->timing_errs,
		b->proto_errs, bit_hz(b->bit_ns_min), bit_hz(b->bit_ns_max),
		b->recovery_clocks, b->busy_clocks);
}

#define SPI_PIN_SCK	0x01
#define SPI_PIN_MOSI	0x02

static int spi_pin(const struct emu_spi_slave *s)
{
	return __builtin_ctz(s->cs);
}

static void spi_proto(struct ftdi_intf_state *st,
		      const struct emu_spi_slave *s, const char *what)
{
	if (st->spi.proto_errs++ >= EMU_BUS_LOG_MAX)
		return;
	if (s->kind == EMU_SPI_ECHO)
		fprintf(stderr, "emu: ch%c: SPI protocol: mode %d slave on AD%d: %s\n",
			bus_chan(st), s->mode, spi_pin(s), what);
	else
		fprintf(stderr, "emu: ch%c: SPI protocol: counter device on AD%d: %s\n",
			bus_chan(st), spi_pin(s), what);
}

/* The counter device shows the value it is at; read once all of it is out */
static void spi_counter_load(struct ftdi_intf_state *st,
			     struct emu_spi_slave *s)
{
	s->sr = 0xA0 + st->read_counter;
	s->miso = s->sr & 0x80;
}

static void spi_frame_end(struct ftdi_intf_state *st, struct emu_spi_slave *s,
			  long long t)
{
	st->spi.frames++;
	if (s->kind == EMU_SPI_ECHO)
		fprintf(stderr, "emu: ch%c: SPI frame cs=AD%d mode=%d bits=%u setup_ns=%lld hold_ns=%lld inactive_ns=%lld wgap_ns=%lld\n",
			bus_chan(st), spi_pin(s), s->mode, s->nbits,
			s->t_first ? s->t_first - s->t_assert : -1,
			s->t_last ? t - s->t_last : -1,
			s->inactive, s->wgap);
	s->sel = false;
	s->t_desel = t;
}

static void spi_frame_start(struct ftdi_intf_state *st,
			    struct emu_spi_slave *s, long long t, uint8_t lvl)
{
	struct emu_spi *b = &st->spi;
	int k;

	/* Two slaves on MISO at once: a chip select of the wrong polarity */
	for (k = 0; k < EMU_SPI_SLAVES; k++)
		if (b->dev[k].sel)
			spi_proto(st, s, "selected while another slave is");

	s->sel = true;
	s->nbits = 0;
	s->rose = false;
	s->t_assert = t;
	s->t_first = 0;
	s->t_last = 0;
	s->t_sample = 0;
	s->wgap = -1;
	s->inactive = s->t_desel ? t - s->t_desel : -1;

	if (s->kind == EMU_SPI_COUNTER) {
		spi_counter_load(st, s);
		return;
	}
	if (!!(lvl & SPI_PIN_SCK) != !!(s->mode & 2))
		spi_proto(st, s, "selected with SCK at its active level");
	/* CPHA = 0: the first bit is out before the first edge */
	if (!(s->mode & 1))
		s->miso = s->sr & 0x80;
}

static void spi_sampled(struct emu_spi_slave *s, long long t)
{
	if (s->nbits && !(s->nbits % 8)) {
		long long gap = t - s->t_sample;

		if (s->wgap < 0 || gap < s->wgap)
			s->wgap = gap;
	}
	s->t_sample = t;
	s->nbits++;
}

/*
 * A mode 0 / mode 3 slave: MOSI sampled on the rising edge (and ignored),
 * MISO changed on the falling edge that follows one. In mode 3 SCK first
 * falls to its active level: nothing moves then.
 */
static void spi_counter_edge(struct ftdi_intf_state *st,
			     struct emu_spi_slave *s, long long t, uint8_t lvl)
{
	if (lvl & SPI_PIN_SCK) {
		s->rose = true;
		spi_sampled(s, t);
		return;
	}
	if (!s->rose)
		return;
	s->rose = false;
	if (s->nbits % 8) {
		s->sr <<= 1;
		s->miso = s->sr & 0x80;
	} else {
		st->read_counter++;
		spi_counter_load(st, s);
	}
}

static void spi_echo_edge(struct emu_spi_slave *s, long long t, uint8_t lvl)
{
	bool cpol = s->mode & 2, cpha = s->mode & 1;
	bool leading = !!(lvl & SPI_PIN_SCK) != cpol;
	bool mosi = lvl & SPI_PIN_MOSI;

	if (!cpha) {
		if (leading) {
			s->in = mosi;
			spi_sampled(s, t);
		} else {
			s->sr = (s->sr << 1) | s->in;
			s->miso = s->sr & 0x80;
		}
	} else {
		if (leading) {
			s->miso = s->sr & 0x80;
		} else {
			s->sr = (s->sr << 1) | mosi;
			spi_sampled(s, t);
		}
	}
}

static void spi_edge(struct ftdi_intf_state *st, struct emu_spi_slave *s,
		     long long t, uint8_t lvl)
{
	if (!s->t_first)
		s->t_first = t;
	s->t_last = t;

	if (s->kind == EMU_SPI_COUNTER)
		spi_counter_edge(st, s, t, lvl);
	else
		spi_echo_edge(s, t, lvl);
}

void emu_spi_init(struct ftdi_intf_state *st)
{
	struct emu_spi *b = &st->spi;
	int k;

	memset(b, 0, sizeof(*b));
	b->dev[0].kind = EMU_SPI_COUNTER;
	b->dev[0].cs = 1u << 3;
	if (ftdi_emu_spi_cs_high) {
		b->dev[1].kind = EMU_SPI_COUNTER;
		b->dev[1].cs = 1u << 4;
		b->dev[1].cs_high = true;
	} else if (ftdi_emu_spi_echo) {
		for (k = 0; k < 4; k++) {
			struct emu_spi_slave *s = &b->dev[EMU_SPI_ECHO_FIRST + k];

			s->kind = EMU_SPI_ECHO;
			s->cs = 1u << (EMU_SPI_ECHO_FIRST_PIN + k);
			s->mode = k;
		}
	}
}

void emu_spi_attach(struct ftdi_intf_state *st, long long t, uint8_t lvl)
{
	int k;

	(void)t;
	st->spi.lvl = lvl;
	for (k = 0; k < EMU_SPI_SLAVES; k++)
		st->spi.dev[k].sel = false;
}

void emu_spi_clocking(struct ftdi_intf_state *st, bool on)
{
	st->spi.clocking = on;
	st->spi.t_clk_rise = 0;
}

void emu_spi_pins(struct ftdi_intf_state *st, long long t, uint8_t lvl)
{
	struct emu_spi *b = &st->spi;
	uint8_t changed = lvl ^ b->lvl;
	int k;

	b->lvl = lvl;
	if (!changed)
		return;

	if ((changed & lvl & SPI_PIN_SCK) && b->clocking) {
		if (b->t_clk_rise) {
			long long bit = t - b->t_clk_rise;

			if (!b->bit_ns_min || bit < b->bit_ns_min)
				b->bit_ns_min = bit;
			if (bit > b->bit_ns_max)
				b->bit_ns_max = bit;
		}
		b->t_clk_rise = t;
	}

	for (k = 0; k < EMU_SPI_SLAVES; k++) {
		struct emu_spi_slave *s = &b->dev[k];
		bool sel;

		if (s->kind == EMU_SPI_NONE)
			continue;
		sel = !(lvl & s->cs) != s->cs_high;
		if (sel != s->sel) {
			if (sel)
				spi_frame_start(st, s, t, lvl);
			else
				spi_frame_end(st, s, t);
		} else if (sel && (changed & SPI_PIN_SCK)) {
			spi_edge(st, s, t, lvl);
		}
	}
}

bool emu_spi_miso(const struct ftdi_intf_state *st)
{
	bool lvl = true;	/* pull-up */
	int k;

	/* Several slaves selected at once fight: a low wins */
	for (k = 0; k < EMU_SPI_SLAVES; k++)
		if (st->spi.dev[k].sel && !st->spi.dev[k].miso)
			lvl = false;
	return lvl;
}

bool emu_spi_cs_asserted(const struct ftdi_intf_state *st)
{
	const struct emu_spi *b = &st->spi;
	uint8_t low = ~b->lvl & EMU_SPI_CS_PINS;
	int k;

	for (k = 0; k < EMU_SPI_SLAVES; k++) {
		if (b->dev[k].kind == EMU_SPI_NONE)
			continue;
		if (b->dev[k].sel)
			return true;
		low &= ~b->dev[k].cs;
	}
	/* A pin with nothing behind it, driven low as a chip select is */
	return low;
}

void emu_spi_stats(const struct ftdi_intf_state *st)
{
	const struct emu_spi *b = &st->spi;

	if (!b->frames && !b->proto_errs)
		return;
	fprintf(stderr, "emu: spi ch%c frames=%lu proto_errs=%lu sck_hz_max=%lld sck_hz_min=%lld\n",
		bus_chan(st), b->frames, b->proto_errs, bit_hz(b->bit_ns_min),
		bit_hz(b->bit_ns_max));
}
