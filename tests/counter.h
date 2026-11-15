/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * The emulated counter devices (SPI behind any chip select, I2C at 0x20,
 * 0x22, 0x50 and 0x51) answer every byte read with the previous one plus
 * 1, from one transfer to the next too.
 */

#ifndef FTDI_TESTS_COUNTER_H
#define FTDI_TESTS_COUNTER_H

#include <stdint.h>
#include <stdio.h>

/* Bytes read must continue the counter; *last is -1 before the first one */
static inline int counter_ok(const char *what, const uint8_t *rx, int len,
			     int *last)
{
	int i;

	for (i = 0; i < len; i++) {
		if (*last >= 0 && rx[i] != (uint8_t)(*last + 1)) {
			fprintf(stderr, "FAIL: %s: byte %d is %02x after %02x, expected the counter\n",
				what, i, rx[i], *last);
			return 0;
		}
		*last = rx[i];
	}
	return 1;
}

#endif
