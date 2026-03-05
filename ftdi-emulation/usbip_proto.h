/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB/IP wire protocol definitions
 *
 * Mirrors the structures from linux/drivers/usb/usbip/ and
 * linux/tools/usb/usbip/ so we can implement a standalone TCP server
 * that speaks the same binary protocol as the kernel's vhci_hcd expects.
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include <stdint.h>

#define USBIP_PORT		3240
#define USBIP_VERSION		0x0111

/* Management phase operation codes */
#define OP_REQ_DEVLIST		0x8005
#define OP_REP_DEVLIST		0x0005
#define OP_REQ_IMPORT		0x8003
#define OP_REP_IMPORT		0x0003

/* Management status codes */
#define ST_OK			0x00
#define ST_NA			0x01
#define ST_DEV_BUSY		0x02
#define ST_DEV_ERR		0x03
#define ST_NODEV		0x04
#define ST_ERROR		0x05

/* URB phase command codes */
#define USBIP_CMD_SUBMIT	0x00000001
#define USBIP_RET_SUBMIT	0x00000003
#define USBIP_CMD_UNLINK	0x00000002
#define USBIP_RET_UNLINK	0x00000004

/* URB direction */
#define USBIP_DIR_OUT		0
#define USBIP_DIR_IN		1

/* USB speeds */
#define USB_SPEED_HIGH		3

/* USB/IP-specific URB transfer flags */
#define USBIP_URB_SHORT_NOT_OK	0x0001
#define USBIP_URB_DIR_IN	0x0200

#define SYSFS_PATH_MAX		256
#define SYSFS_BUS_ID_SIZE	32

/* Management phase: 8-byte common header for all operations */
struct op_common {
	uint16_t version;
	uint16_t code;
	uint32_t status;
} __attribute__((packed));

/*
 * Device description sent over the wire.
 * Field order and sizes match the kernel's usbip_usb_device exactly so
 * the client-side vhci_hcd can parse the import reply without conversion.
 */
struct usbip_usb_device {
	char     path[SYSFS_PATH_MAX];
	char     busid[SYSFS_BUS_ID_SIZE];
	uint32_t busnum;
	uint32_t devnum;
	uint32_t speed;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bConfigurationValue;
	uint8_t  bNumConfigurations;
	uint8_t  bNumInterfaces;
} __attribute__((packed));

/* Interface summary sent alongside each device in OP_REP_DEVLIST */
struct usbip_usb_interface {
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t padding;
} __attribute__((packed));

/* URB phase: 20-byte basic header */
struct usbip_header_basic {
	uint32_t command;
	uint32_t seqnum;
	uint32_t devid;
	uint32_t direction;
	uint32_t ep;
} __attribute__((packed));

struct usbip_header_cmd_submit {
	uint32_t transfer_flags;
	int32_t  transfer_buffer_length;
	int32_t  start_frame;
	int32_t  number_of_packets;
	int32_t  interval;
	uint8_t  setup[8];
} __attribute__((packed));

struct usbip_header_ret_submit {
	int32_t  status;
	int32_t  actual_length;
	int32_t  start_frame;
	int32_t  number_of_packets;
	int32_t  error_count;
} __attribute__((packed));

struct usbip_header_cmd_unlink {
	uint32_t seqnum;
} __attribute__((packed));

struct usbip_header_ret_unlink {
	int32_t status;
} __attribute__((packed));

/*
 * Full 48-byte URB header -- the union is always padded to 28 bytes
 * so every command occupies exactly 48 bytes on the wire.
 */
struct usbip_header {
	struct usbip_header_basic base;
	union {
		struct usbip_header_cmd_submit cmd_submit;
		struct usbip_header_ret_submit ret_submit;
		struct usbip_header_cmd_unlink cmd_unlink;
		struct usbip_header_ret_unlink ret_unlink;
		uint8_t padding[28];
	} u;
} __attribute__((packed));
