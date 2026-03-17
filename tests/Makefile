# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
#
# UML-based test framework for FTDI kernel modules
#
# Builds a User Mode Linux kernel, compiles the FTDI modules against it,
# assembles an initramfs with the FTDI USB/IP emulator, and runs the
# whole thing as an unprivileged process.
#
# Usage:
#   make uml-kernel    # one-time: build UML kernel (~2-5 min)
#   make test          # build modules + initramfs, run tests (~5 sec)

LINUX_SRC  ?= $(HOME)/dev/linux
UML_BUILD  ?= $(CURDIR)/.uml-build
BUSYBOX    ?= /usr/bin/busybox
FTDI_SRC   := $(realpath $(CURDIR)/..)
EMU_SRC    := $(FTDI_SRC)/ftdi-emulation

ROOTFS     := $(CURDIR)/.rootfs
INITRAMFS  := $(CURDIR)/initramfs.cpio.gz

# UML only supports x86 -- bail out early on other architectures
HOST_ARCH  := $(shell uname -m)
ifneq ($(filter $(HOST_ARCH),x86_64 i686 i386),)
else
$(error UML requires an x86 host (detected $(HOST_ARCH)))
endif

# BusyBox applets needed inside the initramfs
BB_APPLETS := sh mount insmod cat ls dmesg grep ifconfig sleep \
	      poweroff test echo mkdir rm ln

.PHONY: all test test-spi test-i2c test-uart test-ft2232h test-ft4232h \
	test-ft4232h-cd test-hotplug test-errors test-i2c-nak test-i2c-stuck \
	test-mpsse-sync-fail \
	test-suspend uml-kernel modules usbip-modules usbip_attach \
	ftdi_usbip initramfs clean

all: test

# --- UML kernel -----------------------------------------------------------

uml-kernel: $(UML_BUILD)/linux

$(UML_BUILD)/linux: uml.config
	mkdir -p $(UML_BUILD)
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD) ARCH=um defconfig
	cd $(LINUX_SRC) && ARCH=um scripts/kconfig/merge_config.sh \
		-O $(UML_BUILD) $(UML_BUILD)/.config $(CURDIR)/uml.config
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD) ARCH=um -j$$(nproc)

# --- FTDI modules (built against UML kernel) -------------------------------

FTDI_MODULES := ftdi_mpsse.ko ftdi_uart.ko ftdi_spi.ko ftdi_i2c.ko ftdi_gpio.ko ftdi_gpio_bitbang.ko

modules: $(FTDI_MODULES)

$(FTDI_MODULES): $(UML_BUILD)/linux
	$(MAKE) -C $(FTDI_SRC) ARCH=um KDIR=$(UML_BUILD)
	cp $(addprefix $(FTDI_SRC)/,$(FTDI_MODULES)) $(CURDIR)/

# --- USB/IP kernel modules (from UML build tree) --------------------------

USBIP_MODULES := usbip-core.ko vhci-hcd.ko
USBIP_BUILD_MODULES := $(UML_BUILD)/drivers/usb/usbip/usbip-core.ko \
                       $(UML_BUILD)/drivers/usb/usbip/vhci-hcd.ko

usbip-modules: $(USBIP_MODULES)

# Rebuild USB/IP modules if missing from UML build tree
$(USBIP_BUILD_MODULES): $(UML_BUILD)/linux
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD) ARCH=um -j$$(nproc)

$(USBIP_MODULES): $(USBIP_BUILD_MODULES)
	cp $(UML_BUILD)/drivers/usb/usbip/usbip-core.ko $(CURDIR)/
	cp $(UML_BUILD)/drivers/usb/usbip/vhci-hcd.ko $(CURDIR)/

# --- Userspace tools (statically linked for initramfs) ---------------------

usbip_attach: usbip_attach.c $(EMU_SRC)/usbip_proto.h
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

ftdi_usbip:
	$(MAKE) -C $(EMU_SRC) clean
	$(MAKE) -C $(EMU_SRC) CFLAGS="-Wall -Wextra -O2" LDFLAGS="-static"
	cp $(EMU_SRC)/ftdi_usbip $(CURDIR)/

# I2C, SPI, UART, GPIO, and hot-unplug test tools
i2c_test: i2c_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

spi_test: spi_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

uart_test: uart_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

gpio_test: gpio_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

hotplug_test: hotplug_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

error_test: error_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

suspend_test: suspend_test.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

# --- Initramfs assembly ---------------------------------------------------

initramfs: $(INITRAMFS)

$(INITRAMFS): $(FTDI_MODULES) $(USBIP_MODULES) usbip_attach ftdi_usbip \
              i2c_test spi_test uart_test gpio_test hotplug_test error_test suspend_test init.sh
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/dev $(ROOTFS)/proc $(ROOTFS)/sys \
		 $(ROOTFS)/lib/modules $(ROOTFS)/usr/bin $(ROOTFS)/tmp
	cp $(BUSYBOX) $(ROOTFS)/bin/busybox
	for cmd in $(BB_APPLETS); do \
		ln -sf busybox $(ROOTFS)/bin/$$cmd; \
	done
	cp usbip_attach $(ROOTFS)/usr/bin/
	cp ftdi_usbip $(ROOTFS)/usr/bin/
	cp i2c_test spi_test uart_test gpio_test hotplug_test error_test suspend_test $(ROOTFS)/usr/bin/
	cp $(FTDI_MODULES) $(USBIP_MODULES) $(ROOTFS)/lib/modules/
	cp init.sh $(ROOTFS)/init && chmod +x $(ROOTFS)/init
	(cd $(ROOTFS) && find . | cpio -o -H newc --quiet | gzip -9) > $@

# --- Test runner -----------------------------------------------------------

# Run a single test with specified mode (spi or i2c)
# Usage: $(call run_test,MODE)
define run_test
	@echo "=== Running FTDI $(1) test in UML ==="
	@timeout 30 $(UML_BUILD)/linux \
		initrd=$(INITRAMFS) \
		mem=256M \
		ftdi_mode=$(1) \
		con=null con0=fd:0,fd:1 \
		2>&1 | tee $(CURDIR)/test-$(1).log; \
	rc=$$$$?; \
	if grep -q "FAIL:" $(CURDIR)/test-$(1).log; then \
		echo "=== $(1) TEST FAILED ==="; \
		exit 1; \
	elif [ $$$$rc -ne 0 ] && [ $$$$rc -ne 124 ]; then \
		echo "=== UML exited with code $$$$rc ==="; \
		exit 1; \
	else \
		echo "=== $(1) TEST PASSED ==="; \
	fi
endef

test: $(INITRAMFS)
	$(call run_test,spi)
	$(call run_test,i2c)
	$(call run_test,uart)
	@echo "=== ALL TESTS PASSED (SPI + I2C + UART) ==="

test-all: $(INITRAMFS)
	$(call run_test,spi)
	$(call run_test,i2c)
	$(call run_test,uart)
	$(call run_test,ft2232h)
	$(call run_test,ft4232h)
	$(call run_test,ft4232h-cd)
	@echo "=== ALL TESTS PASSED (SPI + I2C + UART + FT2232H + FT4232H + FT4232H-CD) ==="

test-spi: $(INITRAMFS)
	$(call run_test,spi)

test-i2c: $(INITRAMFS)
	$(call run_test,i2c)

test-uart: $(INITRAMFS)
	$(call run_test,uart)

test-ft2232h: $(INITRAMFS)
	$(call run_test,ft2232h)

test-ft4232h: $(INITRAMFS)
	$(call run_test,ft4232h)

test-ft4232h-cd: $(INITRAMFS)
	$(call run_test,ft4232h-cd)

test-hotplug: $(INITRAMFS)
	$(call run_test,hotplug)

test-i2c-nak: $(INITRAMFS)
	$(call run_test,i2c-nak)

test-i2c-stuck: $(INITRAMFS)
	$(call run_test,i2c-stuck)

test-errors: $(INITRAMFS)
	$(call run_test,i2c-nak)
	$(call run_test,i2c-stuck)
	@echo "=== ALL ERROR TESTS PASSED ==="

test-mpsse-sync-fail: $(INITRAMFS)
	$(call run_test,mpsse-sync-fail)

test-suspend: $(INITRAMFS)
	$(call run_test,suspend)

# --- Cleanup ---------------------------------------------------------------

clean:
	rm -rf $(ROOTFS) $(INITRAMFS) $(CURDIR)/test-*.log
	rm -f $(FTDI_MODULES) $(USBIP_MODULES) usbip_attach ftdi_usbip
	rm -f i2c_test spi_test uart_test gpio_test hotplug_test error_test suspend_test
	$(MAKE) -C $(EMU_SRC) clean 2>/dev/null || true
	$(MAKE) -C $(FTDI_SRC) clean 2>/dev/null || true
