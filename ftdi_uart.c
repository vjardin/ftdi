// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE UART child driver
 *
 * serial_core port (/dev/ttyFTDIx) on a channel of an FTDI Hi-Speed chip,
 * child of ftdi_mpsse, plus the FT232H CBUS GPIO chip.
 *
 * Baud rate divisor encoding: AN_120 and the D2XX Programmer's Guide.
 */

#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi-uart"
#define FTDI_UART_NR	16
#define FTDI_WRITE_URBS	2
#define FTDI_WRITE_BUF	4096	/* bytes per write URB */
/*
 * A line status report can have been built by the chip just before the
 * data of a write URB reached it: TEMT is only trusted this long after
 * the last write URB completed.
 */
#define FTDI_TEMT_SETTLE_MS	2
/* Suspend: longest wait for the data already handed to USB to go out */
#define FTDI_SUSPEND_DRAIN_MS	1000
#define FTDI_WRITE_URBS_ALL	(BIT(FTDI_WRITE_URBS) - 1)
/* Modem control states waiting for mctrl_work */
#define FTDI_MCTRL_QUEUE	8

#define WDR_TIMEOUT		5000
#define WDR_SHORT_TIMEOUT	1000

/* FT232H CBUS GPIO: the pins of "ACBUS Bit Bang" (DS_FT232H table 3.5) */
#define FTDI_CBUS_NGPIO		4
static const u8 ftdi_cbus_acbus[FTDI_CBUS_NGPIO] = { 5, 6, 8, 9 };

/* Bulk IN packets start with a modem status and a line status byte */
#define FTDI_RS_ERR_MASK (FTDI_RS_BI | FTDI_RS_PE | FTDI_RS_FE | FTDI_RS_OE)
#define FTDI_STATUS_B0_MASK \
	(FTDI_RS0_CTS | FTDI_RS0_DSR | FTDI_RS0_RI | FTDI_RS0_RLSD)

struct ftdi_uart_port {
	struct uart_port port;
	struct platform_device *pdev;
	struct usb_device *udev;
	u8 channel;
	enum ftdi_mpsse_chip_type chip_type;
	unsigned int chip_fifo;		/* chip TX buffer, bytes */

	struct usb_endpoint_descriptor *ep_in;
	struct usb_endpoint_descriptor *ep_out;

	struct urb *read_urb;
	u8 *read_buf;
	struct urb *write_urb[FTDI_WRITE_URBS];
	u8 *write_buf[FTDI_WRITE_URBS];

	u16 max_packet_size;
	u8 prev_status;

	/* port mutex: line settings, flow control and break */
	u16 data_value;			/* SET_DATA: bits, parity, stop */
	bool break_on;
	tcflag_t cflag, iflag;
	u16 xchars;			/* VSTOP << 8 | VSTART */
	u8 latency_timer;		/* ms, 1-255 */
	bool dtr_dsr_flow;

	/* port->lock */
	bool running;			/* from startup to shutdown */
	unsigned long write_urbs_free;	/* bitmap */
	bool tx_stopped;		/* stop_tx: no new data to the URBs */
	bool tx_held;			/* suspend: nothing more to the URBs */
	bool tx_halted;			/* bulk OUT halted, see halt_work */
	bool rx_running;		/* read URB submitted */
	bool rx_stopped;		/* stop_rx */
	bool rx_throttled;
	bool rx_halted;			/* bulk IN halted, see halt_work */
	bool rx_discard;		/* CREAD off */
	bool chip_temt;			/* last line status: TX shifter empty */
	ktime_t tx_done;		/* completion of the last write URB */
	ktime_t tx_drained;		/* ... plus the chip TX buffer time */
	unsigned int mctrl_queue[FTDI_MCTRL_QUEUE];
	unsigned int mctrl_head, mctrl_tail;
	bool mctrl_busy;		/* states queued or being sent */

	struct work_struct mctrl_work;
	struct work_struct halt_work;

	const char *txden_pin;		/* pin with TXDEN (RS-485), or NULL */

	struct gpio_chip cbus_gc;
	struct mutex cbus_lock;		/* cbus_output, cbus_value, cbus_used */
	u8 cbus_valid;			/* pins in I/O mode in the EEPROM */
	u8 cbus_output;
	u8 cbus_value;
	bool cbus_registered;
	bool cbus_used;			/* CBUS bit-bang mode programmed */
};

#define to_ftdi_uart_port(p) container_of(p, struct ftdi_uart_port, port)

static struct uart_driver ftdi_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "ftdi_uart",
	.dev_name	= "ttyFTDI",
	.major		= 0,
	.minor		= 0,
	.nr		= FTDI_UART_NR,
};

static DEFINE_IDA(ftdi_uart_ida);

static int ftdi_uart_ctrl(struct ftdi_uart_port *fp, u8 request,
			  u16 value, u16 index)
{
	return usb_control_msg(fp->udev, usb_sndctrlpipe(fp->udev, 0),
			       request,
			       USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT,
			       value, index, NULL, 0, WDR_TIMEOUT);
}

static u32 ftdi_2232h_baud_base_to_divisor(int baud, int base)
{
	static const unsigned char divfrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
	u32 divisor;
	int divisor3;

	divisor3 = DIV_ROUND_CLOSEST(8 * base, 10 * baud);
	divisor = divisor3 >> 3;
	divisor |= (u32)divfrac[divisor3 & 0x7] << 14;
	if (divisor == 1)
		divisor = 0;
	else if (divisor == 0x4001)
		divisor = 1;
	divisor |= 0x00020000;
	return divisor;
}

static u32 ftdi_232bm_baud_base_to_divisor(int baud, int base)
{
	static const unsigned char divfrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
	u32 divisor;
	int divisor3;

	divisor3 = DIV_ROUND_CLOSEST(base, 2 * baud);
	divisor = divisor3 >> 3;
	divisor |= (u32)divfrac[divisor3 & 0x7] << 14;
	if (divisor == 1)
		divisor = 0;
	else if (divisor == 0x4001)
		divisor = 1;
	return divisor;
}

/*
 * Hi-Speed chips: 120 MHz base clock from 1200 baud up, the BM formula
 * (48 MHz) below; the rate is 300 to 12000000 (set_termios).
 */
static u32 ftdi_uart_get_divisor(speed_t baud)
{
	if (baud < 1200)
		return ftdi_232bm_baud_base_to_divisor(baud, 48000000);
	return ftdi_2232h_baud_base_to_divisor(baud, 120000000);
}

/*
 * From 1200 baud the rate is 12 MHz / divisor, a divisor of 2 or more in
 * 1/8 steps; below 2 only "a divisor of 0 will give 12MBaud, and a divisor
 * of 1 will give 8MBaud. Sub-integer divisors are not allowed if the main
 * divisor (n) is either 0 or 1" (AN_120 4.4). Above 6 Mbaud the nearest of
 * 6, 8 and 12 Mbaud is used.
 */
static speed_t ftdi_uart_hs_baud(speed_t baud)
{
	static const speed_t rates[] = { 6000000, 8000000, 12000000 };
	speed_t best = rates[0];
	int i;

	if (baud <= rates[0])
		return baud;
	for (i = 1; i < ARRAY_SIZE(rates); i++)
		if (abs_diff(rates[i], baud) < abs_diff(best, baud))
			best = rates[i];
	return best;
}

static int ftdi_uart_set_baudrate(struct ftdi_uart_port *fp, speed_t baud)
{
	u32 index_value;
	u16 value, index;
	int ret;

	index_value = ftdi_uart_get_divisor(baud);
	value = (u16)index_value;
	index = (u16)(index_value >> 16);
	if (fp->channel)
		index = (u16)((index << 8) | fp->channel);

	ret = usb_control_msg(fp->udev, usb_sndctrlpipe(fp->udev, 0),
			      FTDI_SIO_SET_BAUDRATE_REQUEST,
			      FTDI_SIO_REQTYPE_OUT,
			      value, index, NULL, 0, WDR_SHORT_TIMEOUT);
	return ret < 0 ? ret : 0;
}

/*
 * One bulk IN packet: modem status, line status, data. port->lock held:
 * the flip buffer inserts allocate atomically. Returns whether anything
 * went to the flip buffer.
 */
static bool ftdi_uart_process_packet(struct ftdi_uart_port *fp,
				     unsigned char *buf, int len)
{
	struct uart_port *port = &fp->port;
	struct tty_port *tport = &port->state->port;
	unsigned char status, prev;
	bool brk = false, overrun = false;
	char flag;
	int n;

	if (len < 2)
		return false;

	if (ktime_ms_delta(ktime_get(), fp->tx_done) >= FTDI_TEMT_SETTLE_MS)
		fp->chip_temt = !!(buf[1] & FTDI_RS_TEMT);

	status = buf[0] & FTDI_STATUS_B0_MASK;
	prev = fp->prev_status;
	if (status != prev) {
		WRITE_ONCE(fp->prev_status, status);
		if ((status ^ prev) & FTDI_RS0_DSR)
			port->icount.dsr++;
		if ((status ^ prev) & FTDI_RS0_RI)
			port->icount.rng++;
		/* These count, wake TIOCMIWAIT and handle carrier/CTS */
		if ((status ^ prev) & FTDI_RS0_RLSD)
			uart_handle_dcd_change(port, status & FTDI_RS0_RLSD);
		if ((status ^ prev) & FTDI_RS0_CTS)
			uart_handle_cts_change(port, status & FTDI_RS0_CTS);
		wake_up_interruptible(&tport->delta_msr_wait);
	}

	/* Status only: errors are reported with data only */
	if (len == 2)
		return false;

	/*
	 * Break, then parity, then framing (same rules as ftdi_sio): a
	 * break only applies to the last byte, and only when it is a NUL.
	 */
	flag = TTY_NORMAL;
	if (buf[1] & FTDI_RS_ERR_MASK) {
		if ((buf[1] & FTDI_RS_BI) && buf[len - 1] == '\0') {
			port->icount.brk++;
			brk = true;
		}
		if (buf[1] & FTDI_RS_PE) {
			port->icount.parity++;
			flag = TTY_PARITY;
		} else if (buf[1] & FTDI_RS_FE) {
			port->icount.frame++;
			flag = TTY_FRAME;
		}
		if (buf[1] & FTDI_RS_OE) {
			port->icount.overrun++;
			overrun = true;
		}
	}
	port->icount.rx += len - 2;
	if (fp->rx_discard)
		return false;

	if (overrun && !tty_insert_flip_char(tport, 0, TTY_OVERRUN))
		port->icount.buf_overrun++;
	if (brk)
		len--;
	n = tty_insert_flip_string_fixed_flag(tport, buf + 2, flag, len - 2);
	port->icount.buf_overrun += len - 2 - n;
	if (brk && !tty_insert_flip_char(tport, 0, TTY_BREAK))
		port->icount.buf_overrun++;
	return true;
}

static void ftdi_uart_read_complete(struct urb *urb);

/*
 * (Re)submit the read URB unless the port is down, RX stopped, throttled
 * or halted: when throttled, the chip's RX buffer fills and its own flow
 * control holds the sender ("will drop RTS if it cannot receive any more",
 * AN232B-04 4.3). port->lock held.
 */
static void ftdi_uart_rx_start(struct ftdi_uart_port *fp)
{
	int ret;

	if (!fp->running || fp->rx_running || fp->rx_stopped ||
	    fp->rx_throttled || fp->rx_halted)
		return;

	ret = usb_submit_urb(fp->read_urb, GFP_ATOMIC);
	if (!ret)
		fp->rx_running = true;
	else if (ret != -ENODEV && ret != -EPERM)
		dev_err_ratelimited(&fp->pdev->dev,
				    "read URB submit failed: %d\n", ret);
}

static void ftdi_uart_read_complete(struct urb *urb)
{
	struct ftdi_uart_port *fp = urb->context;
	struct uart_port *port = &fp->port;
	unsigned char *data = urb->transfer_buffer;
	bool push = false;
	unsigned long flags;
	int i, len;

	uart_port_lock_irqsave(port, &flags);
	fp->rx_running = false;
	switch (urb->status) {
	case 0:
		for (i = 0; i < urb->actual_length; i += fp->max_packet_size) {
			len = min_t(int, urb->actual_length - i,
				    fp->max_packet_size);
			push |= ftdi_uart_process_packet(fp, &data[i], len);
		}
		break;
	case -EPIPE:
		fp->rx_halted = true;
		schedule_work(&fp->halt_work);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPERM:
	case -ENODEV:
		uart_port_unlock_irqrestore(port, flags);
		return;		/* killed or unplugged */
	default:
		/* Transient (-EPROTO, -EILSEQ, -ETIME...): keep receiving */
		dev_err_ratelimited(&fp->pdev->dev, "read URB error: %d\n",
				    urb->status);
		break;
	}
	ftdi_uart_rx_start(fp);
	uart_port_unlock_irqrestore(port, flags);

	if (push)
		tty_flip_buffer_push(&port->state->port);
}

static void ftdi_uart_write_complete(struct urb *urb);

/*
 * Fill the free write URBs from the transmit FIFO, unless the port is
 * down, held for suspend, halted, or modem control states are still
 * queued: TX follows the line changes requested before it. port->lock
 * held.
 */
static void ftdi_uart_tx_kick(struct ftdi_uart_port *fp)
{
	struct uart_port *port = &fp->port;
	struct tty_port *tport;

	if (!fp->running || fp->tx_held || fp->tx_halted || fp->mctrl_busy)
		return;

	tport = &port->state->port;
	while (fp->write_urbs_free) {
		int idx = __ffs(fp->write_urbs_free);
		u8 *buf = fp->write_buf[idx];
		unsigned int count = 0;
		int ret;

		/* XON/XOFF goes first, even while the tty is stopped */
		if (port->x_char) {
			buf[count++] = port->x_char;
			port->icount.tx++;
			port->x_char = 0;
		}
		if (!fp->tx_stopped && !uart_tx_stopped(port))
			count += uart_fifo_out(port, buf + count,
					       FTDI_WRITE_BUF - count);
		if (!count)
			break;

		usb_fill_bulk_urb(fp->write_urb[idx], fp->udev,
				  usb_sndbulkpipe(fp->udev,
						  fp->ep_out->bEndpointAddress),
				  buf, count, ftdi_uart_write_complete, fp);
		__clear_bit(idx, &fp->write_urbs_free);
		fp->chip_temt = false;

		ret = usb_submit_urb(fp->write_urb[idx], GFP_ATOMIC);
		if (ret) {
			__set_bit(idx, &fp->write_urbs_free);
			/* -EPERM: poisoned by remove, -ENODEV: unplugged */
			if (ret != -EPERM && ret != -ENODEV)
				dev_err_ratelimited(&fp->pdev->dev,
						    "write URB submit failed: %d\n",
						    ret);
			break;
		}
	}

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static void ftdi_uart_write_complete(struct urb *urb)
{
	struct ftdi_uart_port *fp = urb->context;
	struct uart_port *port = &fp->port;
	unsigned long flags;
	bool dead = false;
	int idx;

	for (idx = 0; idx < FTDI_WRITE_URBS; idx++)
		if (fp->write_urb[idx] == urb)
			break;

	uart_port_lock_irqsave(port, &flags);
	__set_bit(idx, &fp->write_urbs_free);
	/* The data is in the chip now: not empty until it says so again */
	fp->chip_temt = false;
	fp->tx_done = ktime_get();
	fp->tx_drained = ktime_add_ns(fp->tx_done,
				      (u64)fp->chip_fifo * port->frame_time);

	switch (urb->status) {
	case 0:
		break;
	case -EPIPE:
		fp->tx_halted = true;
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPERM:
	case -ENODEV:
		dead = true;	/* killed or unplugged: no restart */
		break;
	default:
		dev_err_ratelimited(&fp->pdev->dev, "write URB error: %d\n",
				    urb->status);
		break;
	}
	/* The halt is cleared once no write URB is left on the endpoint */
	if (!dead && fp->tx_halted &&
	    fp->write_urbs_free == FTDI_WRITE_URBS_ALL)
		schedule_work(&fp->halt_work);
	if (!dead)
		ftdi_uart_tx_kick(fp);
	uart_port_unlock_irqrestore(port, flags);
}

/*
 * Clear a halted bulk endpoint (usb_clear_halt() sleeps), then restart its
 * URBs. The bulk OUT endpoint waits until no write URB is left on it.
 */
static void ftdi_uart_halt_work(struct work_struct *work)
{
	struct ftdi_uart_port *fp =
		container_of(work, struct ftdi_uart_port, halt_work);
	struct uart_port *port = &fp->port;
	bool tx, rx;
	int ret;

	uart_port_lock_irq(port);
	tx = fp->tx_halted && fp->write_urbs_free == FTDI_WRITE_URBS_ALL;
	rx = fp->rx_halted;
	uart_port_unlock_irq(port);

	if (tx) {
		ret = usb_clear_halt(fp->udev,
				     usb_sndbulkpipe(fp->udev,
						     fp->ep_out->bEndpointAddress));
		if (ret) {
			dev_err(&fp->pdev->dev, "bulk OUT halted: %d\n", ret);
			tx = false;
		}
	}
	if (rx) {
		ret = usb_clear_halt(fp->udev,
				     usb_rcvbulkpipe(fp->udev,
						     fp->ep_in->bEndpointAddress));
		if (ret) {
			dev_err(&fp->pdev->dev, "bulk IN halted: %d\n", ret);
			rx = false;
		}
	}

	uart_port_lock_irq(port);
	if (tx) {
		fp->tx_halted = false;
		ftdi_uart_tx_kick(fp);
	}
	if (rx) {
		fp->rx_halted = false;
		ftdi_uart_rx_start(fp);
	}
	uart_port_unlock_irq(port);
}

/*
 * Empty when no write URB is in flight and the chip reported TEMT after the
 * last one completed. Without RX (throttled, halted) no line status comes:
 * empty once the chip's TX buffer had the time to go out at this rate.
 */
static unsigned int ftdi_uart_tx_empty(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	unsigned long flags;
	bool empty;

	uart_port_lock_irqsave(port, &flags);
	empty = fp->write_urbs_free == FTDI_WRITE_URBS_ALL &&
		(fp->chip_temt ||
		 (!fp->rx_running && ktime_after(ktime_get(), fp->tx_drained)));
	uart_port_unlock_irqrestore(port, flags);

	return empty ? TIOCSER_TEMT : 0;
}

static u16 ftdi_uart_mctrl_value(unsigned int mctrl)
{
	return (mctrl & TIOCM_DTR ? FTDI_SIO_SET_DTR_HIGH
				  : FTDI_SIO_SET_DTR_LOW) |
	       (mctrl & TIOCM_RTS ? FTDI_SIO_SET_RTS_HIGH
				  : FTDI_SIO_SET_RTS_LOW);
}

/* Send the queued modem control states in order, then let TX go on */
static void ftdi_uart_mctrl_work(struct work_struct *work)
{
	struct ftdi_uart_port *fp =
		container_of(work, struct ftdi_uart_port, mctrl_work);
	struct uart_port *port = &fp->port;
	unsigned int mctrl;

	for (;;) {
		uart_port_lock_irq(port);
		if (fp->mctrl_tail == fp->mctrl_head) {
			fp->mctrl_busy = false;
			ftdi_uart_tx_kick(fp);
			uart_port_unlock_irq(port);
			return;
		}
		mctrl = fp->mctrl_queue[fp->mctrl_tail++ % FTDI_MCTRL_QUEUE];
		uart_port_unlock_irq(port);

		ftdi_uart_ctrl(fp, FTDI_SIO_SET_MODEM_CTRL_REQUEST,
			       ftdi_uart_mctrl_value(mctrl), fp->channel);
	}
}

/*
 * port->lock held, so the control request is sent by mctrl_work. Every
 * state is queued (a DTR pulse is two), and TX waits until they are sent.
 */
static void ftdi_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	/* Full: the newest state replaces the last one queued */
	if (fp->mctrl_head - fp->mctrl_tail == FTDI_MCTRL_QUEUE)
		fp->mctrl_head--;
	fp->mctrl_queue[fp->mctrl_head++ % FTDI_MCTRL_QUEUE] = mctrl;
	fp->mctrl_busy = true;
	schedule_work(&fp->mctrl_work);
}

static int ftdi_uart_read_modem_status(struct ftdi_uart_port *fp)
{
	u8 buf;
	int ret;

	ret = usb_control_msg_recv(fp->udev, 0,
				   FTDI_SIO_GET_MODEM_STATUS_REQUEST,
				   FTDI_SIO_REQTYPE_IN,
				   0, fp->channel, &buf, 1,
				   WDR_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	WRITE_ONCE(fp->prev_status, buf & FTDI_STATUS_B0_MASK);
	return 0;
}

static unsigned int ftdi_uart_get_mctrl(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	unsigned int mctrl = 0;
	unsigned char status = READ_ONCE(fp->prev_status);

	if (status & FTDI_RS0_CTS)
		mctrl |= TIOCM_CTS;
	if (status & FTDI_RS0_DSR)
		mctrl |= TIOCM_DSR;
	if (status & FTDI_RS0_RI)
		mctrl |= TIOCM_RI;
	if (status & FTDI_RS0_RLSD)
		mctrl |= TIOCM_CD;

	return mctrl;
}

/*
 * Called by serial_core with port->lock held. URBs already submitted
 * cannot be called back, but nothing more leaves the transmit FIFO.
 */
static void ftdi_uart_stop_tx(struct uart_port *port)
{
	to_ftdi_uart_port(port)->tx_stopped = true;
}

static void ftdi_uart_start_tx(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	fp->tx_stopped = false;
	ftdi_uart_tx_kick(fp);
}

/* port->lock held: the read URB is not resubmitted, shutdown kills it */
static void ftdi_uart_stop_rx(struct uart_port *port)
{
	to_ftdi_uart_port(port)->rx_stopped = true;
}

static void ftdi_uart_throttle(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	guard(uart_port_lock_irqsave)(port);
	fp->rx_throttled = true;
}

static void ftdi_uart_unthrottle(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	guard(uart_port_lock_irqsave)(port);
	fp->rx_throttled = false;
	ftdi_uart_rx_start(fp);
}

/* Flow control of the chip from the saved termios, port mutex held */
static int ftdi_uart_set_flow(struct ftdi_uart_port *fp)
{
	u16 value = 0, index;

	/* DTR/DSR flow control has no termios flag: sysfs dtr_dsr_flow */
	if (fp->cflag & CRTSCTS) {
		index = FTDI_SIO_RTS_CTS_HS;
	} else if (fp->dtr_dsr_flow) {
		index = FTDI_SIO_DTR_DSR_HS;
	} else if (fp->iflag & IXON) {
		index = FTDI_SIO_XON_XOFF_HS;
		value = fp->xchars;
	} else {
		index = FTDI_SIO_DISABLE_FLOW_CTRL;
	}

	return ftdi_uart_ctrl(fp, FTDI_SIO_SET_FLOW_CTRL_REQUEST, value,
			      index | fp->channel);
}

static int ftdi_uart_startup(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	struct tty_struct *tty;
	bool throttled;
	int ret;

	ret = ftdi_uart_ctrl(fp, FTDI_SIO_RESET_REQUEST, FTDI_SIO_RESET_SIO,
			     fp->channel);
	if (ret < 0)
		return ret;

	ret = ftdi_uart_ctrl(fp, FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
			     fp->latency_timer, fp->channel);
	if (ret < 0)
		return ret;

	ftdi_uart_read_modem_status(fp);
	fp->break_on = false;

	/* Resume runs startup too: n_tty may still hold the port throttled */
	tty = tty_port_tty_get(&port->state->port);
	throttled = tty && tty_throttled(tty);
	tty_kref_put(tty);

	usb_fill_bulk_urb(fp->read_urb, fp->udev,
			  usb_rcvbulkpipe(fp->udev,
					  fp->ep_in->bEndpointAddress),
			  fp->read_buf,
			  usb_endpoint_maxp(fp->ep_in),
			  ftdi_uart_read_complete, fp);

	uart_port_lock_irq(port);
	fp->running = true;
	fp->tx_stopped = false;
	fp->tx_held = false;
	fp->tx_halted = false;
	fp->rx_stopped = false;
	fp->rx_throttled = throttled;
	fp->rx_halted = false;
	fp->chip_temt = true;
	fp->rx_running = !throttled;
	uart_port_unlock_irq(port);

	if (throttled)
		return 0;

	ret = usb_submit_urb(fp->read_urb, GFP_KERNEL);
	if (ret) {
		uart_port_lock_irq(port);
		fp->running = false;
		fp->rx_running = false;
		uart_port_unlock_irq(port);
		dev_err(&fp->pdev->dev, "read URB submit failed: %d\n", ret);
	}

	return ret;
}

static void ftdi_uart_shutdown(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	int i;

	/* Nothing is submitted any more, completions included */
	uart_port_lock_irq(port);
	fp->running = false;
	uart_port_unlock_irq(port);

	usb_kill_urb(fp->read_urb);
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_kill_urb(fp->write_urb[i]);
	/* Queued by a completion until then: it restarts nothing now */
	cancel_work_sync(&fp->halt_work);

	if (fp->break_on) {
		ftdi_uart_ctrl(fp, FTDI_SIO_SET_DATA_REQUEST, fp->data_value,
			       fp->channel);
		fp->break_on = false;
	}

	/*
	 * serial_core lowered DTR/RTS (HUPCL, hangup) just before calling
	 * us: let that request reach the chip instead of dropping it.
	 */
	flush_work(&fp->mctrl_work);
}

static void ftdi_uart_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  const struct ktermios *old)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	speed_t baud;
	u16 data_value;

	/* "UART Interface supports 7/8 bit data" (DS_FT232H, DS_FT2232H) */
	if ((termios->c_cflag & CSIZE) != CS7 &&
	    (termios->c_cflag & CSIZE) != CS8) {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= old && (old->c_cflag & CSIZE) == CS7 ?
				    CS7 : CS8;
	}
	data_value = (termios->c_cflag & CSIZE) == CS7 ? 7 : 8;

	if (termios->c_cflag & PARENB) {
		if (termios->c_cflag & CMSPAR)
			data_value |= (termios->c_cflag & PARODD) ?
				FTDI_SIO_SET_DATA_PARITY_MARK :
				FTDI_SIO_SET_DATA_PARITY_SPACE;
		else if (termios->c_cflag & PARODD)
			data_value |= FTDI_SIO_SET_DATA_PARITY_ODD;
		else
			data_value |= FTDI_SIO_SET_DATA_PARITY_EVEN;
	} else {
		data_value |= FTDI_SIO_SET_DATA_PARITY_NONE;
	}

	if (termios->c_cflag & CSTOPB)
		data_value |= FTDI_SIO_SET_DATA_STOP_BITS_2;
	else
		data_value |= FTDI_SIO_SET_DATA_STOP_BITS_1;

	baud = ftdi_uart_hs_baud(uart_get_baud_rate(port, termios, old, 300,
						    12000000));
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
	ftdi_uart_set_baudrate(fp, baud);

	fp->data_value = data_value;
	ftdi_uart_ctrl(fp, FTDI_SIO_SET_DATA_REQUEST,
		       data_value | (fp->break_on ? FTDI_SIO_SET_BREAK : 0),
		       fp->channel);

	/* With CRTSCTS the chip gates TX on CTS and drops RTS by itself */
	uart_port_lock_irq(port);
	uart_update_timeout(port, termios->c_cflag, baud);
	fp->rx_discard = !(termios->c_cflag & CREAD);
	if (termios->c_cflag & CRTSCTS)
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
	else
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	uart_port_unlock_irq(port);

	fp->cflag = termios->c_cflag;
	fp->iflag = termios->c_iflag;
	fp->xchars = (termios->c_cc[VSTOP] << 8) | termios->c_cc[VSTART];
	ftdi_uart_set_flow(fp);
}

/* Port mutex held */
static void ftdi_uart_break_ctl(struct uart_port *port, int break_state)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	u16 value = fp->data_value;

	if (break_state)
		value |= FTDI_SIO_SET_BREAK;
	if (ftdi_uart_ctrl(fp, FTDI_SIO_SET_DATA_REQUEST, value,
			   fp->channel) >= 0)
		fp->break_on = break_state;
}

static const char *ftdi_uart_type(struct uart_port *port)
{
	return "FTDI_MPSSE";
}

static void ftdi_uart_config_port(struct uart_port *port, int flags)
{
	/*
	 * uart_port_startup() bails out on PORT_UNKNOWN (0): any non-zero
	 * type will do (same pattern as liteuart).
	 */
	if (flags & UART_CONFIG_TYPE)
		port->type = 1;
}

static int ftdi_uart_verify_port(struct uart_port *port,
				 struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != 1)
		return -EINVAL;
	return 0;
}

static const char *ftdi_uart_txden_hint(struct ftdi_uart_port *fp)
{
	switch (fp->chip_type) {
	case FTDI_CHIP_FT232H:
	case FTDI_CHIP_FT232HP:
	case FTDI_CHIP_FT233HP:
		return "give ACBUS0-6, 8 or 9 the TXDEN function in the EEPROM";
	case FTDI_CHIP_FT4232H:
	case FTDI_CHIP_FT4232HA:
	case FTDI_CHIP_FT4232HP:
	case FTDI_CHIP_FT4233HP:
		return "select TXDEN instead of RI# for this channel in the EEPROM";
	default:
		return "not available on this chip";
	}
}

/*
 * RS-485: the chip asserts TXDEN by itself while it transmits, on the pin
 * the platform data names (FT232H: an ACBUS pin chosen in the EEPROM,
 * FT2232H: xCBUS0, FT4232H: xDBUS7 instead of RI#). Nothing is
 * programmed here, the driver only checks that the channel has the pin.
 */
static int ftdi_uart_rs485_config(struct uart_port *port,
				  struct ktermios *termios,
				  struct serial_rs485 *rs485)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	if (rs485->flags & SER_RS485_ENABLED) {
		if (!fp->txden_pin) {
			dev_notice_ratelimited(&fp->pdev->dev,
					       "RS-485: no TXDEN pin (%s)\n",
					       ftdi_uart_txden_hint(fp));
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static const struct uart_ops ftdi_uart_ops = {
	.tx_empty	= ftdi_uart_tx_empty,
	.set_mctrl	= ftdi_uart_set_mctrl,
	.get_mctrl	= ftdi_uart_get_mctrl,
	.stop_tx	= ftdi_uart_stop_tx,
	.start_tx	= ftdi_uart_start_tx,
	.throttle	= ftdi_uart_throttle,
	.unthrottle	= ftdi_uart_unthrottle,
	.stop_rx	= ftdi_uart_stop_rx,
	.startup	= ftdi_uart_startup,
	.shutdown	= ftdi_uart_shutdown,
	.set_termios	= ftdi_uart_set_termios,
	.break_ctl	= ftdi_uart_break_ctl,
	.type		= ftdi_uart_type,
	.config_port	= ftdi_uart_config_port,
	.verify_port	= ftdi_uart_verify_port,
};

/*
 * FT232H CBUS GPIO: one SET_BITMODE request in bit mode
 * FTDI_SIO_BITMODE_CBUS carries the direction and value of the four pins,
 * READ_PINS reads them. The EEPROM says which pins are GPIOs (I/O mode).
 * cbus_lock held; the cache follows only what the chip took.
 */
static int ftdi_cbus_write(struct ftdi_uart_port *fp, u8 mode, u8 output,
			   u8 value)
{
	u16 val = (mode << 8) | (output << 4) | value;
	int ret;

	ret = usb_control_msg(fp->udev, usb_sndctrlpipe(fp->udev, 0),
			      FTDI_SIO_SET_BITMODE_REQUEST,
			      FTDI_SIO_REQTYPE_OUT,
			      val, fp->channel, NULL, 0, WDR_TIMEOUT);
	if (ret < 0)
		return ret;

	fp->cbus_output = output;
	fp->cbus_value = value;
	fp->cbus_used = mode == FTDI_SIO_BITMODE_CBUS;
	return 0;
}

static int ftdi_cbus_read_pins(struct ftdi_uart_port *fp)
{
	u8 buf;
	int ret;

	ret = usb_control_msg_recv(fp->udev, 0,
				   FTDI_SIO_READ_PINS_REQUEST,
				   FTDI_SIO_REQTYPE_IN,
				   0, fp->channel, &buf, 1,
				   WDR_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	return buf;
}

/*
 * READ_PINS gives the CBUS pins only in CBUS bit-bang mode: the first
 * request enters it, every pin an input (as ftdi_sio does).
 */
static int ftdi_cbus_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);

	guard(mutex)(&fp->cbus_lock);
	if (fp->cbus_used)
		return 0;
	return ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS, 0, 0);
}

static int ftdi_cbus_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	int ret;

	ret = ftdi_cbus_read_pins(fp);
	if (ret < 0)
		return ret;

	return !!(ret & BIT(offset));
}

static int ftdi_cbus_gpio_get_multiple(struct gpio_chip *gc,
				       unsigned long *mask,
				       unsigned long *bits)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	int ret;

	ret = ftdi_cbus_read_pins(fp);
	if (ret < 0)
		return ret;

	*bits = ret & *mask;

	return 0;
}

static int ftdi_cbus_gpio_set(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	u8 val;

	guard(mutex)(&fp->cbus_lock);
	val = value ? fp->cbus_value | BIT(offset)
		    : fp->cbus_value & ~BIT(offset);
	return ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS, fp->cbus_output, val);
}

static int ftdi_cbus_gpio_set_multiple(struct gpio_chip *gc,
				       unsigned long *mask,
				       unsigned long *bits)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	u8 val;

	guard(mutex)(&fp->cbus_lock);
	val = (fp->cbus_value & ~*mask) | (*bits & *mask);
	return ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS, fp->cbus_output, val);
}

static int ftdi_cbus_gpio_direction_input(struct gpio_chip *gc,
					  unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);

	guard(mutex)(&fp->cbus_lock);
	return ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS,
			       fp->cbus_output & ~BIT(offset), fp->cbus_value);
}

static int ftdi_cbus_gpio_direction_output(struct gpio_chip *gc,
					   unsigned int offset, int value)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	u8 val;

	guard(mutex)(&fp->cbus_lock);
	val = value ? fp->cbus_value | BIT(offset)
		    : fp->cbus_value & ~BIT(offset);
	return ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS,
			       fp->cbus_output | BIT(offset), val);
}

static int ftdi_cbus_gpio_get_direction(struct gpio_chip *gc,
					unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);

	guard(mutex)(&fp->cbus_lock);
	return (fp->cbus_output & BIT(offset)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ftdi_cbus_gpio_init_valid_mask(struct gpio_chip *gc,
					  unsigned long *valid_mask,
					  unsigned int ngpios)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	unsigned long map = fp->cbus_valid;

	bitmap_copy(valid_mask, &map, ngpios);

	return 0;
}

/* Non-fatal: without a usable EEPROM or an I/O mode pin, no CBUS GPIO */
static void ftdi_cbus_gpio_register(struct ftdi_uart_port *fp)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_get_eeprom(fp->pdev);
	struct device *dev = &fp->pdev->dev;
	struct gpio_chip *gc = &fp->cbus_gc;
	int i, ret;

	if (fp->chip_type != FTDI_CHIP_FT232H || !ee ||
	    ee->num_cbus != ARRAY_SIZE(ee->cbus))
		return;

	for (i = 0; i < FTDI_CBUS_NGPIO; i++)
		if (ee->cbus[ftdi_cbus_acbus[i]] == FTDI_CBUSH_IOMODE)
			fp->cbus_valid |= BIT(i);
	if (!fp->cbus_valid)
		return;

	gc->label = devm_kasprintf(dev, GFP_KERNEL, "ftdi-cbus-%s",
				   dev_name(dev->parent));
	if (!gc->label)
		return;
	gc->owner = THIS_MODULE;
	gc->parent = dev;
	gc->request = ftdi_cbus_gpio_request;
	gc->get_direction = ftdi_cbus_gpio_get_direction;
	gc->direction_input = ftdi_cbus_gpio_direction_input;
	gc->direction_output = ftdi_cbus_gpio_direction_output;
	gc->get = ftdi_cbus_gpio_get;
	gc->set = ftdi_cbus_gpio_set;
	gc->get_multiple = ftdi_cbus_gpio_get_multiple;
	gc->set_multiple = ftdi_cbus_gpio_set_multiple;
	gc->init_valid_mask = ftdi_cbus_gpio_init_valid_mask;
	gc->base = -1;
	gc->ngpio = FTDI_CBUS_NGPIO;
	gc->can_sleep = true;

	ret = gpiochip_add_data(gc, fp);
	if (ret) {
		dev_warn(dev, "failed to register CBUS GPIO: %d\n", ret);
		return;
	}

	fp->cbus_registered = true;
	dev_info(dev, "CBUS GPIO: %d pins (valid mask 0x%02x)\n",
		 FTDI_CBUS_NGPIO, fp->cbus_valid);
}

/*
 * Leaving CBUS bit-bang mode does not release the pins (ftdi_sio says so):
 * every pin an input first, then out of the mode.
 */
static void ftdi_cbus_gpio_unregister(struct ftdi_uart_port *fp)
{
	if (!fp->cbus_registered)
		return;

	gpiochip_remove(&fp->cbus_gc);
	fp->cbus_registered = false;

	guard(mutex)(&fp->cbus_lock);
	if (fp->cbus_used) {
		ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS, 0, 0);
		ftdi_cbus_write(fp, FTDI_SIO_BITMODE_RESET, 0, 0);
	}
}

static ssize_t latency_timer_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(fp->latency_timer));
}

static ssize_t latency_timer_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;
	if (val < 1)
		return -EINVAL;

	guard(mutex)(&fp->port.state->port.mutex);
	ret = ftdi_uart_ctrl(fp, FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
			     val, fp->channel);
	if (ret < 0)
		return ret;

	WRITE_ONCE(fp->latency_timer, val);
	return count;
}
static DEVICE_ATTR_RW(latency_timer);

static ssize_t dtr_dsr_flow_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(fp->dtr_dsr_flow));
}

/* Taken at once by an open port */
static ssize_t dtr_dsr_flow_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	struct tty_port *tport = &fp->port.state->port;
	bool val, old;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	guard(mutex)(&tport->mutex);
	old = fp->dtr_dsr_flow;
	WRITE_ONCE(fp->dtr_dsr_flow, val);
	if (tty_port_initialized(tport)) {
		ret = ftdi_uart_set_flow(fp);
		if (ret < 0) {
			WRITE_ONCE(fp->dtr_dsr_flow, old);
			return ret;
		}
	}
	return count;
}
static DEVICE_ATTR_RW(dtr_dsr_flow);

static struct attribute *ftdi_uart_attrs[] = {
	&dev_attr_latency_timer.attr,
	&dev_attr_dtr_dsr_flow.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ftdi_uart);

/*
 * "Pull down I/O pins in USB suspend" (default EEPROM configuration,
 * DS_FT232H 7.2, DS_FT2232H and DS_FT4232H 7.1) pulls TXD low while the
 * chip sleeps: a break for the other end.
 */
static void ftdi_uart_check_eeprom(struct ftdi_uart_port *fp)
{
	const struct ftdi_eeprom *ee = ftdi_mpsse_get_eeprom(fp->pdev);

	if (ee && ee->pulldown)
		dev_warn(&fp->pdev->dev,
			 "EEPROM: I/O pins pulled down in USB suspend: TXD sends a break while the chip sleeps\n");
}

/*
 * serial_core makes every tty wakeup-capable, for ports with an IRQ. With
 * wakeup enabled, a system sleep would leave this one running (URBs killed
 * by usbcore, never restarted) and call enable_irq_wake() on IRQ 0. The
 * tty is found among the descendants of the platform device by its devt.
 */
static int ftdi_uart_tty_no_wakeup(struct device *dev, void *devt)
{
	if (dev->devt == *(dev_t *)devt) {
		/* Disabled first: wakeup may have been enabled already */
		device_init_wakeup(dev, false);
		return 1;
	}
	return device_for_each_child(dev, devt, ftdi_uart_tty_no_wakeup);
}

static int ftdi_uart_plat_probe(struct platform_device *pdev)
{
	const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct tty_driver *tty_drv = ftdi_uart_driver.tty_driver;
	struct ftdi_uart_port *fp;
	struct uart_port *port;
	dev_t devt;
	int ret, i;

	if (!pdata)
		return -EINVAL;

	fp = devm_kzalloc(&pdev->dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;

	fp->pdev = pdev;
	fp->udev = ftdi_mpsse_get_udev(pdev);
	fp->channel = pdata->channel;
	fp->chip_type = pdata->chip_type;
	fp->chip_fifo = pdata->fifo_size;
	fp->latency_timer = 16;
	fp->data_value = 8 | FTDI_SIO_SET_DATA_PARITY_NONE |
			 FTDI_SIO_SET_DATA_STOP_BITS_1;
	fp->write_urbs_free = FTDI_WRITE_URBS_ALL;
	fp->chip_temt = true;
	mutex_init(&fp->cbus_lock);
	INIT_WORK(&fp->mctrl_work, ftdi_uart_mctrl_work);
	INIT_WORK(&fp->halt_work, ftdi_uart_halt_work);

	if (!fp->udev)
		return -ENODEV;

	ret = ftdi_mpsse_get_endpoints(pdev, &fp->ep_in, &fp->ep_out);
	if (ret)
		return ret;

	fp->max_packet_size = usb_endpoint_maxp(fp->ep_in);

	fp->read_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!fp->read_urb)
		return -ENOMEM;

	for (i = 0; i < FTDI_WRITE_URBS; i++) {
		fp->write_urb[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!fp->write_urb[i]) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}
	}

	fp->read_buf = devm_kmalloc(&pdev->dev, fp->max_packet_size,
				    GFP_KERNEL);
	if (!fp->read_buf) {
		ret = -ENOMEM;
		goto err_free_urbs;
	}

	for (i = 0; i < FTDI_WRITE_URBS; i++) {
		fp->write_buf[i] = devm_kmalloc(&pdev->dev, FTDI_WRITE_BUF,
						GFP_KERNEL);
		if (!fp->write_buf[i]) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}
	}

	ret = ida_alloc_max(&ftdi_uart_ida, FTDI_UART_NR - 1, GFP_KERNEL);
	if (ret < 0)
		goto err_free_urbs;

	port = &fp->port;
	port->line = ret;
	port->dev = &pdev->dev;
	port->iotype = UPIO_BUS;
	/* baud_base 12 Mbaud, unchangeable: TIOCSSERIAL then succeeds */
	port->uartclk = 12000000 * 16;
	/* The chip does XON/XOFF: termios changes of IXON reach it */
	port->flags = UPF_BOOT_AUTOCONF | UPF_SOFT_FLOW | UPF_FIXED_PORT;
	/* throttle/unthrottle are always called, see ftdi_uart_rx_start() */
	port->status = UPSTAT_SYNC_FIFO;
	port->ops = &ftdi_uart_ops;
	/*
	 * serial_core waits for tx_empty (close, tcdrain, break) for up to
	 * twice the time fifosize characters take: count all that can be
	 * on its way out, the write URBs plus the chip's TX buffer.
	 */
	port->fifosize = FTDI_WRITE_URBS * FTDI_WRITE_BUF + fp->chip_fifo;
	port->type = PORT_UNKNOWN;
	port->rs485_config = ftdi_uart_rs485_config;
	port->rs485_supported.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;

	platform_set_drvdata(pdev, fp);

	/* Whether RS-485 can be enabled is known before the tty is visible */
	fp->txden_pin = pdata->txden_pin;
	ftdi_uart_check_eeprom(fp);

	ret = uart_add_one_port(&ftdi_uart_driver, port);
	if (ret)
		goto err_ida;

	devt = MKDEV(tty_drv->major, tty_drv->minor_start) + port->line;
	device_for_each_child(&pdev->dev, &devt, ftdi_uart_tty_no_wakeup);

	ftdi_cbus_gpio_register(fp);

	dev_info(&pdev->dev, "FTDI MPSSE UART: /dev/ttyFTDI%u\n", port->line);
	if (fp->txden_pin)
		dev_info(&pdev->dev, "RS-485: TXDEN on %s\n", fp->txden_pin);
	return 0;

err_ida:
	ida_free(&ftdi_uart_ida, port->line);
err_free_urbs:
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_free_urb(fp->write_urb[i]);
	usb_free_urb(fp->read_urb);
	return ret;
}

static void ftdi_uart_plat_remove(struct platform_device *pdev)
{
	struct ftdi_uart_port *fp = platform_get_drvdata(pdev);
	int i;

	ftdi_cbus_gpio_unregister(fp);

	/*
	 * Its hangup waits for a close that drains the output, which needs
	 * the URBs, then shuts the port down: URBs killed, halt_work
	 * cancelled. It also calls set_mctrl, which queues mctrl_work.
	 */
	uart_remove_one_port(&ftdi_uart_driver, &fp->port);

	usb_poison_urb(fp->read_urb);
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_poison_urb(fp->write_urb[i]);
	cancel_work_sync(&fp->halt_work);
	cancel_work_sync(&fp->mctrl_work);
	ida_free(&ftdi_uart_ida, fp->port.line);

	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_free_urb(fp->write_urb[i]);
	usb_free_urb(fp->read_urb);
}

/*
 * serial_core stops an open port (TX drained, shutdown) and on resume
 * runs startup + set_termios + modem lines again, which re-programs the
 * latency timer, line settings and flow control the chip lost.
 */
static int ftdi_uart_suspend(struct device *dev)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	struct uart_port *port = &fp->port;
	unsigned long expire;

	/*
	 * uart_suspend_port() shuts the port down after 30 ms, which kills
	 * the write URBs and loses their data. Stop feeding them (the
	 * rest stays in the transmit FIFO for resume) and let what is on
	 * its way go out first.
	 */
	if (tty_port_initialized(&port->state->port)) {
		uart_port_lock_irq(port);
		fp->tx_held = true;
		uart_port_unlock_irq(port);

		expire = jiffies + min(2 * uart_fifo_timeout(port),
				       msecs_to_jiffies(FTDI_SUSPEND_DRAIN_MS));
		while (!ftdi_uart_tx_empty(port) &&
		       time_before(jiffies, expire))
			msleep(20);
	}

	return uart_suspend_port(&ftdi_uart_driver, port);
}

static int ftdi_uart_resume(struct device *dev)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	int ret;

	ret = uart_resume_port(&ftdi_uart_driver, &fp->port);
	if (ret)
		return ret;

	/* The parent's resume left bit mode 0: re-enter CBUS bit-bang */
	guard(mutex)(&fp->cbus_lock);
	if (fp->cbus_used)
		ret = ftdi_cbus_write(fp, FTDI_SIO_BITMODE_CBUS,
				      fp->cbus_output, fp->cbus_value);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ftdi_uart_pm_ops,
				ftdi_uart_suspend, ftdi_uart_resume);

static struct platform_driver ftdi_uart_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.dev_groups = ftdi_uart_groups,
		.pm = pm_sleep_ptr(&ftdi_uart_pm_ops),
	},
	.probe = ftdi_uart_plat_probe,
	.remove = ftdi_uart_plat_remove,
};

static int __init ftdi_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&ftdi_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&ftdi_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&ftdi_uart_driver);

	return ret;
}

static void __exit ftdi_uart_exit(void)
{
	platform_driver_unregister(&ftdi_uart_platform_driver);
	uart_unregister_driver(&ftdi_uart_driver);
}

module_init(ftdi_uart_init);
module_exit(ftdi_uart_exit);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("FTDI MPSSE UART child driver");
MODULE_LICENSE("GPL");
