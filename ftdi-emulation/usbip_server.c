/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB/IP server -- speaks the kernel's USB/IP wire protocol over TCP
 *
 * Bulk IN URBs stay pending as on the chip: one completes when a full
 * packet of data is waiting or the MPSSE saw SEND_IMMEDIATE, otherwise
 * when the latency timer of the channel expires (a status-only packet if
 * the FIFO is empty, AN232B-04). CMD_UNLINK cancels a pending URB as the
 * kernel's usbip stub driver does (RET_UNLINK status -ECONNRESET).
 *
 * SIGUSR1 power-cycles the chip, SIGUSR2 prints the statistics. An IMPORT
 * is a plug and power-cycles the chip too.
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#define _GNU_SOURCE		/* ppoll() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>

#include "usbip_proto.h"
#include "usbip_server.h"
#include "ftdi_emu.h"

#define EMU_SYSFS_PATH	"/sys/devices/pci0000:00/0000:00:01.0/usb1/1-1"
#define EMU_BUSID	"1-1"
#define EMU_BUSNUM	1
#define EMU_DEVNUM	2

/* Device number on its bus: a new one each time it is plugged in */
static unsigned int emu_devnum = EMU_DEVNUM;

#define URB_EPIPE	(-32)
#define URB_EPROTO	(-71)	/* transaction error: no handshake */
#define URB_EOVERFLOW	(-75)
#define URB_ECONNRESET	(-104)

static volatile sig_atomic_t server_running = 1;
static volatile sig_atomic_t want_power_cycle;
static volatile sig_atomic_t want_stats;

/* A bulk IN URB waiting for data or for the latency timer */
struct pending_urb {
	uint32_t seqnum;
	int      intf;
	int      len;
	bool     blackhole;	/* timeout injection: never completes */
	struct pending_urb *next;
};

static struct pending_urb *pending;
static long long last_packet_ms[FTDI_MAX_INTERFACES];

/*
 * A bulk OUT URB the chip has not fully taken yet (paced UART with a full
 * TX buffer): the device NAKs, the URB completes when all of it went in.
 */
struct pending_out {
	uint32_t seqnum;
	int      intf;
	uint8_t *data;
	int      len;
	int      done;
	struct pending_out *next;
};

static struct pending_out *pending_out;

/* Control FIFO (--ctl): runtime commands, -1 if not used */
static int ctl_fd = -1;
static char ctl_line[128];
static size_t ctl_len;

static void sighandler(int sig)
{
	switch (sig) {
	case SIGUSR1:
		want_power_cycle = 1;
		break;
	case SIGUSR2:
		want_stats = 1;
		break;
	default:
		server_running = 0;
		break;
	}
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long now_ms(void)
{
	return now_ns() / 1000000;
}

static int recv_exact(int fd, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = recv(fd, (char *)buf + done, n - done,
				 MSG_WAITALL);

		if (r <= 0) {
			if (r == 0)
				return -1;
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += r;
	}
	return 0;
}

static int send_iov(int fd, struct iovec *iov, int iovcnt)
{
	while (iovcnt > 0) {
		struct msghdr msg = { .msg_iov = iov, .msg_iovlen = iovcnt };
		ssize_t r = sendmsg(fd, &msg, MSG_NOSIGNAL);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		while (iovcnt > 0 && (size_t)r >= iov->iov_len) {
			r -= iov->iov_len;
			iov++;
			iovcnt--;
		}
		if (iovcnt > 0) {
			iov->iov_base = (char *)iov->iov_base + r;
			iov->iov_len -= r;
		}
	}
	return 0;
}

static int send_exact(int fd, const void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = send(fd, (const char *)buf + done, n - done, 0);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		done += r;
	}
	return 0;
}

static void fill_usb_device(struct usbip_usb_device *ud,
			     const struct ftdi_device *dev)
{
	memset(ud, 0, sizeof(*ud));
	strncpy(ud->path, EMU_SYSFS_PATH, sizeof(ud->path) - 1);
	strncpy(ud->busid, EMU_BUSID, sizeof(ud->busid) - 1);
	ud->busnum = htonl(EMU_BUSNUM);
	ud->devnum = htonl(emu_devnum);
	ud->speed  = htonl(USB_SPEED_HIGH);
	ud->idVendor  = htons(dev->usb_vid);
	ud->idProduct = htons(dev->usb_pid);
	ud->bcdDevice = htons(dev->bcd);
	ud->bDeviceClass = 0;
	ud->bDeviceSubClass = 0;
	ud->bDeviceProtocol = 0;
	ud->bConfigurationValue = 1;
	ud->bNumConfigurations = 1;
	ud->bNumInterfaces = dev->num_interfaces;
}

static int handle_devlist(int fd, const struct ftdi_device *dev)
{
	struct op_common reply;
	uint32_t ndev;
	struct usbip_usb_device ud;
	struct usbip_usb_interface ui;
	int i;

	reply.version = htons(USBIP_VERSION);
	reply.code    = htons(OP_REP_DEVLIST);
	reply.status  = htonl(ST_OK);

	if (send_exact(fd, &reply, sizeof(reply)) < 0)
		return -1;

	ndev = htonl(1);
	if (send_exact(fd, &ndev, sizeof(ndev)) < 0)
		return -1;

	fill_usb_device(&ud, dev);
	if (send_exact(fd, &ud, sizeof(ud)) < 0)
		return -1;

	for (i = 0; i < dev->num_interfaces; i++) {
		memset(&ui, 0, sizeof(ui));
		ui.bInterfaceClass    = 0xFF;
		ui.bInterfaceSubClass = 0xFF;
		ui.bInterfaceProtocol = 0xFF;
		if (send_exact(fd, &ui, sizeof(ui)) < 0)
			return -1;
	}

	return 0;
}

static int handle_import(int fd, struct ftdi_device *dev)
{
	char busid[SYSFS_BUS_ID_SIZE];
	struct op_common reply;
	struct usbip_usb_device ud;

	if (recv_exact(fd, busid, sizeof(busid)) < 0)
		return -1;

	busid[sizeof(busid) - 1] = '\0';
	fprintf(stderr, "usbip: import request for '%s'\n", busid);

	reply.version = htons(USBIP_VERSION);
	reply.code    = htons(OP_REP_IMPORT);
	reply.status  = htonl(ST_OK);

	/* One device is exported: any other bus id is not there */
	if (strcmp(busid, EMU_BUSID)) {
		fprintf(stderr, "usbip: no device '%s' here (only %s)\n", busid,
			EMU_BUSID);
		reply.status = htonl(ST_NA);
		send_exact(fd, &reply, sizeof(reply));
		return -1;
	}

	/* Attaching is plugging the device in: it enumerates anew */
	ftdi_emu_plug(dev);
	if (++emu_devnum > 127)
		emu_devnum = EMU_DEVNUM;

	if (send_exact(fd, &reply, sizeof(reply)) < 0)
		return -1;

	fill_usb_device(&ud, dev);
	if (send_exact(fd, &ud, sizeof(ud)) < 0)
		return -1;

	return 0;
}

/* The setup packet of a CMD_SUBMIT is little-endian, as on the bus */
static void parse_setup(const uint8_t *setup,
			uint8_t *bmRequestType, uint8_t *bRequest,
			uint16_t *wValue, uint16_t *wIndex, uint16_t *wLength)
{
	*bmRequestType = setup[0];
	*bRequest      = setup[1];
	*wValue  = setup[2] | (setup[3] << 8);
	*wIndex  = setup[4] | (setup[5] << 8);
	*wLength = setup[6] | (setup[7] << 8);
}

static int send_ret_submit(int fd, uint32_t seqnum,
			    int32_t status, int32_t actual_length,
			    const uint8_t *data, int data_len)
{
	struct usbip_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.base.command   = htonl(USBIP_RET_SUBMIT);
	hdr.base.seqnum    = htonl(seqnum);
	/* devid, direction, ep are all 0 in responses */
	hdr.u.ret_submit.status          = htonl(status);
	hdr.u.ret_submit.actual_length   = htonl(actual_length);
	hdr.u.ret_submit.start_frame     = 0;
	hdr.u.ret_submit.number_of_packets = htonl(0xFFFFFFFF);
	hdr.u.ret_submit.error_count     = 0;

	/*
	 * Header and payload in one message, as the kernel's stub driver does
	 * (stub_tx.c). Between two send() calls the vhci rx thread can
	 * sleep with the URB still linked to its endpoint, and on a UP
	 * kernel usb_hcd_flush_endpoint() then spins on it for ever.
	 */
	struct iovec iov[2] = {
		{ .iov_base = &hdr, .iov_len = sizeof(hdr) },
		{ .iov_base = (void *)data, .iov_len = data && data_len > 0 ? data_len : 0 },
	};

	return send_iov(fd, iov, iov[1].iov_len ? 2 : 1);
}

static int send_ret_unlink(int fd, uint32_t seqnum, int32_t status)
{
	struct usbip_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.base.command = htonl(USBIP_RET_UNLINK);
	hdr.base.seqnum  = htonl(seqnum);
	hdr.u.ret_unlink.status = htonl(status);

	return send_exact(fd, &hdr, sizeof(hdr));
}

/* Interface N has bulk IN 0x81 + 2N and bulk OUT 0x02 + 2N */
static int ep_to_intf(uint32_t ep)
{
	return ((int)ep - 1) / 2;
}

static void pending_add(uint32_t seqnum, int intf, int len, bool blackhole)
{
	struct pending_urb *p = calloc(1, sizeof(*p)), **pp;

	if (!p) {
		fprintf(stderr, "usbip: out of memory\n");
		exit(1);
	}
	p->seqnum = seqnum;
	p->intf = intf;
	p->len = len;
	p->blackhole = blackhole;

	for (pp = &pending; *pp; pp = &(*pp)->next)
		;
	*pp = p;
}

static void pending_free_all(void)
{
	while (pending) {
		struct pending_urb *n = pending->next;

		free(pending);
		pending = n;
	}
	while (pending_out) {
		struct pending_out *n = pending_out->next;

		free(pending_out->data);
		free(pending_out);
		pending_out = n;
	}
}

static bool pending_out_for(int intf)
{
	struct pending_out *p;

	for (p = pending_out; p; p = p->next)
		if (p->intf == intf)
			return true;
	return false;
}

/* Queue an OUT URB (takes ownership of data) behind the earlier ones */
static void pending_out_add(uint32_t seqnum, int intf, uint8_t *data, int len)
{
	struct pending_out *p = calloc(1, sizeof(*p)), **pp;

	if (!p) {
		fprintf(stderr, "usbip: out of memory\n");
		exit(1);
	}
	p->seqnum = seqnum;
	p->intf = intf;
	p->data = data;
	p->len = len;

	for (pp = &pending_out; *pp; pp = &(*pp)->next)
		;
	*pp = p;
}

/* Feed the chip as it makes room; complete the URBs it took entirely */
static int service_pending_out(int fd, struct ftdi_device *dev)
{
	struct pending_out **pp = &pending_out;
	bool blocked[FTDI_MAX_INTERFACES] = { false };

	while (*pp) {
		struct pending_out *p = *pp;
		int n;

		/* Per endpoint, strictly in order */
		if (blocked[p->intf]) {
			pp = &p->next;
			continue;
		}
		n = ftdi_emu_out_room(dev, p->intf);
		if (n > p->len - p->done)
			n = p->len - p->done;
		if (n > 0) {
			ftdi_emu_bulk_out(dev, p->intf, p->data + p->done, n);
			p->done += n;
		}
		if (p->done < p->len) {
			blocked[p->intf] = true;
			ftdi_emu_out_blocked(dev, p->intf);
			pp = &p->next;
			continue;
		}
		*pp = p->next;
		n = send_ret_submit(fd, p->seqnum, 0, p->len, NULL, 0);
		free(p->data);
		free(p);
		if (n < 0)
			return -1;
	}
	return 0;
}

/* Oldest completable URB of an interface (FIFO order per endpoint) */
static struct pending_urb **pending_head(int intf)
{
	struct pending_urb **pp;

	for (pp = &pending; *pp; pp = &(*pp)->next)
		if ((*pp)->intf == intf)
			return (*pp)->blackhole ? NULL : pp;
	return NULL;
}

/* Complete every bulk IN URB whose data or latency deadline is due */
static int service_pending(int fd, struct ftdi_device *dev)
{
	static uint8_t buf[65536];
	int i;

	for (i = 0; i < dev->num_interfaces; i++) {
		struct pending_urb **pp;

		while ((pp = pending_head(i))) {
			struct pending_urb *p = *pp;
			long long now = now_ms();
			int n, len = p->len;

			if (!ftdi_emu_in_ready(dev, i, now - last_packet_ms[i]))
				break;

			*pp = p->next;
			if (len > (int)sizeof(buf))
				len = sizeof(buf);

			{
				bool babble;
				int r = ftdi_emu_bulk_in(dev, i, buf, len, &babble);

				if (r < 0)
					n = send_ret_submit(fd, p->seqnum, URB_EPIPE,
							    0, NULL, 0);
				else
					n = send_ret_submit(fd, p->seqnum,
							    babble ? URB_EOVERFLOW : 0,
							    r, buf, r);
			}
			last_packet_ms[i] = now;
			free(p);
			if (n < 0)
				return -1;
		}
	}
	return 0;
}

/* A channel letter, A to D, of this chip: its index, or -1 */
static int ctl_chan(const struct ftdi_device *dev, const char *arg)
{
	int c = arg && arg[0] && !arg[1] ? (arg[0] | 0x20) - 'a' : -1;

	return c >= 0 && c < dev->num_interfaces ? c : -1;
}

/*
 * error TYPE [COUNT [SKIP [CHAN]]]
 * pin CHAN D<n>|C<n> 0|1|z
 * power-cycle
 * stats
 */
static void ctl_command(struct ftdi_device *dev, char *line)
{
	char *cmd = strtok(line, " \t");
	char *arg1 = cmd ? strtok(NULL, " \t") : NULL;
	char *arg2 = arg1 ? strtok(NULL, " \t") : NULL;
	char *arg3 = arg2 ? strtok(NULL, " \t") : NULL;
	char *arg4 = arg3 ? strtok(NULL, " \t") : NULL;
	int i;

	if (!cmd)
		return;

	if (!strcmp(cmd, "error") && arg1) {
		int mode = ftdi_emu_error_from_name(arg1);
		int count = arg2 ? atoi(arg2) : 1;
		int skip = arg3 ? atoi(arg3) : 0;
		int chan = arg4 ? ctl_chan(dev, arg4) : -1;

		if (mode < 0) {
			fprintf(stderr, "ctl: unknown error mode '%s'\n", arg1);
			return;
		}
		if (arg4 && chan < 0) {
			fprintf(stderr, "ctl: unknown channel '%s'\n", arg4);
			return;
		}
		for (i = 0; i < dev->num_interfaces; i++)
			if (chan < 0 || chan == i)
				ftdi_emu_set_error(dev, i, mode, count, skip);
		fprintf(stderr, "ctl: error injection %s count=%d skip=%d channel=%s\n",
			arg1, count, skip, arg4 ? arg4 : "all");
	} else if (!strcmp(cmd, "pin") && arg3) {
		int chan = ctl_chan(dev, arg1);
		int port = (arg2[0] | 0x20) == 'c';
		int level = arg3[0] == '0' ? 0 : arg3[0] == '1' ? 1 : -1;

		if (chan < 0 || ((arg2[0] | 0x20) != 'c' && (arg2[0] | 0x20) != 'd') ||
		    arg2[1] < '0' || arg2[1] > '7' || arg2[2] ||
		    (level < 0 && (arg3[0] | 0x20) != 'z') ||
		    ftdi_emu_set_pin(dev, chan, port, arg2[1] - '0', level) < 0)
			fprintf(stderr, "ctl: unknown pin '%s %s %s'\n", arg1, arg2,
				arg3);
	} else if (!strcmp(cmd, "power-cycle")) {
		ftdi_emu_power_cycle(dev);
	} else if (!strcmp(cmd, "stats")) {
		ftdi_emu_dump_stats(dev);
	} else {
		fprintf(stderr, "ctl: unknown command '%s'\n", cmd);
	}
}

static void ctl_read(struct ftdi_device *dev)
{
	char buf[128];
	ssize_t n = read(ctl_fd, buf, sizeof(buf));
	ssize_t i;

	for (i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			ctl_line[ctl_len] = '\0';
			ctl_command(dev, ctl_line);
			ctl_len = 0;
		} else if (ctl_len < sizeof(ctl_line) - 1) {
			ctl_line[ctl_len++] = buf[i];
		}
	}
}

int usbip_server_set_ctl(const char *path)
{
	if (mkfifo(path, 0600) < 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	/* Read-write: the FIFO never reports EOF when a writer closes */
	ctl_fd = open(path, O_RDWR | O_NONBLOCK);
	if (ctl_fd < 0) {
		perror(path);
		return -1;
	}
	return 0;
}

/* ns until the next deadline (latency timer, chip), -1 if none */
static long long next_timeout(const struct ftdi_device *dev)
{
	long long now = now_ms(), best = ftdi_emu_next_event(dev);
	const struct pending_out *po;
	int i;

	/* A held OUT transfer the chip has room for by now */
	for (po = pending_out; po; po = po->next)
		if (ftdi_emu_out_room(dev, po->intf) > 0)
			return 0;

	for (i = 0; i < dev->num_interfaces; i++) {
		long long dl;

		if (!pending_head(i))
			continue;
		dl = (last_packet_ms[i] + ftdi_emu_latency(dev, i) - now) *
		     1000000;
		if (dl < 0)
			dl = 0;
		if (best < 0 || dl < best)
			best = dl;
	}
	return best;
}

static int handle_cmd_submit(int fd, struct ftdi_device *dev,
			     const struct usbip_header *req)
{
	uint32_t seqnum   = ntohl(req->base.seqnum);
	uint32_t direction = ntohl(req->base.direction);
	uint32_t ep       = ntohl(req->base.ep);
	int32_t  buf_len  = ntohl(req->u.cmd_submit.transfer_buffer_length);
	uint8_t *out_data = NULL;
	uint8_t resp[4096];
	int resp_len;
	int intf;

	if (buf_len < 0 || buf_len > 16 * 1024 * 1024)
		return -1;

	if (direction == USBIP_DIR_OUT && buf_len > 0) {
		out_data = malloc(buf_len);
		if (!out_data)
			return -1;
		if (recv_exact(fd, out_data, buf_len) < 0) {
			free(out_data);
			return -1;
		}
	}

	if (ep == 0) {
		uint8_t bmRequestType, bRequest;
		uint16_t wValue, wIndex, wLength;

		parse_setup(req->u.cmd_submit.setup,
			    &bmRequestType, &bRequest,
			    &wValue, &wIndex, &wLength);

		if (wLength > sizeof(resp))
			wLength = sizeof(resp);
		resp_len = ftdi_emu_control(dev, bmRequestType, bRequest,
					    wValue, wIndex, wLength, resp);

		free(out_data);

		if (resp_len == FTDI_EMU_NO_RESPONSE)
			return send_ret_submit(fd, seqnum, URB_EPROTO,
					       0, NULL, 0);
		if (resp_len < 0) {
			/* STALL */
			return send_ret_submit(fd, seqnum, URB_EPIPE,
					       0, NULL, 0);
		}

		if (direction == USBIP_DIR_IN && resp_len > 0) {
			if (resp_len > buf_len)
				resp_len = buf_len;
			return send_ret_submit(fd, seqnum, 0, resp_len,
					       resp, resp_len);
		}

		return send_ret_submit(fd, seqnum, 0, 0, NULL, 0);
	}

	intf = ep_to_intf(ep);
	if (intf < 0 || intf >= dev->num_interfaces) {
		free(out_data);
		return send_ret_submit(fd, seqnum, URB_EPIPE, 0, NULL, 0);
	}

	if (!ftdi_emu_configured(dev, "bulk transfer")) {
		free(out_data);
		return send_ret_submit(fd, seqnum, URB_EPROTO, 0, NULL, 0);
	}

	if (direction == USBIP_DIR_OUT) {
		/* The chip cannot take it all now: completes later */
		if (buf_len > 0 && (pending_out_for(intf) ||
				    ftdi_emu_out_room(dev, intf) < buf_len)) {
			ftdi_emu_out_held(dev, intf);
			pending_out_add(seqnum, intf, out_data, buf_len);
			if (service_pending_out(fd, dev) < 0)
				return -1;
			return service_pending(fd, dev);
		}
		ftdi_emu_bulk_out(dev, intf, out_data, buf_len);
		free(out_data);
		if (send_ret_submit(fd, seqnum, 0, buf_len, NULL, 0) < 0)
			return -1;
		return service_pending(fd, dev);
	}

	/* Bulk IN: completes when data is due, see service_pending() */
	free(out_data);
	pending_add(seqnum, intf, buf_len, ftdi_emu_in_blackhole(dev, intf));
	return service_pending(fd, dev);
}

static int handle_cmd_unlink(int fd, const struct usbip_header *hdr)
{
	uint32_t target = ntohl(hdr->u.cmd_unlink.seqnum);
	struct pending_urb **pp;
	struct pending_out **po;

	/* An OUT URB the chip had not finished taking: the rest is dropped */
	for (po = &pending_out; *po; po = &(*po)->next) {
		if ((*po)->seqnum == target) {
			struct pending_out *p = *po;

			*po = p->next;
			free(p->data);
			free(p);
			return send_ret_unlink(fd, ntohl(hdr->base.seqnum),
					       URB_ECONNRESET);
		}
	}

	for (pp = &pending; *pp; pp = &(*pp)->next) {
		if ((*pp)->seqnum == target) {
			struct pending_urb *p = *pp;

			*pp = p->next;
			free(p);
			return send_ret_unlink(fd, ntohl(hdr->base.seqnum),
					       URB_ECONNRESET);
		}
	}
	/* Already completed: the RET_SUBMIT is on its way */
	return send_ret_unlink(fd, ntohl(hdr->base.seqnum), 0);
}

static void handle_signals(struct ftdi_device *dev)
{
	if (want_power_cycle) {
		want_power_cycle = 0;
		ftdi_emu_power_cycle(dev);
	}
	if (want_stats) {
		want_stats = 0;
		ftdi_emu_dump_stats(dev);
	}
}

static int urb_loop(int fd, struct ftdi_device *dev)
{
	struct usbip_header hdr;
	long long now = now_ms();
	int i, ret = 0;

	for (i = 0; i < FTDI_MAX_INTERFACES; i++)
		last_packet_ms[i] = now;

	fprintf(stderr, "usbip: entering URB dispatch loop\n");

	while (server_running) {
		struct pollfd pfd[2] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = ctl_fd, .events = POLLIN },
		};
		struct timespec ts;
		long long tmo;
		int r;

		handle_signals(dev);
		ftdi_emu_tick(dev, now_ns());
		if (service_pending_out(fd, dev) < 0 ||
		    service_pending(fd, dev) < 0) {
			ret = -1;
			break;
		}

		/* The chip model has deadlines well below a millisecond */
		tmo = next_timeout(dev);
		ts.tv_sec = tmo / 1000000000;
		ts.tv_nsec = tmo % 1000000000;
		r = ppoll(pfd, ctl_fd >= 0 ? 2 : 1, tmo < 0 ? NULL : &ts, NULL);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			ret = -1;
			break;
		}
		if (r == 0)
			continue;

		/* What arrives now is given to the chip now */
		ftdi_emu_tick(dev, now_ns());

		if (pfd[1].revents & POLLIN)
			ctl_read(dev);
		if (!(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;

		if (recv_exact(fd, &hdr, sizeof(hdr)) < 0) {
			fprintf(stderr, "usbip: client disconnected\n");
			break;
		}

		switch (ntohl(hdr.base.command)) {
		case USBIP_CMD_SUBMIT:
			r = handle_cmd_submit(fd, dev, &hdr);
			break;
		case USBIP_CMD_UNLINK:
			r = handle_cmd_unlink(fd, &hdr);
			break;
		default:
			fprintf(stderr, "usbip: unknown command 0x%08x\n",
				ntohl(hdr.base.command));
			r = -1;
			break;
		}
		if (r < 0) {
			ret = -1;
			break;
		}
	}

	pending_free_all();
	return ret;
}

static int handle_connection(int fd, struct ftdi_device *dev)
{
	struct op_common hdr;

	if (recv_exact(fd, &hdr, sizeof(hdr)) < 0)
		return -1;

	uint16_t code = ntohs(hdr.code);

	switch (code) {
	case OP_REQ_DEVLIST:
		fprintf(stderr, "usbip: DEVLIST request\n");
		return handle_devlist(fd, dev);

	case OP_REQ_IMPORT:
		if (handle_import(fd, dev) < 0)
			return -1;
		/* The URB phase runs on the same connection */
		return urb_loop(fd, dev);

	default:
		fprintf(stderr, "usbip: unknown op 0x%04x\n", code);
		return -1;
	}
}

int usbip_server_run(struct ftdi_device *dev, int port)
{
	int listen_fd, client_fd;
	struct sockaddr_in addr;
	struct sigaction sa;
	int opt = 1;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;	/* no SA_RESTART: poll() gets EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return -1;
	}

	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 4) < 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	fprintf(stderr, "usbip: listening on port %d "
		"(VID=%04x PID=%04x bcdDevice=%04x)\n",
		port, dev->usb_vid, dev->usb_pid, dev->bcd);

	while (server_running) {
		struct sockaddr_in peer;
		socklen_t peerlen = sizeof(peer);

		handle_signals(dev);
		client_fd = accept(listen_fd, (struct sockaddr *)&peer,
				   &peerlen);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}

		/* Low latency for interactive MPSSE command-response */
		setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
			   &opt, sizeof(opt));

		fprintf(stderr, "usbip: connection from %s:%d\n",
			inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

		handle_connection(client_fd, dev);
		close(client_fd);
	}

	ftdi_emu_dump_stats(dev);
	close(listen_fd);
	return 0;
}
