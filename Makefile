# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile

ftdi_mpsse-y := ftdi_mpsse_core.o ftdi_eeprom.o
obj-m := ftdi_mpsse.o ftdi_uart.o ftdi_spi.o ftdi_i2c.o ftdi_gpio.o ftdi_gpio_bitbang.o

KDIR ?= /lib/modules/$(shell uname -r)/build
CHECKPATCH ?= $(KDIR)/scripts/checkpatch.pl
SPARSE ?= $(HOME)/bin/sparse
SMATCH ?= $(HOME)/bin/smatch

.PHONY: all modules modules_install tools check sparse smatch checkpatch coccicheck test clean

all: modules

modules modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

check: sparse smatch checkpatch

sparse:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=2 CHECK=$(SPARSE) modules

smatch:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=2 CHECK="$(SMATCH) -p=kernel" modules

checkpatch:
	@$(CHECKPATCH) --strict --no-tree -f \
		$(filter-out %.mod.c,$(wildcard ftdi_*.c ftdi_*.h)) \
		|| test "$$?" = 1

coccicheck:
	$(MAKE) -C $(KDIR) M=$(CURDIR) coccicheck MODE=report

tools:
	$(CC) -Wall -Wextra -o tools/ftdi_eeprom_check tools/ftdi_eeprom_check.c
	$(MAKE) -C ftdi-emulation

test:
	$(MAKE) -C tests test

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@
	$(RM) tools/ftdi_eeprom_check
	$(MAKE) -C ftdi-emulation clean
