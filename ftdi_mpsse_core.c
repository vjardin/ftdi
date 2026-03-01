// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE USB interface driver
 *
 * USB interface driver that manages FTDI Hi-Speed devices (FT232H, FT2232H,
 * FT4232H and variants) and creates child platform devices for UART, SPI,
 * I2C, and GPIO subsystems.
 *
 * The MPSSE (Multi-Protocol Synchronous Serial Engine) built into these
 * chips is the hardware basis for SPI, I2C, and GPIO.  UART mode uses the
 * chip's native async serial path.
 *
 * Architecture follows the viperboard / dln2 pattern:
 *   - This module owns the USB interface and bulk endpoints
 *   - Children communicate via exported ftdi_mpsse_*() transport functions
 *   - Children are created via mfd_add_hotplug_devices() with platform_data
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include <linux/mfd/core.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi_mpsse"
#define DRIVER_DESC	"FTDI MPSSE USB interface driver"

#define FTDI_STATUS_RETRY_MAX	50

/*
 * Per-interface device state.  Children reach this via
 * dev_get_drvdata(pdev->dev.parent).
 */
struct ftdi_mpsse_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	u8 channel;			/* 1-based: FTDI_CHANNEL_A..D */
	enum ftdi_mpsse_chip_type chip_type;
	struct usb_endpoint_descriptor *ep_bulk_in;
	struct usb_endpoint_descriptor *ep_bulk_out;

	/*
	 * I/O serialisation.  bus_lock ensures one complete MPSSE transaction
	 * at a time across sibling drivers (SPI, I2C, GPIO).  io_lock
	 * serialises individual USB bulk transfers within a transaction.
	 *
	 * Lock ordering: bus_lock -> fg->lock -> io_lock -> disconnect_lock.
	 */
	struct mutex bus_lock;		/* whole-transaction exclusion */
	struct mutex io_lock;		/* single USB transfer exclusion */
	struct urb *bulk_in_urb;
	struct urb *bulk_out_urb;
	struct completion io_done;
	int io_status;
	int io_actual;
	u8 *bulk_buf;			/* DMA-safe read buffer */

	/* Disconnect / suspend synchronisation (DLN2 pattern) */
	spinlock_t disconnect_lock;
	bool disconnect;
	bool suspended;
	bool mpsse_mode;		/* true if entered MPSSE, false for UART */
	bool mpsse_capable;		/* false for FT4232H channels C/D */
	int active_transfers;
	wait_queue_head_t disconnect_wq;

	/* Diagnostic info stored at probe for sysfs pinmap */
	char mode_name[16];
	u16 gpio_reserved_mask;
	u8 spi_cs_pins[FTDI_SPI_CS_MAX];
	u8 spi_num_cs;

	struct ftdi_eeprom eeprom;
};

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

/* Module parameter: channel operating mode */
static char *bus_mode = "";
module_param(bus_mode, charp, 0444);
MODULE_PARM_DESC(bus_mode,
		 "Channel mode: 'uart', 'spi', 'i2c', or '' for auto (default). Comma-separated for per-interface: 'spi,uart' sets interface 0=spi, interface 1=uart. Single value applies to all.");

/* Module parameter: SPI chip-select pin assignment */
static char *spi_cs = "3";
module_param(spi_cs, charp, 0444);
MODULE_PARM_DESC(spi_cs,
		 "SPI chip-select AD pin numbers 3-7 (default '3')."
		 " Global: '3,4,5' applies to all SPI channels."
		 " Per-channel: 'A:3,4,5;B:3' for different configs."
		 " Remaining ADBUS pins available as GPIO.");

/* Module parameter: MPSSE latency timer (ms) */
static unsigned int mpsse_latency = 1;
module_param(mpsse_latency, uint, 0644);
MODULE_PARM_DESC(mpsse_latency,
		 "MPSSE mode USB latency timer in ms (1-255, default 1)."
		 " Higher values reduce USB overhead but increase I/O latency."
		 " Legacy compat: mpsse_latency=40");

/* Module parameter: USB autosuspend control */
static bool autosuspend;
module_param(autosuspend, bool, 0644);
MODULE_PARM_DESC(autosuspend,
		 "Enable USB autosuspend (default 0 = disabled). When enabled,"
		 " the FTDI chip loses all configuration (bitmode, clock, GPIO state)"
		 " during USB suspend. On resume, the driver restores: SPI clock/CS,"
		 " I2C 3-phase clocking and open-drain settings, UART baud/format,"
		 " and GPIO direction/values. During suspend, all bus lines are"
		 " tristated (SPI CS# float, I2C/GPIO via pull-ups, UART TX idle).");

/*
 * Extract the mode string for a given interface number from the
 * comma-separated bus_mode parameter.  Copies the ifnum-th token into
 * @out.  If bus_mode has no commas (single value), returns the entire
 * string for all interfaces.  If ifnum exceeds available tokens, returns
 * an empty string (auto).
 */
static void ftdi_mpsse_get_mode(unsigned int ifnum, char *out, size_t out_len)
{
	const char *p = bus_mode;
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

	/* ifnum exceeds available tokens -> auto */
	out[0] = '\0';
}

/*
 * Parse comma-separated CS pin numbers from a string segment.
 * Stops at ';', end of string, or when max pins are parsed.
 * Each pin must be in the range 3-7 (AD3-AD7).
 * Returns the number of CS pins parsed, or -EINVAL on error.
 */
static int ftdi_mpsse_parse_cs_pins(const char *p, u8 *pins, unsigned int max)
{
	unsigned int count = 0;

	while (*p && *p != ';' && count < max) {
		const char *end;
		unsigned long val;
		char tmp[4];
		int len;

		while (*p == ' ')
			p++;
		if (!*p || *p == ';')
			break;

		end = strpbrk(p, ",;");
		len = end ? (int)(end - p) : (int)strlen(p);

		if (len <= 0 || len >= (int)sizeof(tmp))
			return -EINVAL;
		memcpy(tmp, p, len);
		tmp[len] = '\0';

		if (kstrtoul(tmp, 10, &val))
			return -EINVAL;
		if (val < 3 || val > 7)
			return -EINVAL;

		pins[count++] = (u8)val;

		if (!end || *end == ';')
			break;
		p = end + 1;
	}

	return count ? (int)count : -EINVAL;
}

/*
 * Parse the spi_cs module parameter for a specific channel.
 *
 * Supports two syntaxes:
 *   "3,4,5"         -- global: same CS config for all SPI channels
 *   "A:3,4,5;B:3"   -- per-channel: different CS config per channel
 *
 * When per-channel syntax is used but no entry exists for the requested
 * channel, falls back to a single CS0=AD3.
 */
static int ftdi_mpsse_parse_spi_cs(u8 channel, u8 *pins, unsigned int max)
{
	const char *p = spi_cs;
	char ch_upper, ch_lower;

	if (!p || !*p)
		return -EINVAL;

	/* If no ':' present, it's global syntax -- parse directly */
	if (!strchr(p, ':'))
		return ftdi_mpsse_parse_cs_pins(p, pins, max);

	/* Per-channel syntax: find "X:..." section for this channel */
	ch_upper = 'A' + channel - FTDI_CHANNEL_A;
	ch_lower = 'a' + channel - FTDI_CHANNEL_A;

	while (p && *p) {
		while (*p == ' ' || *p == ';')
			p++;
		if (!*p)
			break;

		if ((*p == ch_upper || *p == ch_lower) && *(p + 1) == ':')
			return ftdi_mpsse_parse_cs_pins(p + 2, pins, max);

		/* Skip to next section */
		p = strchr(p, ';');
		if (p)
			p++;
	}

	/* No config for this channel -- default to single CS0=AD3 */
	pins[0] = 3;
	return 1;
}

/*
 * Case-insensitive substring search (strcasestr is not available in kernel).
 */
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
 * Detect protocol mode from EEPROM contents.
 *
 * Priority order:
 *   1. User area protocol hint (0x1A byte)
 *   2. VCP bit set -> UART mode (can't use MPSSE)
 *   3. Product string contains "SPI" or "I2C"
 *   4. Electrical characteristics (slow slew + Schmitt = I2C hint)
 *   5. Default to SPI (most common MPSSE use case)
 *
 * Returns: "uart", "spi", "i2c", or NULL for no hint
 */
static const char *ftdi_mpsse_detect_protocol(const struct ftdi_eeprom *ee,
					      u8 channel)
{
	bool vcp_enabled;

	if (!ee->valid || ee->empty)
		return NULL;

	/* 1. User area protocol hint takes priority */
	switch (ee->protocol_hint) {
	case FTDI_EE_PROTO_SPI:
		return "spi";
	case FTDI_EE_PROTO_I2C:
		return "i2c";
	case FTDI_EE_PROTO_UART:
		return "uart";
	}

	/* 2. VCP bit forces UART mode (device appears as COM port) */
	vcp_enabled = (channel == FTDI_CHANNEL_B) ? ee->chb_vcp : ee->cha_vcp;
	if (vcp_enabled)
		return "uart";

	/* 3. Product string keywords */
	if (str_contains_ci(ee->product, "SPI"))
		return "spi";
	if (str_contains_ci(ee->product, "I2C"))
		return "i2c";

	/* 4. Electrical characteristics heuristic */
	if (ee->group0_slow_slew && ee->group0_schmitt) {
		/*
		 * Slow slew + Schmitt trigger = I2C-optimized settings.
		 * This combination is ideal for open-drain I2C with pull-ups.
		 */
		return "i2c";
	}

	/* 5. Default to SPI (most common MPSSE use case) */
	return "spi";
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

/* FT4232H channels C/D are UART-only; all others have MPSSE */
static bool ftdi_mpsse_capable(enum ftdi_mpsse_chip_type chip, u8 channel)
{
	switch (chip) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		/* Single-channel: always MPSSE-capable */
		return true;
	case FTDI_CHIP_FT2232H:
	case FTDI_CHIP_FT2232HP:
	case FTDI_CHIP_FT2233HP:
		/* Dual-channel: both have MPSSE */
		return true;
	case FTDI_CHIP_FT4232H:
	case FTDI_CHIP_FT4232HA:
	case FTDI_CHIP_FT4232HP:
	case FTDI_CHIP_FT4233HP:
		/* Quad-channel: only A and B have MPSSE */
		return channel <= FTDI_CHANNEL_B;
	default:
		return false;
	}
}

static void ftdi_mpsse_urb_complete(struct urb *urb)
{
	struct ftdi_mpsse_dev *fdev = urb->context;

	fdev->io_status = urb->status;
	fdev->io_actual = urb->actual_length;
	complete(&fdev->io_done);
}

static int ftdi_mpsse_bulk_write(struct ftdi_mpsse_dev *fdev,
				 const u8 *buf, unsigned int len)
{
	int ret;

	reinit_completion(&fdev->io_done);

	usb_fill_bulk_urb(fdev->bulk_out_urb, fdev->udev,
			  usb_sndbulkpipe(fdev->udev,
					  fdev->ep_bulk_out->bEndpointAddress),
			  (void *)buf, len,
			  ftdi_mpsse_urb_complete, fdev);

	ret = usb_submit_urb(fdev->bulk_out_urb, GFP_KERNEL);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&fdev->io_done,
					 msecs_to_jiffies(FTDI_MPSSE_WRITE_TIMEOUT))) {
		usb_kill_urb(fdev->bulk_out_urb);
		/*
		 * usb_kill_urb is synchronous -- callback has run by now.
		 * Follow the kernel usb_start_wait_urb() pattern: if the
		 * URB was actually cancelled, io_status is -ENOENT.  If
		 * the URB raced and completed normally, use that status.
		 */
		return fdev->io_status == -ENOENT ? -ETIMEDOUT
						  : fdev->io_status;
	}

	return fdev->io_status;
}

static int ftdi_mpsse_bulk_read(struct ftdi_mpsse_dev *fdev,
				u8 *buf, unsigned int len)
{
	unsigned int maxp = usb_endpoint_maxp(fdev->ep_bulk_in);
	unsigned int collected = 0;
	bool first_payload = true;
	int retries = 0;

	/*
	 * FTDI prepends 2 modem-status bytes to every bulk-in USB packet.
	 * Status-only packets (2 bytes, no payload) are sent at the latency
	 * timer rate even with no data pending.  Read one USB packet per
	 * iteration, strip the status bytes, and accumulate payload until
	 * we have collected the requested amount.
	 */
	while (collected < len) {
		int actual, payload;
		int ret;

		reinit_completion(&fdev->io_done);

		usb_fill_bulk_urb(fdev->bulk_in_urb, fdev->udev,
				  usb_rcvbulkpipe(fdev->udev,
						  fdev->ep_bulk_in->bEndpointAddress),
				  fdev->bulk_buf, maxp,
				  ftdi_mpsse_urb_complete, fdev);

		ret = usb_submit_urb(fdev->bulk_in_urb, GFP_KERNEL);
		if (ret)
			return ret;

		if (!wait_for_completion_timeout(&fdev->io_done,
						 msecs_to_jiffies(FTDI_MPSSE_READ_TIMEOUT))) {
			usb_kill_urb(fdev->bulk_in_urb);
			return fdev->io_status == -ENOENT ? -ETIMEDOUT
							  : fdev->io_status;
		}

		if (fdev->io_status)
			return fdev->io_status;

		actual = fdev->io_actual;
		if (actual < FTDI_MPSSE_STATUS_BYTES)
			return -EIO;

		payload = actual - FTDI_MPSSE_STATUS_BYTES;

		/* Status-only packet: retry up to FTDI_STATUS_RETRY_MAX */
		if (payload == 0) {
			if (++retries > FTDI_STATUS_RETRY_MAX)
				return -ETIMEDOUT;
			continue;
		}
		retries = 0;

		/*
		 * Bad-command check on first packet with payload only.
		 * MPSSE responds with 0xFA followed by the unrecognised
		 * opcode -- detect it before it corrupts application data.
		 */
		if (first_payload && payload >= 2 &&
		    fdev->bulk_buf[FTDI_MPSSE_STATUS_BYTES] == MPSSE_CMD_BAD) {
			dev_err(&fdev->intf->dev,
				"MPSSE bad command 0x%02x\n",
				fdev->bulk_buf[FTDI_MPSSE_STATUS_BYTES + 1]);
			return -EPROTO;
		}
		first_payload = false;

		if (payload > len - collected)
			payload = len - collected;

		memcpy(buf + collected,
		       fdev->bulk_buf + FTDI_MPSSE_STATUS_BYTES, payload);
		collected += payload;

		/* Short packet: no more data available from the device */
		if (actual < maxp)
			break;
	}

	return collected;
}

static struct ftdi_mpsse_dev *ftdi_mpsse_get_dev(struct platform_device *pdev)
{
	return dev_get_drvdata(pdev->dev.parent);
}

int ftdi_mpsse_xfer(struct platform_device *pdev,
		    const u8 *tx, unsigned int tx_len,
		    u8 *rx, unsigned int rx_len)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);
	int ret = 0;

	if (!fdev)
		return -ENODEV;

	scoped_guard(spinlock, &fdev->disconnect_lock) {
		if (fdev->disconnect || fdev->suspended)
			return -ESHUTDOWN;
		fdev->active_transfers++;
	}

	scoped_guard(mutex, &fdev->io_lock) {
		if (tx && tx_len) {
			ret = ftdi_mpsse_bulk_write(fdev, tx, tx_len);
			if (ret)
				break;
		}

		if (rx && rx_len) {
			ret = ftdi_mpsse_bulk_read(fdev, rx, rx_len);
			if (ret < 0)
				break;
			if (ret < rx_len) {
				ret = -EIO;
				break;
			}
			ret = 0;
		}
	}

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->active_transfers--;
	if (fdev->disconnect)
		wake_up(&fdev->disconnect_wq);

	return ret;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_xfer);

int ftdi_mpsse_write(struct platform_device *pdev,
		     const u8 *buf, unsigned int len)
{
	return ftdi_mpsse_xfer(pdev, buf, len, NULL, 0);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_write);

int ftdi_mpsse_read(struct platform_device *pdev,
		    u8 *buf, unsigned int len)
{
	return ftdi_mpsse_xfer(pdev, NULL, 0, buf, len);
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_read);

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

	scoped_guard(spinlock, &fdev->disconnect_lock) {
		if (fdev->disconnect || fdev->suspended)
			return -ESHUTDOWN;
		fdev->active_transfers++;
	}

	scoped_guard(mutex, &fdev->io_lock)
		ret = usb_control_msg(fdev->udev,
				      usb_sndctrlpipe(fdev->udev, 0),
				      request,
				      USB_TYPE_VENDOR | USB_RECIP_DEVICE |
					USB_DIR_OUT,
				      value, fdev->channel, NULL, 0,
				      FTDI_MPSSE_CTRL_TIMEOUT);

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->active_transfers--;
	if (fdev->disconnect)
		wake_up(&fdev->disconnect_wq);

	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_cfg_msg);

struct usb_device *ftdi_mpsse_get_udev(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? fdev->udev : NULL;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_udev);

u8 ftdi_mpsse_get_channel(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? fdev->channel : 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_channel);

enum ftdi_mpsse_chip_type ftdi_mpsse_get_chip_type(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? fdev->chip_type : FTDI_CHIP_UNKNOWN;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_chip_type);

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

bool ftdi_mpsse_is_mpsse_capable(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	return fdev ? fdev->mpsse_capable : false;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_is_mpsse_capable);

const struct ftdi_eeprom *ftdi_mpsse_get_eeprom(struct platform_device *pdev)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev || !fdev->eeprom.valid || fdev->eeprom.empty)
		return NULL;

	return &fdev->eeprom;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_eeprom);

int ftdi_mpsse_get_cbus_config(struct platform_device *pdev,
			       u8 *buf, unsigned int len)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);
	const struct ftdi_eeprom *ee;
	unsigned int offset, nbytes;

	if (!fdev)
		return -ENODEV;

	ee = &fdev->eeprom;
	if (!ee->valid || ee->empty || !ee->checksum_ok)
		return -ENODATA;

	switch (fdev->chip_type) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		offset = FTDI_EE_232H_CBUS_BASE;
		nbytes = 5;
		break;
	case FTDI_CHIP_FT2232H:
	case FTDI_CHIP_FT2232HP:
	case FTDI_CHIP_FT2233HP:
		offset = FTDI_EE_2232H_CBUS_BASE;
		nbytes = 2;
		break;
	default:
		return -ENODATA;
	}

	if (len < nbytes)
		nbytes = len;

	memcpy(buf, ee->data + offset, nbytes);
	return nbytes;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_cbus_config);

/*
 * Return ADBUS (data bus) drive settings for child drivers.
 * For FT232H the data bus is group1 (byte 0x0d).
 * For FT2232H the data bus is group0 (byte 0x0c).
 */
int ftdi_mpsse_get_eeprom_drive(struct platform_device *pdev,
				u8 *drive_ma, bool *schmitt,
				bool *slow_slew)
{
	struct ftdi_mpsse_dev *fdev = ftdi_mpsse_get_dev(pdev);

	if (!fdev)
		return -ENODEV;

	if (!fdev->eeprom.valid || fdev->eeprom.empty ||
	    !fdev->eeprom.checksum_ok)
		return -ENODATA;

	switch (fdev->chip_type) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		/* FT232H: ADBUS = group1 (byte 0x0d) */
		*drive_ma = fdev->eeprom.group1_drive_ma;
		*schmitt = fdev->eeprom.group1_schmitt;
		*slow_slew = fdev->eeprom.group1_slow_slew;
		break;
	default:
		/* FT2232H and others: ADBUS = group0 (byte 0x0c) */
		*drive_ma = fdev->eeprom.group0_drive_ma;
		*schmitt = fdev->eeprom.group0_schmitt;
		*slow_slew = fdev->eeprom.group0_slow_slew;
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ftdi_mpsse_get_eeprom_drive);

static int ftdi_mpsse_set_bitmode(struct ftdi_mpsse_dev *fdev, u16 mode)
{
	return usb_control_msg(fdev->udev,
			       usb_sndctrlpipe(fdev->udev, 0),
			       FTDI_SIO_SET_BITMODE_REQUEST,
			       FTDI_SIO_REQTYPE_OUT,
			       mode, fdev->channel, NULL, 0,
			       FTDI_MPSSE_CTRL_TIMEOUT);
}

static int ftdi_mpsse_set_latency(struct ftdi_mpsse_dev *fdev, u8 latency_ms)
{
	return usb_control_msg(fdev->udev,
			       usb_sndctrlpipe(fdev->udev, 0),
			       FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
			       FTDI_SIO_REQTYPE_OUT,
			       latency_ms, fdev->channel, NULL, 0,
			       FTDI_MPSSE_CTRL_TIMEOUT);
}

static int ftdi_mpsse_enter_mpsse(struct ftdi_mpsse_dev *fdev)
{
	int ret;

	/* FTDI requires a reset before switching to MPSSE mode */
	ret = ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);
	if (ret < 0)
		return ret;

	ret = ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_MPSSE << 8);
	if (ret < 0)
		return ret;

	/* Configurable latency for MPSSE command-response */
	ret = ftdi_mpsse_set_latency(fdev, clamp_val(mpsse_latency, 1, 255));
	if (ret < 0)
		return ret;

	/* Stale data in FIFOs would corrupt the first MPSSE response */
	ret = usb_control_msg(fdev->udev, usb_sndctrlpipe(fdev->udev, 0),
			      FTDI_SIO_RESET_REQUEST,
			      FTDI_SIO_REQTYPE_OUT,
			      FTDI_SIO_RESET_PURGE_RX, fdev->channel,
			      NULL, 0, FTDI_MPSSE_CTRL_TIMEOUT);
	if (ret < 0)
		dev_warn(&fdev->intf->dev, "purge RX failed: %d\n", ret);

	ret = usb_control_msg(fdev->udev, usb_sndctrlpipe(fdev->udev, 0),
			      FTDI_SIO_RESET_REQUEST,
			      FTDI_SIO_REQTYPE_OUT,
			      FTDI_SIO_RESET_PURGE_TX, fdev->channel,
			      NULL, 0, FTDI_MPSSE_CTRL_TIMEOUT);
	if (ret < 0)
		dev_warn(&fdev->intf->dev, "purge TX failed: %d\n", ret);

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

/* Platform data instances -- one per function type */
static struct ftdi_mpsse_pdata pdata_uart = {
	.function = FTDI_FUNC_UART,
	.gpio_reserved_mask = 0x0000,
};

/* SPI reserves AD0-AD3 (SCK, MOSI, MISO, CS0) */
static struct ftdi_mpsse_pdata pdata_spi = {
	.function = FTDI_FUNC_SPI,
	.gpio_reserved_mask = 0x000f,
};

/* I2C reserves AD0-AD2 (SCL, SDA out, SDA in) */
static struct ftdi_mpsse_pdata pdata_i2c = {
	.function = FTDI_FUNC_I2C,
	.gpio_reserved_mask = 0x0007,
};

static struct ftdi_mpsse_pdata pdata_gpio_uart = {
	.function = FTDI_FUNC_GPIO,
	.gpio_reserved_mask = 0x0000,
	.gpio_dir_in = 0xffff,
	.gpio_dir_out = 0xffff,
};

static struct ftdi_mpsse_pdata pdata_gpio_spi = {
	.function = FTDI_FUNC_GPIO,
	.gpio_reserved_mask = 0x000f,
	.gpio_dir_in = 0xfff0,
	.gpio_dir_out = 0xfff0,
};

static struct ftdi_mpsse_pdata pdata_gpio_i2c = {
	.function = FTDI_FUNC_GPIO,
	.gpio_reserved_mask = 0x0007,
	.gpio_dir_in = 0xfff8,
	.gpio_dir_out = 0xfff8,
};

static const struct mfd_cell cells_uart[] = {
	{
		.name = "ftdi-uart",
		.platform_data = &pdata_uart,
		.pdata_size = sizeof(pdata_uart),
	},
	{
		.name = "ftdi-gpio",
		.platform_data = &pdata_gpio_uart,
		.pdata_size = sizeof(pdata_gpio_uart),
	},
};

/*
 * Non-MPSSE UART cells (FT4232H channels C/D): UART + bit-bang GPIO.
 * Bit-bang GPIO only has 8 pins (DBUS0-7), TXD/RXD always reserved.
 */
static struct ftdi_mpsse_pdata pdata_gpio_bitbang = {
	.function = FTDI_FUNC_GPIO,
	.gpio_reserved_mask = 0x0003,	/* DBUS0=TXD, DBUS1=RXD */
	.gpio_dir_in = 0x00fc,		/* DBUS2-7 */
	.gpio_dir_out = 0x00fc,
};

static const struct mfd_cell cells_uart_bitbang[] = {
	{
		.name = "ftdi-uart",
		.platform_data = &pdata_uart,
		.pdata_size = sizeof(pdata_uart),
	},
	{
		.name = "ftdi-gpio-bitbang",
		.platform_data = &pdata_gpio_bitbang,
		.pdata_size = sizeof(pdata_gpio_bitbang),
	},
};

static const struct mfd_cell cells_spi[] = {
	{
		.name = "ftdi-spi",
		.platform_data = &pdata_spi,
		.pdata_size = sizeof(pdata_spi),
	},
	{
		.name = "ftdi-gpio",
		.platform_data = &pdata_gpio_spi,
		.pdata_size = sizeof(pdata_gpio_spi),
	},
};

static const struct mfd_cell cells_i2c[] = {
	{
		.name = "ftdi-i2c",
		.platform_data = &pdata_i2c,
		.pdata_size = sizeof(pdata_i2c),
	},
	{
		.name = "ftdi-gpio",
		.platform_data = &pdata_gpio_i2c,
		.pdata_size = sizeof(pdata_gpio_i2c),
	},
};

static int ftdi_mpsse_probe(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct ftdi_mpsse_dev *fdev;
	enum ftdi_mpsse_chip_type chip;
	unsigned int ifnum;
	u8 channel;
	const struct mfd_cell *cells;
	int n_cells;
	int ret;

	chip = ftdi_mpsse_detect_chip(udev);
	if (chip == FTDI_CHIP_UNKNOWN) {
		dev_err(&intf->dev, "unsupported FTDI chip (bcdDevice=0x%04x)\n",
			le16_to_cpu(udev->descriptor.bcdDevice));
		return -ENODEV;
	}

	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	channel = FTDI_CHANNEL_A + ifnum;

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
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
	if (!fdev->bulk_buf) {
		ret = -ENOMEM;
		goto err_free_out_urb;
	}

	/*
	 * Disable USB autosuspend by default.  FTDI chips lose all
	 * configuration (bitmode, GPIO state, clock settings) across
	 * USB suspend, requiring full re-initialization.  Users can
	 * enable it via the autosuspend module param if needed.
	 */
	if (!autosuspend)
		usb_disable_autosuspend(udev);

	/* Read and decode EEPROM (non-fatal if it fails) */
	ftdi_eeprom_read(fdev);

	{
		char mode[16];

		ftdi_mpsse_get_mode(ifnum, mode, sizeof(mode));

		/*
		 * Non-MPSSE channels (FT4232H C/D) can only do UART.
		 * Warn if the user requested a non-UART mode and force UART.
		 */
		if (!fdev->mpsse_capable) {
			if (mode[0] && strcmp(mode, "uart") != 0)
				dev_warn(&intf->dev,
					 "bus_mode '%s' not supported on non-MPSSE channel %c, forcing UART\n",
					 mode, 'A' + ifnum);

			ret = ftdi_mpsse_enter_uart(fdev);
			fdev->mpsse_mode = false;
			strscpy(fdev->mode_name, "uart", sizeof(fdev->mode_name));
			fdev->gpio_reserved_mask = 0x0003;
			cells = cells_uart_bitbang;
			n_cells = ARRAY_SIZE(cells_uart_bitbang);
		} else if (strcmp(mode, "uart") == 0) {
			ret = ftdi_mpsse_enter_uart(fdev);
			fdev->mpsse_mode = false;
			strscpy(fdev->mode_name, "uart", sizeof(fdev->mode_name));
			fdev->gpio_reserved_mask = 0x0000;
			cells = cells_uart;
			n_cells = ARRAY_SIZE(cells_uart);
		} else if (strcmp(mode, "spi") == 0) {
			struct ftdi_mpsse_pdata pd_spi = {
				.function = FTDI_FUNC_SPI,
			};
			struct ftdi_mpsse_pdata pd_gpio = {
				.function = FTDI_FUNC_GPIO,
			};
			struct mfd_cell spi_cells[2];
			int n_cs, j;
			u16 mask;

			ret = ftdi_mpsse_enter_mpsse(fdev);
			if (ret < 0) {
				dev_err(&intf->dev,
					"failed to set channel mode\n");
				goto err_buf;
			}
			fdev->mpsse_mode = true;

			n_cs = ftdi_mpsse_parse_spi_cs(fdev->channel,
						       pd_spi.spi_cs_pins,
						       FTDI_SPI_CS_MAX);
			if (n_cs < 0) {
				dev_err(&intf->dev,
					"invalid spi_cs parameter '%s'\n",
					spi_cs);
				ret = n_cs;
				goto err_buf;
			}
			pd_spi.spi_num_cs = n_cs;

			/* Reserved: AD0-AD2 (SCK/MOSI/MISO) + CS pins */
			mask = BIT(0) | BIT(1) | BIT(2);
			for (j = 0; j < n_cs; j++)
				mask |= BIT(pd_spi.spi_cs_pins[j]);
			pd_spi.gpio_reserved_mask = mask;
			pd_gpio.gpio_reserved_mask = mask;
			pd_gpio.gpio_dir_in = ~mask & 0xffff;
			pd_gpio.gpio_dir_out = ~mask & 0xffff;

			strscpy(fdev->mode_name, "spi", sizeof(fdev->mode_name));
			fdev->gpio_reserved_mask = mask;
			fdev->spi_num_cs = n_cs;
			memcpy(fdev->spi_cs_pins, pd_spi.spi_cs_pins, n_cs);

			memset(spi_cells, 0, sizeof(spi_cells));
			spi_cells[0].name = "ftdi-spi";
			spi_cells[0].platform_data = &pd_spi;
			spi_cells[0].pdata_size = sizeof(pd_spi);
			spi_cells[1].name = "ftdi-gpio";
			spi_cells[1].platform_data = &pd_gpio;
			spi_cells[1].pdata_size = sizeof(pd_gpio);

			usb_set_intfdata(intf, fdev);

			ret = mfd_add_hotplug_devices(&intf->dev,
						      spi_cells, 2);
			if (ret) {
				dev_err(&intf->dev,
					"failed to add child devices\n");
				goto err_reset;
			}

			dev_info(&intf->dev,
				 "FTDI MPSSE core: channel %c, mode 'spi'\n",
				 'A' + ifnum);
			goto done;
		} else if (strcmp(mode, "i2c") == 0) {
			ret = ftdi_mpsse_enter_mpsse(fdev);
			fdev->mpsse_mode = true;
			strscpy(fdev->mode_name, "i2c", sizeof(fdev->mode_name));
			fdev->gpio_reserved_mask = 0x0007;
			cells = cells_i2c;
			n_cells = ARRAY_SIZE(cells_i2c);
		} else {
			u8 chan_type = 0xff;
			const char *detected;

			if (mode[0])
				dev_warn(&intf->dev,
					 "unknown bus_mode '%s', defaulting to auto\n",
					 mode);

			if (fdev->eeprom.valid && !fdev->eeprom.empty &&
			    fdev->eeprom.checksum_ok)
				chan_type = ftdi_eeprom_channel_type(&fdev->eeprom, channel);

			switch (chan_type) {
			case FTDI_EE_CHAN_UART:
				ret = ftdi_mpsse_enter_uart(fdev);
				fdev->mpsse_mode = false;
				strscpy(fdev->mode_name, "uart",
					sizeof(fdev->mode_name));
				fdev->gpio_reserved_mask = 0x0000;
				cells = cells_uart;
				n_cells = ARRAY_SIZE(cells_uart);
				break;
			case FTDI_EE_CHAN_FIFO:
			case FTDI_EE_CHAN_OPTO:
			case FTDI_EE_CHAN_CPU:
			case FTDI_EE_CHAN_FT1284:
				dev_warn(&intf->dev,
					 "EEPROM channel type 0x%02x not supported\n",
					 chan_type);
				ret = -ENODEV;
				goto err_buf;
			default:
				/*
				 * Unknown/empty EEPROM channel type.
				 * Try to detect protocol from EEPROM hints.
				 */
				detected = ftdi_mpsse_detect_protocol(&fdev->eeprom, channel);

				if (detected && strcmp(detected, "uart") == 0) {
					/* VCP bit or protocol hint = UART */
					dev_info(&intf->dev,
						 "auto: EEPROM hints UART mode (VCP enabled or protocol hint)\n");
					ret = ftdi_mpsse_enter_uart(fdev);
					fdev->mpsse_mode = false;
					strscpy(fdev->mode_name, "uart",
						sizeof(fdev->mode_name));
					fdev->gpio_reserved_mask = 0x0000;
					cells = cells_uart;
					n_cells = ARRAY_SIZE(cells_uart);
				} else if (detected && strcmp(detected, "i2c") == 0) {
					/* Product string or electrical hints = I2C */
					dev_info(&intf->dev,
						 "auto: EEPROM hints I2C mode\n");
					ret = ftdi_mpsse_enter_mpsse(fdev);
					fdev->mpsse_mode = true;
					strscpy(fdev->mode_name, "i2c",
						sizeof(fdev->mode_name));
					fdev->gpio_reserved_mask = 0x0007;
					cells = cells_i2c;
					n_cells = ARRAY_SIZE(cells_i2c);
				} else {
					/* SPI detected or default */
					struct ftdi_mpsse_pdata pd_spi_auto = {
						.function = FTDI_FUNC_SPI,
					};
					struct ftdi_mpsse_pdata pd_gpio_auto = {
						.function = FTDI_FUNC_GPIO,
					};
					struct mfd_cell spi_cells_auto[2];
					int n_cs, j;
					u16 mask;

					if (detected)
						dev_info(&intf->dev,
							 "auto: EEPROM hints SPI mode\n");
					else
						dev_info(&intf->dev,
							 "auto: no EEPROM hints, defaulting to SPI\n");
					ret = ftdi_mpsse_enter_mpsse(fdev);
					if (ret < 0)
						break;
					fdev->mpsse_mode = true;
					strscpy(fdev->mode_name, "spi",
						sizeof(fdev->mode_name));

					/* Parse spi_cs parameter for CS pins */
					n_cs = ftdi_mpsse_parse_spi_cs(fdev->channel,
								       pd_spi_auto.spi_cs_pins,
								       FTDI_SPI_CS_MAX);
					if (n_cs < 0) {
						dev_err(&intf->dev,
							"invalid spi_cs parameter '%s'\n",
							spi_cs);
						ret = n_cs;
						break;
					}
					pd_spi_auto.spi_num_cs = n_cs;

					/* Reserved: AD0-AD2 (SCK/MOSI/MISO) + CS pins */
					mask = BIT(0) | BIT(1) | BIT(2);
					for (j = 0; j < n_cs; j++)
						mask |= BIT(pd_spi_auto.spi_cs_pins[j]);
					pd_spi_auto.gpio_reserved_mask = mask;
					pd_gpio_auto.gpio_reserved_mask = mask;
					pd_gpio_auto.gpio_dir_in = ~mask & 0xffff;
					pd_gpio_auto.gpio_dir_out = ~mask & 0xffff;

					fdev->gpio_reserved_mask = mask;
					fdev->spi_num_cs = n_cs;
					memcpy(fdev->spi_cs_pins, pd_spi_auto.spi_cs_pins, n_cs);

					memset(spi_cells_auto, 0, sizeof(spi_cells_auto));
					spi_cells_auto[0].name = "ftdi-spi";
					spi_cells_auto[0].platform_data = &pd_spi_auto;
					spi_cells_auto[0].pdata_size = sizeof(pd_spi_auto);
					spi_cells_auto[1].name = "ftdi-gpio";
					spi_cells_auto[1].platform_data = &pd_gpio_auto;
					spi_cells_auto[1].pdata_size = sizeof(pd_gpio_auto);

					usb_set_intfdata(intf, fdev);

					ret = mfd_add_hotplug_devices(&intf->dev,
								      spi_cells_auto, 2);
					if (ret) {
						dev_err(&intf->dev,
							"failed to add child devices\n");
						goto err_reset;
					}

					dev_info(&intf->dev,
						 "FTDI MPSSE core: channel %c, mode 'auto'\n",
						 'A' + ifnum);
					goto done;
				}
				break;
			}
		}

		if (ret < 0) {
			dev_err(&intf->dev, "failed to set channel mode\n");
			goto err_buf;
		}

		usb_set_intfdata(intf, fdev);

		ret = mfd_add_hotplug_devices(&intf->dev, cells, n_cells);
		if (ret) {
			dev_err(&intf->dev, "failed to add child devices\n");
			goto err_reset;
		}

		dev_info(&intf->dev,
			 "FTDI MPSSE core: channel %c, mode '%s'\n",
			 'A' + ifnum, mode[0] ? mode : "auto");
	}

done:
	return 0;

err_reset:
	ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);
	usb_set_intfdata(intf, NULL);
err_buf:
	kfree(fdev->bulk_buf);
err_free_out_urb:
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

	/*
	 * Reset bitmode while the device is still reachable -- after
	 * usb_poison_urb the control message would fail on a device
	 * that is still physically present (software unbind).
	 * Hold bus_lock so we don't corrupt a child's in-flight
	 * MPSSE transaction.
	 */
	mutex_lock(&fdev->bus_lock);
	ftdi_mpsse_set_bitmode(fdev, FTDI_SIO_BITMODE_RESET);
	mutex_unlock(&fdev->bus_lock);

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->disconnect = true;

	/*
	 * Poison URBs: cancels any in-flight bulk operation AND ensures
	 * any future usb_submit_urb() returns -EPERM immediately.  This
	 * closes the TOCTOU race where a transport function that already
	 * passed the disconnect check could submit a URB after a plain
	 * usb_kill_urb() had completed.
	 */
	usb_poison_urb(fdev->bulk_in_urb);
	usb_poison_urb(fdev->bulk_out_urb);

	if (!wait_event_timeout(fdev->disconnect_wq, !fdev->active_transfers,
				msecs_to_jiffies(10000)))
		dev_warn(&intf->dev, "timed out waiting for %d active transfer(s)\n",
			 fdev->active_transfers);

	mfd_remove_devices(&intf->dev);

	usb_set_intfdata(intf, NULL);
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

	/* Kill (not poison) because suspend is reversible */
	usb_kill_urb(fdev->bulk_in_urb);
	usb_kill_urb(fdev->bulk_out_urb);

	if (!wait_event_timeout(fdev->disconnect_wq, !fdev->active_transfers,
				msecs_to_jiffies(5000)))
		dev_warn(&intf->dev,
			 "suspend: timed out waiting for %d transfer(s)\n",
			 fdev->active_transfers);

	return 0;
}

static int ftdi_mpsse_resume(struct usb_interface *intf)
{
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);
	int ret;

	if (!fdev)
		return 0;

	/* FTDI chip loses bitmode configuration across USB suspend */
	if (fdev->mpsse_mode)
		ret = ftdi_mpsse_enter_mpsse(fdev);
	else
		ret = ftdi_mpsse_enter_uart(fdev);

	if (ret)
		dev_err(&intf->dev, "resume: mode re-init failed: %d\n", ret);

	scoped_guard(spinlock, &fdev->disconnect_lock)
		fdev->suspended = false;
	wake_up(&fdev->disconnect_wq);

	return ret;
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
 * Walk child devices to discover registered subsystem device names.
 * Two-level iteration: MFD platform_device children of the USB interface,
 * then each platform_device's children to find subsystem devices.
 */
struct pinmap_info {
	char spi[16];
	char i2c[16];
	char uart[32];
	char gpio[16];
};

static int pinmap_match_child(struct device *dev, void *data)
{
	struct pinmap_info *info = data;
	const char *name = dev_name(dev);

	if (!strncmp(name, "spi", 3) && !info->spi[0])
		strscpy(info->spi, name, sizeof(info->spi));
	else if (!strncmp(name, "i2c-", 4) && !info->i2c[0])
		strscpy(info->i2c, name, sizeof(info->i2c));
	else if (!strncmp(name, "ttyFTDI", 7) && !info->uart[0])
		strscpy(info->uart, name, sizeof(info->uart));
	else if (!strncmp(name, "gpiochip", 8) && !info->gpio[0])
		strscpy(info->gpio, name, sizeof(info->gpio));

	return 0;
}

static int pinmap_walk_platform(struct device *dev, void *data)
{
	device_for_each_child(dev, data, pinmap_match_child);
	return 0;
}

static void pinmap_discover(struct device *intf_dev, struct pinmap_info *info)
{
	memset(info, 0, sizeof(*info));
	device_for_each_child(intf_dev, info, pinmap_walk_platform);
}

static ssize_t pinmap_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct ftdi_mpsse_dev *fdev = usb_get_intfdata(intf);
	struct pinmap_info info;
	int len = 0;
	int i;

	if (!fdev)
		return -ENODEV;

	pinmap_discover(dev, &info);

	/* Header */
	len += sysfs_emit_at(buf, len, "chip: %s\n",
			     ftdi_chip_name(fdev->chip_type));
	len += sysfs_emit_at(buf, len, "channel: %c\n",
			     'A' + fdev->channel - FTDI_CHANNEL_A);
	len += sysfs_emit_at(buf, len, "mode: %s%s\n", fdev->mode_name,
			     fdev->mpsse_capable ? "" : " (non-MPSSE)");

	/* SPI section */
	if (strcmp(fdev->mode_name, "spi") == 0 ||
	    strcmp(fdev->mode_name, "auto") == 0) {
		len += sysfs_emit_at(buf, len, "\nspi: %s\n",
				     info.spi[0] ? info.spi : "-");
		len += sysfs_emit_at(buf, len, "  AD0  SCK\n");
		len += sysfs_emit_at(buf, len, "  AD1  MOSI\n");
		len += sysfs_emit_at(buf, len, "  AD2  MISO\n");
		for (i = 0; i < fdev->spi_num_cs; i++)
			len += sysfs_emit_at(buf, len, "  AD%u  CS%d\n",
					     fdev->spi_cs_pins[i], i);
		if (fdev->spi_num_cs == 0)
			len += sysfs_emit_at(buf, len, "  AD3  CS0\n");
	}

	/* I2C section */
	if (strcmp(fdev->mode_name, "i2c") == 0 ||
	    strcmp(fdev->mode_name, "auto") == 0) {
		len += sysfs_emit_at(buf, len, "\ni2c: %s\n",
				     info.i2c[0] ? info.i2c : "-");
		len += sysfs_emit_at(buf, len, "  AD0  SCL\n");
		len += sysfs_emit_at(buf, len, "  AD1  SDA_OUT\n");
		len += sysfs_emit_at(buf, len, "  AD2  SDA_IN\n");
	}

	/* UART section */
	if (strcmp(fdev->mode_name, "uart") == 0) {
		const char *p = fdev->mpsse_capable ? "AD" : "DBUS";

		len += sysfs_emit_at(buf, len, "\nuart: %s\n",
				     info.uart[0] ? info.uart : "-");
		len += sysfs_emit_at(buf, len, "  %s0  TXD\n", p);
		len += sysfs_emit_at(buf, len, "  %s1  RXD\n", p);
		len += sysfs_emit_at(buf, len, "  %s2  RTS\n", p);
		len += sysfs_emit_at(buf, len, "  %s3  CTS\n", p);
		len += sysfs_emit_at(buf, len, "  %s4  DTR\n", p);
		len += sysfs_emit_at(buf, len, "  %s5  DSR\n", p);
		len += sysfs_emit_at(buf, len, "  %s6  DCD\n", p);
		len += sysfs_emit_at(buf, len, "  %s7  RI\n", p);
	}

	/* GPIO section */
	len += sysfs_emit_at(buf, len, "\ngpio%s: %s\n",
			     fdev->mpsse_capable ? "" : "-bitbang",
			     info.gpio[0] ? info.gpio : "-");

	if (!fdev->mpsse_capable) {
		/* Non-MPSSE: 8 DBUS pins only */
		for (i = 0; i < 8; i++) {
			if (fdev->gpio_reserved_mask & BIT(i)) {
				if (info.uart[0])
					len += sysfs_emit_at(buf, len,
						"  DBUS%d  reserved (%s)\n",
						i, info.uart);
				else
					len += sysfs_emit_at(buf, len,
						"  DBUS%d  reserved\n", i);
			} else {
				len += sysfs_emit_at(buf, len,
					"  DBUS%d  available\n", i);
			}
		}
	} else {
		/* MPSSE: 16 pins (AD0-7 + AC0-7) */
		const char *bus_owner = NULL;

		if (strcmp(fdev->mode_name, "spi") == 0 && info.spi[0])
			bus_owner = info.spi;
		else if (strcmp(fdev->mode_name, "i2c") == 0 && info.i2c[0])
			bus_owner = info.i2c;
		else if (strcmp(fdev->mode_name, "uart") == 0 && info.uart[0])
			bus_owner = info.uart;

		for (i = 0; i < 16; i++) {
			const char *pin = i < 8 ? "AD" : "AC";
			int num = i % 8;
			bool reserved;

			if (strcmp(fdev->mode_name, "uart") == 0 && i < 8)
				reserved = true;
			else
				reserved = fdev->gpio_reserved_mask & BIT(i);

			if (reserved) {
				if (bus_owner)
					len += sysfs_emit_at(buf, len,
						"  %s%d  reserved (%s)\n",
						pin, num, bus_owner);
				else
					len += sysfs_emit_at(buf, len,
						"  %s%d  reserved\n",
						pin, num);
			} else {
				len += sysfs_emit_at(buf, len,
					"  %s%d  available\n", pin, num);
			}
		}
	}

	return len;
}
static DEVICE_ATTR_RO(pinmap);

static struct attribute *ftdi_mpsse_pinmap_attrs[] = {
	&dev_attr_pinmap.attr,
	NULL,
};

static const struct attribute_group ftdi_mpsse_pinmap_group = {
	.attrs = ftdi_mpsse_pinmap_attrs,
};

/*
 * Empty by default to avoid conflict with ftdi_sio.  Users add
 * their device PID at runtime:
 *   echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id
 * or override ftdi_sio's claim on standard PIDs:
 *   echo ftdi_mpsse > /sys/bus/usb/devices/X-Y:Z/driver_override
 */
static const struct usb_device_id ftdi_mpsse_id_table[] = {
	{ }
};
MODULE_DEVICE_TABLE(usb, ftdi_mpsse_id_table);

static const struct attribute_group *ftdi_mpsse_groups[] = {
	&ftdi_eeprom_attr_group,
	&ftdi_mpsse_pinmap_group,
	NULL,
};

static struct usb_driver ftdi_mpsse_driver = {
	.name		= DRIVER_NAME,
	.probe		= ftdi_mpsse_probe,
	.disconnect	= ftdi_mpsse_disconnect,
	.suspend	= ftdi_mpsse_suspend,
	.resume		= ftdi_mpsse_resume,
	.id_table	= ftdi_mpsse_id_table,
	.dev_groups	= ftdi_mpsse_groups,
};
module_usb_driver(ftdi_mpsse_driver);

MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
