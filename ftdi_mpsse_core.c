// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE USB interface driver
 *
 * Owns the USB interface and the bulk endpoints of one channel of an FTDI
 * Hi-Speed chip (FT232H, FT2232H, FT4232H and variants) and creates the
 * UART, SPI, I2C and GPIO platform children (viperboard / dln2 pattern).
 * They do their I/O through the exported ftdi_mpsse_*() functions; SPI, I2C
 * and GPIO run on the MPSSE (Multi-Protocol Synchronous Serial Engine).
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include <linux/mfd/core.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi_mpsse"
#define DRIVER_DESC	"FTDI MPSSE USB interface driver"

/*
 * Longest silence of the MPSSE while a response is due (ms). Re-armed by
 * response data: it bounds a stall of the engine (clock stretching, slow
 * clock), not the length of a transfer.
 */
#define FTDI_MPSSE_RESPONSE_TIMEOUT	2000

/* Bounded: error recovery must not stall a disconnect racing with it */
#define FTDI_MPSSE_RECOVERY_TIMEOUT	500

/* MPSSE synchronisation at probe/resume: wait for the echo (ms) */
#define FTDI_MPSSE_SYNC_TIMEOUT		10000

/* DMA-safe bounce buffer for bulk OUT (callers may pass stack buffers) */
#define FTDI_MPSSE_TX_BOUNCE	FTDI_MPSSE_BUF_SIZE

/* Per-interface state: dev_get_drvdata(pdev->dev.parent) for a child */
struct ftdi_mpsse_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	u8 channel;			/* 1-based: FTDI_CHANNEL_A..D */
	enum ftdi_mpsse_chip_type chip_type;
	struct usb_endpoint_descriptor *ep_bulk_in;
	struct usb_endpoint_descriptor *ep_bulk_out;

	/* Lock ordering: bus_lock -> fg->lock -> io_lock -> disconnect_lock */
	struct mutex bus_lock;		/* whole transaction, pin state */
	struct mutex io_lock;		/* single USB transfer */
	struct urb *bulk_in_urb;
	struct urb *bulk_out_urb;
	struct completion io_done;
	int io_status;
	int io_actual;
	u8 *bulk_buf;			/* DMA-safe read buffer */
	u8 *tx_bounce;

	/*
	 * Pin shadow of the channel by bank (0 = xDBUS, 1 = xCBUS): every
	 * SET_BITS_LOW/HIGH and 0x9E command is built from it, so that a
	 * sibling's pins are never clobbered. Protected by bus_lock.
	 */
	u8 pin_val[2];
	u8 pin_dir[2];
	u8 pin_od[2];

	/*
	 * Pins that no mode reserves, by bank (bus_lock): claimed by the bus
	 * driver at run time, or handed out by the GPIO driver. Never both.
	 */
	u8 pin_claimed[2];
	u8 pin_gpio[2];

	/*
	 * need_resync (io_lock): a transfer failed, no command is sent before
	 * the engine has been reset and synchronised again.
	 * ep_halted (io_lock): a bulk transfer STALLed, the endpoints are
	 * cleared before anything else.
	 */
	bool need_resync;
	bool ep_halted;

	/* Disconnect / suspend synchronisation (DLN2 pattern) */
	spinlock_t disconnect_lock;
	bool disconnect;
	bool suspended;
	bool mpsse_mode;		/* SPI and I2C: MPSSE entered */
	bool mpsse_capable;		/* false for FT4232H channels C/D */
	int active_transfers;
	wait_queue_head_t disconnect_wq;

	/* Diagnostic info stored at probe for the debugfs pinmap */
	char mode_name[16];
	u16 gpio_reserved_mask;
	u8 spi_cs_pins[FTDI_SPI_CS_MAX];
	u8 spi_num_cs;

	struct ftdi_eeprom eeprom;
	struct dentry *debugfs;
};

/* <debugfs>/ftdi_mpsse/<USB interface>/ */
static struct dentry *ftdi_mpsse_debugfs_root;

/* Internal accessors for ftdi_eeprom.c (compiled into same module) */
struct usb_interface *ftdi_mpsse_dev_get_intf(struct ftdi_mpsse_dev *fdev)
{
	return fdev->intf;
}

struct usb_device *ftdi_mpsse_dev_get_udev(struct ftdi_mpsse_dev *fdev)
{
	return fdev->udev;
}

enum ftdi_mpsse_chip_type ftdi_mpsse_dev_get_chip(struct ftdi_mpsse_dev *fdev)
{
	return fdev->chip_type;
}

u8 ftdi_mpsse_dev_get_channel(struct ftdi_mpsse_dev *fdev)
{
	return fdev->channel;
}

struct ftdi_eeprom *ftdi_mpsse_dev_get_eeprom_ptr(struct ftdi_mpsse_dev *fdev)
{
	return &fdev->eeprom;
}

static char *bus_mode = "";
module_param(bus_mode, charp, 0444);
MODULE_PARM_DESC(bus_mode,
		 "Channel mode: 'uart', 'spi', 'i2c', 'bitbang' (alias 'gpio': 8 async bit-bang GPIOs, no UART), or '' for auto (default). Comma-separated for per-interface: 'spi,uart' sets interface 0=spi, interface 1=uart; a missing entry is auto. Single value applies to all. FT4232H C/D accept only uart or bitbang.");

static char *spi_cs = "3";
module_param(spi_cs, charp, 0444);
MODULE_PARM_DESC(spi_cs,
		 "SPI chip-select AD pin numbers 3-7 (default '3'). Global: '3,4,5' applies to all SPI channels. Per-channel: 'A:3,4,5;B:3' for different configs. Remaining ADBUS pins available as GPIO.");

/*
 * Per AN_255 §2.2.2: 16 ms (the chip default) when SEND_IMMEDIATE (0x87)
 * flushes the responses. The timer then only paces the idle status
 * packets: a low value just adds USB traffic.
 */
static unsigned int mpsse_latency = 16;
module_param(mpsse_latency, uint, 0644);
MODULE_PARM_DESC(mpsse_latency,
		 "MPSSE mode USB latency timer in ms (1-255, default 16), applied when a channel enters MPSSE. SEND_IMMEDIATE provides fast response; the timer only affects idle status packet rate (per AN_255).");

/* FTDI_MODE_AUTO: nothing explicit was asked for the interface */
enum ftdi_mpsse_mode {
	FTDI_MODE_AUTO,
	FTDI_MODE_UART,
	FTDI_MODE_SPI,
	FTDI_MODE_I2C,
	FTDI_MODE_BITBANG,
};

static const char * const ftdi_mpsse_mode_names[] = {
	[FTDI_MODE_AUTO]	= "auto",
	[FTDI_MODE_UART]	= "uart",
	[FTDI_MODE_SPI]		= "spi",
	[FTDI_MODE_I2C]		= "i2c",
	[FTDI_MODE_BITBANG]	= "bitbang",
};

/* Upper-case protocol name for the log */
static const char *ftdi_mpsse_mode_label(enum ftdi_mpsse_mode mode)
{
	switch (mode) {
	case FTDI_MODE_UART:
		return "UART";
	case FTDI_MODE_SPI:
		return "SPI";
	case FTDI_MODE_I2C:
		return "I2C";
	case FTDI_MODE_BITBANG:
		return "bit-bang";
	default:
		return "auto";
	}
}

static enum ftdi_mpsse_mode ftdi_mpsse_mode_from_name(const char *name)
{
	if (!strcmp(name, "uart"))
		return FTDI_MODE_UART;
	if (!strcmp(name, "spi"))
		return FTDI_MODE_SPI;
	if (!strcmp(name, "i2c"))
		return FTDI_MODE_I2C;
	if (!strcmp(name, "bitbang") || !strcmp(name, "gpio"))
		return FTDI_MODE_BITBANG;
	return FTDI_MODE_AUTO;
}

/*
 * Entry of interface @ifnum in a bus_mode list (module parameter or EEPROM
 * tag, same syntax). A single value applies to every interface; in a
 * comma-separated list a missing or empty entry yields "" (auto).
 */
static void ftdi_mpsse_mode_token(const char *p, unsigned int ifnum,
				  char *out, size_t out_len)
{
	unsigned int idx = 0;
	size_t len;

	if (!p || !*p) {
		out[0] = '\0';
		return;
	}

	if (!strchr(p, ',')) {
		strscpy(out, p, out_len);
		return;
	}

	while (*p) {
		const char *comma = strchr(p, ',');

		len = comma ? (size_t)(comma - p) : strlen(p);

		if (idx == ifnum) {
			len = min(len, out_len - 1);
			memcpy(out, p, len);
			out[len] = '\0';
			return;
		}

		if (!comma)
			break;
		p = comma + 1;
		idx++;
	}

	out[0] = '\0';
}

/*
 * Comma-separated CS pin numbers (3-7: AD3-AD7, each once) from p up to a
 * ';' or the end of the string. Returns their number, or -EINVAL.
 */
static int ftdi_mpsse_parse_cs_pins(const char *p, u8 *pins, unsigned int max)
{
	unsigned int count = 0, i;

	for (;;) {
		size_t len = strcspn(p, ",;");
		unsigned int val;
		char tmp[4];

		if (!len || len >= sizeof(tmp))
			return -EINVAL;
		memcpy(tmp, p, len);
		tmp[len] = '\0';
		if (kstrtouint(strim(tmp), 10, &val) || val < 3 || val > 7)
			return -EINVAL;
		for (i = 0; i < count; i++)
			if (pins[i] == val)
				return -EINVAL;
		if (count == max)
			return -EINVAL;
		pins[count++] = val;

		p += len;
		if (*p != ',')
			return count;
		p++;
	}
}

/*
 * spi_cs of one channel: "3,4,5" applies to every SPI channel, "A:3,4,5;B:3"
 * is per channel, and a channel without entry gets CS0 = AD3. Every entry
 * is checked, not only the one of this channel: a typo fails the probe.
 */
static int ftdi_mpsse_parse_spi_cs(u8 channel, u8 *pins, unsigned int max)
{
	const char *p = spi_cs;
	u8 other[FTDI_SPI_CS_MAX];
	int ret = 0;

	if (!p || !*p)
		return -EINVAL;

	if (!strchr(p, ':'))
		return strchr(p, ';') ? -EINVAL :
					ftdi_mpsse_parse_cs_pins(p, pins, max);

	for (;;) {
		char ch;
		int n;

		p = skip_spaces(p);
		ch = toupper(*p);

		if (ch < 'A' || ch > 'D' || p[1] != ':')
			return -EINVAL;
		if (ch - 'A' + FTDI_CHANNEL_A == channel) {
			if (ret)
				return -EINVAL;		/* listed twice */
			n = ftdi_mpsse_parse_cs_pins(p + 2, pins, max);
			ret = n;
		} else {
			n = ftdi_mpsse_parse_cs_pins(p + 2, other,
						     ARRAY_SIZE(other));
		}
		if (n < 0)
			return n;

		p = strchr(p, ';');
		if (!p || !*++p)
			break;
	}

	if (ret)
		return ret;
	pins[0] = 3;
	return 1;
}

/* The kernel has no strcasestr() */
static bool str_contains_ci(const char *haystack, const char *needle)
{
	size_t hlen, nlen, i;

	if (!haystack || !needle || !*needle)
		return false;

	hlen = strlen(haystack);
	nlen = strlen(needle);

	if (nlen > hlen)
		return false;

	for (i = 0; i <= hlen - nlen; i++) {
		if (strncasecmp(haystack + i, needle, nlen) == 0)
			return true;
	}
	return false;
}

/*
 * Auto mode: what a valid EEPROM hints for an MPSSE-capable channel. The
 * VCP driver bit marks a serial port; slow slew and Schmitt trigger are
 * the settings of an open-drain bus. The channel type says nothing: MPSSE
 * is entered at run time from type UART. *why is NULL for the default.
 */
static enum ftdi_mpsse_mode
ftdi_mpsse_detect_protocol(const struct ftdi_eeprom *ee,
			   enum ftdi_mpsse_chip_type chip, u8 channel,
			   const char **why)
{
	int drive;

	if (str_contains_ci(ee->product, "SPI")) {
		*why = "product string";
		return FTDI_MODE_SPI;
	}
	if (str_contains_ci(ee->product, "I2C")) {
		*why = "product string";
		return FTDI_MODE_I2C;
	}

	if (ftdi_eeprom_channel_vcp(ee, channel)) {
		*why = "VCP driver bit";
		return FTDI_MODE_UART;
	}

	drive = ftdi_eeprom_dbus_drive(ee, chip, channel);
	if (drive >= 0 && (drive & FTDI_EE_DRIVE_SLOW_SLEW) &&
	    (drive & FTDI_EE_DRIVE_SCHMITT)) {
		*why = "slow slew and Schmitt trigger on the data bus";
		return FTDI_MODE_I2C;
	}

	*why = NULL;
	return FTDI_MODE_SPI;
}

static enum ftdi_mpsse_chip_type
ftdi_mpsse_detect_chip(struct usb_device *udev)
{
	u16 version = le16_to_cpu(udev->descriptor.bcdDevice);

	switch (version) {
	case 0x0700:	return FTDI_CHIP_FT2232H;
	case 0x0800:	return FTDI_CHIP_FT4232H;
	case 0x0900:	return FTDI_CHIP_FT232H;
	case 0x2800:	return FTDI_CHIP_FT2233HP;
	case 0x2900:	return FTDI_CHIP_FT4233HP;
	case 0x3000:	return FTDI_CHIP_FT2232HP;
	case 0x3100:	return FTDI_CHIP_FT4232HP;
	case 0x3200:	return FTDI_CHIP_FT233HP;
	case 0x3300:	return FTDI_CHIP_FT232HP;
	case 0x3600:	return FTDI_CHIP_FT4232HA;
	default:	return FTDI_CHIP_UNKNOWN;
	}
}

/* Every channel has the MPSSE, except C and D of the FT4232H family */
static bool ftdi_mpsse_capable(enum ftdi_mpsse_chip_type chip, u8 channel)
{
	switch (chip) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		return true;
	case FTDI_CHIP_FT2232H:
	case FTDI_CHIP_FT2232HP:
	case FTDI_CHIP_FT2233HP:
		return true;
	case FTDI_CHIP_FT4232H:
	case FTDI_CHIP_FT4232HA:
	case FTDI_CHIP_FT4232HP:
	case FTDI_CHIP_FT4233HP:
		return channel <= FTDI_CHANNEL_B;
	default:
		return false;
	}
}

/*
 * Disconnect and suspend kill the URB in flight, but a transfer is several
 * URBs (write chunks, one bulk IN per response packet): it must not submit
 * the next one. For the transfers of the children only: resume and the
 * recovery do their own I/O while the flag is still set.
 */
static bool ftdi_mpsse_gone(struct ftdi_mpsse_dev *fdev)
{
	return READ_ONCE(fdev->disconnect) || READ_ONCE(fdev->suspended);
}

static void ftdi_mpsse_urb_complete(struct urb *urb)
{
	struct ftdi_mpsse_dev *fdev = urb->context;

	fdev->io_status = urb->status;
	fdev->io_actual = urb->actual_length;
	complete(&fdev->io_done);
}

static int ftdi_mpsse_bulk_write_chunk(struct ftdi_mpsse_dev *fdev,
				       unsigned int len, unsigned int busy_ms)
{
	/*
	 * The chip takes a write larger than what is free in its buffer
	 * only as the MPSSE executes the commands: at a slow clock that is
	 * the time the caller announces, on top of the USB timeout.
	 */
	unsigned long timeout = msecs_to_jiffies(FTDI_MPSSE_WRITE_TIMEOUT) +
				msecs_to_jiffies(busy_ms);
	int ret;

	reinit_completion(&fdev->io_done);

	usb_fill_bulk_urb(fdev->bulk_out_urb, fdev->udev,
			  usb_sndbulkpipe(fdev->udev,
					  fdev->ep_bulk_out->bEndpointAddress),
			  fdev->tx_bounce, len,
			  ftdi_mpsse_urb_complete, fdev);

	ret = usb_submit_urb(fdev->bulk_out_urb, GFP_KERNEL);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&fdev->io_done, timeout)) {
		usb_kill_urb(fdev->bulk_out_urb);
		/*
		 * usb_kill_urb() is synchronous: the callback has run. A
		 * cancelled URB has -ENOENT, or -ECONNRESET on host controllers
		 * that unlink asynchronously (vhci_hcd): it timed out.
		 */
		if (fdev->io_status == -ENOENT ||
		    fdev->io_status == -ECONNRESET)
			return -ETIMEDOUT;
		return fdev->io_status;
	}

	if (!fdev->io_status && fdev->io_actual != len)
		return -EIO;
	return fdev->io_status;
}

/* Called with io_lock held: it protects the URBs and their buffers */
static int ftdi_mpsse_bulk_write(struct ftdi_mpsse_dev *fdev,
				 const u8 *buf, unsigned int len,
				 unsigned int busy_ms)
{
	while (len) {
		unsigned int n = min_t(unsigned int, len, FTDI_MPSSE_TX_BOUNCE);
		int ret;

		if (ftdi_mpsse_gone(fdev))
			return -ESHUTDOWN;

		memcpy(fdev->tx_bounce, buf, n);
		ret = ftdi_mpsse_bulk_write_chunk(fdev, n, busy_ms);
		if (ret)
			return ret;
		buf += n;
		len -= n;
	}
	return 0;
}

static int ftdi_mpsse_bulk_read(struct ftdi_mpsse_dev *fdev,
				u8 *buf, unsigned int len,
				unsigned int busy_ms)
{
	unsigned int maxp = usb_endpoint_maxp(fdev->ep_bulk_in);
	unsigned long timeout = msecs_to_jiffies(FTDI_MPSSE_RESPONSE_TIMEOUT);
	/* The engine first has to get through what the caller queued */
	unsigned long deadline = jiffies + timeout + msecs_to_jiffies(busy_ms);
	unsigned int collected = 0;

	/*
	 * Every bulk IN packet starts with 2 modem-status bytes; status-only
	 * packets come at the latency timer rate while no data is pending.
	 */
	while (collected < len) {
		int actual, payload;
		long left;
		int ret;

		left = deadline - jiffies;
		if (left <= 0)
			return -ETIMEDOUT;

		if (ftdi_mpsse_gone(fdev))
			return -ESHUTDOWN;

		reinit_completion(&fdev->io_done);

		usb_fill_bulk_urb(fdev->bulk_in_urb, fdev->udev,
				  usb_rcvbulkpipe(fdev->udev,
						  fdev->ep_bulk_in->bEndpointAddress),
				  fdev->bulk_buf, maxp,
				  ftdi_mpsse_urb_complete, fdev);

		ret = usb_submit_urb(fdev->bulk_in_urb, GFP_KERNEL);
		if (ret)
			return ret;

		if (!wait_for_completion_timeout(&fdev->io_done, left)) {
			usb_kill_urb(fdev->bulk_in_urb);
			if (fdev->io_status == -ENOENT ||
			    fdev->io_status == -ECONNRESET)
				return -ETIMEDOUT;
			return fdev->io_status;
		}

		if (fdev->io_status)
			return fdev->io_status;

		actual = fdev->io_actual;
		if (actual < FTDI_MPSSE_STATUS_BYTES)
			return -EIO;

		payload = actual - FTDI_MPSSE_STATUS_BYTES;

		/* Status-only packet: the deadline keeps running */
		if (payload == 0)
			continue;

		/*
		 * Callers read exactly what their commands produce: more means
		 * the stream is out of step (the late answer of a command that
		 * timed out) and the error path resynchronises the engine.
		 * 0xFA is valid response data: no "bad command" check here.
		 */
		if (payload > len - collected) {
			dev_warn_ratelimited(&fdev->intf->dev,
					     "%u unexpected response byte(s): MPSSE out of step\n",
					     payload - (len - collected));
			return -EPROTO;
		}

		memcpy(buf + collected,
		       fdev->bulk_buf + FTDI_MPSSE_STATUS_BYTES, payload);
		collected += payload;

		/*
		 * A short packet is not the end of the response: an MPSSE
		 * slower than the latency timer has the chip send what it has
		 * (AN232B-04, TN_103). Data is flowing: full timeout again.
		 */
		deadline = jiffies + timeout;
	}

	return collected;
}

static struct ftdi_mpsse_dev *ftdi_mpsse_get_dev(struct platform_device *pdev)
{
	return dev_get_drvdata(pdev->dev.parent);
}

/* Vendor request without data, for this channel */
static int ftdi_mpsse_vendor_out(struct ftdi_mpsse_dev *fdev, u8 request,
				 u16 value, int timeout)
{
	return usb_control_msg(fdev->udev, usb_sndctrlpipe(fdev->udev, 0),
			       request, FTDI_SIO_REQTYPE_OUT, value,
			       fdev->channel, NULL, 0, timeout);
}

/* Empty the chip's buffers: host-to-chip first, then chip-to-host */
static int ftdi_mpsse_purge(struct ftdi_mpsse_dev *fdev, int timeout)
{
	int ret, ret2;

	ret = ftdi_mpsse_vendor_out(fdev, FTDI_SIO_RESET_REQUEST,
				    FTDI_SIO_RESET_PURGE_RX, timeout);
	ret2 = ftdi_mpsse_vendor_out(fdev, FTDI_SIO_RESET_REQUEST,
				     FTDI_SIO_RESET_PURGE_TX, timeout);
	if (ret < 0)
		return ret;
	return ret2 < 0 ? ret2 : 0;
}

static int ftdi_mpsse_resync(struct ftdi_mpsse_dev *fdev);

/*
 * usb_clear_halt() with the recovery timeout instead of 5 s: CLEAR_FEATURE
 * (ENDPOINT_HALT), then the host side of the data toggle. Not after the
 * disconnect or suspend that a stuck device may be holding up.
 */
static int ftdi_mpsse_clear_halt(struct ftdi_mpsse_dev *fdev, u8 ep)
{
	int ret;

	if (READ_ONCE(fdev->disconnect) || READ_ONCE(fdev->suspended))
		return -ESHUTDOWN;

	ret = usb_control_msg(fdev->udev, usb_sndctrlpipe(fdev->udev, 0),
			      USB_REQ_CLEAR_FEATURE, USB_RECIP_ENDPOINT,
			      USB_ENDPOINT_HALT, ep, NULL, 0,
			      FTDI_MPSSE_RECOVERY_TIMEOUT);
	if (ret < 0)
		return ret;

	usb_reset_endpoint(fdev->udev, ep);
	return 0;
}

/*
 * A halted endpoint stays halted until the host clears it (USB 2.0
 * §9.4.5). Which one stalled is not known: both are cleared. Called with
 * io_lock held.
 */
static int ftdi_mpsse_clear_halts(struct ftdi_mpsse_dev *fdev)
{
	int ret;

	if (!fdev->ep_halted)
		return 0;
	ret = ftdi_mpsse_clear_halt(fdev, fdev->ep_bulk_out->bEndpointAddress);
	if (!ret)
		ret = ftdi_mpsse_clear_halt(fdev,
					    fdev->ep_bulk_in->bEndpointAddress);
	if (!ret)
		fdev->ep_halted = false;
	return ret;
}

/*
 * Called with io_lock held when a transfer failed: the engine is reset
 * before anything else is sent, see ftdi_mpsse_resync(). If that fails,
 * need_resync makes the next transfer try again first.
 */
static void ftdi_mpsse_xfer_failed(struct ftdi_mpsse_dev *fdev, int err)
{
	/*
	 * Not for cancelled URBs (unbind, unplug, suspend): the device may
	 * be gone and control requests would wait for their timeouts while
	 * disconnect waits for this transfer. Resume re-enters the mode.
	 */
	switch (err) {
	case -ESHUTDOWN:
	case -ENODEV:
	case -EPERM:
	case -ENOENT:
	case -ECONNRESET:
		return;
	}
	if (READ_ONCE(fdev->disconnect) || READ_ONCE(fdev->suspended))
		return;

	if (err == -EPIPE)
		fdev->ep_halted = true;

	if (!fdev->mpsse_mode) {
		/* Bit-bang data, no command stream: drop what is queued */
		if (!ftdi_mpsse_clear_halts(fdev))
			ftdi_mpsse_purge(fdev, FTDI_MPSSE_RECOVERY_TIMEOUT);
		return;
	}

	fdev->need_resync = true;
	if (!ftdi_mpsse_resync(fdev))
		dev_warn_ratelimited(&fdev->intf->dev,
				     "MPSSE resynchronised after a failed transfer (%d)\n",
				     err);
}

/* The disconnect/suspend gate of every transport call */
static int ftdi_mpsse_get_transfer(struct ftdi_mpsse_dev *fdev)
{
	guard(spinlock)(&fdev->disconnect_lock);
	if (fdev->disconnect || fdev->suspended)
		return -ESHUTDOWN;
	fdev->active_transfers++;
	return 0;
}

static void ftdi_mpsse_put_transfer(struct ftdi_mpsse_dev *fdev)
{
	bool wake;

	scoped_guard(spinlock, &fdev->disconnect_lock) {
		fdev->active_transfers--;
		wake = !fdev->active_transfers &&
		       (fdev->disconnect || fdev->suspended);
	}
	if (wake)
		wake_up(&fdev->disconnect_wq);
}

/* One command/response exchange. Called with io_lock held. */
static int ftdi_mpsse_do_xfer(struct ftdi_mpsse_dev *fdev,
			      const u8 *tx, unsigned int tx_len,
			      u8 *rx, unsigned int rx_len, unsigned int busy_ms)
{
	int ret = 0;

	if (tx && tx_len)
		ret = ftdi_mpsse_bulk_write(fdev, tx, tx_len, busy_ms);

	if (!ret && rx && rx_len) {
		ret = ftdi_mpsse_bulk_read(fdev, rx, rx_len, busy_ms);
		if (ret >= 0)
			ret = ret < rx_len ? -EIO : 0;
	}

	if (ret) {
		ftdi_mpsse_xfer_failed(fdev, ret);
		/*
		 * Cut short by a disconnect or a suspend: fail the way the
		 * next transfer will, not with the status of a killed URB.
		 */
		if (ftdi_mpsse_gone(fdev))
			ret = -ESHUTDOWN;
	}
	return ret;
}

int ftdi_mpsse_xfer_busy(struct platform_device *pdev,
			 const u8 *tx, unsigned int tx_len,
			 u8 *rx, unsigned int rx_len, unsigned int busy_ms)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);
	int ret = 0;

	if (!fdev)
		return -ENODEV;

	/* The recovery replays the pin shadow, which the bus lock protects */
	if (fdev->mpsse_mode)
		lockdep_assert_held(&fdev->bus_lock);

	ret = ftdi_mpsse_get_transfer(fdev);
	if (ret)
		return ret;

	scoped_guard(mutex, &fdev->io_lock) {
		/* An earlier failure left the engine out of step: nothing goes out */
		if (fdev->need_resync)
			ret = ftdi_mpsse_resync(fdev);
		if (!ret)
			ret = ftdi_mpsse_do_xfer(fdev, tx, tx_len, rx, rx_len,
						 busy_ms);
	}

	ftdi_mpsse_put_transfer(fdev);

	return ret;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_xfer_busy);

int ftdi_mpsse_xfer(struct platform_device *pdev,
		    const u8 *tx, unsigned int tx_len,
		    u8 *rx, unsigned int rx_len)
{
	return ftdi_mpsse_xfer_busy(pdev, tx, tx_len, rx, rx_len, 0);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_xfer);

int ftdi_mpsse_write(struct platform_device *pdev,
		     const u8 *buf, unsigned int len)
{
	return ftdi_mpsse_xfer(pdev, buf, len, NULL, 0);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_write);

void ftdi_mpsse_bus_lock(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (fdev)
		mutex_lock(&fdev->bus_lock);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_bus_lock);

void ftdi_mpsse_bus_unlock(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (fdev)
		mutex_unlock(&fdev->bus_lock);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_bus_unlock);

int ftdi_mpsse_cfg_msg(struct platform_device *pdev, u8 request, u16 value)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);
	int ret;

	if (!fdev)
		return -ENODEV;

	ret = ftdi_mpsse_get_transfer(fdev);
	if (ret)
		return ret;

	scoped_guard(mutex, &fdev->io_lock)
		ret = usb_control_msg(fdev->udev,
				      usb_sndctrlpipe(fdev->udev, 0),
				      request,
				      USB_TYPE_VENDOR | USB_RECIP_DEVICE |
					USB_DIR_OUT,
				      value, fdev->channel, NULL, 0,
				      FTDI_MPSSE_CTRL_TIMEOUT);

	ftdi_mpsse_put_transfer(fdev);

	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_cfg_msg);

/* READ_PINS works in any bit mode */
int ftdi_mpsse_read_pins(struct platform_device *pdev, u8 *val)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);
	int ret;

	if (!fdev)
		return -ENODEV;

	ret = ftdi_mpsse_get_transfer(fdev);
	if (ret)
		return ret;

	scoped_guard(mutex, &fdev->io_lock)
		/* usb_control_msg_recv bounces through a DMA-safe buffer */
		ret = usb_control_msg_recv(fdev->udev, 0,
					   FTDI_SIO_READ_PINS_REQUEST,
					   FTDI_SIO_REQTYPE_IN, 0,
					   fdev->channel, val, 1,
					   FTDI_MPSSE_CTRL_TIMEOUT, GFP_KERNEL);

	ftdi_mpsse_put_transfer(fdev);

	return ret;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_read_pins);

struct usb_device *ftdi_mpsse_get_udev(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? fdev->udev : NULL;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_udev);

int ftdi_mpsse_get_endpoints(struct platform_device *pdev,
			     struct usb_endpoint_descriptor **ep_in,
			     struct usb_endpoint_descriptor **ep_out)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev)
		return -ENODEV;

	if (ep_in)
		*ep_in = fdev->ep_bulk_in;
	if (ep_out)
		*ep_out = fdev->ep_bulk_out;

	return 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_endpoints);

/*
 * The MPSSE stops executing commands while the RX FIFO of its channel is
 * full: a write asking for more response bytes than that stalls until the
 * host reads.
 */
static unsigned int ftdi_mpsse_fifo_size(enum ftdi_mpsse_chip_type chip)
{
	if (ftdi_chip_is_2232h(chip))
		return 4096;	/* DS_FT2232H: 4 KiB per interface */
	if (ftdi_chip_is_4232h(chip))
		return 2048;	/* DS_FT4232H: 2 KiB per channel */
	return 1024;		/* DS_FT232H: 1 KiB */
}

void ftdi_mpsse_pins_set(struct platform_device *pdev, unsigned int bank,
			 u8 mask, u8 val, u8 dir)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return;
	lockdep_assert_held(&fdev->bus_lock);
	fdev->pin_val[bank] = (fdev->pin_val[bank] & ~mask) | (val & mask);
	fdev->pin_dir[bank] = (fdev->pin_dir[bank] & ~mask) | (dir & mask);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_set);

void ftdi_mpsse_pins_get(struct platform_device *pdev, unsigned int bank,
			 u8 *val, u8 *dir)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1) {
		*val = 0;
		*dir = 0;
		return;
	}
	lockdep_assert_held(&fdev->bus_lock);
	*val = fdev->pin_val[bank];
	*dir = fdev->pin_dir[bank];
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_get);

int ftdi_mpsse_pins_claim(struct platform_device *pdev, unsigned int bank,
			  u8 mask)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return -ENODEV;
	lockdep_assert_held(&fdev->bus_lock);
	if (fdev->pin_gpio[bank] & mask)
		return -EBUSY;
	fdev->pin_claimed[bank] |= mask;
	return 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_claim);

void ftdi_mpsse_pins_unclaim(struct platform_device *pdev, unsigned int bank,
			     u8 mask)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return;
	lockdep_assert_held(&fdev->bus_lock);
	fdev->pin_claimed[bank] &= ~mask;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_unclaim);

u8 ftdi_mpsse_pins_claimed(struct platform_device *pdev, unsigned int bank)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return 0;
	lockdep_assert_held(&fdev->bus_lock);
	return fdev->pin_claimed[bank];
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_claimed);

int ftdi_mpsse_pins_gpio_request(struct platform_device *pdev,
				 unsigned int bank, u8 mask)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return -ENODEV;
	lockdep_assert_held(&fdev->bus_lock);
	if (fdev->pin_claimed[bank] & mask)
		return -EBUSY;
	fdev->pin_gpio[bank] |= mask;
	return 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_gpio_request);

void ftdi_mpsse_pins_gpio_free(struct platform_device *pdev,
			       unsigned int bank, u8 mask)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return;
	lockdep_assert_held(&fdev->bus_lock);
	fdev->pin_gpio[bank] &= ~mask;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_pins_gpio_free);

void ftdi_mpsse_od_set(struct platform_device *pdev, unsigned int bank,
		       u8 mask, u8 od)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || bank > 1)
		return;
	lockdep_assert_held(&fdev->bus_lock);
	fdev->pin_od[bank] = (fdev->pin_od[bank] & ~mask) | (od & mask);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_od_set);

void ftdi_mpsse_od_get(struct platform_device *pdev, u8 *od_lo, u8 *od_hi)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev) {
		*od_lo = 0;
		*od_hi = 0;
		return;
	}
	lockdep_assert_held(&fdev->bus_lock);
	*od_lo = fdev->pin_od[0];
	*od_hi = fdev->pin_od[1];
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_od_get);

/* The EEPROM can be trusted: read, programmed, checksum good */
static const struct ftdi_eeprom *ftdi_mpsse_usable_eeprom(struct ftdi_mpsse_dev *fdev)
{
	const struct ftdi_eeprom *ee = &fdev->eeprom;

	return ee->valid && !ee->empty && ee->checksum_ok ? ee : NULL;
}

const struct ftdi_eeprom *ftdi_mpsse_get_eeprom(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? ftdi_mpsse_usable_eeprom(fdev) : NULL;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_eeprom);

static int ftdi_mpsse_set_bitmode(struct ftdi_mpsse_dev *fdev, u16 mode)
{
	return ftdi_mpsse_vendor_out(fdev, FTDI_SIO_SET_BITMODE_REQUEST, mode,
				     FTDI_MPSSE_CTRL_TIMEOUT);
}

static int ftdi_mpsse_set_latency(struct ftdi_mpsse_dev *fdev, u8 latency_ms)
{
	return ftdi_mpsse_vendor_out(fdev, FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
				     latency_ms, FTDI_MPSSE_CTRL_TIMEOUT);
}

/*
 * MPSSE synchronisation (AN_113 §2.3.2, AN_135 §4.3.1, AN_255 §2.2.3): an
 * engine that is in step answers the invalid command 0xAA with 0xFA (bad
 * command) and 0xAA. Runs outside the transfer gate with its own URBs:
 * the caller (probe, resume, error recovery) excludes other transfers.
 */
static int ftdi_mpsse_sync(struct ftdi_mpsse_dev *fdev, int timeout)
{
	unsigned int pipe_out = usb_sndbulkpipe(fdev->udev,
					fdev->ep_bulk_out->bEndpointAddress);
	unsigned int pipe_in = usb_rcvbulkpipe(fdev->udev,
					fdev->ep_bulk_in->bEndpointAddress);
	unsigned int maxp = usb_endpoint_maxp(fdev->ep_bulk_in);
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout);
	int step = min(timeout, 1000);
	int actual = 0;
	int ret;

	fdev->tx_bounce[0] = 0xAA;
	fdev->tx_bounce[1] = MPSSE_SEND_IMMEDIATE;
	ret = usb_bulk_msg(fdev->udev, pipe_out, fdev->tx_bounce, 2, &actual,
			   step);
	if (ret) {
		dev_err_ratelimited(&fdev->intf->dev,
				    "MPSSE sync write failed: %d\n", ret);
		return ret;
	}

	/* Skip the status-only packets until the answer shows up */
	do {
		actual = 0;
		ret = usb_bulk_msg(fdev->udev, pipe_in, fdev->bulk_buf, maxp,
				   &actual, step);
		if (ret && ret != -ETIMEDOUT)
			return ret;
		if (!ret && actual > FTDI_MPSSE_STATUS_BYTES)
			break;
	} while (time_before(jiffies, deadline));

	if (actual == FTDI_MPSSE_STATUS_BYTES + 2 &&
	    fdev->bulk_buf[FTDI_MPSSE_STATUS_BYTES] == MPSSE_CMD_BAD &&
	    fdev->bulk_buf[FTDI_MPSSE_STATUS_BYTES + 1] == 0xAA) {
		dev_dbg(&fdev->intf->dev, "MPSSE sync OK\n");
		return 0;
	}

	dev_err_ratelimited(&fdev->intf->dev,
			    "MPSSE sync failed: expected FA AA, got %d bytes\n",
			    actual);
	return -EPROTO;
}

/*
 * Reset the channel into MPSSE mode and check that the engine answers. The
 * bitmode reset is the MPSSE reset (AN_135 §4.2): it stops the engine and
 * puts its setup (clock divisor, 3-phase, adaptive clocking, loopback) back
 * to the defaults, with every pin an input. Timeouts in ms.
 */
static int ftdi_mpsse_reset_mpsse(struct ftdi_mpsse_dev *fdev,
				  int ctrl_timeout, int sync_timeout)
{
	int ret;

	ret = ftdi_mpsse_vendor_out(fdev, FTDI_SIO_SET_BITMODE_REQUEST,
				    FTDI_SIO_BITMODE_RESET, ctrl_timeout);
	if (ret < 0)
		return ret;

	ret = ftdi_mpsse_vendor_out(fdev, FTDI_SIO_SET_BITMODE_REQUEST,
				    FTDI_SIO_BITMODE_MPSSE << 8, ctrl_timeout);
	if (ret < 0)
		return ret;

	ret = ftdi_mpsse_vendor_out(fdev, FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
				    clamp_val(mpsse_latency, 1, 255),
				    ctrl_timeout);
	if (ret < 0)
		return ret;

	/* Stale data in FIFOs would corrupt the first MPSSE response */
	ret = ftdi_mpsse_purge(fdev, ctrl_timeout);
	if (ret < 0)
		dev_warn_ratelimited(&fdev->intf->dev, "purge failed: %d\n",
				     ret);

	return ftdi_mpsse_sync(fdev, sync_timeout);
}

/* Probe and resume: no transfer is waiting, use the regular timeouts */
static int ftdi_mpsse_enter_mpsse(struct ftdi_mpsse_dev *fdev)
{
	int ret;

	ret = ftdi_mpsse_reset_mpsse(fdev, FTDI_MPSSE_CTRL_TIMEOUT,
				     FTDI_MPSSE_SYNC_TIMEOUT);
	if (!ret)
		fdev->need_resync = false;
	return ret;
}

/*
 * Drive the pins from the shadow after an MPSSE reset (all inputs). The
 * open-drain mask goes first, so that an open-drain pin is never driven
 * high (AN_108 §7.1). Called with bus_lock and io_lock held.
 */
static int ftdi_mpsse_replay_pins(struct ftdi_mpsse_dev *fdev)
{
	u8 *c = fdev->tx_bounce;
	unsigned int n = 0;

	if (fdev->chip_type == FTDI_CHIP_FT232H) {
		c[n++] = MPSSE_DRIVE_ZERO_ONLY;
		c[n++] = fdev->pin_od[0];
		c[n++] = fdev->pin_od[1];
	}
	c[n++] = MPSSE_SET_BITS_LOW;
	c[n++] = fdev->pin_val[0];
	c[n++] = fdev->pin_dir[0];
	if (!ftdi_chip_is_4232h(fdev->chip_type)) {
		c[n++] = MPSSE_SET_BITS_HIGH;
		c[n++] = fdev->pin_val[1];
		c[n++] = fdev->pin_dir[1];
	}
	return ftdi_mpsse_bulk_write_chunk(fdev, n, 0);
}

/*
 * Bring the MPSSE back in step after a failed transfer. A purge is not
 * enough: a command still running (adaptive clocking, slow clock) answers
 * later and every following response is the previous one; a command cut
 * short has the next commands clocked out as its payload (AN_108 §3.1).
 * Only the bitmode reset stops the engine (AN_135 §4.2).
 * Called with bus_lock and io_lock held.
 */
static int ftdi_mpsse_resync(struct ftdi_mpsse_dev *fdev)
{
	int ret;

	lockdep_assert_held(&fdev->bus_lock);

	if (READ_ONCE(fdev->disconnect) || READ_ONCE(fdev->suspended))
		return -ESHUTDOWN;

	ret = ftdi_mpsse_clear_halts(fdev);
	if (!ret)
		ret = ftdi_mpsse_reset_mpsse(fdev, FTDI_MPSSE_RECOVERY_TIMEOUT,
					     FTDI_MPSSE_RECOVERY_TIMEOUT);
	if (!ret)
		ret = ftdi_mpsse_replay_pins(fdev);
	if (ret) {
		/* A STALL here too: cleared at the next attempt */
		if (ret == -EPIPE)
			fdev->ep_halted = true;
		dev_err_ratelimited(&fdev->intf->dev,
				    "MPSSE resynchronisation failed: %d\n",
				    ret);
		return ret;
	}

	fdev->need_resync = false;
	return 0;
}

static int ftdi_mpsse_enter_uart(struct ftdi_mpsse_dev *fdev)
{
	int ret;

	/* Bitmode reset restores native UART operation */
	ret = ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);
	if (ret < 0)
		return ret;

	/* 16 ms balances throughput and responsiveness for serial traffic */
	return ftdi_mpsse_set_latency(fdev, 16);
}

/*
 * Mode of a channel, most explicit source first: the bus_mode parameter,
 * the "bus_mode=" tag of a valid EEPROM, UART on a channel without MPSSE,
 * the other hints of a valid EEPROM, SPI. Returns the mode (*auto_mode set
 * unless the parameter chose it), or a negative errno when the EEPROM wires
 * the channel for something else.
 */
static int ftdi_mpsse_select_mode(struct ftdi_mpsse_dev *fdev,
				  unsigned int ifnum, bool *auto_mode)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_usable_eeprom(fdev);
	struct device *dev = &fdev->intf->dev;
	enum ftdi_mpsse_mode mode = FTDI_MODE_AUTO;
	const char *why = NULL;
	char name[16];
	const char *tok;
	u8 chan_type;

	*auto_mode = false;

	ftdi_mpsse_mode_token(bus_mode, ifnum, name, sizeof(name));
	tok = strim(name);
	if (tok[0]) {
		mode = ftdi_mpsse_mode_from_name(tok);
		if (mode == FTDI_MODE_AUTO)
			dev_warn(dev, "unknown bus_mode '%s', defaulting to auto\n",
				 tok);
	}

	/* Never act on what a corrupt image says */
	if (mode == FTDI_MODE_AUTO) {
		*auto_mode = true;
		name[0] = '\0';
		if (ee) {
			ftdi_mpsse_mode_token(ee->bus_mode, ifnum, name,
					      sizeof(name));
			if (ee->bus_mode_tag && !ee->bus_mode[0])
				dev_warn(dev, "EEPROM: bus_mode tag without a value (lower-case names expected), ignored\n");
		}
		if (name[0]) {
			mode = ftdi_mpsse_mode_from_name(name);
			if (mode == FTDI_MODE_AUTO)
				dev_warn(dev, "EEPROM: unknown bus_mode '%s' in the user area, ignored\n",
					 name);
			else
				why = "bus_mode tag";
		}
	}

	if (!fdev->mpsse_capable) {
		/* FT4232H C/D: UART or bit-bang, whatever was asked */
		if (mode == FTDI_MODE_SPI || mode == FTDI_MODE_I2C) {
			dev_warn(dev, "bus_mode '%s' not supported on non-MPSSE channel %c (uart or bitbang), forcing UART\n",
				 name, 'A' + ifnum);
			mode = FTDI_MODE_UART;
		} else if (mode == FTDI_MODE_AUTO) {
			mode = FTDI_MODE_UART;
		}
		return mode;
	}

	if (mode != FTDI_MODE_AUTO) {
		if (why)
			dev_info(dev, "auto: EEPROM hints %s mode (%s)\n",
				 ftdi_mpsse_mode_label(mode), why);
		return mode;
	}

	if (!ee) {
		dev_info(dev, "auto: no usable EEPROM, defaulting to SPI\n");
		return FTDI_MODE_SPI;
	}

	/* The channel is wired as a FIFO or a CPU bus, not for serial/MPSSE */
	chan_type = ftdi_eeprom_channel_type(ee, fdev->channel);
	switch (chan_type) {
	case FTDI_EE_CHAN_FIFO:
	case FTDI_EE_CHAN_OPTO:
	case FTDI_EE_CHAN_CPU:
	case FTDI_EE_CHAN_FT1284:
		dev_warn(dev, "EEPROM channel type 0x%02x not supported\n",
			 chan_type);
		return -ENODEV;
	}

	mode = ftdi_mpsse_detect_protocol(ee, fdev->chip_type, fdev->channel,
					  &why);
	if (why)
		dev_info(dev, "auto: EEPROM hints %s mode (%s)\n",
			 ftdi_mpsse_mode_label(mode), why);
	else
		dev_info(dev, "auto: no EEPROM hint, defaulting to SPI\n");

	return mode;
}

/*
 * The children of a channel, by mode, with their platform data: the facts
 * of the channel and the pins of its bus. mfd copies the platform data.
 *
 * UART: the channel runs in bit mode 0 (no MPSSE), so no MPSSE GPIO child
 * can share it, its opcodes would go out on TXD; ftdi-uart provides the
 * FT232H CBUS GPIOs itself. Async bit-bang: the 8 xDBUS pins are GPIOs and
 * the UART of the channel is gone ("UART or bit-bang", DS_FT4232H), so
 * that child is alone too. SPI and I2C reserve AD0-AD2 (and the chip
 * selects) and leave the other pins to ftdi-gpio.
 */
static int ftdi_mpsse_add_children(struct ftdi_mpsse_dev *fdev,
				   enum ftdi_mpsse_mode mode)
{
	struct ftdi_mpsse_pdata pd[2] = { };
	struct mfd_cell cells[2] = { };
	const char *bus = NULL;
	int n, i;
	u16 mask = 0;

	switch (mode) {
	case FTDI_MODE_UART:
		cells[0].name = "ftdi-uart";
		n = 1;
		break;
	case FTDI_MODE_BITBANG:
		cells[0].name = "ftdi-gpio-bitbang";
		pd[0].gpio_dir_in = 0x00ff;
		pd[0].gpio_dir_out = 0x00ff;
		n = 1;
		break;
	case FTDI_MODE_I2C:
		bus = "ftdi-i2c";
		mask = BIT(0) | BIT(1) | BIT(2);
		break;
	case FTDI_MODE_SPI:
		bus = "ftdi-spi";
		mask = BIT(0) | BIT(1) | BIT(2);
		for (i = 0; i < fdev->spi_num_cs; i++)
			mask |= BIT(fdev->spi_cs_pins[i]);
		pd[0].spi_num_cs = fdev->spi_num_cs;
		memcpy(pd[0].spi_cs_pins, fdev->spi_cs_pins, fdev->spi_num_cs);
		break;
	default:
		return -EINVAL;
	}

	if (bus) {
		cells[0].name = bus;
		cells[1].name = "ftdi-gpio";
		pd[1].gpio_dir_in = (u16)~mask;
		pd[1].gpio_dir_out = (u16)~mask;
		n = 2;
	}
	fdev->gpio_reserved_mask = mask;

	for (i = 0; i < n; i++) {
		pd[i].chip_type = fdev->chip_type;
		pd[i].channel = fdev->channel;
		pd[i].has_gpioh = !ftdi_chip_is_4232h(fdev->chip_type);
		pd[i].has_drive_zero = fdev->chip_type == FTDI_CHIP_FT232H;
		pd[i].fifo_size = ftdi_mpsse_fifo_size(fdev->chip_type);
		pd[i].txden_pin =
			ftdi_eeprom_txden_pin(ftdi_mpsse_usable_eeprom(fdev),
					      fdev->chip_type, fdev->channel);
		pd[i].gpio_reserved_mask = mask;
		cells[i].platform_data = &pd[i];
		cells[i].pdata_size = sizeof(pd[i]);
	}

	return mfd_add_hotplug_devices(&fdev->intf->dev, cells, n);
}

static const char *ftdi_chip_name(enum ftdi_mpsse_chip_type chip)
{
	switch (chip) {
	case FTDI_CHIP_FT232H:		return "FT232H";
	case FTDI_CHIP_FT2232H:	return "FT2232H";
	case FTDI_CHIP_FT4232H:	return "FT4232H";
	case FTDI_CHIP_FT4232HA:	return "FT4232HA";
	case FTDI_CHIP_FT232HP:	return "FT232HP";
	case FTDI_CHIP_FT233HP:	return "FT233HP";
	case FTDI_CHIP_FT2232HP:	return "FT2232HP";
	case FTDI_CHIP_FT2233HP:	return "FT2233HP";
	case FTDI_CHIP_FT4232HP:	return "FT4232HP";
	case FTDI_CHIP_FT4233HP:	return "FT4233HP";
	default:			return "unknown";
	}
}

/*
 * Devices the children registered: spiN, i2c-N and gpiochipN hang under the
 * platform devices, ttyFTDIn under the serial_base devices of the port.
 */
#define PINMAP_DEPTH	4

struct pinmap_info {
	char spi[16];
	char i2c[16];
	char uart[32];
	char gpio[16];
	unsigned int depth;
};

/*
 * The controllers and gpiochips of the channel are children of its cells
 * (depth 1 here): deeper ones belong to clients of its buses. The tty is
 * below the serial_base devices of ftdi-uart.
 */
static int pinmap_match_child(struct device *dev, void *data)
{
	struct pinmap_info *info = data;
	const char *name = dev_name(dev);

	if (!strncmp(name, "ttyFTDI", 7)) {
		if (!info->uart[0])
			strscpy(info->uart, name, sizeof(info->uart));
	} else if (info->depth == 1) {
		if (!strncmp(name, "spi", 3) && !info->spi[0])
			strscpy(info->spi, name, sizeof(info->spi));
		else if (!strncmp(name, "i2c-", 4) && !info->i2c[0])
			strscpy(info->i2c, name, sizeof(info->i2c));
		else if (!strncmp(name, "gpiochip", 8) && !info->gpio[0])
			strscpy(info->gpio, name, sizeof(info->gpio));
	}

	if (++info->depth < PINMAP_DEPTH)
		device_for_each_child(dev, info, pinmap_match_child);
	info->depth--;
	return 0;
}

static void pinmap_discover(struct device *intf_dev, struct pinmap_info *info)
{
	memset(info, 0, sizeof(*info));
	device_for_each_child(intf_dev, info, pinmap_match_child);
}

/*
 * Pin names of the datasheets: xDBUS/xCBUS of channel x (DS_FT2232H,
 * DS_FT4232H), shortened to xD/xC on MPSSE channels.
 */
static int ftdi_mpsse_pinmap_show(struct seq_file *s, void *unused)
{
	struct ftdi_mpsse_dev *fdev = s->private;
	char ch = 'A' + fdev->channel - FTDI_CHANNEL_A;
	const char *d = fdev->mpsse_capable ? "D" : "DBUS";
	struct pinmap_info info;
	const char *txden;
	u16 claimed;
	int i;

	pinmap_discover(&fdev->intf->dev, &info);
	/* A snapshot is enough: this is a report */
	claimed = READ_ONCE(fdev->pin_claimed[0]) |
		  READ_ONCE(fdev->pin_claimed[1]) << 8;

	seq_printf(s, "chip: %s\n", ftdi_chip_name(fdev->chip_type));
	seq_printf(s, "channel: %c\n", ch);
	seq_printf(s, "mode: %s%s\n", fdev->mode_name,
		   fdev->mpsse_capable ? "" : " (non-MPSSE)");

	if (strcmp(fdev->mode_name, "spi") == 0) {
		seq_printf(s, "\nspi: %s\n", info.spi[0] ? info.spi : "-");
		seq_printf(s, "  %cD0  SCK\n", ch);
		seq_printf(s, "  %cD1  MOSI\n", ch);
		seq_printf(s, "  %cD2  MISO\n", ch);
		for (i = 0; i < fdev->spi_num_cs; i++)
			seq_printf(s, "  %cD%u  CS%d\n", ch,
				   fdev->spi_cs_pins[i], i);
	}

	if (strcmp(fdev->mode_name, "i2c") == 0) {
		seq_printf(s, "\ni2c: %s\n", info.i2c[0] ? info.i2c : "-");
		seq_printf(s, "  %cD0  SCL\n", ch);
		seq_printf(s, "  %cD1  SDA_OUT\n", ch);
		seq_printf(s, "  %cD2  SDA_IN\n", ch);
		/* Taken by ftdi-i2c once adaptive clocking is on */
		if (claimed & BIT(7))
			seq_printf(s, "  %cD7  SCL_SENSE (clock stretching)\n",
				   ch);
	}

	if (strcmp(fdev->mode_name, "uart") == 0) {
		static const char * const sig[] = {
			"TXD", "RXD", "RTS", "CTS", "DTR", "DSR", "DCD",
		};

		seq_printf(s, "\nuart: %s\n", info.uart[0] ? info.uart : "-");
		for (i = 0; i < ARRAY_SIZE(sig); i++)
			seq_printf(s, "  %c%s%d  %s\n", ch, d, i, sig[i]);

		/* RS-485 transmit enable: on pin 7 instead of RI, or apart */
		txden = ftdi_eeprom_txden_pin(ftdi_mpsse_usable_eeprom(fdev),
					      fdev->chip_type, fdev->channel);
		if (txden && ftdi_chip_is_4232h(fdev->chip_type)) {
			seq_printf(s, "  %c%s7  TXDEN\n", ch, d);
		} else {
			seq_printf(s, "  %c%s7  RI\n", ch, d);
			if (txden)
				seq_printf(s, "  %s  TXDEN\n", txden);
		}

		/* FT232H: the CBUS chip of ftdi-uart, ACBUS5/6/8/9 */
		if (info.gpio[0])
			seq_printf(s, "\ngpio: %s (CBUS)\n", info.gpio);
		else
			seq_puts(s, "\ngpio: - (UART mode)\n");
		return 0;
	}

	seq_printf(s, "\ngpio%s: %s\n", fdev->mpsse_mode ? "" : "-bitbang",
		   info.gpio[0] ? info.gpio : "-");

	if (!fdev->mpsse_mode) {
		/* Bit-bang: no bus, every pin is a line */
		for (i = 0; i < 8; i++)
			seq_printf(s, "  %c%s%d  available\n", ch, d, i);
	} else {
		int npins = ftdi_chip_is_4232h(fdev->chip_type) ? 8 : 16;
		const char *bus_owner = NULL;

		if (strcmp(fdev->mode_name, "spi") == 0 && info.spi[0])
			bus_owner = info.spi;
		else if (strcmp(fdev->mode_name, "i2c") == 0 && info.i2c[0])
			bus_owner = info.i2c;

		for (i = 0; i < npins; i++) {
			char bus = i < 8 ? 'D' : 'C';

			if (claimed & BIT(i))
				seq_printf(s, "  %c%c%d  in use (%s)\n", ch, bus,
					   i % 8, bus_owner ?: "bus driver");
			else if (!(fdev->gpio_reserved_mask & BIT(i)))
				seq_printf(s, "  %c%c%d  available\n", ch, bus,
					   i % 8);
			else if (bus_owner)
				seq_printf(s, "  %c%c%d  reserved (%s)\n", ch,
					   bus, i % 8, bus_owner);
			else
				seq_printf(s, "  %c%c%d  reserved\n", ch, bus,
					   i % 8);
		}
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ftdi_mpsse_pinmap);

static void ftdi_mpsse_debugfs_init(struct ftdi_mpsse_dev *fdev)
{
	fdev->debugfs = debugfs_create_dir(dev_name(&fdev->intf->dev),
					   ftdi_mpsse_debugfs_root);
	debugfs_create_file("pinmap", 0444, fdev->debugfs, fdev,
			    &ftdi_mpsse_pinmap_fops);
	ftdi_eeprom_debugfs_init(fdev, fdev->debugfs);
}

static int ftdi_mpsse_probe(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct ftdi_mpsse_dev *fdev;
	enum ftdi_mpsse_chip_type chip;
	enum ftdi_mpsse_mode mode;
	unsigned int ifnum;
	bool auto_mode;
	u8 channel;
	int ret;

	chip = ftdi_mpsse_detect_chip(udev);
	if (chip == FTDI_CHIP_UNKNOWN) {
		dev_err(&intf->dev, "unsupported FTDI chip (bcdDevice=0x%04x)\n",
			le16_to_cpu(udev->descriptor.bcdDevice));
		return -ENODEV;
	}

	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	channel = FTDI_CHANNEL_A + ifnum;

	fdev = kzalloc_obj(*fdev);
	if (!fdev)
		return -ENOMEM;

	fdev->udev = usb_get_dev(udev);
	fdev->intf = intf;
	fdev->channel = channel;
	fdev->chip_type = chip;
	fdev->mpsse_capable = ftdi_mpsse_capable(chip, channel);
	mutex_init(&fdev->bus_lock);
	mutex_init(&fdev->io_lock);
	spin_lock_init(&fdev->disconnect_lock);
	init_waitqueue_head(&fdev->disconnect_wq);

	ret = usb_find_common_endpoints(intf->cur_altsetting,
					&fdev->ep_bulk_in,
					&fdev->ep_bulk_out,
					NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "failed to find bulk endpoints\n");
		goto err_put;
	}

	fdev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!fdev->bulk_in_urb) {
		ret = -ENOMEM;
		goto err_put;
	}

	fdev->bulk_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!fdev->bulk_out_urb) {
		ret = -ENOMEM;
		goto err_free_in_urb;
	}

	init_completion(&fdev->io_done);

	fdev->bulk_buf = kmalloc(FTDI_MPSSE_BUF_SIZE, GFP_KERNEL);
	fdev->tx_bounce = kmalloc(FTDI_MPSSE_TX_BOUNCE, GFP_KERNEL);
	if (!fdev->bulk_buf || !fdev->tx_bounce) {
		ret = -ENOMEM;
		goto err_buf;
	}

	/* Not fatal if it fails */
	ftdi_eeprom_read(fdev);

	ret = ftdi_mpsse_select_mode(fdev, ifnum, &auto_mode);
	if (ret < 0)
		goto err_buf;
	mode = ret;

	/* Refuse a bad spi_cs before touching the chip */
	if (mode == FTDI_MODE_SPI) {
		ret = ftdi_mpsse_parse_spi_cs(fdev->channel, fdev->spi_cs_pins,
					      FTDI_SPI_CS_MAX);
		if (ret < 0) {
			dev_err(&intf->dev, "invalid spi_cs parameter '%s'\n",
				spi_cs);
			goto err_buf;
		}
		fdev->spi_num_cs = ret;
	}

	strscpy(fdev->mode_name, ftdi_mpsse_mode_names[mode],
		sizeof(fdev->mode_name));
	fdev->mpsse_mode = mode == FTDI_MODE_SPI || mode == FTDI_MODE_I2C;

	/* Bit-bang starts from UART mode: its child enters the bit mode */
	if (fdev->mpsse_mode)
		ret = ftdi_mpsse_enter_mpsse(fdev);
	else
		ret = ftdi_mpsse_enter_uart(fdev);
	if (ret < 0) {
		dev_err(&intf->dev, "failed to set channel mode\n");
		goto err_reset;
	}

	usb_set_intfdata(intf, fdev);

	ret = ftdi_mpsse_add_children(fdev, mode);
	if (ret) {
		dev_err(&intf->dev, "failed to add child devices\n");
		goto err_reset;
	}

	ftdi_mpsse_debugfs_init(fdev);

	dev_info(&intf->dev, "FTDI MPSSE core: channel %c, mode '%s'%s\n",
		 'A' + ifnum, fdev->mode_name, auto_mode ? " (auto)" : "");

	return 0;

err_reset:
	ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);
	usb_set_intfdata(intf, NULL);
err_buf:
	kfree(fdev->tx_bounce);
	kfree(fdev->bulk_buf);
	usb_free_urb(fdev->bulk_out_urb);
err_free_in_urb:
	usb_free_urb(fdev->bulk_in_urb);
err_put:
	usb_put_dev(fdev->udev);
	kfree(fdev);
	return ret;
}

static void ftdi_mpsse_disconnect(struct usb_interface *intf)
{
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);

	if (!fdev)
		return;

	/* Its readers walk the children and read fdev: gone first */
	debugfs_remove(fdev->debugfs);

	/* Refuse new transfers first, then drain the in-flight ones */
	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->disconnect = true;

	/*
	 * Poison, not kill: a transfer that already passed the disconnect
	 * check gets -EPERM from usb_submit_urb() instead of a new URB.
	 */
	usb_poison_urb(fdev->bulk_in_urb);
	usb_poison_urb(fdev->bulk_out_urb);

	if (!wait_event_timeout(fdev->disconnect_wq, !fdev->active_transfers,
				msecs_to_jiffies(10000)))
		dev_warn(&intf->dev, "timed out waiting for %d active transfer(s)\n",
			 fdev->active_transfers);

	mfd_remove_devices(&intf->dev);

	/*
	 * Leave the channel in its default (UART) mode for whichever driver
	 * binds next. ep0 is not affected by the poisoned bulk URBs; this
	 * simply fails if the device was unplugged.
	 */
	ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);

	usb_set_intfdata(intf, NULL);
	kfree(fdev->tx_bounce);
	kfree(fdev->bulk_buf);
	usb_free_urb(fdev->bulk_out_urb);
	usb_free_urb(fdev->bulk_in_urb);
	usb_put_dev(fdev->udev);
	kfree(fdev);
}

static int ftdi_mpsse_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);

	if (!fdev)
		return 0;

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->suspended = true;

	/*
	 * Poisoned, so that a transfer that passed the check before the flag
	 * was set cannot submit a new URB either; resume unpoisons them.
	 */
	usb_poison_urb(fdev->bulk_in_urb);
	usb_poison_urb(fdev->bulk_out_urb);

	/*
	 * usbcore ignores an error during a system sleep: a transfer that does
	 * not end is left to fail against the suspended device, the gate and
	 * the poison stay as resume expects them.
	 */
	if (!wait_event_timeout(fdev->disconnect_wq, !fdev->active_transfers,
				msecs_to_jiffies(5000)))
		dev_warn(&intf->dev,
			 "suspend: timed out waiting for %d transfer(s)\n",
			 READ_ONCE(fdev->active_transfers));

	return 0;
}

static int ftdi_mpsse_resume(struct usb_interface *intf)
{
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);
	int ret;

	if (!fdev)
		return 0;

	usb_unpoison_urb(fdev->bulk_in_urb);
	usb_unpoison_urb(fdev->bulk_out_urb);

	/* FTDI chip loses bitmode configuration across USB suspend */
	if (fdev->mpsse_mode) {
		/*
		 * Transfers are still refused (suspended), but the pin
		 * shadow belongs to bus_lock and the buffers to io_lock.
		 */
		mutex_lock(&fdev->bus_lock);
		mutex_lock(&fdev->io_lock);
		ret = ftdi_mpsse_enter_mpsse(fdev);
		if (!ret)
			ret = ftdi_mpsse_replay_pins(fdev);
		/* The engine is not in step: the next transfer resets it */
		if (ret)
			fdev->need_resync = true;
		if (ret == -EPIPE)
			fdev->ep_halted = true;
		mutex_unlock(&fdev->io_lock);
		mutex_unlock(&fdev->bus_lock);
	} else {
		ret = ftdi_mpsse_enter_uart(fdev);
	}

	if (ret)
		dev_err(&intf->dev, "resume: mode re-init failed: %d\n", ret);

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->suspended = false;
	wake_up(&fdev->disconnect_wq);

	return ret;
}

/*
 * Empty, so as to coexist with ftdi_sio. Devices are bound at run time
 * (the USB bus has no driver_override):
 *   echo X-Y:Z > /sys/bus/usb/drivers/ftdi_sio/unbind
 *   echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id
 */
static const struct usb_device_id ftdi_mpsse_id_table[] = {
	{ }
};
MODULE_DEVICE_TABLE(usb, ftdi_mpsse_id_table);

static const struct attribute_group *ftdi_mpsse_groups[] = {
	&ftdi_eeprom_attr_group,
	NULL,
};

/*
 * No .pre_reset/.post_reset: the children are not told about a device
 * reset (the UART runs its own URBs), so usbcore unbinds and binds the
 * interface again around it. After a reset-resume the chip has lost its
 * state as after any suspend: resume rebuilds it.
 */
static struct usb_driver ftdi_mpsse_driver = {
	.name		= DRIVER_NAME,
	.probe		= ftdi_mpsse_probe,
	.disconnect	= ftdi_mpsse_disconnect,
	.suspend	= ftdi_mpsse_suspend,
	.resume		= ftdi_mpsse_resume,
	.reset_resume	= ftdi_mpsse_resume,
	.id_table	= ftdi_mpsse_id_table,
	/*
	 * ->disconnect drains its own URBs and the children kill theirs:
	 * usbcore need not flush the endpoints first. That flush spins with
	 * IRQs off until every URB is unlinked, which livelocks a UP kernel
	 * on an URB that the vhci_hcd rx thread is giving back.
	 */
	.soft_unbind	= 1,
	.dev_groups	= ftdi_mpsse_groups,
};

static int __init ftdi_mpsse_init(void)
{
	int ret;

	ftdi_mpsse_debugfs_root = debugfs_create_dir(DRIVER_NAME, NULL);
	ret = usb_register(&ftdi_mpsse_driver);
	if (ret)
		debugfs_remove(ftdi_mpsse_debugfs_root);
	return ret;
}
module_init(ftdi_mpsse_init);

static void __exit ftdi_mpsse_exit(void)
{
	usb_deregister(&ftdi_mpsse_driver);
	debugfs_remove(ftdi_mpsse_debugfs_root);
}
module_exit(ftdi_mpsse_exit);

MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
