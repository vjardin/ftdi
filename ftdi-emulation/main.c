/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ftdi_usbip -- USB/IP server emulating an FTDI Hi-Speed device (FT232H,
 * FT2232H, FT4232H), which the client's vhci_hcd attaches as a USB device
 *
 * Usage:
 *   ftdi_usbip --chip ft232h [--eeprom dump.bin] [--port 3240]
 *
 *   # On the client:
 *   usbip attach -r <server> -b 1-1
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/prctl.h>

#include "ftdi_emu.h"
#include "usbip_server.h"
#include "usbip_proto.h"

static void usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"  --chip TYPE     Chip to emulate: ft232h, ft2232h, ft4232h\n"
		"                  (default: ft232h)\n"
		"  --mode MODE     What the generated EEPROM asks for: spi, i2c or\n"
		"                  uart (default: spi). spi/i2c: 'bus_mode=MODE' tag\n"
		"                  in the user area, MPSSE channels D2XX; uart: VCP\n"
		"                  driver bit on every channel\n"
		"  --ee-tag TEXT   User-area text instead of the --mode default, e.g.\n"
		"                  'bus_mode=i2c,spi'; 'none' for no user data\n"
		"  --ee-product S  Product string instead of the chip default\n"
		"  --ee-vcp CHANS  Channels with the VCP driver bit, e.g. 'ab';\n"
		"                  'none' for D2XX everywhere\n"
		"  --ee-set O=V    Set EEPROM byte O to V after the image is built\n"
		"                  (repeatable), e.g. 0x0d=0x0c; checksum recomputed\n"
		"  --ee-93c46      A 93C46 instead of a 93C56/66: 64 words, read back\n"
		"                  mirrored, with no room for the user-area text\n"
		"  --ee-blank      Blank EEPROM (all 0xFF)\n"
		"  --ee-bad-checksum  Corrupt the checksum of the image\n"
		"  --eeprom FILE   Use an EEPROM image file as it is (256 bytes, or\n"
		"                  128 for a 93C46); the options above do not apply\n"
		"  --port PORT     TCP port to listen on (default: %d)\n"
		"  --error TYPE    Error injection: i2c-nak, i2c-stuck, i2c-stretch,\n"
		"                  i2c-stretch-short, i2c-scl-stuck, i2c-stuck-byte,\n"
		"                  usb-stall, usb-timeout, mpsse-sync, mpsse-stall,\n"
		"                  mpsse-extra, uart-line\n"
		"  --error-count N Number of errors to inject (default: 1, 0=infinite);\n"
		"                  i2c-stuck, i2c-scl-stuck and mpsse-sync hold until\n"
		"                  'error none'\n"
		"  --error-skip N  Let N occasions for the error pass first (default 0)\n"
		"  --error-chan C  Channel the error is for, A to D (default: all)\n"
		"  --ctl FIFO      Control FIFO, created if missing. One command per\n"
		"                  line: 'error TYPE [COUNT [SKIP [CHAN]]]' arms an\n"
		"                  injection, 'pin CHAN D<n>|C<n> 0|1|z' makes the\n"
		"                  board drive a pin (z: let go), 'power-cycle',\n"
		"                  'stats'\n"
		"  --wire-ad7      Board has AD7 (GPIOL3) wired to AD0 (SCL)\n"
		"  --spi-echo      Board has four shift-register SPI slaves, one\n"
		"                  per SPI mode: AD4 = mode 0 ... AD7 = mode 3\n"
		"  --spi-cs-high   Board has a second counter device on AD4, selected\n"
		"                  by a high level (pull-down on the line)\n"
		"  --uart-pace     UART data leaves the chip at the baud rate; without\n"
		"                  it, as fast as the receiver has room for it\n"
		"  --help          Show this help\n"
		"\n"
		"Signals: SIGUSR1 power-cycles the chip (state lost as across a USB\n"
		"suspend), SIGUSR2 prints per-channel statistics.\n",
		progname, USBIP_PORT);
}

static enum ftdi_error_mode parse_error_mode(const char *name)
{
	int mode = ftdi_emu_error_from_name(name);

	if (mode < 0) {
		fprintf(stderr, "Unknown error mode: %s\n", name);
		exit(1);
	}
	return mode;
}

static enum ftdi_chip parse_chip(const char *name)
{
	if (!strcasecmp(name, "ft232h"))
		return CHIP_FT232H;
	if (!strcasecmp(name, "ft2232h"))
		return CHIP_FT2232H;
	if (!strcasecmp(name, "ft4232h"))
		return CHIP_FT4232H;

	fprintf(stderr, "Unknown chip type: %s\n", name);
	exit(1);
}

static const char *parse_mode(const char *name)
{
	if (!strcasecmp(name, "spi"))
		return "spi";
	if (!strcasecmp(name, "i2c"))
		return "i2c";
	if (!strcasecmp(name, "uart"))
		return "uart";

	fprintf(stderr, "Unknown mode: %s (use spi, i2c, or uart)\n", name);
	exit(1);
}

static unsigned int parse_vcp(const char *arg)
{
	unsigned int mask = 0;

	if (!strcasecmp(arg, "none"))
		return 0;
	for (; *arg; arg++) {
		int c = *arg | 0x20;

		if (c < 'a' || c > 'd') {
			fprintf(stderr, "--ee-vcp: channels are a to d, or 'none'\n");
			exit(1);
		}
		mask |= 1u << (c - 'a');
	}
	return mask;
}

#define MAX_EE_SETS	32

struct ee_set {
	unsigned int off;
	unsigned int val;
};

static struct ee_set parse_ee_set(const char *arg)
{
	struct ee_set set;
	char *end;

	set.off = strtoul(arg, &end, 0);
	if (*end != '=' || set.off >= FTDI_EEPROM_SIZE) {
		fprintf(stderr, "--ee-set: expected OFFSET=VALUE, offset below %d\n",
			FTDI_EEPROM_SIZE);
		exit(1);
	}
	set.val = strtoul(end + 1, &end, 0);
	if (*end || set.val > 0xFF) {
		fprintf(stderr, "--ee-set: the value is one byte\n");
		exit(1);
	}
	return set;
}

int main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "chip",        required_argument, NULL, 'c' },
		{ "mode",        required_argument, NULL, 'm' },
		{ "eeprom",      required_argument, NULL, 'e' },
		{ "port",        required_argument, NULL, 'p' },
		{ "error",       required_argument, NULL, 'E' },
		{ "error-count", required_argument, NULL, 'n' },
		{ "wire-ad7",    no_argument,       NULL, 'w' },
		{ "ctl",         required_argument, NULL, 'C' },
		{ "uart-pace",   no_argument,       NULL, 'P' },
		{ "spi-echo",    no_argument,       NULL, 'O' },
		{ "spi-cs-high", no_argument,       NULL, 'G' },
		{ "ee-tag",      required_argument, NULL, 'T' },
		{ "ee-product",  required_argument, NULL, 'D' },
		{ "ee-vcp",      required_argument, NULL, 'V' },
		{ "ee-set",      required_argument, NULL, 'S' },
		{ "ee-blank",    no_argument,       NULL, 'B' },
		{ "ee-93c46",    no_argument,       NULL, '4' },
		{ "error-skip",  required_argument, NULL, 'k' },
		{ "error-chan",  required_argument, NULL, 'H' },
		{ "ee-bad-checksum", no_argument,   NULL, 'X' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	struct ftdi_device dev;
	const char *chip_name = "ft232h";
	const char *mode_name = "spi";
	const char *eeprom_path = NULL;
	const char *error_mode_name = NULL;
	const char *ctl_path = NULL;
	const char *ee_tag = NULL, *ee_product = NULL, *ee_vcp = NULL;
	struct ee_set ee_sets[MAX_EE_SETS];
	int n_ee_sets = 0;
	bool ee_blank = false, ee_bad_checksum = false, ee_c46 = false;
	char tag_buf[64];
	enum ftdi_error_mode error_mode = FTDI_ERR_NONE;
	int error_count = 1, error_skip = 0;
	const char *error_chan = NULL;
	int port = USBIP_PORT;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:m:e:p:E:n:wC:POGT:D:V:S:BX4k:H:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			chip_name = optarg;
			break;
		case 'm':
			mode_name = optarg;
			break;
		case 'e':
			eeprom_path = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'E':
			error_mode_name = optarg;
			error_mode = parse_error_mode(optarg);
			break;
		case 'n':
			error_count = atoi(optarg);
			break;
		case 'w':
			ftdi_emu_ad7_wired = true;
			break;
		case 'C':
			ctl_path = optarg;
			break;
		case 'P':
			ftdi_emu_uart_pace = true;
			break;
		case 'O':
			ftdi_emu_spi_echo = true;
			break;
		case 'G':
			ftdi_emu_spi_cs_high = true;
			break;
		case 'T':
			ee_tag = optarg;
			break;
		case 'D':
			ee_product = optarg;
			break;
		case 'V':
			ee_vcp = optarg;
			break;
		case 'S':
			if (n_ee_sets == MAX_EE_SETS) {
				fprintf(stderr, "too many --ee-set\n");
				return 1;
			}
			ee_sets[n_ee_sets++] = parse_ee_set(optarg);
			break;
		case 'B':
			ee_blank = true;
			break;
		case '4':
			ee_c46 = true;
			break;
		case 'k':
			error_skip = atoi(optarg);
			break;
		case 'H':
			error_chan = optarg;
			break;
		case 'X':
			ee_bad_checksum = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (ftdi_emu_spi_echo && ftdi_emu_spi_cs_high) {
		fprintf(stderr, "--spi-echo and --spi-cs-high both use AD4\n");
		return 1;
	}

	ftdi_emu_init(&dev, parse_chip(chip_name));

	mode_name = parse_mode(mode_name);

	if (eeprom_path) {
		if (ftdi_emu_load_eeprom(&dev, eeprom_path) < 0)
			return 1;
		fprintf(stderr, "Loaded EEPROM from %s\n", eeprom_path);
		if (ee_tag || ee_product || ee_vcp || n_ee_sets || ee_blank ||
		    ee_bad_checksum || ee_c46)
			fprintf(stderr, "--ee-* options ignored with --eeprom\n");
	} else if (ee_blank) {
		memset(dev.eeprom, 0xFF, sizeof(dev.eeprom));
	} else {
		struct ftdi_ee_opts opts = { .product = ee_product, .c46 = ee_c46 };
		int i;

		/*
		 * uart: the VCP driver bit on every channel, the default of
		 * FT_PROG and ftdi_eeprom. spi/i2c: D2XX on the MPSSE channels
		 * and a "bus_mode=" tag with one entry per MPSSE channel; the
		 * channels without MPSSE (FT4232H C/D) stay serial ports.
		 */
		if (!strcmp(mode_name, "uart")) {
			opts.vcp = (1u << dev.num_interfaces) - 1;
		} else {
			int n = ftdi_emu_mpsse_channels(&dev);
			int len = snprintf(tag_buf, sizeof(tag_buf), "bus_mode=");

			for (i = 0; i < n; i++)
				len += snprintf(tag_buf + len,
						sizeof(tag_buf) - len, "%s%s",
						i ? "," : "", mode_name);
			opts.user_data = tag_buf;
			opts.vcp = ((1u << dev.num_interfaces) - 1) &
				   ~((1u << n) - 1);
		}

		if (ee_tag)
			opts.user_data = strcasecmp(ee_tag, "none") ? ee_tag : NULL;
		if (ee_vcp)
			opts.vcp = parse_vcp(ee_vcp);

		if (ee_c46 && opts.user_data)
			fprintf(stderr, "--ee-93c46: no room for the user-area text '%s'\n",
				opts.user_data);
		ftdi_emu_build_eeprom(&dev, &opts);

		for (i = 0; i < n_ee_sets; i++) {
			if (ee_c46 && ee_sets[i].off >= FTDI_EEPROM_SIZE / 2) {
				fprintf(stderr, "--ee-set: a 93C46 has %d bytes\n",
					FTDI_EEPROM_SIZE / 2);
				return 1;
			}
			dev.eeprom[ee_sets[i].off] = ee_sets[i].val;
		}
		if (n_ee_sets)
			ftdi_emu_fix_checksum(&dev);
		if (ee_bad_checksum) {
			dev.eeprom[FTDI_EEPROM_SIZE - 1] ^= 0x5A;
			if (ee_c46)
				dev.eeprom[FTDI_EEPROM_SIZE / 2 - 1] ^= 0x5A;
		}
	}
	/* The chip reads its EEPROM when it starts: VID, PID, strings */
	ftdi_emu_usb_identity(&dev);

	if (error_mode != FTDI_ERR_NONE) {
		int i, chan = -1;

		if (error_chan) {
			chan = (error_chan[0] | 0x20) - 'a';
			if (chan < 0 || chan >= dev.num_interfaces || error_chan[1]) {
				fprintf(stderr, "--error-chan: no channel '%s' on this chip\n",
					error_chan);
				return 1;
			}
		}
		for (i = 0; i < dev.num_interfaces; i++)
			if (chan < 0 || chan == i)
				ftdi_emu_set_error(&dev, i, error_mode,
						   error_count, error_skip);
		fprintf(stderr, "Error injection: mode=%s count=%d skip=%d channel=%s\n",
			error_mode_name, error_count, error_skip,
			error_chan ? error_chan : "all");
	}

	fprintf(stderr, "Emulating %s (VID=%04x PID=%04x bcdDevice=%04x) mode=%s\n",
		chip_name, dev.usb_vid, dev.usb_pid, dev.bcd, mode_name);

	if (ctl_path && usbip_server_set_ctl(ctl_path) < 0)
		return 1;

	/* The MPSSE model waits for times far below the default 50 us slack */
	prctl(PR_SET_TIMERSLACK, 1UL);

	return usbip_server_run(&dev, port) < 0 ? 1 : 0;
}
