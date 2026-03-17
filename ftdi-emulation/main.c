/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ftdi_usbip -- USB/IP server emulating an FTDI Hi-Speed device
 *
 * Presents a virtual FTDI device (FT232H / FT2232H / FT4232H) over
 * the USB/IP TCP protocol.  The client side uses "usbip attach" which
 * causes the kernel's vhci_hcd to bind the device, allowing the
 * ftdi_mpsse driver (or any other FTDI driver) to probe it.
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
		"  --mode MODE     Protocol mode: spi, i2c, uart (default: spi)\n"
		"                  Sets the EEPROM protocol hint byte at 0x1A\n"
		"  --eeprom FILE   Load EEPROM image from binary file (256 bytes)\n"
		"  --port PORT     TCP port to listen on (default: %d)\n"
		"  --error TYPE    Error injection: i2c-nak, i2c-stuck, usb-stall, usb-timeout, mpsse-sync\n"
		"  --error-count N Number of errors to inject (default: 1, 0=infinite)\n"
		"  --help          Show this help\n",
		progname, USBIP_PORT);
}

static enum ftdi_error_mode parse_error_mode(const char *name)
{
	if (!strcasecmp(name, "i2c-nak"))
		return FTDI_ERR_I2C_NAK;
	if (!strcasecmp(name, "i2c-stuck") || !strcasecmp(name, "i2c-frozen"))
		return FTDI_ERR_I2C_BUS_STUCK;
	if (!strcasecmp(name, "usb-stall"))
		return FTDI_ERR_USB_STALL;
	if (!strcasecmp(name, "usb-timeout"))
		return FTDI_ERR_USB_TIMEOUT;
	if (!strcasecmp(name, "mpsse-sync"))
		return FTDI_ERR_MPSSE_SYNC;
	if (!strcasecmp(name, "none"))
		return FTDI_ERR_NONE;

	fprintf(stderr, "Unknown error mode: %s\n", name);
	exit(1);
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

/* Protocol hint byte offset in EEPROM */
#define EEPROM_PROTOCOL_HINT	0x1A

static char parse_mode(const char *name)
{
	if (!strcasecmp(name, "spi"))
		return 'S';
	if (!strcasecmp(name, "i2c"))
		return 'I';
	if (!strcasecmp(name, "uart"))
		return 'U';

	fprintf(stderr, "Unknown mode: %s (use spi, i2c, or uart)\n", name);
	exit(1);
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
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	struct ftdi_device dev;
	const char *chip_name = "ft232h";
	const char *mode_name = "spi";
	const char *eeprom_path = NULL;
	const char *error_mode_name = NULL;
	enum ftdi_error_mode error_mode = FTDI_ERR_NONE;
	int error_count = 1;
	int port = USBIP_PORT;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:m:e:p:E:n:h", longopts, NULL)) != -1) {
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
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	ftdi_emu_init(&dev, parse_chip(chip_name));

	if (eeprom_path) {
		if (ftdi_emu_load_eeprom(&dev, eeprom_path) < 0)
			return 1;
		fprintf(stderr, "Loaded EEPROM from %s\n", eeprom_path);
	}

	/* Set protocol hint in EEPROM (overrides default or loaded value) */
	dev.eeprom[EEPROM_PROTOCOL_HINT] = parse_mode(mode_name);
	ftdi_emu_fix_checksum(&dev);

	/* Apply error injection to all interfaces */
	if (error_mode != FTDI_ERR_NONE) {
		int i;

		for (i = 0; i < dev.num_interfaces; i++)
			ftdi_emu_set_error(&dev, i, error_mode, error_count);
		fprintf(stderr, "Error injection: mode=%s count=%d\n",
			error_mode_name, error_count);
	}

	fprintf(stderr, "Emulating %s (VID=%04x PID=%04x bcdDevice=%04x) mode=%s\n",
		chip_name, dev.vid, dev.pid, dev.bcd, mode_name);

	return usbip_server_run(&dev, port) < 0 ? 1 : 0;
}
