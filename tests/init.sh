#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# init.sh -- UML initramfs init script for FTDI driver testing
#
# Starts the FTDI USB/IP emulator, attaches it via vhci_hcd, loads the
# FTDI modules and runs the assertions of the mode given on the kernel
# command line: ftdi_mode=<mode> (tests/README.md lists them).
#
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile

# Only /bin/busybox is guaranteed: mount /proc with it, then install
# every applet (--install resolves the binary through /proc/self/exe)
/bin/busybox mount -t proc none /proc
/bin/busybox --install -s 2>/dev/null
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mount -t debugfs none /sys/kernel/debug 2>/dev/null

FTDI_MODE="spi"
for arg in $(cat /proc/cmdline); do
	case "$arg" in
		ftdi_mode=*)
			FTDI_MODE="${arg#ftdi_mode=}"
			;;
		ftdi_stress_secs=*)
			STRESS_SECS="${arg#ftdi_stress_secs=}"
			;;
		ftdi_watchdog=*)
			WATCHDOG_SECS="${arg#ftdi_watchdog=}"
			;;
	esac
done
STRESS_SECS=${STRESS_SECS:-15}
echo "=== FTDI Test Mode: $FTDI_MODE ==="

# Debug aid (ftdi_watchdog=SECS): if the run is still going after SECS
# seconds of uptime, dump blocked tasks, the locks held (lockdep kernels)
# and CPU backtraces to the log.
if [ -n "$WATCHDOG_SECS" ]; then
	(
		while awk -v e="$WATCHDOG_SECS" '{ exit !($1 < e) }' /proc/uptime; do
			usleep 200000
		done
		echo "=== watchdog: ${WATCHDOG_SECS}s elapsed, dumping tasks ==="
		echo w > /proc/sysrq-trigger
		echo d > /proc/sysrq-trigger
		echo l > /proc/sysrq-trigger
	) &
fi

# USB/IP goes through the loopback interface
ifconfig lo 127.0.0.1 up

case "$FTDI_MODE" in
	ft2232h-spi)
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		;;
	ft2232h-i2c)
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="i2c"
		;;
	ft2232h-uart)
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="uart"
		;;
	ft2232h)
		# SPI: the GPIO tests need an MPSSE mode
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		;;
	ft4232h-spi)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	ft4232h-i2c)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="i2c"
		;;
	ft4232h-uart)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="uart"
		;;
	ft4232h)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	ft4232h-cd)
		# A+B SPI, C UART, D bit-bang GPIO (bus_mode parameter)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	ft2232h-mixed)
		# SPI on A, I2C on B (bus_mode parameter)
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		;;
	ft4232h-mixed)
		# I2C on A, SPI on B, UART on C, bit-bang GPIO on D
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	spi|i2c|uart)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="$FTDI_MODE"
		;;
	i2c-100k)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="i2c_speed=100"
		;;
	i2c-400k)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="i2c_speed=400"
		;;
	hotplug)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
	i2c-nak)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="i2c-nak"
		FTDI_ERROR_COUNT="1"
		;;
	i2c-stuck)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="i2c-stuck"
		FTDI_ERROR_COUNT="0"	# for good: the recovery cannot free it
		;;
	i2c-stretch)
		# A slave stretches the clock, AD7 not wired: the fault is
		# armed after probe (emu.ctl)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		;;
	i2c-scl-stuck)
		# A slave holds SCL low for good
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="i2c-scl-stuck"
		FTDI_ERROR_COUNT="0"
		;;
	i2c-stuck-byte)
		# A slave stuck in a byte, armed after probe (emu.ctl)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		;;
	i2c-ad7-stretch)
		# AD7 wired to SCL and a slave that stretches the clock after
		# every byte, before the repeated START and the STOP too
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		;;
	i2c-ad7-stretch-short)
		# A slave lets SCL go 2 us after the master, inside the setup
		# time of a repeated START or STOP: the condition it spoils
		# comes again, which the bus model times below the minimum
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		EXPECT_I2C_TIMING=1
		;;
	i2c-ad7-late)
		# SCL held low while the adapter is set up: the AD7 wiring is
		# checked once the bus is free
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		FTDI_ERROR="i2c-scl-stuck"
		FTDI_ERROR_COUNT="0"
		;;
	i2c-ad7-missing)
		# clock_stretching=1 on a board without the AD7 wire: the
		# MPSSE waits for RTCK for ever (AN_108 6.9)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="clock_stretching=1"
		EXPECT_RTCK_HANG=1
		;;
	i2c-an113)
		# strict_timing=0: the AN_113 timing, which the emulator
		# measures below the specification (UM10204)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="strict_timing=0"
		EXPECT_I2C_TIMING=1
		;;
	i2c-check-free)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="check_bus_free=1"
		;;
	spi-cs-high)
		# A second counter device on AD4, selected by a high level
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--spi-cs-high"
		;;
	spi-modes|spi-timing|spi-cs)
		# Shift-register slaves, one per SPI mode, on AD4-AD7
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--spi-echo"
		;;
	mpsse-sync-fail)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="mpsse-sync"
		FTDI_ERROR_COUNT="0"  # Permanent
		;;
	spi-error|spi-stall|spi-halt)
		# SPI error recovery: the fault is armed after probe (emu.ctl)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
	uart-halt)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		;;
	ee-product-i2c)
		# EEPROM-driven mode selection (ee-*): the emulator builds the
		# image a board maker would write, one detection path per mode.
		# Product string, with the VCP bit ftdi_eeprom sets by default.
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp a --ee-product FT232H-I2C-bridge"
		;;
	ee-vcp-uart)
		# VCP driver bit; a tag with an unknown value is ignored
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		FTDI_EMU_EXTRA="--ee-tag bus_mode=bogus"
		;;
	ee-slew-i2c)
		# D2XX, ADBUS (byte 0x0d on FT232H) slow slew + Schmitt
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp none --ee-set 0x0d=0x0c"
		;;
	ee-default-spi)
		# D2XX, nothing else: SPI. Slow slew + Schmitt on ACBUS (byte
		# 0x0c on FT232H) is not the data bus and must not count.
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp none --ee-set 0x0c=0x0c"
		;;
	ee-cbus-1a)
		# FT232H byte 0x1a is the ACBUS4/5 function (here TXDEN and
		# PWREN#, 0x49 = 'I'), not a protocol hint
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp none --ee-set 0x1a=0x49"
		;;
	ee-fifo)
		# Channel wired as a 245 FIFO: not for this driver
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag none --ee-set 0x00=0x01"
		EXPECT_PROBE_FAILURE=1
		;;
	ee-blank)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-blank"
		EXPECT_NO_EEPROM=1
		;;
	ee-93c46)
		# 93C46: 64 words, read back mirrored, no user area
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--ee-93c46 --ee-tag none --ee-vcp none --ee-product FT2232H-I2C-bridge"
		;;
	ee-custom-pid)
		# EEPROM bytes 4 and 5: the product ID
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-set 0x04=0x34 --ee-set 0x05=0x12"
		FTDI_NEW_ID="0403 1234"
		;;
	ee-badsum)
		# A tag in an image with a bad checksum is not trusted
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag bus_mode=i2c --ee-bad-checksum"
		EXPECT_NO_EEPROM=1
		;;
	ee-param-wins)
		# The module parameter beats the EEPROM tag (i2c here)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		;;
	ee-tag-ff)
		# The tag ends at erased cells (0xFF), not at a NUL: byte 0x2c
		# is the one after "bus_mode=i2c" at 0x20
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--ee-tag bus_mode=i2c --ee-vcp none --ee-set 0x2c=0xff"
		;;
	ee-ft2232h-chan)
		# One tag, one entry per channel
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag bus_mode=i2c,spi"
		;;
	ee-ft2232h-drive)
		# Each channel has its own drive group: BL (byte 0x0d low
		# nibble) slow slew + Schmitt makes B an I2C bus, not A
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp none --ee-set 0x0d=0x0c"
		;;
	ee-ft4232h-vcp)
		# FT4232H has no channel type: byte 0x01 = 0x09 is the VCP bit
		# of B plus a bit that means nothing, not "FIFO"
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		FTDI_EMU_EXTRA="--ee-tag none --ee-vcp b --ee-set 0x01=0x09"
		;;
	uart-drain|uart-flow)
		# UART with the baud-rate model
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		FTDI_EMU_EXTRA="--uart-pace"
		;;
	i2c-stall|mpsse-desync)
		# I2C error recovery: the fault is armed after probe (emu.ctl)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		;;
	suspend)
		# Suspend and resume of an SPI channel and its GPIO chip. The
		# suspend-* modes run the callbacks through ftdi_pm_test.ko,
		# with the emulated chip power-cycled in between.
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
	suspend-i2c)
		# ... of an I2C channel with clock stretching (AD7 wired)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		;;
	suspend-uart)
		# ... of a UART with a CBUS GPIO pin: ACBUS5 in I/O mode, the
		# high nibble of EEPROM byte 0x1a
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		FTDI_EMU_EXTRA="--ee-set 0x1a=0x80"
		;;
	suspend-ft4232h)
		# ... of four channels in four modes
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	i2c-ad7)
		# Board with AD7 wired to SCL: adaptive clocking path
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		;;
	i2c-map-chan)
		# Maps keyed by USB interface: one entry per channel of a
		# two-channel chip, the delay of B over the device's
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="i2c_bus_nr_map=1-1:1.0:30,1-1:1.1:31 i2c_delay_us_map=1-1:10,1-1:1.1:25"
		;;
	i2c-ad7-gpio)
		# AD7 wired to SCL: the pin is the I2C driver's, or a GPIO
		# line, never both
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_EMU_EXTRA="--wire-ad7"
		;;
	uart-rs485)
		# FT232H with TXDEN on ACBUS4 (cbush4=TXDEN for ftdi_eeprom):
		# the low nibble of EEPROM byte 0x1a
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		FTDI_EMU_EXTRA="--ee-set 0x1a=0x09"
		;;
	latency)
		# mpsse_latency on an MPSSE channel, latency_timer on a UART
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		;;
	uart-rs485-ft4232h)
		# FT4232H: EEPROM byte 0x0b bits 4-7 select TXDEN instead of
		# RI# for channels A-D; here B and C
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="uart"
		FTDI_EMU_EXTRA="--ee-set 0x0b=0x60"
		;;
	i2c-map)
		# Per-devpath maps: the device (1-1) is in the middle of each
		# list, not last
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		I2C_ARGS="i2c_bus_nr_map=9-9:30,1-1:42,8-8:50 i2c_delay_us_map=9-9:5,1-1:20,8-8:7"
		;;
	stress-spi|stress-unbind|stress-hotplug|stress-params|stress-failslab)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
	stress-i2c)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		;;
	stress-uart)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		;;
	stress-uart-errors)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="uart"
		FTDI_ERROR="uart-line"
		FTDI_ERROR_COUNT="0"
		;;
	stress-ft2232h)
		FTDI_CHIP="ft2232h"
		FTDI_PROTO="spi"
		;;
	stress-ft4232h)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	*)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
esac

# /tmp/emu.ctl: runtime control, "echo error TYPE [COUNT] > /tmp/emu.ctl"
EMU_ARGS="--chip $FTDI_CHIP --mode $FTDI_PROTO --ctl /tmp/emu.ctl"
if [ -n "$FTDI_ERROR" ]; then
	EMU_ARGS="$EMU_ARGS --error $FTDI_ERROR --error-count $FTDI_ERROR_COUNT"
	echo "=== Error injection: $FTDI_ERROR (count=$FTDI_ERROR_COUNT) ==="
fi
[ -n "$FTDI_EMU_EXTRA" ] && EMU_ARGS="$EMU_ARGS $FTDI_EMU_EXTRA"
/usr/bin/ftdi_usbip $EMU_ARGS 2>>/tmp/emu.log &
EMU_PID=$!
sleep 1

insmod /lib/modules/usbip-core.ko
insmod /lib/modules/vhci-hcd.ko
sleep 1

/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
ATTACH_PID=$!
sleep 1

# spi_cs=3,4,5: AD3=CS0, AD4=CS1, AD5=CS2
MPSSE_ARGS=""
case "$FTDI_MODE" in
	spi|stress-spi|stress-unbind|stress-hotplug|stress-failslab)
		MPSSE_ARGS="spi_cs=3,4,5"
		;;
	stress-ft2232h)
		MPSSE_ARGS="bus_mode=spi,i2c spi_cs=3,4"
		;;
	stress-ft4232h)
		MPSSE_ARGS="bus_mode=spi,i2c,uart,bitbang spi_cs=3"
		;;
	spi-modes|spi-timing|spi-cs)
		MPSSE_ARGS="spi_cs=3,4,5,6,7"
		;;
	spi-cs-high)
		MPSSE_ARGS="spi_cs=3,4"
		;;
	ft2232h-mixed)
		MPSSE_ARGS="bus_mode=spi,i2c spi_cs=3,4"
		;;
	ft4232h-mixed)
		MPSSE_ARGS="bus_mode=i2c,spi,uart,bitbang spi_cs=3"
		;;
	suspend-ft4232h)
		MPSSE_ARGS="bus_mode=spi,i2c,uart,bitbang spi_cs=3"
		;;
	latency)
		MPSSE_ARGS="bus_mode=spi,uart mpsse_latency=3"
		;;
esac
case "$FTDI_MODE" in
	ft4232h-cd)
		MPSSE_ARGS="bus_mode=spi,spi,uart,bitbang spi_cs=3,4,5 $MPSSE_ARGS"
		;;
	ee-param-wins)
		MPSSE_ARGS="bus_mode=spi $MPSSE_ARGS"
		;;
esac
insmod /lib/modules/ftdi_mpsse.ko $MPSSE_ARGS
insmod /lib/modules/ftdi_uart.ko
insmod /lib/modules/ftdi_spi.ko register_spidev=1
insmod /lib/modules/ftdi_i2c.ko ${I2C_ARGS:-}
insmod /lib/modules/ftdi_gpio.ko
insmod /lib/modules/ftdi_gpio_bitbang.ko 2>/dev/null || true
sleep 1

# The ID table of ftdi_mpsse is empty: new_id makes it probe the device
case "${FTDI_NEW_ID:+custom}$FTDI_CHIP" in
	custom*)
		echo "DEBUG: Adding $FTDI_NEW_ID to ftdi_mpsse driver"
		echo "$FTDI_NEW_ID" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>&1 || echo "ERROR: new_id failed"
		;;
	ft232h)
		echo "DEBUG: Adding FT232H (0403:6014) to ftdi_mpsse driver"
		echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>&1 || echo "ERROR: new_id failed"
		;;
	ft2232h)
		echo "DEBUG: Adding FT2232H (0403:6010) to ftdi_mpsse driver"
		echo "0403 6010" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>&1 || echo "ERROR: new_id failed"
		;;
	ft4232h)
		echo "DEBUG: Adding FT4232H (0403:6011) to ftdi_mpsse driver"
		echo "0403 6011" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>&1 || echo "ERROR: new_id failed"
		;;
esac
sleep 1

echo "DEBUG: USB devices after new_id:"
ls -la /sys/bus/usb/devices/ 2>&1
if [ -d "/sys/bus/usb/devices/1-1:1.0/driver" ]; then
	echo "DEBUG: Device bound to driver:"
	ls -l /sys/bus/usb/devices/1-1:1.0/driver 2>&1
else
	echo "DEBUG: Device not yet bound to a driver"
fi

PASS=0
FAIL=0
TOTAL=0

check() {
	desc="$1"
	shift
	TOTAL=$((TOTAL + 1))
	if eval "$@"; then
		echo "PASS: $desc"
		PASS=$((PASS + 1))
	else
		echo "FAIL: $desc"
		FAIL=$((FAIL + 1))
	fi
}

# Wait until PID exits or SECS seconds of uptime elapsed
wait_gone() {
	_end=$(awk -v s="$2" '{ print $1 + s }' /proc/uptime)
	while kill -0 "$1" 2>/dev/null &&
	      awk -v e="$_end" '{ exit !($1 < e) }' /proc/uptime; do
		usleep 20000
	done
}

# Wait up to $1 seconds for the shell condition $2 to hold
wait_for() {
	_end=$(awk -v s="$1" '{ print $1 + s }' /proc/uptime)
	until eval "$2"; do
		awk -v e="$_end" '{ exit !($1 < e) }' /proc/uptime || return 1
		usleep 20000
	done
}

# A check that cannot run in this environment: reported, never a PASS
skip() {
	echo "SKIP: $*"
}

# Emulator statistics line for a channel ("emu: stats chA ...")
emu_stats() {
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
	grep "emu: stats ch$1 " /tmp/emu.log | tail -1
}

# "emu: uart" statistics of channel $1
emu_uart() {
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
	grep "emu: uart ch$1 " /tmp/emu.log | tail -1
}

# $1 within 0.1 % of $2: the emulated engine counts whole ns (833 ns for
# a third of a 400 kHz bit, one phase of 3-phase clocking); from 83 kHz up
# the next divisor is 0.4 % away or more
near() {
	[ -n "$1" ] && [ $((($1 - $2) * 1000 / $2)) -eq 0 ]
}

# Emulator I2C statistics line for a channel ("emu: i2c chA ..."), whose
# fields frame_field reads: scl_hz_max/scl_hz_min (SCL while clocking
# data, from the engine clock), recovery_clocks (SCL pulsed by pin writes
# while SDA is held low), ...
emu_i2c_line() {
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
	grep "emu: i2c ch$1 " /tmp/emu.log | tail -1
}
# Same for the SPI bus model: sck_hz_max/sck_hz_min
emu_spi_line() {
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
	grep "emu: spi ch$1 " /tmp/emu.log | tail -1
}

# Run stress_test for STRESS_SECS with the given workers
run_stress() {
	desc="$1"
	shift
	/usr/bin/stress_test -t "$STRESS_SECS" "$@" > /tmp/stress.log 2>&1
	cat /tmp/stress.log
	check "$desc" 'grep -q "STRESS RESULT: PASS" /tmp/stress.log'
}

# The ftdi-gpio-bitbang chip, if any
bitbang_chip() {
	for g in /sys/bus/gpio/devices/gpiochip*; do
		readlink -f "$g" | grep -q "ftdi-gpio-bitbang" && echo /dev/$(basename "$g")
	done
}

# Last "SPI frame" line of the emulator (--spi-echo), and one field of
# that or any other "name=value" line
spi_frame() {
	grep "SPI frame" /tmp/emu.log | tail -1
}
frame_field() {
	echo "$1" | sed -n "s/.* $2=\\([-0-9]*\\).*/\\1/p"
}

# The same for a field that is a word
frame_word() {
	echo "$1" | sed -n "s/.* $2=\\([^ ]*\\).*/\\1/p"
}

# N-th "SPI frame" line of the chip select on AD4 (1 = first)
spi_frame_n() {
	grep "SPI frame cs=AD4" /tmp/emu.log | sed -n "$1p"
}

# tty of the UART on interface $1 (0 = A), e.g. /dev/ttyFTDI2
chan_tty() {
	_t=$(find /sys/bus/usb/devices/1-1:1.$1/ -name 'ttyFTDI*' 2>/dev/null | head -1)
	[ -n "$_t" ] && echo "/dev/$(basename "$_t")"
}

# Device nodes of the bus, and of the GPIO chip, on interface $1 (0 = A)
chan_spidev() {
	_s=$(ls -d /sys/bus/usb/devices/1-1:1.$1/ftdi-spi.*/spi_master/spi* 2>/dev/null | head -1)
	[ -n "$_s" ] && echo "/dev/spidev${_s##*/spi}.0"
}
chan_i2c() {
	_a=$(ls -d /sys/bus/usb/devices/1-1:1.$1/ftdi-i2c.*/i2c-* 2>/dev/null | head -1)
	[ -n "$_a" ] && echo "/dev/${_a##*/}"
}
chan_gpiochip() {
	_g=$(ls -d /sys/bus/usb/devices/1-1:1.$1/ftdi-gpio*/gpiochip* 2>/dev/null | head -1)
	[ -n "$_g" ] && echo "/dev/${_g##*/}"
}

# Data path of the channel on interface $1, whatever its mode: one check.
# $2 is appended to its description.
chan_io_test() {
	_c=$(echo ABCD | cut -c$(($1 + 1)))
	case "$(chan_mode $1)" in
	spi)
		/usr/bin/spi_test "$(chan_spidev $1)" > /tmp/chan_io.log 2>&1
		check "channel $_c: SPI read/write$2" 'grep -q "All SPI tests PASSED" /tmp/chan_io.log'
		;;
	i2c)
		/usr/bin/i2c_test "$(chan_i2c $1)" > /tmp/chan_io.log 2>&1
		check "channel $_c: I2C read/write$2" 'grep -q "All I2C tests PASSED" /tmp/chan_io.log'
		;;
	uart)
		/usr/bin/uart_test "$(chan_tty $1)" > /tmp/chan_io.log 2>&1
		check "channel $_c: UART loopback$2" 'grep -q "All UART tests PASSED" /tmp/chan_io.log'
		;;
	bitbang)
		/usr/bin/gpio_test "$(chan_gpiochip $1)" 0 > /tmp/chan_io.log 2>&1
		check "channel $_c: bit-bang GPIO$2" 'grep -q "All GPIO tests PASSED" /tmp/chan_io.log'
		;;
	*)
		echo "no known mode on interface $1" > /tmp/chan_io.log
		check "channel $_c: mode known$2" 'false'
		;;
	esac
	grep -q "PASSED" /tmp/chan_io.log || cat /tmp/chan_io.log
}

# What the emulated chip holds for channel $1 (A-D) and loses when it is
# power-cycled: bit mode, output latches, directions, open-drain masks,
# CBUS pins, baud rate, latency timer
emu_state() {
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
	grep "emu: state ch$1 " /tmp/emu.log | tail -1 | sed "s/^emu: state ch$1 //"
}
EMU_STATE_OFF="bitmode=0x00 lo=00/00/00 hi=00/00/00 cbus=0/0 baud=9600 latency=16"

# Suspend or resume the drivers of the FTDI device. The callbacks are run
# by ftdi_pm_test.ko: see tests/kmod/ftdi_pm_test.c for why and how.
pm_cmd() {
	echo "$1 1-1" > /sys/module/ftdi_pm_test/parameters/cmd
}

# The chip loses everything, as when its supply goes away during a sleep
emu_power_cycle() {
	echo "power-cycle" > /tmp/emu.ctl
	usleep 200000
}

# Load the PM test module and suspend. Not possible without
# CONFIG_PM_SLEEP (UML SMP): the drivers then have no such callbacks.
pm_suspend() {
	insmod /lib/modules/ftdi_pm_test.ko
	if pm_cmd suspend 2>/tmp/pm_cmd.err; then
		return 0
	fi
	cat /tmp/pm_cmd.err
	if grep -qi "not supported" /tmp/pm_cmd.err; then
		skip "suspend/resume: this kernel has no CONFIG_PM_SLEEP"
	else
		check "suspend callbacks ran" 'false'
	fi
	return 1
}

# debugfs file $2 (pinmap, eeprom_cbus, eeprom_drive, eeprom_config) of
# interface $1 (0 = A)
dbgfs() {
	echo /sys/kernel/debug/ftdi_mpsse/1-1:1.$1/$2
}

# i2c_stats (debugfs) of the first I2C adapter
i2c_stats() {
	ls /sys/kernel/debug/i2c/i2c-*/i2c_stats 2>/dev/null | head -1
}

# Mode a channel runs in, from the pinmap of interface $1 (0 = A)
chan_mode() {
	sed -n 's/^mode: \([a-z0-9]*\).*/\1/p' "$(dbgfs $1 pinmap)" 2>/dev/null
}

if [ -n "$EXPECT_PROBE_FAILURE" ]; then
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
elif [ -n "$EXPECT_NO_EEPROM" ]; then
	check "FTDI MPSSE core probed"  'dmesg | grep -q "FTDI MPSSE core:"'
	check "GPIO driver registered"  'dmesg | grep -q "FTDI MPSSE GPIO:"'
	check "gpiochip device exists"  'ls /dev/gpiochip* >/dev/null 2>&1'
elif [ "$FTDI_MODE" != "mpsse-sync-fail" ] && [ "$FTDI_PROTO" = "uart" ]; then
	check "FTDI MPSSE core probed"  'dmesg | grep -q "FTDI MPSSE core:"'
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
	check "No MPSSE GPIO in UART mode" '! dmesg | grep -q "FTDI MPSSE GPIO:"'
	STATS_A=$(emu_stats A)
	echo "DEBUG: $STATS_A"
	check "Nothing sent on TXD at probe" 'echo "$STATS_A" | grep -q "uart_tx=0 "'
elif [ "$FTDI_MODE" != "mpsse-sync-fail" ]; then
	check "FTDI MPSSE core probed"  'dmesg | grep -q "FTDI MPSSE core:"'
	check "GPIO driver registered"  'dmesg | grep -q "FTDI MPSSE GPIO:"'
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
	check "gpiochip device exists"  'ls /dev/gpiochip* >/dev/null 2>&1'
else
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
fi

case "$FTDI_MODE" in
	spi)
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		# 0x003f: AD0-AD5 = SCK, MOSI, MISO, CS0, CS1, CS2
		check "SPI 3 CS pins reserved"    'dmesg | grep -q "reserved mask 0x003f"'
		check "spidev devices created"    'dmesg | grep -q "registered.*spidev"'
		echo "DEBUG: /sys/bus/spi/devices:"
		ls -la /sys/bus/spi/devices/ 2>&1 || echo "  (none)"
		echo "DEBUG: /sys/bus/spi/drivers:"
		ls -la /sys/bus/spi/drivers/ 2>&1 || echo "  (none)"
		echo "DEBUG: /dev/spi*:"
		ls -la /dev/spi* 2>&1 || echo "  (none)"
		check "spidev device exists"      'ls /dev/spidev* >/dev/null 2>&1'
		echo "Running SPI I/O test..."
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		if [ -n "$SPI_DEV" ]; then
			/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_test.log 2>&1
			check "SPI read/write test"   'grep -q "All SPI tests PASSED" /tmp/spi_test.log'
			check "SPI mode 1 test"       'grep -q "mode 1 xfer OK" /tmp/spi_test.log'
			check "SPI mode 1 AN_114 notice" 'dmesg | grep -q "CPHA=1.*AN_114"'
			cat /tmp/spi_test.log
			SPI_LINE=$(emu_spi_line A)
			SCK_HZ=$(frame_field "$SPI_LINE" sck_hz_max)
			SCK_HZ_MIN=$(frame_field "$SPI_LINE" sck_hz_min)
			check "every bit at 1 MHz (measured $SCK_HZ_MIN-$SCK_HZ Hz)" \
				'near "$SCK_HZ" 1000000 && near "$SCK_HZ_MIN" 1000000'
			/usr/bin/spi_test "${SPI_DEV%.0}.1" empty > /tmp/spi_empty.log 2>&1
			cat /tmp/spi_empty.log
			check "CS1 selects nothing: only CS0 reaches the device" \
				'grep -q "PASS: nothing answers" /tmp/spi_empty.log'
		else
			check "SPI read/write test"   'false'
		fi
		# Line 8 = AC0, not reserved by SPI
		echo "Running GPIO I/O test..."
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO read/write test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
			cat /tmp/gpio_test.log
			# Line 15 = ACBUS7 (pull-down, DS_FT232H 3.4)
			/usr/bin/gpio_test "$GPIO_DEV" 15 get 0 > /tmp/gpio_get.log 2>&1
			cat /tmp/gpio_get.log
			check "ACBUS7 reads its pull-down" 'grep -q "PASS: line 15 reads 0" /tmp/gpio_get.log'
			# Line 9 = ACBUS1
			/usr/bin/gpio_test "$GPIO_DEV" 9 edge 4 > /tmp/gpio_edge.log 2>&1 &
			EDGE_PID=$!
			wait_for 5 'grep -q "HOLD:" /tmp/gpio_edge.log'
			usleep 200000
			echo "pin A C1 0" > /tmp/emu.ctl
			usleep 200000
			echo "pin A C1 z" > /tmp/emu.ctl
			wait $EDGE_PID
			cat /tmp/gpio_edge.log
			check "GPIO edge events: one falling, one rising" \
				'[ "$(grep -c "^EDGE falling" /tmp/gpio_edge.log)" -eq 1 ] && [ "$(grep -c "^EDGE rising" /tmp/gpio_edge.log)" -eq 1 ] && [ "$(grep "^EDGE" /tmp/gpio_edge.log | head -1)" = "EDGE falling" ]'
		else
			check "GPIO read/write test"  'false'
		fi
		check "No I2C adapter (SPI mode)" '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		;;
	i2c)
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		# 0x0007: AD0-AD2 = SCL, SDA out, SDA in
		check "I2C pins reserved"         'dmesg | grep -q "reserved mask 0x0007"'
		check "i2c-dev device exists"     'ls /dev/i2c-* >/dev/null 2>&1'
		I2C_STATS=$(i2c_stats)
		if [ -n "$I2C_STATS" ]; then
			echo 0 > "$I2C_STATS"
			check "i2c_stats in debugfs"  'true'
		else
			check "i2c_stats in debugfs"  'false'
		fi
		echo "Running I2C I/O test..."
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
			check "I2C read/write test"   'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
			cat /tmp/i2c_test.log
		else
			check "I2C read/write test"   'false'
		fi
		# Default 100 kHz, standard mode: 83.3 kHz (i2c-100k)
		SCL_HZ=$(frame_field "$(emu_i2c_line A)" scl_hz_max)
		check "SCL measured at 83.3 kHz ($SCL_HZ Hz)" 'near "$SCL_HZ" 83333'
		if [ -n "$I2C_STATS" ]; then
			echo "Checking the i2c_stats counters..."
			cat "$I2C_STATS"
			check "xfer_ok > 0"     'grep -q "xfer_ok=[1-9]" "$I2C_STATS"'
			# i2c_test addresses a device that does not exist, once
			check "xfer_fail == 1 (no device at 0x30)" 'grep -q "xfer_fail=1$" "$I2C_STATS"'
			check "nack == 1 (no device at 0x30)"      'grep -q "^nack=1$" "$I2C_STATS"'
			check "bytes_tx > 0"    'grep -q "bytes_tx=[1-9]" "$I2C_STATS"'
			check "bytes_rx > 0"    'grep -q "bytes_rx=[1-9]" "$I2C_STATS"'
		fi
		# Line 8 = AC0, not reserved by I2C
		echo "Running GPIO I/O test..."
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO read/write test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
			cat /tmp/gpio_test.log
		else
			check "GPIO read/write test"  'false'
		fi
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 7 > /tmp/gpio_ad7.log 2>&1
			check "AD7 is a GPIO line without clock stretching" \
				'grep -q "All GPIO tests PASSED" /tmp/gpio_ad7.log'
			check "pinmap: AD7 available" \
				'grep -q "^  AD7  available" $(dbgfs 0 pinmap)'
		fi
		check "No SPI controller (I2C mode)" '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		if [ -n "$I2C_DEV" ]; then
			# COUNT 1, SKIP 1
			echo "error i2c-nak 1 1" > /tmp/emu.ctl
			usleep 100000
			/usr/bin/error_test i2c-nak-sr "$I2C_DEV" > /tmp/nak_sr.log 2>&1
			cat /tmp/nak_sr.log
			check "address NAK at the repeated START: ENXIO" \
				'grep -q "^PASS:" /tmp/nak_sr.log && grep -q "I2C NAK injected on address 0x50 (read)" /tmp/emu.log'
			/usr/bin/error_test i2c-nak-sr "$I2C_DEV" > /tmp/nak_sr2.log 2>&1
			check "the same transfer works without the fault" \
				'grep -q "write+read succeeded" /tmp/nak_sr2.log'
		fi
		;;
	i2c-100k|i2c-400k)
		SPEED_KHZ=$(echo "$FTDI_MODE" | sed 's/i2c-\([0-9]*\)k/\1/')
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "driver reports $SPEED_KHZ kHz" "dmesg | grep -q 'I2C adapter at ${SPEED_KHZ} kHz'"
		check "i2c-dev device exists"     'ls /dev/i2c-* >/dev/null 2>&1'
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
			check "I2C read/write at $SPEED_KHZ kHz" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
			cat /tmp/i2c_test.log
		else
			check "I2C read/write at $SPEED_KHZ kHz" 'false'
		fi
		# What the data bits ran at, from the emulated engine's clock:
		# 3-phase clocking, SCL = 60 MHz / ((1 + divisor) * 3).
		# Standard mode: SCL high for 4.0 us needs divisor 239, 83.3 kHz;
		# fast mode runs at the speed asked for (divisor 49).
		I2C_LINE=$(emu_i2c_line A)
		SCL_HZ=$(frame_field "$I2C_LINE" scl_hz_max)
		SCL_HZ_MIN=$(frame_field "$I2C_LINE" scl_hz_min)
		if [ "$SPEED_KHZ" = 100 ]; then
			WANT_HZ=83333
			check "driver reports SCL at 83 kHz" 'dmesg | grep -q "SCL runs at 83 kHz"'
		else
			WANT_HZ=$((SPEED_KHZ * 1000))
			check "driver does not slow SCL down" '! dmesg | grep -q "SCL runs at"'
		fi
		check "every data bit at $WANT_HZ Hz (measured $SCL_HZ_MIN-$SCL_HZ Hz)" \
			'near "$SCL_HZ" $WANT_HZ && near "$SCL_HZ_MIN" $WANT_HZ'
		;;
	uart)
		check "UART mode detected"        'dmesg | grep -q "EEPROM hints UART mode"'
		check "UART driver registered"    'dmesg | grep -q "FTDI MPSSE UART:"'
		check "ttyFTDI device exists"     'ls /dev/ttyFTDI* >/dev/null 2>&1'
		echo "DEBUG: /dev/ttyFTDI*:"
		ls -la /dev/ttyFTDI* 2>&1 || echo "  (none)"
		echo "Running UART I/O test..."
		UART_DEV=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		if [ -n "$UART_DEV" ]; then
			/usr/bin/uart_test "$UART_DEV" > /tmp/uart_test.log 2>&1
			check "UART read/write test"  'grep -q "All UART tests PASSED" /tmp/uart_test.log'
			cat /tmp/uart_test.log
		else
			check "UART read/write test"  'false'
		fi
		/usr/bin/rs485_test "$UART_DEV" off > /tmp/rs485.log 2>&1
		cat /tmp/rs485.log
		check "RS-485 refused without a TXDEN pin" 'grep -q "PASS:" /tmp/rs485.log'
		check "no TXDEN pin announced"        '! dmesg | grep -q "RS-485: TXDEN on"'
		U=$(emu_uart A); echo "$U"
		check "emu: no TXDEN pin in this EEPROM" '[ "$(frame_word "$U" txden)" = none ]'
		check "emu: one break received"       '[ "$(frame_field "$U" breaks)" = 1 ]'
		check "No SPI controller (UART mode)" '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		check "No I2C adapter (UART mode)"    '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		;;
	uart-rs485)
		UART_DEV=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		check "TXDEN found on ACBUS4"     'dmesg | grep -q "RS-485: TXDEN on ACBUS4"'
		cat $(dbgfs 0 pinmap)
		check "pinmap: TXDEN on ACBUS4"   'grep -q "^  ACBUS4  TXDEN" $(dbgfs 0 pinmap)'
		/usr/bin/rs485_test "$UART_DEV" on > /tmp/rs485.log 2>&1
		cat /tmp/rs485.log
		check "RS-485 accepted"           'grep -q "PASS:" /tmp/rs485.log'
		/usr/bin/uart_test "$UART_DEV" > /tmp/uart_test.log 2>&1
		cat /tmp/uart_test.log
		check "UART read/write test"      'grep -q "All UART tests PASSED" /tmp/uart_test.log'
		U=$(emu_uart A); echo "$U"
		check "emu: the chip drives TXDEN on ACBUS4" \
			'[ "$(frame_word "$U" txden)" = ACBUS4 ]'
		check "emu: TXDEN asserted while transmitting" \
			'[ "$(frame_field "$U" txden_pulses)" -gt 0 ]'
		;;
	uart-rs485-ft4232h)
		for N in 0 1 2 3; do
			T=$(chan_tty $N)
			C=$(echo ABCD | cut -c$((N + 1)))
			case $N in
			1|2)	WANT=on ;;
			*)	WANT=off ;;
			esac
			/usr/bin/rs485_test "$T" $WANT > /tmp/rs485.log 2>&1
			cat /tmp/rs485.log
			check "channel $C ($T): RS-485 $WANT" 'grep -q "PASS:" /tmp/rs485.log'
		done
		check "TXDEN on BDBUS7 and CDBUS7 only" \
			'[ "$(dmesg | grep "RS-485: TXDEN on" | sed "s/.*TXDEN on //" | sort | tr "\n" " ")" = "BDBUS7 CDBUS7 " ]'
		check "pinmap A: pin 7 is RI"     'grep -q "7  RI$" $(dbgfs 0 pinmap)'
		check "pinmap B: pin 7 is TXDEN"  'grep -q "7  TXDEN$" $(dbgfs 1 pinmap)'
		check "pinmap C: pin 7 is TXDEN"  'grep -q "7  TXDEN$" $(dbgfs 2 pinmap)'
		kill -USR2 $EMU_PID 2>/dev/null
		sleep 1
		check "emu: the chip has TXDEN on BDBUS7 and CDBUS7 only" \
			'[ "$(grep "emu: uart ch" /tmp/emu.log | tail -4 | sed "s/.*txden=\([^ ]*\).*/\1/" | tr "\n" " ")" = "none BDBUS7 CDBUS7 none " ]'
		T=$(chan_tty 2)
		/usr/bin/uart_test "$T" > /tmp/uart_test.log 2>&1
		cat /tmp/uart_test.log
		check "UART read/write on channel C" 'grep -q "All UART tests PASSED" /tmp/uart_test.log'
		;;
	latency)
		# The latency timer of each channel, as the chip holds it
		check "channel A is SPI"         '[ "$(chan_mode 0)" = spi ]'
		check "channel B is a UART"      '[ "$(chan_mode 1)" = uart ]'
		check "mpsse_latency=3 on the MPSSE channel" \
			'[ "$(frame_field "$(emu_state A)" latency)" = 3 ]'
		check "UART channel at the default 16 ms" \
			'[ "$(frame_field "$(emu_state B)" latency)" = 16 ]'
		LAT=$(ls /sys/bus/usb/devices/1-1:1.1/ftdi-uart.*/latency_timer 2>/dev/null | head -1)
		echo 2 > "$LAT"
		check "latency_timer=2 reaches the UART channel" \
			'[ "$(frame_field "$(emu_state B)" latency)" = 2 ]'
		echo 0 > "$LAT" 2>/dev/null
		echo 256 > "$LAT" 2>/dev/null
		check "latency_timer 0 and 256 refused" \
			'[ "$(cat "$LAT")" = 2 ] && [ "$(frame_field "$(emu_state B)" latency)" = 2 ]'
		# The parameter is taken, clamped to 1-255, when a channel
		# enters MPSSE: here at the next bind
		for v in 0:1 1000:255 7:7; do
			echo "${v%:*}" > /sys/module/ftdi_mpsse/parameters/mpsse_latency
			echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/unbind
			echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/bind
			wait_for 5 '[ "$(chan_mode 0)" = spi ]'
			check "mpsse_latency=${v%:*}: ${v#*:} ms on the chip" \
				'[ "$(frame_field "$(emu_state A)" latency)" = "${v#*:}" ]'
		done
		chan_io_test 0 " after the rebinds"
		;;
	ft2232h-mixed)
		check "channel A is SPI"         '[ "$(chan_mode 0)" = spi ]'
		check "channel B is I2C"         '[ "$(chan_mode 1)" = i2c ]'
		check "pinmap B: pins named after channel B" \
			'grep -q "^  BD0  SCL" $(dbgfs 1 pinmap)'
		for N in 0 1; do
			chan_io_test $N
		done
		/usr/bin/i2c_test "$(chan_i2c 1)" > /tmp/i2c_bg.log 2>&1 &
		BG_PID=$!
		chan_io_test 0 " while I2C runs on channel B"
		wait $BG_PID
		check "channel B: I2C read/write while SPI runs on channel A" \
			'grep -q "All I2C tests PASSED" /tmp/i2c_bg.log'
		for N in 0 1; do
			/usr/bin/gpio_test "$(chan_gpiochip $N)" 8 > /tmp/gpio_t.log 2>&1
			check "channel $(echo AB | cut -c$((N + 1))): GPIO on xCBUS0" \
				'grep -q "All GPIO tests PASSED" /tmp/gpio_t.log'
		done
		;;
	ft4232h-mixed)
		check "channel A is I2C"         '[ "$(chan_mode 0)" = i2c ]'
		check "channel B is SPI"         '[ "$(chan_mode 1)" = spi ]'
		check "channel C is a UART"      '[ "$(chan_mode 2)" = uart ]'
		check "channel D is bit-bang"    '[ "$(chan_mode 3)" = bitbang ]'
		for N in 0 1 2 3; do
			chan_io_test $N
		done
		# The MPSSE GPIO lines next to each bus (no high byte here)
		/usr/bin/gpio_test "$(chan_gpiochip 0)" 5 > /tmp/gpio_t.log 2>&1
		check "channel A: GPIO on ADBUS5 next to I2C" 'grep -q "All GPIO tests PASSED" /tmp/gpio_t.log'
		/usr/bin/gpio_test "$(chan_gpiochip 1)" 6 > /tmp/gpio_t.log 2>&1
		check "channel B: GPIO on BDBUS6 next to SPI" 'grep -q "All GPIO tests PASSED" /tmp/gpio_t.log'
		;;
	ft2232h*)
		check "Channel A probed"         'dmesg | grep -q "FTDI MPSSE core: channel A"'
		check "Channel B probed"         'dmesg | grep -q "FTDI MPSSE core: channel B"'
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "Both channels probed"     '[ "$INTF_COUNT" -eq 2 ]'

		if [ "$FTDI_PROTO" = "spi" ]; then
			check "SPI mode detected"  'dmesg | grep -q "EEPROM hints SPI mode"'
			SPI_COUNT=$(ls -d /sys/class/spi_master/spi* 2>/dev/null | wc -l)
			check "Two SPI controllers"  '[ "$SPI_COUNT" -ge 2 ]'
		fi

		if [ "$FTDI_PROTO" = "uart" ]; then
			TTY_COUNT=$(ls /dev/ttyFTDI* 2>/dev/null | wc -l)
			check "Two ttyFTDI ports"   '[ "$TTY_COUNT" -eq 2 ]'
			for t in $(ls /dev/ttyFTDI* 2>/dev/null); do
				/usr/bin/uart_test "$t" > /tmp/uart_t.log 2>&1
				UART_RC=$?
				cat /tmp/uart_t.log
				check "UART loopback on $t" '[ "$UART_RC" -eq 0 ]'
			done
			# FT2232H: TXDEN is on xCBUS0 of a UART channel,
			# with no EEPROM setting (DS_FT2232H table 3.5)
			check "TXDEN on ACBUS0"     'dmesg | grep -q "RS-485: TXDEN on ACBUS0"'
			check "TXDEN on BCBUS0"     'dmesg | grep -q "RS-485: TXDEN on BCBUS0"'
			for N in 0 1; do
				T=$(chan_tty $N)
				/usr/bin/rs485_test "$T" on > /tmp/rs485.log 2>&1
				cat /tmp/rs485.log
				check "RS-485 accepted on $T" 'grep -q "PASS:" /tmp/rs485.log'
			done
			for C in A B; do
				U=$(emu_uart $C); echo "$U"
				check "emu: the chip drives TXDEN on ${C}CBUS0" \
					'[ "$(frame_word "$U" txden)" = ${C}CBUS0 ]'
			done
		else
			GPIO_COUNT=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
			check "Two GPIO chips exist"     '[ "$GPIO_COUNT" -ge 2 ]'
			for g in $(ls /dev/gpiochip* 2>/dev/null); do
				/usr/bin/gpio_test "$g" 8 > /tmp/gpio_t.log 2>&1
				GPIO_RC=$?
				cat /tmp/gpio_t.log
				check "GPIO test on $g (AC0)" '[ "$GPIO_RC" -eq 0 ]'
			done
			for N in 0 1; do
				chan_io_test $N
			done
		fi
		if [ "$FTDI_MODE" = ft2232h-i2c ]; then
			# COUNT 1, SKIP 0, channel B
			echo "error i2c-nak 1 0 B" > /tmp/emu.ctl
			usleep 100000
			/usr/bin/i2c_test "$(chan_i2c 0)" > /tmp/i2c_a.log 2>&1
			check "fault for channel B: channel A works" \
				'grep -q "All I2C tests PASSED" /tmp/i2c_a.log'
			/usr/bin/error_test i2c-nak "$(chan_i2c 1)" > /tmp/nak_b.log 2>&1
			cat /tmp/nak_b.log
			check "fault for channel B: its address is NAKed" \
				'grep -q "^PASS:" /tmp/nak_b.log && grep -q "chB: I2C NAK injected" /tmp/emu.log && ! grep -q "chA: I2C NAK injected" /tmp/emu.log'
		fi
		;;
	ft4232h-cd)
		# C/D have no MPSSE: UART or bit-bang (DS_FT4232H)
		for c in A B C D; do
			check "Channel $c probed" "dmesg | grep -q 'FTDI MPSSE core: channel $c'"
		done
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "All 4 channels probed"   '[ "$INTF_COUNT" -eq 4 ]'
		check "Channel C is UART"        "dmesg | grep 'channel C' | grep -q \"mode 'uart'\""
		check "Channel D is bit-bang"    "dmesg | grep 'channel D' | grep -q \"mode 'bitbang'\""
		TTY_COUNT=$(ls /dev/ttyFTDI* 2>/dev/null | wc -l)
		check "One ttyFTDI (channel C)"  '[ "$TTY_COUNT" -eq 1 ]'
		check "Bit-bang GPIO registered" 'dmesg | grep -q "FTDI bit-bang GPIO"'
		cat $(dbgfs 2 pinmap) $(dbgfs 3 pinmap)
		check "pinmap C: UART, no GPIO" \
			'grep -q "^gpio: - (UART mode)" $(dbgfs 2 pinmap)'
		check "pinmap D: 8 bit-bang lines" \
			'[ "$(grep -c "^  DDBUS[0-7]  available" $(dbgfs 3 pinmap))" -eq 8 ]'

		TTYFTDI=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		if [ -n "$TTYFTDI" ]; then
			/usr/bin/uart_test "$TTYFTDI" > /tmp/uart_cd.log 2>&1
			UART_RC=$?
			cat /tmp/uart_cd.log
			check "UART I/O on channel C" '[ "$UART_RC" -eq 0 ]'
		else
			check "ttyFTDI found for C" 'false'
		fi

		for N in 0 1; do
			chan_io_test $N
		done

		BB_CHIP=$(bitbang_chip)
		if [ -n "$BB_CHIP" ]; then
			for l in 0 5; do
				/usr/bin/gpio_test "$BB_CHIP" $l > /tmp/gpio_bb.log 2>&1
				GPIO_RC=$?
				cat /tmp/gpio_bb.log
				check "Bit-bang GPIO line $l" '[ "$GPIO_RC" -eq 0 ]'
			done
		else
			check "Bit-bang gpiochip found" 'false'
		fi
		;;
	ft4232h*)
		# FT4232H: A+B MPSSE (low byte only: no ACBUS/BCBUS), C+D UART
		for c in A B C D; do
			check "Channel $c probed" "dmesg | grep -q 'FTDI MPSSE core: channel $c'"
		done
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "All 4 channels probed"   '[ "$INTF_COUNT" -eq 4 ]'
		USB_INTF_COUNT=$(ls -d /sys/bus/usb/devices/1-1:1.[0-3] 2>/dev/null | wc -l)
		check "Four USB interfaces exist" '[ "$USB_INTF_COUNT" -eq 4 ]'

		if [ "$FTDI_PROTO" != "uart" ]; then
			GPIO_COUNT=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
			check "MPSSE GPIO chips for A+B" '[ "$GPIO_COUNT" -eq 2 ]'
			check "FT4232H MPSSE GPIO has 8 lines" 'dmesg | grep "FTDI MPSSE GPIO:" | grep -q " 8 pins"'
			GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
			/usr/bin/gpio_test "$GPIO_DEV" 7 > /tmp/gpio_a.log 2>&1
			GPIO_RC=$?
			cat /tmp/gpio_a.log
			check "Channel A GPIO (AD7)" '[ "$GPIO_RC" -eq 0 ]'
			for N in 0 1; do
				chan_io_test $N
			done
		fi
		for t in $(ls /dev/ttyFTDI* 2>/dev/null); do
			/usr/bin/uart_test "$t" > /tmp/uart_t.log 2>&1
			UART_RC=$?
			cat /tmp/uart_t.log
			check "UART loopback on $t" '[ "$UART_RC" -eq 0 ]'
		done
		;;
	hotplug)
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'

		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)

		if [ -z "$SPI_DEV" ] || [ -z "$GPIO_DEV" ]; then
			echo "ERROR: Required devices not found"
			check "Devices available" 'false'
		else
			echo "=== Hot-unplug Test: SPI ==="
			echo "Starting continuous SPI operations..."

			/usr/bin/hotplug_test spi "$SPI_DEV" > /tmp/hotplug_spi.log 2>&1 &
			HOTPLUG_PID=$!

			sleep 1

			if kill -0 $HOTPLUG_PID 2>/dev/null; then
				echo "SPI operations running (PID $HOTPLUG_PID)"

				echo "Simulating USB disconnect (killing emulator PID $EMU_PID)..."
				echo "init: unplug (kill emulator $EMU_PID)" > /dev/kmsg
				kill $EMU_PID 2>/dev/null

				# A plain "sleep 2" returns early: the SIGCHLD
				# of the killed emulator interrupts it in PID 1
				wait_gone $HOTPLUG_PID 2

				if ! kill -0 $HOTPLUG_PID 2>/dev/null; then
					wait $HOTPLUG_PID
					HOTPLUG_RC=$?
					echo "Hotplug test exited with code $HOTPLUG_RC"
					cat /tmp/hotplug_spi.log
					check "SPI hotplug handled" '[ "$HOTPLUG_RC" -eq 0 ]'
				else
					echo "ERROR: SPI I/O still running 2 s after unplug"
					kill $HOTPLUG_PID 2>/dev/null
					wait $HOTPLUG_PID 2>/dev/null
					cat /tmp/hotplug_spi.log
					check "SPI hotplug handled" 'false'
				fi
			else
				echo "ERROR: Hotplug test failed to start"
				cat /tmp/hotplug_spi.log
				check "SPI hotplug handled" 'false'
			fi

			echo ""
			echo "=== Hot-unplug Test: GPIO ==="

			/usr/bin/ftdi_usbip --chip "$FTDI_CHIP" --mode "$FTDI_PROTO" 2>>/tmp/emu.log &
			EMU_PID=$!
			sleep 1

			/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
			sleep 2

			echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>&1 || true
			sleep 1

			GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
			if [ -n "$GPIO_DEV" ]; then
				echo "Starting continuous GPIO operations..."
				/usr/bin/hotplug_test gpio "$GPIO_DEV" > /tmp/hotplug_gpio.log 2>&1 &
				HOTPLUG_PID=$!
				sleep 1

				if kill -0 $HOTPLUG_PID 2>/dev/null; then
					echo "GPIO operations running (PID $HOTPLUG_PID)"
					echo "Simulating USB disconnect..."
					echo "init: unplug (kill emulator $EMU_PID)" > /dev/kmsg
					kill $EMU_PID 2>/dev/null
					wait_gone $HOTPLUG_PID 2

					if ! kill -0 $HOTPLUG_PID 2>/dev/null; then
						wait $HOTPLUG_PID
						HOTPLUG_RC=$?
						echo "Hotplug test exited with code $HOTPLUG_RC"
						cat /tmp/hotplug_gpio.log
						check "GPIO hotplug handled" '[ "$HOTPLUG_RC" -eq 0 ]'
					else
						echo "ERROR: GPIO I/O still running 2 s after unplug (uptime $(cut -d" " -f1 /proc/uptime))"
						kill $HOTPLUG_PID 2>/dev/null
						wait $HOTPLUG_PID 2>/dev/null; echo "DEBUG: GPIO hotplug_test gone at uptime $(cut -d" " -f1 /proc/uptime)"
						cat /tmp/hotplug_gpio.log
						check "GPIO hotplug handled" 'false'
					fi
				else
					cat /tmp/hotplug_gpio.log
					check "GPIO hotplug handled" 'false'
				fi
			else
				check "GPIO hotplug handled" 'false'
			fi
		fi
		;;
	i2c-nak)
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'

		echo "=== I2C NAK Error Test ==="
		echo "Emulator will inject NAK on I2C transaction"

		I2C_STATS=$(i2c_stats)
		[ -n "$I2C_STATS" ] && echo 0 > "$I2C_STATS"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/error_test i2c-nak "$I2C_DEV" > /tmp/error_test.log 2>&1
			ERR_RC=$?
			cat /tmp/error_test.log
			check "address NAK: the write fails with ENXIO" '[ "$ERR_RC" -eq 0 ]'
			if [ -n "$I2C_STATS" ]; then
				echo "i2c_stats:"
				cat "$I2C_STATS"
				check "i2c_stats: nack > 0"      'grep -q "^nack=[1-9]" "$I2C_STATS"'
				check "i2c_stats: xfer_fail > 0" 'grep -q "xfer_fail=[1-9]" "$I2C_STATS"'
			else
				check "i2c_stats in debugfs" 'false'
			fi
		else
			echo "ERROR: No I2C device found"
			check "I2C NAK handled correctly" 'false'
		fi
		;;
	i2c-stuck)
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'

		echo "=== I2C Bus Stuck (Frozen Bus) Test ==="
		echo "Emulator will simulate SDA stuck low"

		I2C_STATS=$(i2c_stats)
		[ -n "$I2C_STATS" ] && echo 0 > "$I2C_STATS"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			LINE0=$(emu_i2c_line A)
			RCLK0=$(frame_field "$LINE0" recovery_clocks)
			BCLK0=$(frame_field "$LINE0" busy_clocks)
			/usr/bin/error_test i2c-stuck "$I2C_DEV" > /tmp/error_test.log 2>&1
			ERR_RC=$?
			cat /tmp/error_test.log
			check "reads fail with EBUSY while SDA is held low" '[ "$ERR_RC" -eq 0 ]'
			# Each read ends with the STOP of the failed transfer and
			# i2c_generic_scl_recovery(), which pulses SCL 9 times
			# while SDA stays low: the emulator counts the SCL rises
			# made by pin writes on the wire, 10 per read
			LINE=$(emu_i2c_line A)
			RCLK=$(frame_field "$LINE" recovery_clocks)
			RCLK=$((${RCLK:-0} - ${RCLK0:-0}))
			check "bus recovery: SCL pulsed against the held SDA ($RCLK)" \
				'[ "$RCLK" -ge 20 ]'
			# The busy bus is seen before the START: no START, no
			# address clocked into a slave that holds SDA
			BCLK=$(frame_field "$LINE" busy_clocks)
			BCLK=$((${BCLK:-0} - ${BCLK0:-0}))
			check "nothing clocked while SDA is held ($BCLK)" '[ "$BCLK" -eq 0 ]'
			if [ -n "$I2C_STATS" ]; then
				echo "i2c_stats:"
				cat "$I2C_STATS"
				check "i2c_stats: xfer_fail > 0"   'grep -q "xfer_fail=[1-9]" "$I2C_STATS"'
				check "i2c_stats: bus_recovery > 0" 'grep -q "bus_recovery=[1-9]" "$I2C_STATS"'
			else
				check "i2c_stats in debugfs" 'false'
			fi

			echo "error none" > /tmp/emu.ctl
			/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_free.log 2>&1
			check "the bus works once SDA is let go" \
				'grep -q "All I2C tests PASSED" /tmp/i2c_free.log'
			BCLK0=$(frame_field "$(emu_i2c_line A)" busy_clocks)
			echo "error i2c-stuck" > /tmp/emu.ctl
			/usr/bin/error_test i2c-stuck "$I2C_DEV" > /tmp/error_test.log 2>&1
			ERR_RC=$?
			cat /tmp/error_test.log
			check "SDA held after a good transfer: EBUSY at the START" \
				'[ "$ERR_RC" -eq 0 ]'
			BCLK=$(frame_field "$(emu_i2c_line A)" busy_clocks)
			BCLK=$((${BCLK:-0} - ${BCLK0:-0}))
			check "one address byte at most clocked into the bus held again ($BCLK)" \
				'[ "$BCLK" -le 9 ]'
		else
			echo "ERROR: No I2C device found"
			check "I2C bus stuck handled" 'false'
		fi
		;;
	i2c-check-free)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		check "I2C adapter exists"        '[ -n "$I2C_DEV" ]'
		check "check_bus_free=1 announced" 'dmesg | grep -q "check_bus_free=1"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "transfers work with the bus read before each" \
			'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'

		BCLK0=$(frame_field "$(emu_i2c_line A)" busy_clocks)
		echo "error i2c-stuck" > /tmp/emu.ctl
		/usr/bin/error_test i2c-stuck "$I2C_DEV" > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "SDA held after a good transfer: EBUSY" '[ "$ERR_RC" -eq 0 ]'
		LINE=$(emu_i2c_line A); echo "$LINE"
		BCLK=$(frame_field "$LINE" busy_clocks)
		BCLK=$((${BCLK:-0} - ${BCLK0:-0}))
		check "nothing clocked into the bus held again ($BCLK)" \
			'[ -n "$BCLK0" ] && [ "$BCLK" -eq 0 ]'

		echo "error none" > /tmp/emu.ctl
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_free.log 2>&1
		cat /tmp/i2c_free.log
		check "the bus works once SDA is let go" \
			'grep -q "All I2C tests PASSED" /tmp/i2c_free.log'
		;;
	i2c-stretch)
		# The slave holds SCL for 20 ms after its address. AD7 is not
		# wired, the MPSSE clocks on and the slave misses the byte: the
		# transfer is expected to fail, the bus to work afterwards.
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'

		I2C_STATS=$(i2c_stats)
		[ -n "$I2C_STATS" ] && echo 0 > "$I2C_STATS"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		echo "error i2c-stretch 1" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/error_test i2c-fail "$I2C_DEV" > /tmp/stretch_test.log 2>&1
		ERR_RC=$?
		cat /tmp/stretch_test.log
		check "slave stretched the clock" 'grep -q "I2C slave stretches SCL" /tmp/emu.log'
		check "transfer fails: the byte was clocked while SCL was held" \
			'[ "$ERR_RC" -eq 0 ]'
		# A read: the byte and the STOP go past the slave holding SCL
		# after its address; the read must fail, not return what SDA
		# showed, and the STOP is sent again once SCL is up
		echo "error i2c-stretch 1" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/error_test i2c-fail-read "$I2C_DEV" > /tmp/stretch_read.log 2>&1
		ERR_RC=$?
		cat /tmp/stretch_read.log
		check "read fails: SCL still held at the STOP" \
			'[ "$ERR_RC" -eq 0 ] && dmesg | grep -q "SCL held low at the STOP"'
		check "SCL released in time: not reported stuck" '! dmesg | grep -q "SCL stuck low"'
		cat "$I2C_STATS"
		check "i2c_stats: xfer_fail > 0"   'grep -q "xfer_fail=[1-9]" "$I2C_STATS"'
		check "i2c_stats: scl_stretch == 0" 'grep -q "scl_stretch=0" "$I2C_STATS"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "bus works after the stretch" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
	i2c-stuck-byte)
		# The slave holds SDA low from the next clock and lets go after
		# three recovery pulses: the read fails, the recovery frees the
		# bus and must end with a STOP before the next START (else the
		# emulator reports an I2C protocol error)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		echo "error i2c-stuck-byte 1" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/error_test i2c-fail-read "$I2C_DEV" > /tmp/stuck_byte.log 2>&1
		ERR_RC=$?
		cat /tmp/stuck_byte.log
		check "slave stuck in a byte" 'grep -q "I2C slave stuck in a byte" /tmp/emu.log'
		check "read fails while it holds SDA" '[ "$ERR_RC" -eq 0 ]'
		check "it let go during the bus recovery" \
			'grep -q "I2C slave lets go of SDA" /tmp/emu.log && dmesg | grep -q "SDA held low after the STOP"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "bus works after the recovery" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
	i2c-scl-stuck)
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		# SCL held, AD7 not wired: AD7 high while SCL is low says it
		check "AD7 not wired, although SCL is held" \
			'dmesg | grep -q "AD7 not wired to SCL"'

		I2C_STATS=$(i2c_stats)
		[ -n "$I2C_STATS" ] && echo 0 > "$I2C_STATS"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/error_test i2c-fail "$I2C_DEV" > /tmp/stretch_test.log 2>&1
			ERR_RC=$?
			cat /tmp/stretch_test.log
			check "transfer fails" '[ "$ERR_RC" -eq 0 ]'
			check "Driver did not crash" '! dmesg | grep -qE "Oops:|[^A-Za-z]BUG: |Kernel panic"'
			check "SCL timeout in dmesg" 'dmesg | grep -q "SCL stuck low"'
			if [ -n "$I2C_STATS" ]; then
				echo "i2c_stats:"
				cat "$I2C_STATS"
				check "i2c_stats: scl_stretch > 0" 'grep -q "scl_stretch=[1-9]" "$I2C_STATS"'
				check "i2c_stats: xfer_fail > 0"   'grep -q "xfer_fail=[1-9]" "$I2C_STATS"'
			else
				check "i2c_stats in debugfs" 'false'
			fi
		else
			check "I2C device found" 'false'
		fi
		;;
	i2c-ad7-stretch)
		# The slave stretches for 20 ms after every byte. The MPSSE
		# waits before the next byte (adaptive clocking), but Sr and
		# STOP are pin writes it does not hold back. The memory device
		# only gives the data back if it saw them all.
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		I2C_STATS=$(i2c_stats)
		check "AD7 wiring detected"      'dmesg | grep -q "AD7<->SCL wiring detected"'
		echo 0 > "$I2C_STATS"
		echo "error i2c-stretch 0" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		echo "error none" > /tmp/emu.ctl
		usleep 100000
		check "slave stretched the clock" 'grep -q "I2C slave stretches SCL" /tmp/emu.log'
		check "write, repeated START, read and STOP through the stretches" \
			'grep -q "memory device data OK" /tmp/i2c_mem.log'
		cat "$I2C_STATS"
		check "no transfer failed"       'grep -q "xfer_fail=0" "$I2C_STATS"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C with adaptive clocking" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
	i2c-ad7-stretch-short)
		# The bus model reports an address after a repeated START that
		# came too soon after the slave let SCL go: a slave may have
		# missed it. SCL is read once released, not just before SDA
		# moves, and the condition is redone if it was still low.
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		I2C_STATS=$(i2c_stats)
		check "AD7 wiring detected"      'dmesg | grep -q "AD7<->SCL wiring detected"'
		echo 0 > "$I2C_STATS"
		echo "error i2c-stretch-short 0" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		echo "error none" > /tmp/emu.ctl
		usleep 100000
		check "slave let SCL go after the master" 'grep -q "I2C slave lets SCL go" /tmp/emu.log'
		check "write, repeated START, read and STOP" \
			'grep -q "memory device data OK" /tmp/i2c_mem.log'
		cat "$I2C_STATS"
		check "no transfer failed"       'grep -q "xfer_fail=0" "$I2C_STATS"'
		;;
	i2c-ad7-late)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		check "AD7 not judged while SCL is held" \
			'dmesg | grep -q "SCL held low: AD7<->SCL wiring checked once the bus is free"'
		check "no wiring verdict at probe" '! dmesg | grep -q "AD7 not wired to SCL"'
		echo "error none" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C works once SCL is free" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		check "AD7 wiring detected at the first transfer" \
			'dmesg | grep -q "AD7<->SCL wiring detected, adaptive clocking enabled"'
		echo "error i2c-stretch 0" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		echo "error none" > /tmp/emu.ctl
		check "clock stretching handled (adaptive clocking)" \
			'grep -q "memory device data OK" /tmp/i2c_mem.log'
		;;
	i2c-ad7-missing)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		I2C_STATS=$(i2c_stats)
		check "adaptive clocking forced" 'dmesg | grep -q "clock stretching (AD7)"'
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		check "MPSSE stopped waiting for RTCK" 'grep -q "the engine waits for ever" /tmp/emu.log'
		check "transfer fails instead of hanging" 'grep -q "FAIL:" /tmp/i2c_mem.log'
		check "core resynchronised the MPSSE" \
			'dmesg | grep -q "MPSSE resynchronised after a failed transfer"'
		cat "$I2C_STATS"
		check "i2c_stats: timeout > 0"       'grep -q "^timeout=[1-9]" "$I2C_STATS"'
		;;
	i2c-an113)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		check "strict_timing=0 announced" 'dmesg | grep -q "strict_timing=0"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C read/write test"      'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		check "SCL high time of AN_113 timing measured: 3.33 us" \
			'grep -q "I2C timing: tHIGH 3333 ns" /tmp/emu.log'
		check "START hold time of AN_113 timing measured: 320 ns" \
			'grep -q "I2C timing: tHD;STA 320 ns" /tmp/emu.log'
		;;
	spi-modes)
		# AD4 = mode 0 ... AD7 = mode 3: a byte only comes back intact
		# if it is changed and sampled on the clock edges of that mode
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		check "5 spidev devices"          '[ "$(ls /dev/spidev* 2>/dev/null | wc -w)" -eq 5 ]'
		SPI_PFX=$(ls /dev/spidev*.0 2>/dev/null | head -1 | sed 's/\.0$//')
		/usr/bin/spi_echo_test modes "$SPI_PFX" > /tmp/spi_echo.log 2>&1
		cat /tmp/spi_echo.log
		for M in 0 1 2 3; do
			check "SPI mode $M: data intact on the mode $M slave" \
				"grep -q 'PASS: SPI mode $M echo' /tmp/spi_echo.log"
			check "SPI mode $M: slave on AD$((4 + M)) was clocked" \
				"grep -q 'SPI frame cs=AD$((4 + M)) mode=$M' /tmp/emu.log"
		done
		check "SPI modes 0-3"             'grep -q "All SPI mode tests PASSED" /tmp/spi_echo.log'
		/usr/bin/spi_echo_test wrong-mode "$SPI_PFX" > /tmp/spi_wrong.log 2>&1
		cat /tmp/spi_wrong.log
		check "the slaves tell a wrong clock phase" 'grep -q "PASS:" /tmp/spi_wrong.log'
		check "CPHA=1 notice"             'dmesg | grep -q "CPHA=1.*AN_114"'
		/usr/bin/spi_test "$SPI_PFX.0" > /tmp/spi_test.log 2>&1
		cat /tmp/spi_test.log
		check "counter device on CS0"     'grep -q "All SPI tests PASSED" /tmp/spi_test.log'
		;;
	spi-timing)
		# 10 kHz: the data is still in the chip's buffer when the USB
		# write completes
		SPI_PFX=$(ls /dev/spidev*.0 2>/dev/null | head -1 | sed 's/\.0$//')
		DEV="$SPI_PFX.1"
		/usr/bin/spi_echo_test sync "$DEV" > /tmp/spi_sync.log 2>&1
		cat /tmp/spi_sync.log
		check "a write returns when the data is on the wire" \
			'grep -q "PASS:" /tmp/spi_sync.log'
		# 2.4 s of clocking: longer than the 2 s the core waits for a
		# response, which must count from the end of the clocking
		/usr/bin/spi_echo_test sync "$DEV" 3000 > /tmp/spi_sync.log 2>&1
		cat /tmp/spi_sync.log
		check "a write longer than the response timeout succeeds" \
			'grep -q "PASS:" /tmp/spi_sync.log'
		# 3 KiB wait for room in the chip (1 KiB): 6 s at 4 kHz
		/usr/bin/spi_echo_test sync "$DEV" 4096 4000 > /tmp/spi_sync.log 2>&1
		cat /tmp/spi_sync.log
		check "a write the chip holds up for more than 5 s succeeds" \
			'grep -q "PASS:" /tmp/spi_sync.log'
		kill -USR2 $EMU_PID 2>/dev/null
		sleep 1
		HELD=$(frame_field "$(grep "emu: stats chA " /tmp/emu.log | tail -1)" out_held)
		check "bulk OUT waited for room in the chip (${HELD:-0} times)" \
			'[ "${HELD:-0}" -gt 0 ]'

		/usr/bin/spi_echo_test delay "$DEV" 20000
		F=$(spi_frame); echo "$F"
		check "transfer delay: 50 bytes clocked" '[ "$(frame_field "$F" bits)" -eq 400 ]'
		check "transfer delay: CS released 20 ms after the last clock edge" \
			'[ "$(frame_field "$F" hold_ns)" -ge 20000000 ]'

		/usr/bin/spi_echo_test cs-change "$DEV"
		F=$(spi_frame); echo "$F"
		check "cs_change: second transfer in its own frame" \
			'[ "$(frame_field "$F" bits)" -eq 32 ]'
		check "cs_change: CS released for 10 us at least" \
			'[ "$(frame_field "$F" inactive_ns)" -ge 10000 ]'

		# spidev: word_delay_usecs is 8 bits wide. At 10 kHz the
		# words of one command are 100 us apart.
		/usr/bin/spi_echo_test word-delay "$DEV" 250
		F=$(spi_frame); echo "$F"
		check "word delay: 6 words clocked" '[ "$(frame_field "$F" bits)" -eq 48 ]'
		check "word delay: 250 us between the words" \
			'[ "$(frame_field "$F" wgap_ns)" -ge 250000 ]'
		;;
	spi-cs)
		# Chip select timing and cs_off, which only kernel drivers can
		# ask for: tests/kmod/ftdi_spi_cstest.c is bound to the mode 0
		# slave on AD4 and sends messages A to D from its probe. One
		# frame line each for A and B, two for C and for D.
		SPI_DEV=$(basename "$(ls -d /sys/bus/spi/devices/spi*.1 2>/dev/null | head -1)")
		insmod /lib/modules/ftdi_spi_cstest.ko
		echo "$SPI_DEV" > /sys/bus/spi/drivers/spidev/unbind
		echo ftdi_spi_cstest > "/sys/bus/spi/devices/$SPI_DEV/driver_override"
		echo "$SPI_DEV" > /sys/bus/spi/drivers/ftdi_spi_cstest/bind
		dmesg | grep cstest
		check "test driver ran its messages" 'dmesg | grep -q "cstest: done"'
		grep "SPI frame cs=AD4" /tmp/emu.log
		check "6 frames on the wire" \
			'[ "$(grep -c "SPI frame cs=AD4" /tmp/emu.log)" -eq 6 ]'
		A=$(spi_frame_n 1); B=$(spi_frame_n 2); C2=$(spi_frame_n 4)
		D1=$(spi_frame_n 5); D2=$(spi_frame_n 6)
		check "cs_setup: 300 us from CS to the first clock edge" \
			'[ "$(frame_field "$A" setup_ns)" -ge 300000 ]'
		check "cs_hold: 500 us from the last clock edge to CS" \
			'[ "$(frame_field "$A" hold_ns)" -ge 500000 ]'
		check "cs_inactive: CS released for 700 us between messages" \
			'[ "$(frame_field "$B" inactive_ns)" -ge 700000 ]'
		check "cs_change_delay: CS released for 2 ms between transfers" \
			'[ "$(frame_field "$C2" inactive_ns)" -ge 2000000 ]'
		check "cs_off: two bytes before, in their own frame" \
			'[ "$(frame_field "$D1" bits)" -eq 16 ]'
		check "cs_off: two bytes after, in their own frame" \
			'[ "$(frame_field "$D2" bits)" -eq 16 ]'
		check "cs_off: the slave did not get the bytes in between" \
			'dmesg | grep -q "cstest: cs_off PASS"'
		;;
	spi-error)
		# usb-timeout: a bulk IN URB that never completes
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'

		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		echo "error usb-timeout 1" > /tmp/emu.ctl
		/usr/bin/error_test spi-recover "$SPI_DEV" 1 > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "USB timeout injected during a transfer" \
			'grep -q "USB timeout injected" /tmp/emu.log'
		check "SPI transfer fails on the USB timeout" \
			'grep -q "PASS: transfer failed as expected" /tmp/error_test.log'
		check "core resynchronised the MPSSE" \
			'dmesg | grep -q "MPSSE resynchronised after a failed transfer"'
		check "SPI data in step after the timeout" '[ "$ERR_RC" -eq 0 ]'
		;;
	spi-cs-high)
		SPI_PFX=$(ls /dev/spidev*.0 2>/dev/null | head -1 | sed 's/\.0$//')
		/usr/bin/spi_test "$SPI_PFX.1" cs-high > /tmp/spi_csh.log 2>&1
		cat /tmp/spi_csh.log
		check "SPI_CS_HIGH: the device behind an active-high CS answers" \
			'grep -q "All SPI tests PASSED" /tmp/spi_csh.log'
		/usr/bin/spi_test "$SPI_PFX.0" > /tmp/spi_cs0.log 2>&1
		cat /tmp/spi_cs0.log
		check "the active-low device answers alone on MISO" \
			'grep -q "All SPI tests PASSED" /tmp/spi_cs0.log'
		;;
	spi-halt)
		# usb-stall: the bulk IN endpoint stays halted until cleared
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		echo "error usb-stall 1" > /tmp/emu.ctl
		/usr/bin/error_test spi-recover "$SPI_DEV" 0 > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "bulk IN halted during a transfer" \
			'grep -q "USB STALL injected on bulk IN of interface 0, halted" /tmp/emu.log'
		check "SPI transfer fails on the halt" \
			'grep -q "PASS: transfer failed as expected" /tmp/error_test.log'
		check "the core cleared the halt" \
			'grep -q "bulk IN halt of interface 0 cleared" /tmp/emu.log'
		check "core resynchronised the MPSSE" \
			'dmesg | grep -q "MPSSE resynchronised after a failed transfer (-32)"'
		check "SPI data in step after the halt" '[ "$ERR_RC" -eq 0 ]'
		# Again, with a second STALL during the resynchronisation: the
		# next transfer clears it before anything else
		echo "error usb-stall 2" > /tmp/emu.ctl
		/usr/bin/error_test spi-recover "$SPI_DEV" 0 > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "STALL during the recovery" \
			'dmesg | grep -q "MPSSE resynchronisation failed: -32"'
		check "three halts, three cleared" \
			'[ "$(grep -c "USB STALL injected on bulk IN" /tmp/emu.log)" -eq 3 ] && [ "$(grep -c "bulk IN halt of interface 0 cleared" /tmp/emu.log)" -eq 3 ]'
		check "SPI data in step after both" '[ "$ERR_RC" -eq 0 ]'
		;;
	uart-halt)
		# usb-stall on the read URB of the port: RX must come back
		UART_DEV=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		echo "error usb-stall 1" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/uart_test "$UART_DEV" > /tmp/uart_test.log 2>&1
		cat /tmp/uart_test.log
		check "bulk IN halted" \
			'grep -q "USB STALL injected on bulk IN of interface 0, halted" /tmp/emu.log'
		check "the UART cleared the halt" \
			'grep -q "bulk IN halt of interface 0 cleared" /tmp/emu.log'
		check "UART loopback after the halt" 'grep -q "All UART tests PASSED" /tmp/uart_test.log'
		check "no error clearing it" '! dmesg | grep -q "bulk IN halted:"'
		;;
	ee-product-i2c)
		check "channel A is I2C"             '[ "$(chan_mode 0)" = i2c ]'
		check "chosen by the product string" \
			'dmesg | grep -q "EEPROM hints I2C mode (product string)"'
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C read/write test" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
	ee-vcp-uart)
		check "channel A is a UART"          '[ "$(chan_mode 0)" = uart ]'
		check "chosen by the VCP driver bit" \
			'dmesg | grep -q "EEPROM hints UART mode (VCP driver bit)"'
		check "unknown bus_mode tag reported and ignored" \
			"dmesg | grep -q \"unknown bus_mode 'bogus' in the user area\""
		check "eeprom_bus_mode shows the tag" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_bus_mode)" = bogus ]'
		check "ttyFTDI device exists" 'ls /dev/ttyFTDI* >/dev/null 2>&1'
		;;
	ee-slew-i2c)
		cat $(dbgfs 0 eeprom_drive)
		check "channel A is I2C"             '[ "$(chan_mode 0)" = i2c ]'
		check "chosen by the ADBUS drive settings" \
			'dmesg | grep -q "EEPROM hints I2C mode (slow slew and Schmitt"'
		check "eeprom_drive: ADBUS is group1" \
			'grep -q "^group1 (ADBUS): 4mA slew=slow schmitt=on" $(dbgfs 0 eeprom_drive)'
		;;
	ee-default-spi)
		cat $(dbgfs 0 eeprom_drive)
		check "channel A is SPI"             '[ "$(chan_mode 0)" = spi ]'
		check "SPI is the default, ACBUS settings do not count" \
			'dmesg | grep -q "auto: no EEPROM hint, defaulting to SPI"'
		check "eeprom_drive: ACBUS is group0" \
			'grep -q "^group0 (ACBUS): 4mA slew=slow schmitt=on" $(dbgfs 0 eeprom_drive)'
		check "eeprom_drive: ADBUS untouched" \
			'grep -q "^group1 (ADBUS): 4mA slew=fast schmitt=off" $(dbgfs 0 eeprom_drive)'
		check "eeprom_bus_mode: no tag" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_bus_mode)" = none ]'
		;;
	ee-cbus-1a)
		cat $(dbgfs 0 eeprom_cbus)
		check "channel A is SPI: byte 0x1a is not a hint" \
			'[ "$(chan_mode 0)" = spi ]'
		check "SPI is the default" \
			'dmesg | grep -q "auto: no EEPROM hint, defaulting to SPI"'
		check "byte 0x1a decoded as ACBUS4 = TXDEN" \
			'grep -q "^CBUS4: TXDEN" $(dbgfs 0 eeprom_cbus)'
		check "byte 0x1a decoded as ACBUS5 = PWREN#" \
			'grep -q "^CBUS5: PWREN" $(dbgfs 0 eeprom_cbus)'
		;;
	ee-fifo)
		check "FIFO channel type refused" \
			'dmesg | grep -q "EEPROM channel type 0x01 not supported"'
		check "no channel set up"   '! dmesg | grep -q "FTDI MPSSE core:"'
		check "no child device" \
			'! ls /sys/bus/platform/devices | grep -q "^ftdi-"'
		;;
	ee-blank)
		check "blank EEPROM reported"   'dmesg | grep -q "EEPROM: empty"'
		check "channel A is SPI"        '[ "$(chan_mode 0)" = spi ]'
		check "SPI without a usable EEPROM" \
			'dmesg | grep -q "auto: no usable EEPROM, defaulting to SPI"'
		check "eeprom_empty is 1" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_empty)" = 1 ]'
		check "eeprom_bus_mode: n/a" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_bus_mode)" = n/a ]'
		# "the device will not have a serial number" (DS_FT232H 7.1)
		check "built-in descriptors: product FT232H, no serial number" \
			'[ "$(cat /sys/bus/usb/devices/1-1/product)" = FT232H ] && [ ! -e /sys/bus/usb/devices/1-1/serial ]'
		;;
	ee-93c46)
		EE=/sys/bus/usb/devices/1-1:1.0/eeprom
		check "channel A is I2C"        '[ "$(chan_mode 0)" = i2c ]'
		check "channel B is I2C"        '[ "$(chan_mode 1)" = i2c ]'
		check "chosen by the product string" \
			'dmesg | grep -q "EEPROM hints I2C mode (product string)"'
		check "checksum of the 64 words accepted" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_checksum)" = ok ] && [ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_valid)" = 1 ]'
		check "the image reads back mirrored" \
			'[ "$(dd if=$EE bs=128 count=1 2>/dev/null | md5sum)" = "$(dd if=$EE bs=128 skip=1 count=1 2>/dev/null | md5sum)" ]'
		cat $(dbgfs 0 eeprom_config)
		check "strings found behind the fixed part" \
			'grep -q "^product=\"FT2232H-I2C-bridge\"" $(dbgfs 0 eeprom_config) && grep -q "^serial=\"FT000001\"" $(dbgfs 0 eeprom_config)'
		check "the device reports them" \
			'[ "$(cat /sys/bus/usb/devices/1-1/product)" = FT2232H-I2C-bridge ] && [ "$(cat /sys/bus/usb/devices/1-1/serial)" = FT000001 ]'
		chan_io_test 0
		chan_io_test 1
		;;
	ee-custom-pid)
		check "enumerated with the PID of the EEPROM" \
			'[ "$(cat /sys/bus/usb/devices/1-1/idVendor):$(cat /sys/bus/usb/devices/1-1/idProduct)" = 0403:1234 ]'
		check "bound through new_id, FT232H from bcdDevice" \
			'[ "$(chan_mode 0)" = spi ] && [ "$(cat /sys/bus/usb/devices/1-1/bcdDevice)" = 0900 ]'
		check "eeprom_pid" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_pid)" = 0x1234 ]'
		chan_io_test 0
		;;
	ee-badsum)
		check "bad checksum reported"   'dmesg | grep -q "EEPROM: checksum mismatch"'
		check "channel A is SPI: the i2c tag is not trusted" \
			'[ "$(chan_mode 0)" = spi ]'
		check "SPI without a usable EEPROM" \
			'dmesg | grep -q "auto: no usable EEPROM, defaulting to SPI"'
		;;
	ee-tag-ff)
		check "channel A is I2C"        '[ "$(chan_mode 0)" = i2c ]'
		check "chosen by the tag" \
			'dmesg | grep -q "auto: EEPROM hints I2C mode (bus_mode tag)"'
		check "eeprom_bus_mode: the value stops at the 0xFF cells" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_bus_mode)" = i2c ]'
		;;
	ee-param-wins)
		check "channel A is SPI"        '[ "$(chan_mode 0)" = spi ]'
		check "EEPROM tag says i2c" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.0/eeprom_bus_mode)" = i2c ]'
		check "no auto detection" '! dmesg | grep -q "1-1:1.0: auto: "'
		check "mode not reported as auto" \
			"dmesg | grep \"FTDI MPSSE core: channel A, mode 'spi'\" | grep -qv '(auto)'"
		;;
	ee-ft2232h-chan)
		check "channel A is I2C"        '[ "$(chan_mode 0)" = i2c ]'
		check "channel B is SPI"        '[ "$(chan_mode 1)" = spi ]'
		check "eeprom_bus_mode shows both entries" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.1/eeprom_bus_mode)" = i2c,spi ]'
		check "I2C adapter on A"  'ls /sys/bus/usb/devices/1-1:1.0/ftdi-i2c.*/i2c-* >/dev/null 2>&1'
		check "SPI controller on B" 'ls -d /sys/bus/usb/devices/1-1:1.1/ftdi-spi.*/spi_master >/dev/null 2>&1'
		;;
	ee-ft2232h-drive)
		cat $(dbgfs 1 eeprom_drive)
		check "channel A is SPI"        '[ "$(chan_mode 0)" = spi ]'
		check "channel B is I2C"        '[ "$(chan_mode 1)" = i2c ]'
		check "B chosen by its own drive group" \
			'dmesg | grep "1-1:1.1" | grep -q "EEPROM hints I2C mode (slow slew and Schmitt"'
		check "eeprom_drive: four groups" \
			'[ "$(grep -c "^group[0-3] (" $(dbgfs 1 eeprom_drive))" -eq 4 ]'
		check "eeprom_drive: BL is group2" \
			'grep -q "^group2 (BL): 4mA slew=slow schmitt=on" $(dbgfs 1 eeprom_drive)'
		check "eeprom_drive: AL untouched" \
			'grep -q "^group0 (AL): 4mA slew=fast schmitt=off" $(dbgfs 1 eeprom_drive)'
		;;
	ee-ft4232h-vcp)
		check "no channel refused" '! dmesg | grep -q "not supported"'
		check "channel A is SPI (D2XX)"  '[ "$(chan_mode 0)" = spi ]'
		check "channel B is a UART (VCP)" '[ "$(chan_mode 1)" = uart ]'
		check "B chosen by the VCP driver bit" \
			'dmesg | grep "1-1:1.1" | grep -q "EEPROM hints UART mode (VCP driver bit)"'
		check "channel C is a UART"      '[ "$(chan_mode 2)" = uart ]'
		check "channel D is a UART"      '[ "$(chan_mode 3)" = uart ]'
		check "eeprom_channel_type of B: uart" \
			'[ "$(cat /sys/bus/usb/devices/1-1:1.1/eeprom_channel_type)" = uart ]'
		;;
	uart-flow)
		TTY=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		check "ttyFTDI device exists" '[ -n "$TTY" ]'
		DSRFLOW=$(ls /sys/bus/platform/devices/ftdi-uart.*/dtr_dsr_flow 2>/dev/null | head -1)
		check "dtr_dsr_flow attribute exists" '[ -n "$DSRFLOW" ]'

		for F in rtscts dtrdsr xonxoff; do
			[ "$F" = dtrdsr ] && echo 1 > "$DSRFLOW"
			H0=$(frame_field "$(emu_uart A)" rx_holds)
			/usr/bin/uart_flow_test "$TTY" $F > /tmp/flow.log 2>&1
			cat /tmp/flow.log
			[ "$F" = dtrdsr ] && echo 0 > "$DSRFLOW"
			check "$F: every byte read back from a port left unread" \
				'grep -q "PASS:" /tmp/flow.log'
			U=$(emu_uart A); echo "$U"
			check "$F: the chip held its sender" \
				'[ "$(frame_field "$U" rx_holds)" -gt "${H0:-0}" ]'
			check "$F: no overrun in the chip" \
				'[ "$(frame_field "$U" rx_overruns)" = 0 ]'
		done

		/usr/bin/uart_flow_test "$TTY" none > /tmp/flow.log 2>&1
		cat /tmp/flow.log
		check "no flow control: the overrun of the chip is counted" \
			'grep -q "PASS:" /tmp/flow.log'
		U=$(emu_uart A); echo "$U"
		check "no flow control: the chip lost bytes" \
			'[ "$(frame_field "$U" rx_overruns)" -gt 0 ]'
		;;
	uart-drain)
		# Once the tty layer is empty, up to two write URBs and the
		# chip's TX buffer are still on their way out (9 KiB on
		# FT232H, 4.8 s at 19200 baud)
		TTY=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		check "ttyFTDI device exists" '[ -n "$TTY" ]'

		/usr/bin/uart_drain_test "$TTY" drain 12288 > /tmp/drain.log 2>&1
		DRAIN_RC=$?
		cat /tmp/drain.log
		check "tcdrain() returns when the data is on the wire" \
			'[ "$DRAIN_RC" -eq 0 ]'

		TX0=$(emu_stats A | sed 's/.*uart_tx=\([0-9]*\).*/\1/')
		/usr/bin/uart_drain_test "$TTY" close 12288 > /tmp/close.log 2>&1
		cat /tmp/close.log
		# Whatever the chip still holds after the close goes out
		sleep 2
		TX1=$(emu_stats A | sed 's/.*uart_tx=\([0-9]*\).*/\1/')
		echo "chip transmitted $((TX1 - TX0)) of the 12288 bytes written before close()"
		check "close() does not drop written data" \
			'[ "$((TX1 - TX0))" -eq 12288 ]'
		;;
	mpsse-desync)
		# mpsse-extra: a byte nobody asked for precedes a response
		DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		check "device node exists" '[ -n "$DEV" ]'
		echo "error mpsse-extra 1" > /tmp/emu.ctl
		/usr/bin/error_test i2c-recover "$DEV" 1 > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "stray response byte injected" \
			'grep -q "stray response byte injected" /tmp/emu.log'
		check "transfer with the stray byte fails" \
			'grep -q "PASS: transfer failed as expected: Protocol error" /tmp/error_test.log'
		check "core reported the unexpected byte" \
			'dmesg | grep -q "unexpected response byte"'
		check "core resynchronised the MPSSE" \
			'dmesg | grep -q "MPSSE resynchronised after a failed transfer"'
		check "data in step afterwards" '[ "$ERR_RC" -eq 0 ]'
		;;
	spi-stall|i2c-stall)
		# The MPSSE hangs on a read command for longer than the driver
		# waits and answers afterwards: that late answer must never be
		# taken for the response to a later command
		if [ "$FTDI_MODE" = "i2c-stall" ]; then
			DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
			TOOL=i2c-recover
		else
			DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
			TOOL=spi-recover
		fi
		check "device node exists" '[ -n "$DEV" ]'
		echo "error mpsse-stall 1" > /tmp/emu.ctl
		/usr/bin/error_test $TOOL "$DEV" 4 > /tmp/error_test.log 2>&1
		ERR_RC=$?
		cat /tmp/error_test.log
		check "MPSSE stalled on a read command" \
			'grep -q "MPSSE stall injected" /tmp/emu.log'
		check "stalled transfer fails" \
			'grep -q "PASS: transfer failed as expected" /tmp/error_test.log'
		check "core reset the MPSSE during the stall" \
			'grep -q "MPSSE reset during the stall" /tmp/emu.log'
		check "no late response from the stalled command" \
			'! grep -q "MPSSE stall over" /tmp/emu.log'
		check "core resynchronised the MPSSE" \
			'dmesg | grep -q "MPSSE resynchronised after a failed transfer"'
		check "data in step after the stall" '[ "$ERR_RC" -eq 0 ]'
		if [ "$FTDI_MODE" = "i2c-stall" ]; then
			I2C_STATS=$(i2c_stats)
			cat "$I2C_STATS"
			check "i2c_stats: one timeout" 'grep -q "^timeout=1$" "$I2C_STATS"'
			check "i2c_stats: later transfers succeeded" \
				'grep -q "^xfer_ok=8$" "$I2C_STATS"'
			# The reset cleared the 0x9E mask: SCL, SDA out and in
			# open drain again (lo=value/direction/open-drain)
			check "open-drain mask replayed after the resynchronisation" \
				'emu_state A | grep -q " lo=[0-9a-f]*/[0-9a-f]*/07 "'
		fi
		;;
	mpsse-sync-fail)
		echo "=== MPSSE Sync Failure Test ==="
		echo "Emulator returns wrong echo for 0xAA bad command"
		check "MPSSE sync failed"          'dmesg | grep -q "MPSSE sync failed"'
		check "No I2C adapter (probe failed)" '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		check "No SPI controller"          '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		check "No crash/oops"              '! dmesg | grep -qE "Oops:|[^A-Za-z]BUG: |Kernel panic"'
		;;
	suspend)
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		SPI_DEV=$(chan_spidev 0)
		GPIO_DEV=$(chan_gpiochip 0)
		chan_io_test 0 " before suspend"
		# Outputs to find again: AC0 high, AD5 open drain, AC1 low
		/usr/bin/gpio_test "$GPIO_DEV" 8 set 1
		/usr/bin/gpio_test "$GPIO_DEV" 5 set 1 od
		/usr/bin/gpio_test "$GPIO_DEV" 9 set 0
		BEFORE=$(emu_state A)
		echo "chip state before: $BEFORE"
		check "chip in MPSSE mode with the GPIO outputs set" \
			'echo "$BEFORE" | grep -q "^bitmode=0x02 lo=[0-9a-f]*/[0-9a-f]*/20 hi=01/03/00 "'

		# A 2.4 s write is on the wire when the suspend comes: the SPI
		# core lets the message in flight finish, then holds its queue
		/usr/bin/spi_echo_test sync "$SPI_DEV" 3000 > /tmp/spi_bg.log 2>&1 &
		BG_PID=$!
		usleep 500000
		if pm_suspend; then
			check "suspend waited for the SPI message in flight" \
				'grep -q "PASS: write of 3000 bytes" /tmp/spi_bg.log'
			wait $BG_PID
			cat /tmp/spi_bg.log
			check "suspend: 1 interface, callbacks of ftdi-spi and ftdi-gpio" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 suspended: 1 interface(s), 2 child callback(s): 0"'

			S1=$(emu_stats A)
			/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_susp.log 2>&1
			check "SPI I/O refused while suspended" \
				'! grep -q "All SPI tests PASSED" /tmp/spi_susp.log'
			/usr/bin/gpio_test "$GPIO_DEV" 10 set 1 > /tmp/gpio_susp.log 2>&1
			check "GPIO I/O refused while suspended" '! grep -q "PASS:" /tmp/gpio_susp.log'
			S2=$(emu_stats A)
			check "nothing sent to the chip while suspended" '[ "$S1" = "$S2" ]'

			emu_power_cycle
			check "chip lost its state" '[ "$(emu_state A)" = "$EMU_STATE_OFF" ]'

			pm_cmd resume
			check "resume: 1 interface, callbacks of ftdi-spi and ftdi-gpio" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 resumed: 1 interface(s), 2 child callback(s): 0"'
			AFTER=$(emu_state A)
			echo "chip state after:  $AFTER"
			check "MPSSE mode, pins and open-drain mask restored" '[ "$AFTER" = "$BEFORE" ]'
			chan_io_test 0 " after resume"
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO works after resume" 'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
		else
			wait $BG_PID
		fi
		;;
	suspend-i2c)
		# The clock divisor, 3-phase and adaptive clocking and the
		# open-drain setup of SCL/SDA all have to come back
		I2C_DEV=$(chan_i2c 0)
		GPIO_DEV=$(chan_gpiochip 0)
		check "AD7 wiring detected"      'dmesg | grep -q "AD7<->SCL wiring detected"'
		chan_io_test 0 " before suspend"
		/usr/bin/gpio_test "$GPIO_DEV" 8 set 1
		/usr/bin/gpio_test "$GPIO_DEV" 5 set 1 od
		BEFORE=$(emu_state A)
		echo "chip state before: $BEFORE"
		check "chip in MPSSE mode, SCL/SDA and AD5 open drain" \
			'echo "$BEFORE" | grep -q "^bitmode=0x02 lo=[0-9a-f]*/[0-9a-f]*/27 hi=01/01/00 "'

		# An I2C transfer, slowed down by a stretching slave, is in
		# flight when the suspend comes (as from a kernel client: user
		# space is frozen by then). The core cuts it short.
		echo "error i2c-stretch 0" > /tmp/emu.ctl
		usleep 100000
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_bg.log 2>&1 &
		BG_PID=$!
		usleep 300000
		if pm_suspend; then
			wait $BG_PID
			cat /tmp/i2c_bg.log
			echo "error none" > /tmp/emu.ctl
			check "suspend: 1 interface, callback of ftdi-gpio" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 suspended: 1 interface(s), 1 child callback(s): 0"'
			check "suspend cuts the I2C transfer in flight short (ESHUTDOWN)" \
				'grep -q "FAIL:.*transport endpoint shutdown" /tmp/i2c_bg.log'
			check "suspend did not have to give up on that transfer" \
				'! dmesg | grep -q "suspend: timed out"'
			/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_susp.log 2>&1
			check "I2C I/O refused while suspended" \
				'grep -q "FAIL:.*transport endpoint shutdown" /tmp/i2c_susp.log'

			emu_power_cycle
			check "chip lost its state" '[ "$(emu_state A)" = "$EMU_STATE_OFF" ]'

			pm_cmd resume
			# ftdi-i2c: the bus is read before the next START
			check "resume: 1 interface, callbacks of ftdi-i2c and ftdi-gpio" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 resumed: 1 interface(s), 2 child callback(s): 0"'
			AFTER=$(emu_state A)
			echo "chip state after:  $AFTER"
			check "MPSSE mode, pins and open-drain mask restored" '[ "$AFTER" = "$BEFORE" ]'
			chan_io_test 0 " after resume"
			echo "error i2c-stretch 0" > /tmp/emu.ctl
			usleep 100000
			/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
			cat /tmp/i2c_mem.log
			echo "error none" > /tmp/emu.ctl
			usleep 100000
			check "clock stretching works after resume" 'grep -q "memory device data OK" /tmp/i2c_mem.log'
		else
			wait $BG_PID
			echo "error none" > /tmp/emu.ctl
		fi
		;;
	suspend-uart)
		UART_DEV=$(chan_tty 0)
		CBUS_DEV=$(ls -d /sys/bus/usb/devices/1-1:1.0/ftdi-uart.*/gpiochip* 2>/dev/null | head -1)
		CBUS_DEV="/dev/${CBUS_DEV##*/}"
		check "CBUS GPIO chip registered" 'dmesg | grep -q "CBUS GPIO: 4 pins"'
		cat $(dbgfs 0 pinmap)
		check "pinmap: the tty and the CBUS chip of the UART" \
			'grep -q "^uart: ttyFTDI" $(dbgfs 0 pinmap) && grep -q "^gpio: gpiochip[0-9]* (CBUS)" $(dbgfs 0 pinmap)'
		chan_io_test 0 " before suspend"
		/usr/bin/gpio_test "$CBUS_DEV" 0 set 1
		/usr/bin/uart_test "$UART_DEV" hold 8 > /tmp/uart_hold.log 2>&1 &
		HOLD_PID=$!
		wait_for 5 'grep -q "HOLD:" /tmp/uart_hold.log'
		BEFORE=$(emu_state A)
		echo "chip state before: $BEFORE"
		check "chip in CBUS bit-bang mode, ACBUS5 high, 57600 baud" \
			'echo "$BEFORE" | grep -q "^bitmode=0x20 .* cbus=1/1 baud=575[0-9][0-9] "'
		if pm_suspend; then
			check "suspend: 1 interface, callback of ftdi-uart" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 suspended: 1 interface(s), 1 child callback(s): 0"'
			emu_power_cycle
			check "chip lost its state" '[ "$(emu_state A)" = "$EMU_STATE_OFF" ]'

			pm_cmd resume
			check "resume: 1 interface, callback of ftdi-uart" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 resumed: 1 interface(s), 1 child callback(s): 0"'
			AFTER=$(emu_state A)
			echo "chip state after:  $AFTER"
			check "baud rate, latency timer and CBUS pins restored" '[ "$AFTER" = "$BEFORE" ]'
			wait $HOLD_PID
			cat /tmp/uart_hold.log
			check "port held open: loopback works after resume" \
				'grep -q "PASS: loopback still works" /tmp/uart_hold.log'
			chan_io_test 0 " after resume"
		else
			kill $HOLD_PID 2>/dev/null
		fi
		;;
	suspend-ft4232h)
		# The interfaces are suspended last to first and resumed first
		# to last, as usbcore does
		for N in 0 1 2 3; do
			chan_io_test $N " before suspend"
		done
		/usr/bin/gpio_test "$(chan_gpiochip 0)" 6 set 1
		/usr/bin/gpio_test "$(chan_gpiochip 1)" 4 set 1
		/usr/bin/gpio_test "$(chan_gpiochip 3)" 2 set 1
		/usr/bin/uart_test "$(chan_tty 2)" hold 10 > /tmp/uart_hold.log 2>&1 &
		HOLD_PID=$!
		wait_for 5 'grep -q "HOLD:" /tmp/uart_hold.log'
		for C in A B C D; do
			eval "BEFORE_$C=\$(emu_state $C)"
			eval "echo \"chip state before, $C: \$BEFORE_$C\""
		done
		check "A and B in MPSSE mode, C a UART, D in bit-bang mode" \
			'[ "${BEFORE_A%% *}${BEFORE_B%% *}${BEFORE_C%% *}${BEFORE_D%% *}" = "bitmode=0x02bitmode=0x02bitmode=0x00bitmode=0x01" ]'
		if pm_suspend; then
			# ftdi-spi + ftdi-gpio (A), ftdi-gpio (B), ftdi-uart (C),
			# ftdi-gpio-bitbang (D)
			check "suspend: 4 interfaces, 5 child callbacks" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 suspended: 4 interface(s), 5 child callback(s): 0"'
			emu_power_cycle
			for C in A B C D; do
				check "channel $C: chip lost its state" "[ \"\$(emu_state $C)\" = \"\$EMU_STATE_OFF\" ]"
			done

			pm_cmd resume
			# ... and ftdi-i2c (B)
			check "resume: 4 interfaces, 6 child callbacks" \
				'dmesg | grep -q "ftdi_pm_test: 1-1 resumed: 4 interface(s), 6 child callback(s): 0"'
			for C in A B C D; do
				eval "AFTER=\$(emu_state $C)"
				echo "chip state after, $C:  $AFTER"
				check "channel $C: mode and pins restored" "[ \"\$AFTER\" = \"\$BEFORE_$C\" ]"
			done
			wait $HOLD_PID
			cat /tmp/uart_hold.log
			check "channel C held open: loopback works after resume" \
				'grep -q "PASS: loopback still works" /tmp/uart_hold.log'
			for N in 0 1 2 3; do
				chan_io_test $N " after resume"
			done
		else
			kill $HOLD_PID 2>/dev/null
		fi
		;;
	stress-spi)
		SPIS=$(ls /dev/spidev* 2>/dev/null)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		INTF=/sys/kernel/debug/ftdi_mpsse/1-1:1.0
		check "3 spidev devices" '[ "$(echo $SPIS | wc -w)" -eq 3 ]'
		# spidev modes are per device: CS0 runs without loopback
		# (counter pattern), CS1/CS2 in loopback (data integrity)
		W="spi:$(echo $SPIS | cut -d' ' -f1)"
		for d in $(echo $SPIS | cut -d' ' -f2-); do W="$W spi-loop:$d"; done
		run_stress "SPI+GPIO+sysfs concurrent stress" $W \
			gpio:$GPIO_DEV:6,7,8,9,10,11 gpioin:$GPIO_DEV:15:0 \
			sysfs:$INTF/pinmap sysfs:$INTF/eeprom_config
		;;
	stress-i2c)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		SPEED=$(find /sys/devices -name i2c_speed_khz 2>/dev/null | head -1)
		STATS=$(i2c_stats)
		run_stress "I2C+GPIO+sysfs concurrent stress" \
			i2c:$I2C_DEV i2c:$I2C_DEV:0x51 i2c:$I2C_DEV:0x20 \
			gpio:$GPIO_DEV:3,4,5,6,8,9,10 gpioin:$GPIO_DEV:15:0 \
			"sysfs:$SPEED=100,400,10,50" "sysfs:$STATS=0"
		cat "$STATS"
		;;
	stress-uart)
		TTY=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		LAT=$(find /sys/devices -name latency_timer 2>/dev/null | head -1)
		run_stress "UART loopback+modem lines+sysfs stress" \
			uart:$TTY mctrl:$TTY "sysfs:$LAT=1,2,16,64"
		;;
	stress-uart-errors)
		# Every packet carries a line error (OE/PE/FE/BI in turn)
		TTY=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		stty -F "$TTY" 115200 raw -echo 2>/dev/null
		timeout 10 cat "$TTY" > /dev/null 2>&1 &
		RD=$!
		i=0
		while [ $i -lt 40 ]; do
			dd if=/dev/urandom of="$TTY" bs=512 count=4 2>/dev/null
			i=$((i + 1))
		done
		wait $RD
		cat /proc/tty/driver/ftdi_uart 2>/dev/null
		check "UART line errors accounted" \
			'grep -qE "(fe|pe|brk|oe):[1-9]" /proc/tty/driver/ftdi_uart'
		;;
	stress-ft2232h)
		SPIS=$(ls /dev/spidev* 2>/dev/null)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		W=""
		for d in $SPIS; do W="$W spi-loop:$d"; done
		for g in $(ls /dev/gpiochip* 2>/dev/null); do W="$W gpio:$g:5,6,8,9,10"; done
		run_stress "FT2232H SPI(A)+I2C(B)+GPIO concurrent stress" $W \
			i2c:$I2C_DEV i2c:$I2C_DEV:0x22
		;;
	stress-ft4232h)
		SPIS=$(ls /dev/spidev* 2>/dev/null)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		TTY=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		BB=$(bitbang_chip)
		W=""
		for d in $SPIS; do W="$W spi-loop:$d"; done
		for g in $(ls /dev/gpiochip* 2>/dev/null); do
			[ "$g" = "$BB" ] && continue
			W="$W gpio:$g:4,5,6"
		done
		[ -n "$BB" ] && W="$W gpio:$BB:0,1,2,3,6,7"
		run_stress "FT4232H SPI(A)+I2C(B)+UART(C)+bit-bang(D) stress" $W \
			i2c:$I2C_DEV uart:$TTY
		;;
	stress-unbind)
		# -k: I/O errors are tolerated, corrupted data is not
		SPIS=$(ls /dev/spidev* 2>/dev/null)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		INTF=1-1:1.0
		W=""
		for d in $SPIS; do W="$W spi-loop:$d"; done
		/usr/bin/stress_test -k -t $((STRESS_SECS + 25)) $W \
			gpio:$GPIO_DEV:6,7,8 gpioin:$GPIO_DEV:12 \
			sysfs:/sys/kernel/debug/ftdi_mpsse/$INTF/pinmap \
			> /tmp/stress.log 2>&1 &
		SPID=$!
		sleep 2
		i=0
		while [ $i -lt 15 ]; do
			echo $INTF > /sys/bus/usb/drivers/ftdi_mpsse/unbind 2>/dev/null
			usleep $(( (i % 5) * 100000 ))
			echo $INTF > /sys/bus/usb/drivers/ftdi_mpsse/bind 2>/dev/null
			sleep 1
			i=$((i + 1))
		done
		for m in ftdi_spi ftdi_gpio; do
			A=""
			[ $m = ftdi_spi ] && A=register_spidev=1
			for r in 1 2 3; do
				rmmod $m && insmod /lib/modules/$m.ko $A
				sleep 1
			done
		done
		wait $SPID
		cat /tmp/stress.log
		check "unbind/rmmod storm: no corrupted transfer" \
			'grep -q "STRESS RESULT: PASS" /tmp/stress.log'
		sleep 1
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_after.log 2>&1
		cat /tmp/spi_after.log
		check "SPI works after the storm" 'grep -q "All SPI tests PASSED" /tmp/spi_after.log'
		;;
	stress-hotplug)
		# Unplug and replug (vhci detach + new USB/IP import), then an
		# emulator crash
		SPIS=$(ls /dev/spidev* 2>/dev/null)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		W=""
		for d in $SPIS; do W="$W spi-loop:$d"; done
		/usr/bin/stress_test -k -t $((STRESS_SECS + 25)) $W \
			gpio:$GPIO_DEV:6,7,8 gpioin:$GPIO_DEV:12 \
			> /tmp/stress.log 2>&1 &
		SPID=$!
		sleep 2
		VHCI=/sys/devices/platform/vhci_hcd.0
		i=0
		while [ $i -lt 8 ]; do
			# Pull the import once it is on the port and let its
			# usb_device go before the next one: vhci_hcd keeps
			# PORT_ENABLE across a disconnect, so hub_wq takes a
			# quick re-import for the old device ("resuscitate")
			wait_for 10 '[ -e /sys/bus/usb/devices/1-1 ]'
			PORT=$(awk '$3 == "006" { print $2 + 0; exit }' $VHCI/status)
			[ -n "$PORT" ] && echo $PORT > $VHCI/detach
			kill $ATTACH_PID 2>/dev/null
			wait_for 20 '[ ! -e /sys/bus/usb/devices/1-1 ]' ||
				echo "WARN: 1-1 still present 20 s after detach"
			usleep $(( (i % 4) * 200000 + 100000 ))
			/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
			ATTACH_PID=$!
			sleep 2
			i=$((i + 1))
		done
		wait_for 10 '[ -e /sys/bus/usb/devices/1-1 ]'
		kill -9 $EMU_PID 2>/dev/null
		kill $ATTACH_PID 2>/dev/null
		wait_for 20 '[ ! -e /sys/bus/usb/devices/1-1 ]' ||
			echo "WARN: 1-1 still present 20 s after the emulator crash"
		/usr/bin/ftdi_usbip $EMU_ARGS 2>>/tmp/emu.log &
		EMU_PID=$!
		sleep 1
		/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
		ATTACH_PID=$!
		sleep 2
		wait $SPID
		cat /tmp/stress.log
		check "hotplug storm: no corrupted transfer" \
			'grep -q "STRESS RESULT: PASS" /tmp/stress.log'
		# The last import may still be enumerating (slow debug kernels)
		wait_for 30 'ls /dev/spidev* >/dev/null 2>&1'
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_after.log 2>&1
		cat /tmp/spi_after.log
		check "SPI works after the hotplug storm" 'grep -q "All SPI tests PASSED" /tmp/spi_after.log'
		;;
	stress-params)
		# Reload the whole stack with odd module parameters
		reload() {
			echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/unbind 2>/dev/null
			for m in ftdi_gpio_bitbang ftdi_gpio ftdi_i2c ftdi_spi ftdi_uart ftdi_mpsse; do
				rmmod $m 2>/dev/null
			done
			echo "--- reload: ftdi_mpsse [$1] ftdi_i2c [$2] ftdi_gpio [$3]"
			insmod /lib/modules/ftdi_mpsse.ko $1 || echo "ftdi_mpsse refused [$1]"
			insmod /lib/modules/ftdi_uart.ko
			insmod /lib/modules/ftdi_spi.ko register_spidev=1
			insmod /lib/modules/ftdi_i2c.ko $2 || echo "ftdi_i2c refused [$2]"
			insmod /lib/modules/ftdi_gpio.ko $3 || echo "ftdi_gpio refused [$3]"
			insmod /lib/modules/ftdi_gpio_bitbang.ko
			echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id 2>/dev/null
			sleep 1
		}
		N=0
		for P in "bus_mode=spi spi_cs=3,4,5,6,7" "bus_mode=spi spi_cs=7,3" \
			 "bus_mode=spi spi_cs=3,3" "bus_mode=spi spi_cs=8" \
			 "bus_mode=spi spi_cs=,,," "bus_mode=spi spi_cs=3,4,5,6,7,3,4" \
			 "bus_mode=spi spi_cs=A:4;B:5;Z:9" \
			 "bus_mode=i2c,spi,uart,uart,extra,more" "bus_mode=bogus" \
			 "bus_mode=uart mpsse_latency=0" "bus_mode=spi mpsse_latency=100000" \
			 "bus_mode=bitbang"; do
			reload "$P" "" ""
			N=$((N + 1))
		done
		for Q in "i2c_speed=0" "i2c_speed=100000" \
			 "i2c_bus_nr_map=1-1:7,junk,::,1-1:abc" "i2c_bus_nr_map=1-1:-5" \
			 "i2c_delay_us_map=1-1:100000" "bus_checks=1" "clock_stretching=1" \
			 "check_bus_free=1"; do
			reload "bus_mode=i2c" "$Q" ""
			N=$((N + 1))
		done
		reload "bus_mode=spi" "" "gpio_base=100000"
		reload "bus_mode=spi" "" "gpio_offset_compat=1"
		reload "spi_cs=3,4,5" "" ""
		N=$((N + 3))
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_after.log 2>&1
		cat /tmp/spi_after.log
		check "$N parameter reloads, then SPI works" 'grep -q "All SPI tests PASSED" /tmp/spi_after.log'
		;;
	stress-failslab)
		# Probe with one slab allocation failure each (debug kernels)
		FS=/sys/kernel/debug/failslab
		# No failure inside vhci-hcd: vhci_tx_urb() drops the URB but
		# enqueue returns 0, so the URB is never sent nor given back
		# and usb_kill_urb() waits for ever (a USB/IP bug)
		VHCI_MOD=$(awk '$1 == "vhci_hcd" { print $6, $2 }' /proc/modules)
		if [ ! -d $FS ]; then
			skip "failslab not available in this kernel"
		elif [ ! -f $FS/reject-start ] || [ -z "$VHCI_MOD" ]; then
			skip "failslab needs CONFIG_FAULT_INJECTION_STACKTRACE_FILTER to spare vhci-hcd"
		else
			set -- $VHCI_MOD
			echo $1 > $FS/reject-start
			echo $(( $1 + $2 )) > $FS/reject-end
			echo "failslab: no failure under vhci_hcd [$1, +$2)"
			echo 0 > $FS/verbose 2>/dev/null
			echo N > $FS/ignore-gfp-wait 2>/dev/null
			# /proc/self/fail-nth fails the N-th slab allocation
			# of the binding task (open, write, driver core, then
			# the probes), N sweeping deep into the probe
			i=0
			OK=0
			while [ $i -lt 60 ]; do
				N=$(( 1 + i * i * 3 / 2 ))
				echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/unbind 2>/dev/null
				sh -c "echo $N > /proc/self/fail-nth; echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/bind" 2>/dev/null && OK=$((OK + 1))
				i=$((i + 1))
			done
			echo "failslab: $OK/60 binds succeeded with allocation N failed, N = 1 .. $N"
			echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/unbind 2>/dev/null
			echo 1-1:1.0 > /sys/bus/usb/drivers/ftdi_mpsse/bind
			sleep 1
			SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
			/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_after.log 2>&1
			cat /tmp/spi_after.log
			check "SPI works after 60 fault-injected probes" 'grep -q "All SPI tests PASSED" /tmp/spi_after.log'
		fi
		;;
	i2c-map-chan)
		PA=$(basename "$(ls -d /sys/bus/usb/devices/1-1:1.0/ftdi-i2c.* 2>/dev/null | head -1)")
		PB=$(basename "$(ls -d /sys/bus/usb/devices/1-1:1.1/ftdi-i2c.* 2>/dev/null | head -1)")
		check "channel A: adapter is i2c-30" \
			'readlink -f /sys/bus/i2c/devices/i2c-30 | grep -q "/1-1:1.0/"'
		check "channel B: adapter is i2c-31" \
			'readlink -f /sys/bus/i2c/devices/i2c-31 | grep -q "/1-1:1.1/"'
		check "channel A: delay of the device entry (10 us)" \
			'dmesg | grep -q "$PA: inter-phase delay 10 us"'
		check "channel B: delay of its own entry (25 us)" \
			'dmesg | grep -q "$PB: inter-phase delay 25 us"'
		for NR in 30 31; do
			/usr/bin/i2c_test /dev/i2c-$NR > /tmp/i2c_test.log 2>&1
			cat /tmp/i2c_test.log
			check "I2C on i2c-$NR" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		done
		rmmod ftdi_i2c
		insmod /lib/modules/ftdi_i2c.ko i2c_bus_nr_map=1-1:40
		sleep 1
		check "device entry: one channel is i2c-40" '[ -e /sys/bus/i2c/devices/i2c-40 ]'
		check "device entry: the other channel is told to use its interface name" \
			'dmesg | grep -q "i2c_bus_nr_map: an entry for a USB device.*1-1:1.1:41"'
		;;
	i2c-ad7-gpio)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		PINMAP=$(dbgfs 0 pinmap)
		I2C_PDEV=$(basename "$(ls -d /sys/bus/usb/devices/1-1:1.0/ftdi-i2c.* 2>/dev/null | head -1)")
		check "AD7 wiring detected"      'dmesg | grep -q "AD7<->SCL wiring detected"'
		cat $PINMAP
		check "pinmap: AD7 is an I2C pin" 'grep -q "^  AD7  SCL_SENSE" $PINMAP'
		check "pinmap: AD7 is not offered as a GPIO line" 'grep -q "^  AD7  in use" $PINMAP'
		/usr/bin/gpio_test "$GPIO_DEV" 7 busy > /tmp/gpio_ad7.log 2>&1
		cat /tmp/gpio_ad7.log
		check "GPIO line 7 (AD7) refused"  'grep -q "PASS:" /tmp/gpio_ad7.log'
		/usr/bin/gpio_test "$GPIO_DEV" 6 > /tmp/gpio_ad6.log 2>&1
		check "GPIO line 6 (AD6) works"    'grep -q "All GPIO tests PASSED" /tmp/gpio_ad6.log'
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		check "I2C works after the refused request" 'grep -q "memory device data OK" /tmp/i2c_mem.log'

		# The other way round: AD7 is held as a GPIO when ftdi-i2c binds
		echo "$I2C_PDEV" > /sys/bus/platform/drivers/ftdi-i2c/unbind
		check "pinmap: AD7 available without the I2C driver" 'grep -q "^  AD7  available" $PINMAP'
		/usr/bin/gpio_test "$GPIO_DEV" 7 hold 30 > /tmp/gpio_hold.log 2>&1 &
		HOLD_PID=$!
		wait_for 5 'grep -q "HOLD:" /tmp/gpio_hold.log'
		echo "$I2C_PDEV" > /sys/bus/platform/drivers/ftdi-i2c/bind
		check "AD7 held as GPIO: not probed for SCL wiring" \
			'dmesg | grep -q "AD7 is in use as a GPIO line"'
		check "pinmap: AD7 left to the GPIO consumer" 'grep -q "^  AD7  available" $PINMAP'
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		/usr/bin/i2c_test "$I2C_DEV" mem > /tmp/i2c_mem.log 2>&1
		cat /tmp/i2c_mem.log
		check "I2C works without clock stretching" 'grep -q "memory device data OK" /tmp/i2c_mem.log'

		rmmod ftdi_i2c
		insmod /lib/modules/ftdi_i2c.ko clock_stretching=1
		sleep 1
		check "clock_stretching=1 refused while AD7 is a GPIO line" \
			'dmesg | grep -q "clock_stretching=1 needs AD7"'
		check "no I2C adapter then"      '! ls /dev/i2c-* >/dev/null 2>&1'

		kill $HOLD_PID 2>/dev/null
		wait $HOLD_PID 2>/dev/null
		rmmod ftdi_i2c
		insmod /lib/modules/ftdi_i2c.ko
		sleep 1
		check "AD7 detected again once the line is free" \
			'[ "$(dmesg | grep -c "AD7<->SCL wiring detected")" -eq 2 ]'
		/usr/bin/gpio_test "$GPIO_DEV" 7 busy > /tmp/gpio_ad7.log 2>&1
		check "GPIO line 7 refused again" 'grep -q "PASS:" /tmp/gpio_ad7.log'
		;;
	i2c-map)
		check "i2c_bus_nr_map: adapter is i2c-42" '[ -e /sys/bus/i2c/devices/i2c-42 ]'
		check "i2c_delay_us_map: 20 us delay"     'dmesg | grep -q "inter-phase delay 20 us"'
		/usr/bin/i2c_test /dev/i2c-42 > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C on the numbered adapter"       'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
	i2c-ad7)
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		check "AD7 wiring detected"      'dmesg | grep -q "AD7<->SCL wiring detected"'
		/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
		cat /tmp/i2c_test.log
		check "I2C with adaptive clocking" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
		;;
esac

# The emulator checks that the driver uses the chip as documented
if kill -0 $EMU_PID 2>/dev/null; then
	kill -USR2 $EMU_PID 2>/dev/null
	sleep 1
fi
check "emu: no MPSSE bad command"            '! grep -q "MPSSE bad command" /tmp/emu.log'
check "emu: no MPSSE on a non-MPSSE channel" '! grep -q "has no MPSSE" /tmp/emu.log'
check "emu: no access to missing pins"       '! grep -q "no high-byte pins" /tmp/emu.log'
check "emu: no SPI read without chip select" '! grep -q "no chip select asserted" /tmp/emu.log'
check "emu: no write overrunning the RX FIFO" '! grep -q "a real chip stalls" /tmp/emu.log'
# Requests the chip refuses or the emulator cannot model, a typo in an
# emu.ctl command, data a buffer had to drop, URBs of an earlier import
check "emu: every request taken" \
	'! grep -qE "STALLed|unhandled control|not emulated|bit-bang not available|wait-on-I/O|usbip: unknown|ctl: unknown" /tmp/emu.log'
check "emu: no data dropped"                '! grep -qE "dropp(ed|ing)" /tmp/emu.log'
check "emu: no URB before SET_CONFIGURATION" '! grep -q "before SET_CONFIGURATION" /tmp/emu.log'
# The I2C bus model measures START, STOP and the clock against UM10204;
# its slaves and the SPI ones report what they cannot make sense of
if [ -z "$EXPECT_I2C_TIMING" ]; then
	check "emu: I2C timing within the specification" '! grep -q "I2C timing:" /tmp/emu.log'
fi
check "emu: no I2C protocol error"           '! grep -q "I2C protocol:" /tmp/emu.log'
check "emu: no SPI protocol error"           '! grep -q "SPI protocol:" /tmp/emu.log'
if [ -z "$EXPECT_RTCK_HANG" ]; then
	check "emu: MPSSE never left waiting for RTCK" '! grep -q "the engine waits for ever" /tmp/emu.log'
fi

# kmemleak (debug kernels only)
KML=/sys/kernel/debug/kmemleak
if [ -f $KML ]; then
	sleep 6
	echo scan > $KML
	sleep 1
	echo scan > $KML
	FTDI_LEAKS=$(awk '/unreferenced object/ { obj++ }
		/ftdi/ { if (obj != last) { n++; last = obj } }
		END { print n + 0 }' $KML)
	echo "kmemleak: $(grep -c 'unreferenced object' $KML) unreferenced object(s), $FTDI_LEAKS from ftdi"
	[ "$FTDI_LEAKS" -gt 0 ] && cat $KML
	check "kmemleak: no leak from ftdi modules" '[ "$FTDI_LEAKS" -eq 0 ]'
fi

# Splats of known upstream (non-ftdi) bugs are listed, not counted
dmesg | awk -f /known_splats.awk > /tmp/dmesg.chk
if grep -q '^KNOWN UPSTREAM ISSUE:' /tmp/dmesg.chk; then
	echo "NOTE: kernel splat(s) from known upstream bugs (tests/known_splats.awk):"
	grep '^KNOWN UPSTREAM ISSUE:' /tmp/dmesg.chk | sort | uniq -c
fi
check "no kernel warnings"      '! grep -q "WARNING:" /tmp/dmesg.chk'
check "no kernel oops"          '! grep -q "Oops:" /tmp/dmesg.chk'
check "no KASAN/UBSAN/lockdep/BUG report" \
	'! grep -qE "KASAN|UBSAN|BUG:|possible circular|possible recursive|inconsistent lock state|sleeping function called" /tmp/dmesg.chk'

echo "== $FTDI_MODE mode: $PASS/$TOTAL passed, $FAIL failed =="

echo "--- emulator log ---"
cat /tmp/emu.log

dmesg

exec poweroff -f
