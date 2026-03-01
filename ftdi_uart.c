// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * FTDI MPSSE UART child driver
 *
 * serial_core uart_port driver for FTDI Hi-Speed devices, registered as
 * a child of ftdi_mpsse.  Creates /dev/ttyFTDIx devices.
 *
 * Baud rate divisor calculation adapted from the FTDI baud rate encoding
 * documented in FTDI AN_120 and the D2XX Programmer's Guide.
 *
 * References: AN_120, D2XX Programmer's Guide, liteuart.c (serial_core pattern)
 */

#include <linux/cleanup.h>
#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/usb.h>
#include <linux/xarray.h>

#include "ftdi_mpsse.h"
#include "ftdi_eeprom.h"

#define DRIVER_NAME	"ftdi-uart"
#define FTDI_UART_NR	16	/* max simultaneous ports */
#define FTDI_UART_FIFO	512	/* virtual FIFO depth for tx_empty */
#define FTDI_WRITE_URBS	2	/* double-buffered writes */

#define WDR_TIMEOUT		5000
#define WDR_SHORT_TIMEOUT	1000

/* FT232H CBUS GPIO (4 pins: ACBUS5, ACBUS6, ACBUS8, ACBUS9) */
#define FTDI_CBUS_NGPIO		4
#define FTDI_CBUS_EEPROM_ADDR	0x1a
#define FTDI_CBUS_EEPROM_LEN	4

/* Line status error mask */
#define FTDI_RS_ERR_MASK (FTDI_RS_BI | FTDI_RS_PE | FTDI_RS_FE | FTDI_RS_OE)
#define FTDI_STATUS_B0_MASK \
	(FTDI_RS0_CTS | FTDI_RS0_DSR | FTDI_RS0_RI | FTDI_RS0_RLSD)

struct ftdi_uart_port {
	struct uart_port port;
	struct platform_device *pdev;
	struct usb_device *udev;
	u8 channel;
	enum ftdi_mpsse_chip_type chip_type;

	struct usb_endpoint_descriptor *ep_in;
	struct usb_endpoint_descriptor *ep_out;

	struct urb *read_urb;
	u8 *read_buf;
	struct urb *write_urb[FTDI_WRITE_URBS];
	u8 *write_buf[FTDI_WRITE_URBS];

	u16 max_packet_size;
	u16 last_set_data_value;
	u8 prev_status;
	int tx_outstanding;		/* count of in-flight write URBs */

	struct work_struct mctrl_work;
	unsigned int pending_mctrl;

	spinlock_t write_lock;		/* protects tx_outstanding */

	u8 latency_timer;		/* ms, 1-255 */
	bool dtr_dsr_flow;		/* DTR/DSR hardware flow control */
	bool has_txden;			/* EEPROM has TXDEN on a CBUS pin */

	/* EEPROM drive settings cached for baud-rate checks */
	bool ee_checked;		/* EEPROM check performed */
	bool ee_slow_slew;		/* ADBUS slew rate setting */
	bool slew_warned;		/* baud-dependent slew warning issued */

	/* CBUS GPIO */
	struct gpio_chip cbus_gc;
	struct mutex cbus_lock;		/* protects cbus_output, cbus_value */
	u8 cbus_altfunc;		/* bitmask: pins NOT configured for GPIO */
	u8 cbus_output;			/* pin direction: 1 = output */
	u8 cbus_value;			/* output values */
	bool cbus_registered;
};

#define to_ftdi_uart_port(p) container_of(p, struct ftdi_uart_port, port)

/* Global uart_driver and ID allocation */
static struct uart_driver ftdi_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "ftdi_uart",
	.dev_name	= "ttyFTDI",
	.major		= 0,		/* dynamic */
	.minor		= 0,
	.nr		= FTDI_UART_NR,
};

static DEFINE_XARRAY_ALLOC(ftdi_uart_xa);

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

static u32 ftdi_uart_get_divisor(struct ftdi_uart_port *fp, speed_t baud)
{
	/*
	 * All MPSSE-capable chips are Hi-Speed (FT232H and later).
	 * base clock = 120 MHz, but sub-1200 baud falls back to BM formula.
	 */
	if (baud >= 1200 && baud <= 12000000)
		return ftdi_2232h_baud_base_to_divisor(baud, 120000000);
	if (baud < 1200)
		return ftdi_232bm_baud_base_to_divisor(baud, 48000000);

	/* Out of range -- default 9600 */
	return ftdi_232bm_baud_base_to_divisor(9600, 48000000);
}

static int ftdi_uart_set_baudrate(struct ftdi_uart_port *fp, speed_t baud)
{
	u32 index_value;
	u16 value, index;
	int ret;

	if (!baud)
		baud = 9600;

	index_value = ftdi_uart_get_divisor(fp, baud);
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

static void ftdi_uart_process_packet(struct ftdi_uart_port *fp,
				     unsigned char *buf, int len)
{
	struct uart_port *port = &fp->port;
	unsigned char status, prev;
	char flag;

	if (len < 2)
		return;

	status = buf[0] & FTDI_STATUS_B0_MASK;
	prev = READ_ONCE(fp->prev_status);
	if (status != prev) {
		if ((status ^ prev) & FTDI_RS0_CTS)
			port->icount.cts++;
		if ((status ^ prev) & FTDI_RS0_DSR)
			port->icount.dsr++;
		if ((status ^ prev) & FTDI_RS0_RI)
			port->icount.rng++;
		if ((status ^ prev) & FTDI_RS0_RLSD)
			port->icount.dcd++;

		wake_up_interruptible(&port->state->port.delta_msr_wait);
		WRITE_ONCE(fp->prev_status, status);
	}

	if (len == 2)
		return;		/* status only, no data */

	flag = TTY_NORMAL;
	if (buf[1] & FTDI_RS_ERR_MASK) {
		if (buf[1] & FTDI_RS_BI) {
			port->icount.brk++;
			flag = TTY_BREAK;
		} else if (buf[1] & FTDI_RS_PE) {
			port->icount.parity++;
			flag = TTY_PARITY;
		} else if (buf[1] & FTDI_RS_FE) {
			port->icount.frame++;
			flag = TTY_FRAME;
		}
		if (buf[1] & FTDI_RS_OE) {
			port->icount.overrun++;
			tty_insert_flip_char(&port->state->port, 0,
					     TTY_OVERRUN);
		}
	}

	port->icount.rx += len - 2;

	tty_insert_flip_string_fixed_flag(&port->state->port,
					  buf + 2, flag, len - 2);
}

static void ftdi_uart_read_complete(struct urb *urb)
{
	struct ftdi_uart_port *fp = urb->context;
	unsigned char *data = urb->transfer_buffer;
	int i, len;
	int ret;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
		    urb->status != -ESHUTDOWN && urb->status != -EPERM)
			dev_err(&fp->pdev->dev, "read URB error: %d\n",
				urb->status);
		return;
	}

	for (i = 0; i < urb->actual_length; i += fp->max_packet_size) {
		len = min_t(int, urb->actual_length - i, fp->max_packet_size);
		ftdi_uart_process_packet(fp, &data[i], len);
	}

	tty_flip_buffer_push(&fp->port.state->port);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -ENODEV && ret != -EPERM)
		dev_err(&fp->pdev->dev, "read URB resubmit failed: %d\n", ret);
}

static void ftdi_uart_write_complete(struct urb *urb)
{
	struct ftdi_uart_port *fp = urb->context;

	if (urb->status) {
		if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
		    urb->status != -ESHUTDOWN && urb->status != -EPERM)
			dev_err(&fp->pdev->dev, "write URB error: %d\n",
				urb->status);
	}

	scoped_guard(spinlock_irqsave, &fp->write_lock)
		fp->tx_outstanding--;

	uart_write_wakeup(&fp->port);
}

static unsigned int ftdi_uart_tx_empty(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	return READ_ONCE(fp->tx_outstanding) == 0 ? TIOCSER_TEMT : 0;
}

/*
 * set_mctrl is called by serial_core under port->lock (spinlock), so we
 * cannot issue USB control messages directly.  Defer to a workqueue.
 */
static void ftdi_uart_mctrl_work(struct work_struct *work)
{
	struct ftdi_uart_port *fp =
		container_of(work, struct ftdi_uart_port, mctrl_work);
	unsigned int mctrl = READ_ONCE(fp->pending_mctrl);
	u16 value = 0;

	if (mctrl & TIOCM_DTR)
		value |= FTDI_SIO_SET_DTR_HIGH;
	else
		value |= FTDI_SIO_SET_DTR_LOW;

	if (mctrl & TIOCM_RTS)
		value |= FTDI_SIO_SET_RTS_HIGH;
	else
		value |= FTDI_SIO_SET_RTS_LOW;

	ftdi_uart_ctrl(fp, FTDI_SIO_SET_MODEM_CTRL_REQUEST, value, fp->channel);
}

static void ftdi_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	WRITE_ONCE(fp->pending_mctrl, mctrl);
	schedule_work(&fp->mctrl_work);
}

/*
 * Read live modem status via USB control transfer.  Updates the
 * cached prev_status so subsequent reads reflect the current state.
 * Returns negative on error.
 */
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

static void ftdi_uart_stop_tx(struct uart_port *port)
{
	/* Nothing to do -- USB bulk OUT is fire-and-forget */
}

static void ftdi_uart_start_tx(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	struct tty_port *tport = &port->state->port;
	int idx, count, ret;

	scoped_guard(spinlock_irqsave, &fp->write_lock) {
		if (fp->tx_outstanding >= FTDI_WRITE_URBS)
			return;
		idx = fp->tx_outstanding;
		fp->tx_outstanding++;
	}

	count = kfifo_out(&tport->xmit_fifo, fp->write_buf[idx],
			  usb_endpoint_maxp(fp->ep_out));
	if (count == 0) {
		scoped_guard(spinlock_irqsave, &fp->write_lock)
			fp->tx_outstanding--;
		return;
	}

	port->icount.tx += count;

	usb_fill_bulk_urb(fp->write_urb[idx], fp->udev,
			  usb_sndbulkpipe(fp->udev,
					  fp->ep_out->bEndpointAddress),
			  fp->write_buf[idx], count,
			  ftdi_uart_write_complete, fp);

	ret = usb_submit_urb(fp->write_urb[idx], GFP_ATOMIC);
	if (ret) {
		dev_err(&fp->pdev->dev, "write URB submit failed: %d\n", ret);
		scoped_guard(spinlock_irqsave, &fp->write_lock)
			fp->tx_outstanding--;
	}
}

static void ftdi_uart_stop_rx(struct uart_port *port)
{
	/*
	 * Called under port->lock (spinlock) -- cannot sleep.
	 * The read URB will be killed in shutdown() which runs
	 * under the port mutex where sleeping is permitted.
	 */
}

static int ftdi_uart_startup(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
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

	usb_fill_bulk_urb(fp->read_urb, fp->udev,
			  usb_rcvbulkpipe(fp->udev,
					  fp->ep_in->bEndpointAddress),
			  fp->read_buf,
			  usb_endpoint_maxp(fp->ep_in),
			  ftdi_uart_read_complete, fp);

	ret = usb_submit_urb(fp->read_urb, GFP_KERNEL);
	if (ret)
		dev_err(&fp->pdev->dev, "read URB submit failed: %d\n", ret);

	return ret;
}

static void ftdi_uart_shutdown(struct uart_port *port)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	int i;

	usb_kill_urb(fp->read_urb);
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_kill_urb(fp->write_urb[i]);
	cancel_work_sync(&fp->mctrl_work);
}

static void ftdi_uart_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  const struct ktermios *old)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	speed_t baud;
	u16 data_value;

	baud = uart_get_baud_rate(port, termios, old, 300, 12000000);
	ftdi_uart_set_baudrate(fp, baud);
	uart_update_timeout(port, termios->c_cflag, baud);

	/*
	 * Data bits: FTDI Hi-Speed chips support 7 and 8 data bits.
	 * CS5 is accepted (smartcard reader quirk) but maps to 8 bits
	 * on hardware.  CS6 is unsupported and falls back to CS8.
	 */
	if ((termios->c_cflag & CSIZE) == CS6) {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= old ? (old->c_cflag & CSIZE) : CS8;
	}

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		data_value = 8;	/* CS5 quirk: accepted but maps to 8-bit */
		break;
	case CS7:
		data_value = 7;
		break;
	case CS8:
	default:
		data_value = 8;
		break;
	}

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

	fp->last_set_data_value = data_value;
	ftdi_uart_ctrl(fp, FTDI_SIO_SET_DATA_REQUEST, data_value, fp->channel);

	/*
	 * Flow control priority: CRTSCTS > DTR/DSR > XON/XOFF > disable.
	 * DTR/DSR has no standard termios flag; enabled via sysfs attribute.
	 */
	if (termios->c_cflag & CRTSCTS) {
		ftdi_uart_ctrl(fp, FTDI_SIO_SET_FLOW_CTRL_REQUEST,
			       0, FTDI_SIO_RTS_CTS_HS | fp->channel);
	} else if (fp->dtr_dsr_flow) {
		ftdi_uart_ctrl(fp, FTDI_SIO_SET_FLOW_CTRL_REQUEST,
			       0, FTDI_SIO_DTR_DSR_HS | fp->channel);
	} else if (termios->c_iflag & IXON) {
		u16 xchars = (termios->c_cc[VSTOP] << 8) | termios->c_cc[VSTART];

		ftdi_uart_ctrl(fp, FTDI_SIO_SET_FLOW_CTRL_REQUEST,
			       xchars, FTDI_SIO_XON_XOFF_HS | fp->channel);
	} else {
		ftdi_uart_ctrl(fp, FTDI_SIO_SET_FLOW_CTRL_REQUEST,
			       0, FTDI_SIO_DISABLE_FLOW_CTRL | fp->channel);
	}

	/* Baud-rate-dependent EEPROM slew rate check (once per device) */
	if (fp->ee_checked && !fp->slew_warned) {
		if (baud > 1000000 && fp->ee_slow_slew) {
			dev_notice(&fp->pdev->dev,
				   "EEPROM: slow slew rate at %u baud, consider fast slew for >1 Mbaud\n",
				   baud);
			fp->slew_warned = true;
		} else if (baud <= 1000000 && !fp->ee_slow_slew) {
			dev_notice(&fp->pdev->dev,
				   "EEPROM: fast slew rate at %u baud, slow slew recommended for <=1 Mbaud\n",
				   baud);
			fp->slew_warned = true;
		}
	}
}

static void ftdi_uart_break_ctl(struct uart_port *port, int break_state)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);
	u16 value;

	if (break_state)
		value = fp->last_set_data_value | FTDI_SIO_SET_BREAK;
	else
		value = fp->last_set_data_value;

	ftdi_uart_ctrl(fp, FTDI_SIO_SET_DATA_REQUEST, value, fp->channel);
}

static const char *ftdi_uart_type(struct uart_port *port)
{
	return "FTDI_MPSSE";
}

static void ftdi_uart_config_port(struct uart_port *port, int flags)
{
	/*
	 * Driver core for serial ports forces a non-zero value for port type.
	 * PORT_UNKNOWN (0) causes uart_port_startup to bail out immediately.
	 * Write an arbitrary non-zero value (same pattern as liteuart).
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

/*
 * RS-485: FTDI hardware auto-asserts TXDEN during transmission when a
 * CBUS pin is configured as TXDEN in the EEPROM.  The driver validates
 * the configuration and reports capability; timing is hardware-driven.
 */
static int ftdi_uart_rs485_config(struct uart_port *port,
				  struct ktermios *termios,
				  struct serial_rs485 *rs485)
{
	struct ftdi_uart_port *fp = to_ftdi_uart_port(port);

	if (rs485->flags & SER_RS485_ENABLED) {
		if (!fp->has_txden) {
			dev_err(&fp->pdev->dev,
				"RS-485: no TXDEN pin configured in EEPROM\n");
			return -EOPNOTSUPP;
		}
		/*
		 * TXDEN timing is handled by hardware; delay values
		 * are ignored (FTDI provides no control over them).
		 */
		rs485->delay_rts_before_send = 0;
		rs485->delay_rts_after_send = 0;
	}

	return 0;
}

static const struct uart_ops ftdi_uart_ops = {
	.tx_empty	= ftdi_uart_tx_empty,
	.set_mctrl	= ftdi_uart_set_mctrl,
	.get_mctrl	= ftdi_uart_get_mctrl,
	.stop_tx	= ftdi_uart_stop_tx,
	.start_tx	= ftdi_uart_start_tx,
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
 * FTDI CBUS GPIO uses a different mechanism than MPSSE GPIO:
 * - SET_BITMODE_REQUEST with FTDI_SIO_BITMODE_CBUS (0x20)
 * - wValue = (mode << 8) | (direction << 4) | value
 * - READ_PINS_REQUEST to read current pin state
 * Pin availability is determined by EEPROM configuration.
 */

static int ftdi_cbus_set_pins(struct ftdi_uart_port *fp)
{
	u16 val;

	val = (FTDI_SIO_BITMODE_CBUS << 8) |
	      (fp->cbus_output << 4) | fp->cbus_value;

	return usb_control_msg(fp->udev, usb_sndctrlpipe(fp->udev, 0),
			       FTDI_SIO_SET_BITMODE_REQUEST,
			       FTDI_SIO_REQTYPE_OUT,
			       val, fp->channel, NULL, 0, WDR_TIMEOUT);
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
	int ret;

	guard(mutex)(&fp->cbus_lock);

	if (value)
		fp->cbus_value |= BIT(offset);
	else
		fp->cbus_value &= ~BIT(offset);

	ret = ftdi_cbus_set_pins(fp);

	return ret < 0 ? ret : 0;
}

static int ftdi_cbus_gpio_set_multiple(struct gpio_chip *gc,
				       unsigned long *mask,
				       unsigned long *bits)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&fp->cbus_lock);

	fp->cbus_value = (fp->cbus_value & ~(*mask)) | (*bits & *mask);
	ret = ftdi_cbus_set_pins(fp);

	return ret < 0 ? ret : 0;
}

static int ftdi_cbus_gpio_direction_input(struct gpio_chip *gc,
					  unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&fp->cbus_lock);

	fp->cbus_output &= ~BIT(offset);
	ret = ftdi_cbus_set_pins(fp);

	return ret < 0 ? ret : 0;
}

static int ftdi_cbus_gpio_direction_output(struct gpio_chip *gc,
					   unsigned int offset, int value)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&fp->cbus_lock);

	fp->cbus_output |= BIT(offset);
	if (value)
		fp->cbus_value |= BIT(offset);
	else
		fp->cbus_value &= ~BIT(offset);

	ret = ftdi_cbus_set_pins(fp);

	return ret < 0 ? ret : 0;
}

static int ftdi_cbus_gpio_get_direction(struct gpio_chip *gc,
					unsigned int offset)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);

	return (fp->cbus_output & BIT(offset)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ftdi_cbus_gpio_init_valid_mask(struct gpio_chip *gc,
					  unsigned long *valid_mask,
					  unsigned int ngpios)
{
	struct ftdi_uart_port *fp = gpiochip_get_data(gc);
	unsigned long map = fp->cbus_altfunc;

	/* cbus_altfunc bits set = NOT gpio; complement to get valid */
	bitmap_complement(valid_mask, &map, ngpios);

	return 0;
}

/*
 * Read FT232H CBUS config from the parent's pre-read EEPROM data.
 * EEPROM offset 0x1a, 4 bytes, each nibble contains the pin mux function.
 * FTDI_FTX_CBUS_MUX_GPIO (0x8) means the pin is available as GPIO.
 */
static int ftdi_cbus_read_eeprom(struct ftdi_uart_port *fp)
{
	u8 buf[FTDI_CBUS_EEPROM_LEN];
	u16 cbus_config;
	int ret, i;

	ret = ftdi_mpsse_get_cbus_config(fp->pdev, buf, FTDI_CBUS_EEPROM_LEN);
	if (ret < 0)
		return ret;

	/*
	 * FT232H CBUS pin config layout:
	 *   buf[0] upper nibble -> CBUS0 (AC5)
	 *   buf[1] lower nibble -> CBUS1 (AC6)
	 *   buf[2] lower nibble -> CBUS2 (AC8)
	 *   buf[2] upper nibble -> CBUS3 (AC9)
	 */
	cbus_config = buf[2] << 8 | (buf[1] & 0xf) << 4 |
		      (buf[0] & 0xf0) >> 4;

	fp->cbus_altfunc = 0xff;
	fp->has_txden = false;
	for (i = 0; i < FTDI_CBUS_NGPIO; i++) {
		u8 func = cbus_config & 0xf;

		if (func == FTDI_FTX_CBUS_MUX_GPIO)
			fp->cbus_altfunc &= ~BIT(i);
		if (func == FTDI_FTX_CBUS_MUX_TXDEN)
			fp->has_txden = true;
		cbus_config >>= 4;
	}

	return 0;
}

static int ftdi_cbus_gpio_register(struct ftdi_uart_port *fp)
{
	struct gpio_chip *gc = &fp->cbus_gc;
	int ret;

	/* Only FT232H supported initially */
	if (fp->chip_type != FTDI_CHIP_FT232H)
		return 0;

	ret = ftdi_cbus_read_eeprom(fp);
	if (ret) {
		dev_dbg(&fp->pdev->dev, "CBUS EEPROM read failed: %d\n", ret);
		return 0;	/* non-fatal: just skip CBUS GPIO */
	}

	if (fp->cbus_altfunc == 0xff)
		return 0;

	gc->owner = THIS_MODULE;
	gc->parent = &fp->pdev->dev;
	gc->label = "ftdi-cbus-gpio";
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
		dev_warn(&fp->pdev->dev,
			 "failed to register CBUS GPIO: %d\n", ret);
		return 0;	/* non-fatal */
	}

	fp->cbus_registered = true;
	dev_info(&fp->pdev->dev, "CBUS GPIO: %d pins (valid mask 0x%02x)\n",
		 FTDI_CBUS_NGPIO, (u8)~fp->cbus_altfunc & 0xf);

	return 0;
}

static void ftdi_cbus_gpio_unregister(struct ftdi_uart_port *fp)
{
	if (fp->cbus_registered) {
		gpiochip_remove(&fp->cbus_gc);
		fp->cbus_registered = false;
	}
}

static ssize_t latency_timer_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", fp->latency_timer);
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

	ret = ftdi_uart_ctrl(fp, FTDI_SIO_SET_LATENCY_TIMER_REQUEST,
			     val, fp->channel);
	if (ret < 0)
		return ret;

	fp->latency_timer = val;
	return count;
}
static DEVICE_ATTR_RW(latency_timer);

static ssize_t dtr_dsr_flow_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", fp->dtr_dsr_flow);
}

static ssize_t dtr_dsr_flow_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	fp->dtr_dsr_flow = val;
	return count;
}
static DEVICE_ATTR_RW(dtr_dsr_flow);

static struct attribute *ftdi_uart_attrs[] = {
	&dev_attr_latency_timer.attr,
	&dev_attr_dtr_dsr_flow.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ftdi_uart);

static void ftdi_uart_check_eeprom(struct ftdi_uart_port *fp)
{
	const struct ftdi_eeprom *ee;
	u8 drive_ma;
	bool schmitt, slow_slew;

	ee = ftdi_mpsse_get_eeprom(fp->pdev);
	if (!ee)
		return;

	if (ftdi_mpsse_get_eeprom_drive(fp->pdev, &drive_ma, &schmitt,
					&slow_slew))
		return;

	fp->ee_slow_slew = slow_slew;
	fp->ee_checked = true;

	if (!schmitt)
		dev_warn(&fp->pdev->dev,
			 "EEPROM: Schmitt trigger OFF on data bus, UART requires Schmitt for reliable RX sampling\n");

	if (drive_ma < 8)
		dev_notice(&fp->pdev->dev,
			   "EEPROM: data bus drive current is %u mA, 8 mA recommended for UART\n",
			   drive_ma);

	if (ee->pulldown)
		dev_notice(&fp->pdev->dev,
			   "EEPROM: suspend_pull_downs enabled, may cause break condition on TXD during USB suspend\n");

	if ((fp->chip_type == FTDI_CHIP_FT232H ||
	     fp->chip_type == FTDI_CHIP_FT232HP ||
	     fp->chip_type == FTDI_CHIP_FT233HP) && !ee->cha_vcp)
		dev_notice(&fp->pdev->dev,
			   "EEPROM: VCP driver not enabled, device may not appear as a standard serial port\n");
}

static int ftdi_uart_plat_probe(struct platform_device *pdev)
{
	struct ftdi_uart_port *fp;
	struct uart_port *port;
	u32 dev_id;
	int ret, i;

	fp = devm_kzalloc(&pdev->dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;

	fp->pdev = pdev;
	fp->udev = ftdi_mpsse_get_udev(pdev);
	fp->channel = ftdi_mpsse_get_channel(pdev);
	fp->chip_type = ftdi_mpsse_get_chip_type(pdev);
	fp->latency_timer = 16;
	spin_lock_init(&fp->write_lock);
	mutex_init(&fp->cbus_lock);
	INIT_WORK(&fp->mctrl_work, ftdi_uart_mctrl_work);

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
		fp->write_buf[i] = devm_kmalloc(&pdev->dev,
						usb_endpoint_maxp(fp->ep_out),
						GFP_KERNEL);
		if (!fp->write_buf[i]) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}
	}

	ret = xa_alloc(&ftdi_uart_xa, &dev_id, fp,
		       XA_LIMIT(0, FTDI_UART_NR - 1), GFP_KERNEL);
	if (ret)
		goto err_free_urbs;

	port = &fp->port;
	port->dev = &pdev->dev;
	port->iotype = UPIO_MEM;
	/*
	 * serial_core's uart_configure_port() bails out if all of iobase,
	 * mapbase, and membase are zero.  We are a USB device with no
	 * memory-mapped registers -- set a dummy membase so config_port
	 * is actually reached during uart_add_one_port().
	 */
	port->membase = (void __iomem *)0x1;
	port->flags = UPF_BOOT_AUTOCONF;
	port->ops = &ftdi_uart_ops;
	port->fifosize = FTDI_UART_FIFO;
	port->type = PORT_UNKNOWN;
	port->line = dev_id;
	port->rs485_config = ftdi_uart_rs485_config;
	port->rs485_supported.flags = SER_RS485_ENABLED;
	spin_lock_init(&port->lock);

	platform_set_drvdata(pdev, fp);

	ret = uart_add_one_port(&ftdi_uart_driver, port);
	if (ret)
		goto err_xa;

	ftdi_cbus_gpio_register(fp);
	ftdi_uart_check_eeprom(fp);

	dev_info(&pdev->dev, "FTDI MPSSE UART: /dev/ttyFTDI%u\n", dev_id);
	return 0;

err_xa:
	xa_erase(&ftdi_uart_xa, dev_id);
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

	/* Remove CBUS GPIO before UART port and URB teardown */
	ftdi_cbus_gpio_unregister(fp);

	/*
	 * Poison URBs first: cancels in-flight I/O and prevents future
	 * submissions.  This must happen before uart_remove_one_port()
	 * which frees port->state -- otherwise, a read URB callback could
	 * access freed port->state (tty_insert_flip_char, process_packet).
	 */
	usb_poison_urb(fp->read_urb);
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_poison_urb(fp->write_urb[i]);

	uart_remove_one_port(&ftdi_uart_driver, &fp->port);

	/*
	 * cancel_work_sync must come AFTER uart_remove_one_port because
	 * the hangup path inside remove calls set_mctrl -> schedule_work,
	 * which would re-queue mctrl_work after an earlier cancel.
	 */
	cancel_work_sync(&fp->mctrl_work);
	xa_erase(&ftdi_uart_xa, fp->port.line);

	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_free_urb(fp->write_urb[i]);
	usb_free_urb(fp->read_urb);
}

static int ftdi_uart_suspend(struct device *dev)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	int i;

	usb_kill_urb(fp->read_urb);
	for (i = 0; i < FTDI_WRITE_URBS; i++)
		usb_kill_urb(fp->write_urb[i]);
	cancel_work_sync(&fp->mctrl_work);

	return 0;
}

static int ftdi_uart_resume(struct device *dev)
{
	struct ftdi_uart_port *fp = dev_get_drvdata(dev);
	int ret = 0;

	if (tty_port_initialized(&fp->port.state->port)) {
		ftdi_uart_read_modem_status(fp);

		usb_fill_bulk_urb(fp->read_urb, fp->udev,
				  usb_rcvbulkpipe(fp->udev,
						  fp->ep_in->bEndpointAddress),
				  fp->read_buf,
				  usb_endpoint_maxp(fp->ep_in),
				  ftdi_uart_read_complete, fp);

		ret = usb_submit_urb(fp->read_urb, GFP_NOIO);
		if (ret)
			dev_err(dev, "resume: read URB submit failed: %d\n",
				ret);
	}

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
