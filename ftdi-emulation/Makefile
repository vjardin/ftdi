# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile

CFLAGS ?= -Wall -Wextra -O2 -g
LDFLAGS ?=

SRCS := main.c ftdi_emu.c usbip_server.c
OBJS := $(SRCS:.c=.o)
BIN  := ftdi_usbip

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: ftdi_emu.h usbip_server.h usbip_proto.h
ftdi_emu.o: ftdi_emu.h
usbip_server.o: usbip_proto.h usbip_server.h ftdi_emu.h

clean:
	$(RM) $(OBJS) $(BIN)
