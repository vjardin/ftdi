/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB/IP server -- TCP listener and URB dispatch loop
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include "ftdi_emu.h"

/*
 * Run the USB/IP server on the given TCP port.
 * Listens for connections, handles DEVLIST and IMPORT, then enters
 * the URB dispatch loop for the emulated FTDI device.
 * Returns 0 on clean shutdown, negative on error.
 */
int usbip_server_run(struct ftdi_device *dev, int port);
