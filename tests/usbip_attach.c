/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * usbip_attach -- minimal USB/IP attach helper for UML testing
 *
 * Connects to a USB/IP server, imports a device, and attaches it to
 * the kernel's vhci_hcd via sysfs.  Stays alive so the socket remains
 * open (vhci_hcd needs the fd to stay valid).
 *
 * Usage: usbip_attach <host> <port> <busid>
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "../ftdi-emulation/usbip_proto.h"

#define VHCI_STATUS_PATH "/sys/devices/platform/vhci_hcd.0/status"
#define VHCI_ATTACH_PATH "/sys/devices/platform/vhci_hcd.0/attach"

/* VHCI port status codes */
#define VDEV_ST_NULL	0x004

static int recv_exact(int fd, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = recv(fd, (char *)buf + done, n - done, 0);

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

/*
 * Find a free vhci port by scanning the status sysfs file.
 * Lines look like: "hs  0000 004 000 00000000 000000 0-0"
 * A port with status 004 (VDEV_ST_NULL) is free.
 */
static int find_free_port(void)
{
	FILE *fp;
	char line[256];
	int port;
	unsigned int status;

	fp = fopen(VHCI_STATUS_PATH, "r");
	if (!fp) {
		perror("open vhci status");
		return -1;
	}

	/* Skip the header line */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%*s %d %x", &port, &status) == 2) {
			if (status == VDEV_ST_NULL) {
				fclose(fp);
				return port;
			}
		}
	}

	fclose(fp);
	fprintf(stderr, "usbip_attach: no free vhci port\n");
	return -1;
}

int main(int argc, char *argv[])
{
	struct sockaddr_in addr;
	struct op_common hdr;
	char busid_buf[SYSFS_BUS_ID_SIZE];
	struct usbip_usb_device udev;
	int sockfd, port;
	FILE *fp;

	if (argc < 4) {
		fprintf(stderr, "Usage: %s <host> <port> <busid>\n", argv[0]);
		return 1;
	}

	/* Connect to USB/IP server */
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(argv[2]));
	if (inet_pton(AF_INET, argv[1], &addr.sin_addr) <= 0) {
		perror("inet_pton");
		return 1;
	}

	if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}

	/* Send OP_REQ_IMPORT */
	hdr.version = htons(USBIP_VERSION);
	hdr.code = htons(OP_REQ_IMPORT);
	hdr.status = 0;
	if (send_exact(sockfd, &hdr, sizeof(hdr)) < 0) {
		fprintf(stderr, "usbip_attach: send op_common failed\n");
		return 1;
	}

	/* Send 32-byte busid */
	memset(busid_buf, 0, sizeof(busid_buf));
	strncpy(busid_buf, argv[3], sizeof(busid_buf) - 1);
	if (send_exact(sockfd, busid_buf, sizeof(busid_buf)) < 0) {
		fprintf(stderr, "usbip_attach: send busid failed\n");
		return 1;
	}

	/* Read OP_REP_IMPORT reply */
	if (recv_exact(sockfd, &hdr, sizeof(hdr)) < 0) {
		fprintf(stderr, "usbip_attach: recv reply failed\n");
		return 1;
	}

	if (ntohl(hdr.status) != ST_OK) {
		fprintf(stderr, "usbip_attach: import failed (status %u)\n",
			ntohl(hdr.status));
		return 1;
	}

	/* Read device descriptor */
	if (recv_exact(sockfd, &udev, sizeof(udev)) < 0) {
		fprintf(stderr, "usbip_attach: recv device failed\n");
		return 1;
	}

	fprintf(stderr, "usbip_attach: imported %04x:%04x speed=%u\n",
		ntohs(udev.idVendor), ntohs(udev.idProduct),
		ntohl(udev.speed));

	/* Find a free vhci port */
	port = find_free_port();
	if (port < 0)
		return 1;

	/*
	 * Attach via sysfs: write "port fd devid speed" to the attach file.
	 * devid = (busnum << 16) | devnum
	 */
	fp = fopen(VHCI_ATTACH_PATH, "w");
	if (!fp) {
		perror("open vhci attach");
		return 1;
	}

	fprintf(fp, "%d %d %u %u\n",
		port, sockfd,
		(ntohl(udev.busnum) << 16) | ntohl(udev.devnum),
		ntohl(udev.speed));
	fclose(fp);

	fprintf(stderr, "usbip_attach: attached on port %d\n", port);

	/* Keep alive -- vhci_hcd needs the socket fd to stay open */
	pause();
	return 0;
}
