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
#   make uml-kernel          # one-time: build UML kernel (~2-5 min)
#   make test                # build modules + initramfs, run tests (~5 sec)
#   make test-<mode>         # any ftdi_mode of init.sh, e.g. test-stress-spi
#   make test-eeprom         # EEPROM-driven mode selection, one image each
#   make test-stress         # concurrency / unbind / hotplug / param stress
#
# Debug kernel (KASAN, lockdep, kmemleak, UBSAN, failslab; uniprocessor):
#   make uml-kernel-debug
#   make test-stress test-stress-failslab DEBUG=1
#
# SMP kernel (4 CPUs, UBSAN, debug objects, hung task detector):
#   make uml-kernel-smp
#   make test-stress SMP=1
#
# A run passes only if init.sh printed its summary line with 0 failures
# and the kernel log has no splat (see check_log.sh).

LINUX_SRC  ?= $(HOME)/dev/linux
UML_BUILD_STD   := $(CURDIR)/.uml-build
UML_BUILD_DEBUG := $(CURDIR)/.uml-build-debug
UML_BUILD_SMP   := $(CURDIR)/.uml-build-smp
ifneq ($(DEBUG),)
UML_BUILD  ?= $(UML_BUILD_DEBUG)
UML_MEM    ?= 1G
else ifneq ($(SMP),)
UML_BUILD  ?= $(UML_BUILD_SMP)
UML_MEM    ?= 512M
UML_EXTRA  ?= seccomp=on ncpus=4
else
UML_BUILD  ?= $(UML_BUILD_STD)
UML_MEM    ?= 256M
endif
UML_EXTRA  ?=
TEST_TIMEOUT ?= 120
BUSYBOX    ?= /usr/bin/busybox
FTDI_SRC   := $(realpath $(CURDIR)/..)
EMU_SRC    := $(FTDI_SRC)/ftdi-emulation

# Modules and initramfs are specific to the UML kernel they run on
MODDIR     := $(UML_BUILD)/ftdi-modules
ROOTFS     := $(UML_BUILD)/rootfs
INITRAMFS  := $(UML_BUILD)/initramfs.cpio.gz

# UML only supports x86 -- bail out early on other architectures
HOST_ARCH  := $(shell uname -m)
ifneq ($(filter $(HOST_ARCH),x86_64 i686 i386),)
else
$(error UML requires an x86 host (detected $(HOST_ARCH)))
endif

TEST_TOOLS := i2c_test spi_test uart_test gpio_test hotplug_test error_test \
	      stress_test uart_drain_test uart_flow_test spi_echo_test \
	      rs485_test

# EEPROM-driven mode selection, one image per detection path
EEPROM_MODES := ee-product-i2c ee-vcp-uart ee-slew-i2c ee-default-spi \
		ee-cbus-1a ee-fifo ee-blank ee-badsum ee-tag-ff ee-param-wins \
		ee-ft2232h-chan ee-ft2232h-drive ee-ft4232h-vcp ee-93c46 \
		ee-custom-pid

STRESS_MODES := stress-spi stress-i2c stress-uart stress-uart-errors \
		stress-ft2232h stress-ft4232h stress-unbind stress-hotplug \
		stress-params

# Suspend and resume callbacks, run by kmod/ftdi_pm_test.ko with the
# emulated chip power-cycled in between
PM_MODES := suspend suspend-i2c suspend-uart suspend-ft4232h

# A short run of the concurrency tests, for CI
STRESS_SHORT_MODES := stress-spi stress-i2c stress-uart stress-ft2232h \
		      stress-ft4232h

# What is on the wire, against the emulator's I2C bus and SPI slaves:
# clock stretching, I2C timing, SPI modes, SPI delays and chip select
BUS_MODES := i2c-ad7-stretch i2c-ad7-stretch-short i2c-ad7-late \
	     i2c-ad7-missing i2c-an113 spi-modes spi-timing spi-cs \
	     spi-cs-high

.PHONY: all test test-all test-errors test-i2c-speeds \
	test-eeprom test-stress test-stress-short test-bus test-pm uml-kernel uml-kernel-debug uml-kernel-smp modules usbip-modules \
	usbip_attach ftdi_usbip initramfs clean

all: test

# --- UML kernels -----------------------------------------------------------

uml-kernel: $(UML_BUILD_STD)/linux

$(UML_BUILD_STD)/linux: uml.config
	mkdir -p $(UML_BUILD_STD)
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_STD) ARCH=um defconfig
	cd $(LINUX_SRC) && ARCH=um scripts/kconfig/merge_config.sh \
		-O $(UML_BUILD_STD) $(UML_BUILD_STD)/.config $(CURDIR)/uml.config
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_STD) ARCH=um -j$$(nproc)

uml-kernel-debug: $(UML_BUILD_DEBUG)/linux

$(UML_BUILD_DEBUG)/linux: uml.config uml-debug.config
	mkdir -p $(UML_BUILD_DEBUG)
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_DEBUG) ARCH=um defconfig
	cd $(LINUX_SRC) && ARCH=um scripts/kconfig/merge_config.sh \
		-O $(UML_BUILD_DEBUG) $(UML_BUILD_DEBUG)/.config \
		$(CURDIR)/uml.config $(CURDIR)/uml-debug.config
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_DEBUG) ARCH=um -j$$(nproc)

uml-kernel-smp: $(UML_BUILD_SMP)/linux

$(UML_BUILD_SMP)/linux: uml.config uml-smp.config
	mkdir -p $(UML_BUILD_SMP)
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_SMP) ARCH=um defconfig
	cd $(LINUX_SRC) && ARCH=um scripts/kconfig/merge_config.sh \
		-O $(UML_BUILD_SMP) $(UML_BUILD_SMP)/.config \
		$(CURDIR)/uml.config $(CURDIR)/uml-smp.config
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD_SMP) ARCH=um -j$$(nproc)

# --- FTDI modules (built against the selected UML kernel) ------------------

FTDI_MODULES := ftdi_mpsse.ko ftdi_uart.ko ftdi_spi.ko ftdi_i2c.ko ftdi_gpio.ko ftdi_gpio_bitbang.ko
FTDI_SOURCES := $(wildcard $(FTDI_SRC)/ftdi_*.c $(FTDI_SRC)/ftdi_*.h) $(FTDI_SRC)/Makefile

# Test-only kernel modules (kmod/): an SPI client for what spidev cannot ask
KMOD_SRC     := $(CURDIR)/kmod
KMOD_MODULES := ftdi_spi_cstest.ko ftdi_pm_test.ko
KMOD_SOURCES := $(wildcard $(KMOD_SRC)/*.c) $(KMOD_SRC)/Makefile

modules: $(MODDIR)/.stamp

# Rebuilt whenever a driver source or the kernel changes
$(MODDIR)/.stamp: $(UML_BUILD)/linux $(FTDI_SOURCES) $(KMOD_SOURCES)
	$(MAKE) -C $(FTDI_SRC) ARCH=um KDIR=$(UML_BUILD)
	$(MAKE) -C $(UML_BUILD) M=$(KMOD_SRC) ARCH=um modules
	mkdir -p $(MODDIR)
	cp $(addprefix $(FTDI_SRC)/,$(FTDI_MODULES)) \
	   $(addprefix $(KMOD_SRC)/,$(KMOD_MODULES)) $(MODDIR)/
	touch $@

# --- USB/IP kernel modules (from UML build tree) --------------------------

USBIP_BUILD_MODULES := $(UML_BUILD)/drivers/usb/usbip/usbip-core.ko \
                       $(UML_BUILD)/drivers/usb/usbip/vhci-hcd.ko

usbip-modules: $(USBIP_BUILD_MODULES)

# Rebuild USB/IP modules if missing from UML build tree
$(USBIP_BUILD_MODULES): $(UML_BUILD)/linux
	$(MAKE) -C $(LINUX_SRC) O=$(UML_BUILD) ARCH=um -j$$(nproc)

# --- Userspace tools (statically linked for initramfs) ---------------------

usbip_attach: usbip_attach.c $(EMU_SRC)/usbip_proto.h
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

ftdi_usbip:
	$(MAKE) -C $(EMU_SRC) clean
	$(MAKE) -C $(EMU_SRC) CFLAGS="-Wall -Wextra -O2" LDFLAGS="-static"
	cp $(EMU_SRC)/ftdi_usbip $(CURDIR)/

$(TEST_TOOLS): %: %.c
	$(CC) -Wall -Wextra -O2 -static -pthread -o $@ $<

spi_test i2c_test hotplug_test: counter.h

# --- Initramfs assembly ---------------------------------------------------

initramfs: $(INITRAMFS)

$(INITRAMFS): $(MODDIR)/.stamp $(USBIP_BUILD_MODULES) usbip_attach ftdi_usbip \
              $(TEST_TOOLS) init.sh known_splats.awk
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/dev $(ROOTFS)/proc $(ROOTFS)/sys \
		 $(ROOTFS)/lib/modules $(ROOTFS)/usr/bin $(ROOTFS)/tmp
	cp $(BUSYBOX) $(ROOTFS)/bin/busybox
	ln -sf busybox $(ROOTFS)/bin/sh
	cp usbip_attach ftdi_usbip $(TEST_TOOLS) $(ROOTFS)/usr/bin/
	cp $(MODDIR)/*.ko $(USBIP_BUILD_MODULES) $(ROOTFS)/lib/modules/
	cp init.sh $(ROOTFS)/init && chmod +x $(ROOTFS)/init
	cp known_splats.awk $(ROOTFS)/
	(cd $(ROOTFS) && find . | cpio -o -H newc --quiet | gzip -9) > $@

# --- Test runner -----------------------------------------------------------

# Run one ftdi_mode in UML and judge it strictly with check_log.sh.
# Usage: $(call run_test,MODE)
# A recipe line break, to run one run_test after the other in a foreach
define newline


endef

# The UML console reads fd 0: at end of file (stdin a closed pipe, as in
# CI) it hangs up and drops all later output, kernel log included.
define run_test
	@echo "=== Running FTDI $(1) test in UML ==="
	@{ timeout $(TEST_TIMEOUT) $(UML_BUILD)/linux \
		initrd=$(INITRAMFS) \
		mem=$(UML_MEM) $(UML_EXTRA) \
		ftdi_mode=$(1) \
		con=null con0=fd:0,fd:1 < /dev/null; \
	  echo "UML_EXIT=$$?"; } 2>&1 | tee $(CURDIR)/test-$(1).log
	@sh $(CURDIR)/check_log.sh $(1) $(CURDIR)/test-$(1).log
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
	$(call run_test,ft2232h-i2c)
	$(call run_test,ft2232h-uart)
	$(call run_test,ft2232h-mixed)
	$(call run_test,ft4232h)
	$(call run_test,ft4232h-i2c)
	$(call run_test,ft4232h-uart)
	$(call run_test,ft4232h-cd)
	$(call run_test,ft4232h-mixed)
	$(call run_test,i2c-ad7)
	$(call run_test,i2c-map)
	$(call run_test,i2c-map-chan)
	$(call run_test,i2c-ad7-gpio)
	$(call run_test,uart-drain)
	$(call run_test,uart-flow)
	$(call run_test,uart-rs485)
	$(call run_test,uart-rs485-ft4232h)
	$(call run_test,latency)
	@echo "=== ALL TESTS PASSED (SPI + I2C + UART + FT2232H + FT4232H + FT4232H-CD + latency) ==="

test-errors: $(INITRAMFS)
	$(call run_test,i2c-nak)
	$(call run_test,i2c-stuck)
	$(call run_test,i2c-check-free)
	$(call run_test,i2c-stretch)
	$(call run_test,i2c-scl-stuck)
	$(call run_test,i2c-stuck-byte)
	$(call run_test,spi-error)
	$(call run_test,spi-halt)
	$(call run_test,uart-halt)
	$(call run_test,spi-stall)
	$(call run_test,i2c-stall)
	$(call run_test,mpsse-desync)
	$(call run_test,mpsse-sync-fail)
	@echo "=== ALL ERROR TESTS PASSED ==="

test-i2c-speeds: $(INITRAMFS)
	$(call run_test,i2c-100k)
	$(call run_test,i2c-400k)
	@echo "=== ALL I2C SPEED TESTS PASSED ==="

test-eeprom: $(INITRAMFS)
	$(foreach m,$(EEPROM_MODES),$(call run_test,$(m))$(newline))
	@echo "=== ALL EEPROM TESTS PASSED ==="

test-stress: $(INITRAMFS)
	$(foreach m,$(STRESS_MODES),$(call run_test,$(m))$(newline))
	@echo "=== ALL STRESS TESTS PASSED ==="

test-bus: $(INITRAMFS)
	$(foreach m,$(BUS_MODES),$(call run_test,$(m))$(newline))
	@echo "=== ALL BUS TESTS PASSED ==="

test-pm: $(INITRAMFS)
	$(foreach m,$(PM_MODES),$(call run_test,$(m))$(newline))
	@echo "=== ALL SUSPEND/RESUME TESTS PASSED ==="

# 5 s of stress per mode instead of 15
test-stress-short: UML_EXTRA += ftdi_stress_secs=5
test-stress-short: $(INITRAMFS)
	$(foreach m,$(STRESS_SHORT_MODES),$(call run_test,$(m))$(newline))
	@echo "=== ALL SHORT STRESS TESTS PASSED ==="

# Any other ftdi_mode of init.sh: make test-<mode>
test-%: $(INITRAMFS)
	$(call run_test,$*)

# --- Cleanup ---------------------------------------------------------------

clean:
	rm -rf $(UML_BUILD_STD)/rootfs $(UML_BUILD_STD)/ftdi-modules \
	       $(UML_BUILD_STD)/initramfs.cpio.gz \
	       $(UML_BUILD_DEBUG)/rootfs $(UML_BUILD_DEBUG)/ftdi-modules \
	       $(UML_BUILD_DEBUG)/initramfs.cpio.gz \
	       $(UML_BUILD_SMP)/rootfs $(UML_BUILD_SMP)/ftdi-modules \
	       $(UML_BUILD_SMP)/initramfs.cpio.gz $(CURDIR)/test-*.log
	rm -f usbip_attach ftdi_usbip $(TEST_TOOLS)
	$(MAKE) -C $(EMU_SRC) clean 2>/dev/null || true
	$(MAKE) -C $(FTDI_SRC) clean 2>/dev/null || true
	rm -f $(addprefix $(KMOD_SRC)/,*.o *.ko *.mod *.mod.c .*.cmd \
		Module.symvers modules.order)
