/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB/IP server -- speaks the kernel's USB/IP wire protocol over TCP
 *
 * Accepts connections on the USB/IP port, handles device listing and
 * import requests, then enters a URB dispatch loop that forwards USB
 * requests to the FTDI emulation layer.
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "usbip_proto.h"
#include "usbip_server.h"
#include "ftdi_emu.h"

/* Emulated sysfs path and bus ID seen by the client */
#define EMU_SYSFS_PATH	"/sys/devices/pci0000:00/0000:00:01.0/usb1/1-1"
#define EMU_BUSID	"1-1"
#define EMU_BUSNUM	1
#define EMU_DEVNUM	2

static volatile sig_atomic_t server_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	server_running = 0;
}

/* Read exactly n bytes from a socket, retrying on partial reads */
static int recv_exact(int fd, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = recv(fd, (char *)buf + done, n - done,
				 MSG_WAITALL);

		if (r <= 0) {
			if (r == 0)
				return -1; /* peer closed */
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += r;
	}
	return 0;
}

/* Send exactly n bytes, retrying on partial writes */
static int send_exact(int fd, const void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = send(fd, (const char *)buf + done, n - done, 0);

		if (r <= 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += r;
	}
	return 0;
}

/* Fill a usbip_usb_device in network byte order */
static void fill_usb_device(struct usbip_usb_device *ud,
			     const struct ftdi_device *dev)
{
	memset(ud, 0, sizeof(*ud));
	strncpy(ud->path, EMU_SYSFS_PATH, sizeof(ud->path) - 1);
	strncpy(ud->busid, EMU_BUSID, sizeof(ud->busid) - 1);
	ud->busnum = htonl(EMU_BUSNUM);
	ud->devnum = htonl(EMU_DEVNUM);
	ud->speed  = htonl(USB_SPEED_HIGH);
	ud->idVendor  = htons(dev->vid);
	ud->idProduct = htons(dev->pid);
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

	/* Send one interface descriptor per interface */
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

static int handle_import(int fd, const struct ftdi_device *dev)
{
	char busid[SYSFS_BUS_ID_SIZE];
	struct op_common reply;
	struct usbip_usb_device ud;

	if (recv_exact(fd, busid, sizeof(busid)) < 0)
		return -1;

	fprintf(stderr, "usbip: import request for '%s'\n", busid);

	reply.version = htons(USBIP_VERSION);
	reply.code    = htons(OP_REP_IMPORT);
	reply.status  = htonl(ST_OK);

	if (send_exact(fd, &reply, sizeof(reply)) < 0)
		return -1;

	fill_usb_device(&ud, dev);
	if (send_exact(fd, &ud, sizeof(ud)) < 0)
		return -1;

	/* Connection stays open for URB phase */
	return 0;
}

/* Parse the USB setup packet from a CMD_SUBMIT (little-endian per USB spec) */
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

	if (send_exact(fd, &hdr, sizeof(hdr)) < 0)
		return -1;

	if (data && data_len > 0) {
		if (send_exact(fd, data, data_len) < 0)
			return -1;
	}

	return 0;
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

/*
 * Map an endpoint address to a 0-based interface index.
 * FTDI convention: interface N has bulk-IN 0x81+2N, bulk-OUT 0x02+2N.
 */
static int ep_to_intf(uint32_t ep, uint32_t direction)
{
	if (ep == 0)
		return 0;

	if (direction == USBIP_DIR_IN)
		return (ep - 1) / 2;
	else
		return (ep - 1) / 2;
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

	/* Read OUT data payload if present */
	if (direction == USBIP_DIR_OUT && buf_len > 0) {
		out_data = malloc(buf_len);
		if (!out_data)
			return -1;
		if (recv_exact(fd, out_data, buf_len) < 0) {
			free(out_data);
			return -1;
		}
	}

	/* Control endpoint (ep 0) */
	if (ep == 0) {
		uint8_t bmRequestType, bRequest;
		uint16_t wValue, wIndex, wLength;

		parse_setup(req->u.cmd_submit.setup,
			    &bmRequestType, &bRequest,
			    &wValue, &wIndex, &wLength);

		resp_len = ftdi_emu_control(dev, bmRequestType, bRequest,
					    wValue, wIndex, wLength, resp);

		free(out_data);

		if (resp_len < 0) {
			/* STALL */
			return send_ret_submit(fd, seqnum, -32 /* -EPIPE */,
					       0, NULL, 0);
		}

		if (direction == USBIP_DIR_IN && resp_len > 0)
			return send_ret_submit(fd, seqnum, 0, resp_len,
					       resp, resp_len);

		return send_ret_submit(fd, seqnum, 0, 0, NULL, 0);
	}

	intf = ep_to_intf(ep, direction);

	/* Bulk OUT -- feed data to the MPSSE engine */
	if (direction == USBIP_DIR_OUT) {
		ftdi_emu_bulk_out(dev, intf, out_data, buf_len);
		free(out_data);
		return send_ret_submit(fd, seqnum, 0, buf_len, NULL, 0);
	}

	/* Bulk IN -- return pending response with FTDI status header */
	free(out_data);
	resp_len = ftdi_emu_bulk_in(dev, intf, resp, sizeof(resp));
	return send_ret_submit(fd, seqnum, 0, resp_len, resp, resp_len);
}

static int urb_loop(int fd, struct ftdi_device *dev)
{
	struct usbip_header hdr;

	fprintf(stderr, "usbip: entering URB dispatch loop\n");

	while (server_running) {
		if (recv_exact(fd, &hdr, sizeof(hdr)) < 0) {
			fprintf(stderr, "usbip: client disconnected\n");
			return 0;
		}

		uint32_t cmd = ntohl(hdr.base.command);

		switch (cmd) {
		case USBIP_CMD_SUBMIT:
			if (handle_cmd_submit(fd, dev, &hdr) < 0)
				return -1;
			break;

		case USBIP_CMD_UNLINK:
			/*
			 * We process URBs synchronously so nothing is pending.
			 * Reply that the URB already completed.
			 */
			if (send_ret_unlink(fd, ntohl(hdr.base.seqnum), 0) < 0)
				return -1;
			break;

		default:
			fprintf(stderr, "usbip: unknown command 0x%08x\n", cmd);
			return -1;
		}
	}

	return 0;
}

/* Handle one client connection: management phase then URB phase */
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
		/* Import succeeded -- enter URB phase on same connection */
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
	int opt = 1;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
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
		port, dev->vid, dev->pid, dev->bcd);

	while (server_running) {
		struct sockaddr_in peer;
		socklen_t peerlen = sizeof(peer);

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

	close(listen_fd);
	return 0;
}
