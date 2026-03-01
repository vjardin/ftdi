// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * Enumerate all FTDI USB devices and check whether their EEPROM is empty.
 *
 * Build:  cc -o ftdi_eeprom_check ftdi_eeprom_check.c
 * Usage:  sudo ./ftdi_eeprom_check
 *
 * Uses only raw Linux USB devfs ioctls -- no libusb/libftdi dependency.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>

#define FTDI_VID		0x0403

/* FTDI SIO vendor request to read EEPROM (word-addressed, returns 2 bytes) */
#define SIO_READ_EEPROM_REQUEST	0x90
#define SIO_REQTYPE_IN		0xC0	/* vendor, device-to-host */

#define DEFAULT_EEPROM_WORDS	128

struct ftdi_product {
	uint16_t pid;
	const char *name;
	int eeprom_words;	/* EEPROM size in 16-bit words */
};

static const struct ftdi_product products[] = {
	{ 0x6001, "FT232R",      64 },
	{ 0x6010, "FT2232H",    128 },
	{ 0x6011, "FT4232H",    128 },
	{ 0x6014, "FT232H",     128 },
	{ 0x6015, "FT-X series",  64 },
	{ 0x6040, "FT2233HP",   128 },
	{ 0x6041, "FT4233HP",   128 },
	{ 0x6042, "FT2232HP",   128 },
	{ 0x6043, "FT4232HP",   128 },
	{ 0x6044, "FT233HP",    128 },
	{ 0x6045, "FT232HP",    128 },
	{ 0x6048, "FT4232HA",   128 },
};

static const struct ftdi_product *find_product(uint16_t pid)
{
	for (size_t i = 0; i < sizeof(products) / sizeof(products[0]); i++) {
		if (products[i].pid == pid)
			return &products[i];
	}
	return NULL;
}

/*
 * Issue a USB control transfer via usbdevfs ioctl.
 * Returns the number of bytes transferred or -1 on error.
 */
static int usb_control(int fd, uint8_t reqtype, uint8_t request,
		       uint16_t value, uint16_t index,
		       void *data, uint16_t size, unsigned int timeout_ms)
{
	struct usbdevfs_ctrltransfer ct = {
		.bRequestType = reqtype,
		.bRequest     = request,
		.wValue       = value,
		.wIndex       = index,
		.wLength      = size,
		.timeout      = timeout_ms,
		.data         = data,
	};

	return ioctl(fd, USBDEVFS_CONTROL, &ct);
}

/*
 * Detach the kernel driver from interface 0 so we can issue control transfers.
 * Returns 1 if a driver was detached (needs reattach), 0 otherwise.
 */
static int detach_kernel_driver(int fd)
{
	struct usbdevfs_getdriver gd;

	memset(&gd, 0, sizeof(gd));
	gd.interface = 0;

	if (ioctl(fd, USBDEVFS_GETDRIVER, &gd) < 0)
		return 0;	/* no driver attached */

	struct usbdevfs_ioctl cmd = {
		.ifno         = 0,
		.ioctl_code   = USBDEVFS_DISCONNECT,
		.data         = NULL,
	};

	if (ioctl(fd, USBDEVFS_IOCTL, &cmd) < 0)
		return 0;

	return 1;
}

static void reattach_kernel_driver(int fd)
{
	struct usbdevfs_ioctl cmd = {
		.ifno         = 0,
		.ioctl_code   = USBDEVFS_CONNECT,
		.data         = NULL,
	};

	ioctl(fd, USBDEVFS_IOCTL, &cmd);
}

/*
 * Read EEPROM word by word.  Returns 0 on success, -1 on error.
 */
static int read_eeprom(int fd, uint16_t *words, int num_words)
{
	uint8_t buf[2];

	for (int addr = 0; addr < num_words; addr++) {
		int ret = usb_control(fd, SIO_REQTYPE_IN,
				      SIO_READ_EEPROM_REQUEST,
				      0, addr, buf, 2, 5000);
		if (ret < 2) {
			fprintf(stderr, "  EEPROM read error at word 0x%04x: %s\n",
				addr, ret < 0 ? strerror(errno) : "short read");
			return -1;
		}
		words[addr] = buf[0] | (buf[1] << 8);
	}
	return 0;
}

static int eeprom_is_empty(const uint16_t *words, int num_words)
{
	for (int i = 0; i < num_words; i++) {
		if (words[i] != 0xFFFF)
			return 0;
	}
	return 1;
}

static void eeprom_summary(const uint16_t *words, int num_words)
{
	int n = num_words < 8 ? num_words : 8;

	printf("  First words:");
	for (int i = 0; i < n; i++)
		printf(" %04x", words[i]);
	if (num_words > 8)
		printf(" ...");
	printf("\n");
}

/*
 * Read the USB device descriptor from an open usbdevfs file descriptor.
 * Returns 0 on success, -1 on error.
 */
static int read_device_descriptor(int fd, struct usb_device_descriptor *desc)
{
	ssize_t n = read(fd, desc, sizeof(*desc));

	if (n < (ssize_t)sizeof(*desc))
		return -1;
	return 0;
}

/*
 * Scan /dev/bus/usb for FTDI devices.
 */
static int scan_devices(void)
{
	DIR *busdir;
	struct dirent *busent;
	int found = 0;

	busdir = opendir("/dev/bus/usb");
	if (!busdir) {
		perror("opendir /dev/bus/usb");
		return -1;
	}

	while ((busent = readdir(busdir)) != NULL) {
		if (busent->d_name[0] == '.')
			continue;

		char buspath[PATH_MAX];
		int blen;

		blen = snprintf(buspath, sizeof(buspath), "/dev/bus/usb/%s",
				busent->d_name);
		if (blen < 0 || (size_t)blen >= sizeof(buspath))
			continue;

		DIR *devdir = opendir(buspath);
		if (!devdir)
			continue;

		struct dirent *devent;

		while ((devent = readdir(devdir)) != NULL) {
			if (devent->d_name[0] == '.')
				continue;

			char devpath[PATH_MAX];
			int dlen;

			dlen = snprintf(devpath, sizeof(devpath), "%s/%s",
					buspath, devent->d_name);
			if (dlen < 0 || (size_t)dlen >= sizeof(devpath))
				continue;

			int fd = open(devpath, O_RDWR);
			if (fd < 0) {
				fd = open(devpath, O_RDONLY);
				if (fd < 0)
					continue;
			}

			struct usb_device_descriptor desc;

			if (read_device_descriptor(fd, &desc) < 0) {
				close(fd);
				continue;
			}

			if (desc.idVendor != FTDI_VID) {
				close(fd);
				continue;
			}

			uint16_t pid = desc.idProduct;
			const struct ftdi_product *prod = find_product(pid);
			const char *name = prod ? prod->name : "Unknown";
			int num_words = prod ? prod->eeprom_words
					     : DEFAULT_EEPROM_WORDS;

			printf("[%s:%s] %s (VID:PID %04x:%04x)\n",
			       busent->d_name, devent->d_name,
			       name, FTDI_VID, pid);

			int reattach = detach_kernel_driver(fd);

			uint16_t *words = calloc(num_words, sizeof(uint16_t));
			if (!words) {
				fprintf(stderr, "  malloc failed\n");
				goto next;
			}

			if (read_eeprom(fd, words, num_words) < 0) {
				printf("  EEPROM: read failed\n");
			} else if (eeprom_is_empty(words, num_words)) {
				printf("  EEPROM: EMPTY (all 0xFFFF, %d bytes)\n",
				       num_words * 2);
			} else {
				printf("  EEPROM: programmed (%d bytes)\n",
				       num_words * 2);
				eeprom_summary(words, num_words);
			}

			free(words);
next:
			if (reattach)
				reattach_kernel_driver(fd);
			close(fd);
			printf("\n");
			found++;
		}
		closedir(devdir);
	}
	closedir(busdir);

	return found;
}

int main(void)
{
	int found = scan_devices();

	if (found < 0)
		return 1;
	if (found == 0) {
		printf("No FTDI devices found.\n");
		return 0;
	}

	printf("Found %d FTDI device(s).\n", found);
	return 0;
}
