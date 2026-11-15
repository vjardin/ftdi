/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB/IP server -- TCP listener and URB dispatch loop
 *
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 */

#pragma once

#include "ftdi_emu.h"

/*
 * Listen on the TCP port, answer DEVLIST and IMPORT, then serve the URBs of
 * the importing client. Returns 0 on clean shutdown, negative on error.
 */
int usbip_server_run(struct ftdi_device *dev, int port);

/*
 * Control FIFO (created if missing), read while a client is attached.
 * One command per line:
 *   error TYPE [COUNT]   arm an injection on every channel (TYPE as for
 *                        --error, COUNT 0 = for ever)
 *   power-cycle          as SIGUSR1
 *   stats                as SIGUSR2
 * Returns 0 on success, negative on error.
 */
int usbip_server_set_ctl(const char *path);
