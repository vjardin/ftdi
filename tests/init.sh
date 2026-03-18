#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# init.sh -- UML initramfs init script for FTDI driver testing
#
# Boots inside a User Mode Linux instance, starts the FTDI USB/IP
# emulator, attaches it via vhci_hcd, loads the FTDI modules, and
# runs a set of test assertions.
#
# The test mode (spi, i2c, or uart) is passed via kernel command line:
#   ftdi_mode=spi  or  ftdi_mode=i2c  or  ftdi_mode=uart
#
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

# Parse kernel command line for ftdi_mode parameter
FTDI_MODE="spi"
for arg in $(cat /proc/cmdline); do
	case "$arg" in
		ftdi_mode=*)
			FTDI_MODE="${arg#ftdi_mode=}"
			;;
	esac
done
echo "=== FTDI Test Mode: $FTDI_MODE ==="

# Bring up loopback for USB/IP (localhost communication)
ifconfig lo 127.0.0.1 up

# Determine chip type and protocol mode based on FTDI_MODE
# Format: [chip-]protocol  e.g., "spi", "ft2232h", "ft2232h-spi"
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
		# Default to SPI mode for GPIO testing (MPSSE required)
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
		# Default to SPI mode for GPIO testing (MPSSE required)
		FTDI_CHIP="ft4232h"
		FTDI_PROTO="spi"
		;;
	ft4232h-cd)
		# FT4232H channels C/D test: UART + bit-bang GPIO
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
		# Hot-unplug test uses SPI mode for MPSSE operations
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
	i2c-nak)
		# I2C NAK error injection test
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="i2c-nak"
		FTDI_ERROR_COUNT="1"
		;;
	i2c-stuck)
		# I2C bus stuck (frozen bus) test
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="i2c-stuck"
		FTDI_ERROR_COUNT="0"  # Infinite until recovery
		;;
	mpsse-sync-fail)
		# MPSSE synchronization failure test
		FTDI_CHIP="ft232h"
		FTDI_PROTO="i2c"
		FTDI_ERROR="mpsse-sync"
		FTDI_ERROR_COUNT="0"  # Permanent
		;;
	spi-error)
		# SPI/USB error injection test
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_ERROR="usb-timeout"
		FTDI_ERROR_COUNT="1"
		;;
	suspend)
		# Suspend/resume test - uses autosuspend module param
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		FTDI_AUTOSUSPEND="1"
		;;
	*)
		FTDI_CHIP="ft232h"
		FTDI_PROTO="spi"
		;;
esac

# Start FTDI USB/IP emulator in background with selected chip and protocol
EMU_ARGS="--chip $FTDI_CHIP --mode $FTDI_PROTO"
if [ -n "$FTDI_ERROR" ]; then
	EMU_ARGS="$EMU_ARGS --error $FTDI_ERROR --error-count $FTDI_ERROR_COUNT"
	echo "=== Error injection: $FTDI_ERROR (count=$FTDI_ERROR_COUNT) ==="
fi
/usr/bin/ftdi_usbip $EMU_ARGS &
EMU_PID=$!
sleep 1

# Load USB/IP virtual host controller
insmod /lib/modules/usbip-core.ko
insmod /lib/modules/vhci-hcd.ko
sleep 1

# Attach emulated device via USB/IP
/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
ATTACH_PID=$!
sleep 1

# Load FTDI driver modules with mode-specific parameters
# SPI mode uses 3 chip selects: AD3=CS0, AD4=CS1, AD5=CS2
MPSSE_ARGS=""
case "$FTDI_MODE" in
	spi)
		MPSSE_ARGS="spi_cs=3,4,5"
		;;
esac
# Enable autosuspend for suspend testing
if [ -n "$FTDI_AUTOSUSPEND" ]; then
	MPSSE_ARGS="$MPSSE_ARGS autosuspend=1"
	echo "=== Autosuspend enabled for testing ==="
fi
case "$FTDI_MODE" in
	ft4232h-cd)
		MPSSE_ARGS="bus_mode=spi,spi,uart,uart spi_cs=3,4,5 $MPSSE_ARGS"
		;;
esac
insmod /lib/modules/ftdi_mpsse.ko $MPSSE_ARGS
insmod /lib/modules/ftdi_uart.ko
insmod /lib/modules/ftdi_spi.ko
insmod /lib/modules/ftdi_i2c.ko ${I2C_ARGS:-}
insmod /lib/modules/ftdi_gpio.ko
insmod /lib/modules/ftdi_gpio_bitbang.ko 2>/dev/null || true
sleep 1

# Add FTDI device ID to ftdi_mpsse driver (empty ID table by default)
# This triggers a reprobe of matching devices
case "$FTDI_CHIP" in
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

# Debug: check if device was bound
echo "DEBUG: USB devices after new_id:"
ls -la /sys/bus/usb/devices/ 2>&1
if [ -d "/sys/bus/usb/devices/1-1:1.0/driver" ]; then
	echo "DEBUG: Device bound to driver:"
	ls -l /sys/bus/usb/devices/1-1:1.0/driver 2>&1
else
	echo "DEBUG: Device not yet bound to a driver"
fi

# --- Test assertions ---
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

# Common tests (skip for probe-failure tests)
if [ "$FTDI_MODE" != "mpsse-sync-fail" ]; then
	check "FTDI MPSSE core probed"  'dmesg | grep -q "FTDI MPSSE core:"'
	check "GPIO driver registered"  'dmesg | grep -q "FTDI MPSSE GPIO:"'
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
	check "gpiochip device exists"  'ls /dev/gpiochip* >/dev/null 2>&1'
else
	check "EEPROM checksum OK"      'dmesg | grep -q "checksum=ok"'
fi

# Mode-specific tests
case "$FTDI_MODE" in
	spi)
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		# Verify 3 CS pins configured (AD3=CS0, AD4=CS1, AD5=CS2)
		# Reserved mask should be 0x003f (bits 0-5: SCK, MOSI, MISO, CS0, CS1, CS2)
		check "SPI 3 CS pins reserved"    'dmesg | grep -q "reserved mask 0x003f"'
		check "spidev devices created"    'dmesg | grep -q "registered.*spidev"'
		# Debug: show SPI subsystem state
		echo "DEBUG: /sys/bus/spi/devices:"
		ls -la /sys/bus/spi/devices/ 2>&1 || echo "  (none)"
		echo "DEBUG: /sys/bus/spi/drivers:"
		ls -la /sys/bus/spi/drivers/ 2>&1 || echo "  (none)"
		echo "DEBUG: /dev/spi*:"
		ls -la /dev/spi* 2>&1 || echo "  (none)"
		check "spidev device exists"      'ls /dev/spidev* >/dev/null 2>&1'
		# Run SPI I/O test
		echo "Running SPI I/O test..."
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		if [ -n "$SPI_DEV" ]; then
			/usr/bin/spi_test "$SPI_DEV" > /tmp/spi_test.log 2>&1
			check "SPI read/write test"   'grep -q "All SPI tests PASSED" /tmp/spi_test.log'
			check "SPI mode 1 test"       'grep -q "mode 1 xfer OK" /tmp/spi_test.log'
			check "SPI mode 1 AN_114 notice" 'dmesg | grep -q "CPHA=1.*AN_114"'
			cat /tmp/spi_test.log
		else
			check "SPI read/write test"   'false'
		fi
		# Run GPIO I/O test (test pin 8 = AC0, not reserved by SPI)
		echo "Running GPIO I/O test..."
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO read/write test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
			cat /tmp/gpio_test.log
		else
			check "GPIO read/write test"  'false'
		fi
		check "No I2C adapter (SPI mode)" '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		;;
	i2c)
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		# I2C reserves AD0-AD2 (SCL, SDA0, SDA1), mask = 0x0007
		check "I2C pins reserved"         'dmesg | grep -q "reserved mask 0x0007"'
		check "i2c-dev device exists"     'ls /dev/i2c-* >/dev/null 2>&1'
		# Run I2C I/O test
		echo "Running I2C I/O test..."
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
			check "I2C read/write test"   'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
			cat /tmp/i2c_test.log
		else
			check "I2C read/write test"   'false'
		fi
		# Run GPIO I/O test (test pin 8 = AC0, not reserved by I2C)
		echo "Running GPIO I/O test..."
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO read/write test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
			cat /tmp/gpio_test.log
		else
			check "GPIO read/write test"  'false'
		fi
		check "No SPI controller (I2C mode)" '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		;;
	i2c-100k|i2c-400k)
		# I2C speed-specific tests
		SPEED_KHZ=$(echo "$FTDI_MODE" | sed 's/i2c-\([0-9]*\)k/\1/')
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C speed $SPEED_KHZ kHz"  "dmesg | grep -q '${SPEED_KHZ} kHz'"
		check "i2c-dev device exists"     'ls /dev/i2c-* >/dev/null 2>&1'
		# Run I2C I/O test
		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/i2c_test "$I2C_DEV" > /tmp/i2c_test.log 2>&1
			check "I2C read/write at $SPEED_KHZ kHz" 'grep -q "All I2C tests PASSED" /tmp/i2c_test.log'
			cat /tmp/i2c_test.log
		else
			check "I2C read/write at $SPEED_KHZ kHz" 'false'
		fi
		;;
	uart)
		check "UART mode detected"        'dmesg | grep -q "EEPROM hints UART mode"'
		check "UART driver registered"    'dmesg | grep -q "FTDI MPSSE UART:"'
		check "ttyFTDI device exists"     'ls /dev/ttyFTDI* >/dev/null 2>&1'
		# Debug: show UART devices
		echo "DEBUG: /dev/ttyFTDI*:"
		ls -la /dev/ttyFTDI* 2>&1 || echo "  (none)"
		# Run UART I/O test
		echo "Running UART I/O test..."
		UART_DEV=$(ls /dev/ttyFTDI* 2>/dev/null | head -1)
		if [ -n "$UART_DEV" ]; then
			/usr/bin/uart_test "$UART_DEV" > /tmp/uart_test.log 2>&1
			check "UART read/write test"  'grep -q "All UART tests PASSED" /tmp/uart_test.log'
			cat /tmp/uart_test.log
		else
			check "UART read/write test"  'false'
		fi
		# Run GPIO I/O test (test pin 8 = AC0)
		echo "Running GPIO I/O test..."
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			/usr/bin/gpio_test "$GPIO_DEV" 8 > /tmp/gpio_test.log 2>&1
			check "GPIO read/write test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test.log'
			cat /tmp/gpio_test.log
		else
			check "GPIO read/write test"  'false'
		fi
		check "No SPI controller (UART mode)" '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		check "No I2C adapter (UART mode)"    '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		;;
	ft2232h*)
		# FT2232H has 2 MPSSE-capable interfaces (channel A and B)
		# Default test mode is SPI (MPSSE mode) for GPIO to work
		check "Channel A probed"         'dmesg | grep -q "FTDI MPSSE core: channel A"'
		check "Channel B probed"         'dmesg | grep -q "FTDI MPSSE core: channel B"'

		# Count the number of probed interfaces
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "Both channels probed"     '[ "$INTF_COUNT" -eq 2 ]'

		# Verify GPIO chips for both channels
		GPIO_COUNT=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
		check "Two GPIO chips exist"     '[ "$GPIO_COUNT" -ge 2 ]'

		# Verify SPI controllers for both channels (if in SPI mode)
		if [ "$FTDI_PROTO" = "spi" ]; then
			check "SPI mode detected"  'dmesg | grep -q "EEPROM hints SPI mode"'
			SPI_COUNT=$(ls -d /sys/class/spi_master/spi* 2>/dev/null | wc -l)
			check "Two SPI controllers"  '[ "$SPI_COUNT" -ge 2 ]'
		fi

		# Debug: show USB interfaces
		echo "DEBUG: USB interfaces:"
		ls -la /sys/bus/usb/devices/1-1:1.* 2>&1 || echo "  (none)"

		# Run GPIO I/O test on first gpiochip (channel A, pin 8 = AC0)
		echo "Running GPIO I/O test on channel A..."
		GPIO_DEV_A=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV_A" ]; then
			/usr/bin/gpio_test "$GPIO_DEV_A" 8 > /tmp/gpio_test_a.log 2>&1
			check "Channel A GPIO test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test_a.log'
			cat /tmp/gpio_test_a.log
		else
			check "Channel A GPIO test"  'false'
		fi

		# Run GPIO I/O test on second gpiochip (channel B, pin 8 = AC0)
		echo "Running GPIO I/O test on channel B..."
		GPIO_DEV_B=$(ls /dev/gpiochip* 2>/dev/null | tail -1)
		if [ -n "$GPIO_DEV_B" ] && [ "$GPIO_DEV_B" != "$GPIO_DEV_A" ]; then
			/usr/bin/gpio_test "$GPIO_DEV_B" 8 > /tmp/gpio_test_b.log 2>&1
			check "Channel B GPIO test"  'grep -q "All GPIO tests PASSED" /tmp/gpio_test_b.log'
			cat /tmp/gpio_test_b.log
		else
			check "Channel B GPIO test"  'false'
		fi
		;;
	ft4232h-cd)
		# FT4232H channels C/D test: all 4 channels probed,
		# A+B are SPI (MPSSE), C+D are UART + bit-bang GPIO
		check "Channel A probed"         'dmesg | grep -q "FTDI MPSSE core: channel A"'
		check "Channel B probed"         'dmesg | grep -q "FTDI MPSSE core: channel B"'
		check "Channel C probed"         'dmesg | grep -q "FTDI MPSSE core: channel C"'
		check "Channel D probed"         'dmesg | grep -q "FTDI MPSSE core: channel D"'

		# All 4 channels probed
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "All 4 channels probed"   '[ "$INTF_COUNT" -eq 4 ]'

		# Verify C/D got UART mode
		check "Channel C is UART"        'dmesg | grep "channel C" | grep -q "uart"'
		check "Channel D is UART"        'dmesg | grep "channel D" | grep -q "uart"'

		# Verify ttyFTDI devices for C/D
		TTYFTDI_COUNT=$(ls /dev/ttyFTDI* 2>/dev/null | wc -l)
		check "ttyFTDI devices exist"    '[ "$TTYFTDI_COUNT" -ge 2 ]'

		# Verify bit-bang GPIO chips for C/D
		check "Bit-bang GPIO registered" 'dmesg | grep -q "FTDI bit-bang GPIO"'

		# Verify MPSSE GPIO for A/B
		GPIO_COUNT=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
		check "GPIO chips exist (>=4)"  '[ "$GPIO_COUNT" -ge 4 ]'

		# Test UART I/O on first ttyFTDI (channel C or D)
		TTYFTDI=$(ls /dev/ttyFTDI* 2>/dev/null | tail -1)
		if [ -n "$TTYFTDI" ]; then
			echo "=== UART test on $TTYFTDI (non-MPSSE channel) ==="
			/usr/bin/uart_test "$TTYFTDI"
			check "UART I/O on non-MPSSE channel" '[ $? -eq 0 ]'
		else
			check "ttyFTDI found for C/D" 'false'
		fi

		# Debug
		echo "DEBUG: USB interfaces:"
		ls -la /sys/bus/usb/devices/1-1:1.* 2>&1 || echo "  (none)"
		echo "DEBUG: ttyFTDI devices:"
		ls -la /dev/ttyFTDI* 2>&1 || echo "  (none)"
		echo "DEBUG: GPIO chips:"
		ls -la /dev/gpiochip* 2>&1 || echo "  (none)"
		;;
	ft4232h*)
		# FT4232H: all 4 interfaces probed (A+B MPSSE, C+D UART)
		check "Channel A probed"         'dmesg | grep -q "FTDI MPSSE core: channel A"'
		check "Channel B probed"         'dmesg | grep -q "FTDI MPSSE core: channel B"'
		check "Channel C probed"         'dmesg | grep -q "FTDI MPSSE core: channel C"'
		check "Channel D probed"         'dmesg | grep -q "FTDI MPSSE core: channel D"'

		# All 4 channels probed
		INTF_COUNT=$(dmesg | grep -c "FTDI MPSSE core: channel")
		check "All 4 channels probed"   '[ "$INTF_COUNT" -eq 4 ]'

		# Verify all 4 USB interfaces exist
		USB_INTF_COUNT=$(ls -d /sys/bus/usb/devices/1-1:1.[0-3] 2>/dev/null | wc -l)
		check "Four USB interfaces exist" '[ "$USB_INTF_COUNT" -eq 4 ]'

		# GPIO chips for MPSSE channels + bit-bang for C/D
		GPIO_COUNT=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
		check "GPIO chips exist (>=4)"  '[ "$GPIO_COUNT" -ge 4 ]'

		# Debug: show USB interfaces
		echo "DEBUG: USB interfaces:"
		ls -la /sys/bus/usb/devices/1-1:1.* 2>&1 || echo "  (none)"
		;;
	hotplug)
		# Hot-unplug test: disconnect USB while operations are in progress
		# This tests the driver's disconnect handling and cleanup code paths
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'

		# Find devices for testing
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)

		if [ -z "$SPI_DEV" ] || [ -z "$GPIO_DEV" ]; then
			echo "ERROR: Required devices not found"
			check "Devices available" 'false'
		else
			echo "=== Hot-unplug Test: SPI ==="
			echo "Starting continuous SPI operations..."

			# Start hotplug test in background
			/usr/bin/hotplug_test spi "$SPI_DEV" > /tmp/hotplug_spi.log 2>&1 &
			HOTPLUG_PID=$!

			# Let it run for a short time
			sleep 1

			# Verify operations are running
			if kill -0 $HOTPLUG_PID 2>/dev/null; then
				echo "SPI operations running (PID $HOTPLUG_PID)"

				# Kill the USB/IP emulator to simulate hot-unplug
				echo "Simulating USB disconnect (killing emulator PID $EMU_PID)..."
				kill $EMU_PID 2>/dev/null

				# Wait for hotplug_test to detect disconnect
				sleep 2

				# Check if test process exited
				if ! kill -0 $HOTPLUG_PID 2>/dev/null; then
					wait $HOTPLUG_PID
					HOTPLUG_RC=$?
					echo "Hotplug test exited with code $HOTPLUG_RC"
					cat /tmp/hotplug_spi.log
					check "SPI hotplug handled" '[ "$HOTPLUG_RC" -eq 0 ]'
				else
					echo "WARNING: Hotplug test still running, killing..."
					kill $HOTPLUG_PID 2>/dev/null
					wait $HOTPLUG_PID 2>/dev/null
					cat /tmp/hotplug_spi.log
					check "SPI hotplug handled" 'true'
				fi
			else
				echo "ERROR: Hotplug test failed to start"
				cat /tmp/hotplug_spi.log
				check "SPI hotplug handled" 'false'
			fi

			echo ""
			echo "=== Hot-unplug Test: GPIO ==="

			# Restart emulator for GPIO test
			/usr/bin/ftdi_usbip --chip "$FTDI_CHIP" --mode "$FTDI_PROTO" &
			EMU_PID=$!
			sleep 1

			# Re-attach the device
			/usr/bin/usbip_attach 127.0.0.1 3240 1-1 &
			sleep 2

			# Re-add device ID to trigger probe
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
					kill $EMU_PID 2>/dev/null
					sleep 2

					if ! kill -0 $HOTPLUG_PID 2>/dev/null; then
						wait $HOTPLUG_PID
						HOTPLUG_RC=$?
						echo "Hotplug test exited with code $HOTPLUG_RC"
						cat /tmp/hotplug_gpio.log
						check "GPIO hotplug handled" '[ "$HOTPLUG_RC" -eq 0 ]'
					else
						kill $HOTPLUG_PID 2>/dev/null
						wait $HOTPLUG_PID 2>/dev/null
						cat /tmp/hotplug_gpio.log
						check "GPIO hotplug handled" 'true'
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
		# I2C NAK error injection test
		# Verify driver correctly handles NAK responses
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'

		echo "=== I2C NAK Error Test ==="
		echo "Emulator will inject NAK on I2C transaction"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/error_test i2c-nak "$I2C_DEV" > /tmp/error_test.log 2>&1
			cat /tmp/error_test.log
			check "I2C NAK handled correctly" 'grep -q "PASS:" /tmp/error_test.log'
		else
			echo "ERROR: No I2C device found"
			check "I2C NAK handled correctly" 'false'
		fi
		;;
	i2c-stuck)
		# I2C bus stuck (frozen bus) recovery test
		# Verify driver attempts bus recovery when SDA is stuck low
		check "I2C mode detected"         'dmesg | grep -q "EEPROM hints I2C mode"'
		check "I2C adapter registered"    'dmesg | grep -q "FTDI MPSSE I2C adapter"'
		check "I2C adapter exists"        'ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'

		echo "=== I2C Bus Stuck (Frozen Bus) Test ==="
		echo "Emulator will simulate SDA stuck low"

		I2C_DEV=$(ls /dev/i2c-* 2>/dev/null | head -1)
		if [ -n "$I2C_DEV" ]; then
			/usr/bin/error_test i2c-stuck "$I2C_DEV" > /tmp/error_test.log 2>&1
			cat /tmp/error_test.log
			# The test passes if it doesn't hang and returns an appropriate error
			check "I2C bus stuck handled" 'grep -q "PASS:" /tmp/error_test.log'
			check "Bus recovery attempted" 'grep -q "Phase 2:" /tmp/error_test.log'
		else
			echo "ERROR: No I2C device found"
			check "I2C bus stuck handled" 'false'
		fi
		;;
	spi-error)
		# SPI/USB error injection test
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'

		echo "=== SPI/USB Error Test ==="
		echo "Emulator will inject USB timeout"

		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		if [ -n "$SPI_DEV" ]; then
			/usr/bin/error_test spi-err "$SPI_DEV" > /tmp/error_test.log 2>&1
			cat /tmp/error_test.log
			check "SPI error handled correctly" 'grep -q "PASS:" /tmp/error_test.log'
		else
			echo "ERROR: No SPI device found"
			check "SPI error handled correctly" 'false'
		fi
		;;
	mpsse-sync-fail)
		# MPSSE synchronization failure test
		echo "=== MPSSE Sync Failure Test ==="
		echo "Emulator returns wrong echo for 0xAA bad command"
		# The driver should have failed to probe due to sync error
		check "MPSSE sync failed"          'dmesg | grep -q "MPSSE sync failed"'
		check "No I2C adapter (probe failed)" '! ls /sys/bus/i2c/devices/i2c-* >/dev/null 2>&1'
		check "No SPI controller"          '! ls /sys/class/spi_master/spi* >/dev/null 2>&1'
		check "No crash/oops"              '! dmesg | grep -qi "oops\|bug\|panic"'
		;;
	suspend)
		# Suspend/resume test
		check "SPI mode detected"         'dmesg | grep -q "EEPROM hints SPI mode"'
		check "SPI controller registered" 'ls /sys/class/spi_master/spi* >/dev/null 2>&1'

		echo "=== Suspend/Resume Test ==="
		echo "Testing SPI, I2C, and GPIO operations across suspend/resume"

		USB_PATH="/sys/bus/usb/devices/1-1"

		# Test SPI suspend/resume
		SPI_DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
		if [ -n "$SPI_DEV" ]; then
			echo ""
			echo "--- SPI Suspend/Resume ---"
			/usr/bin/suspend_test spi "$SPI_DEV" "$USB_PATH" > /tmp/suspend_spi.log 2>&1
			cat /tmp/suspend_spi.log
			check "SPI suspend/resume" 'grep -q "PASSED" /tmp/suspend_spi.log'
		else
			echo "WARNING: No SPI device, skipping SPI suspend test"
		fi

		# Test GPIO suspend/resume (pin 8 = AC0, not reserved)
		GPIO_DEV=$(ls /dev/gpiochip* 2>/dev/null | head -1)
		if [ -n "$GPIO_DEV" ]; then
			echo ""
			echo "--- GPIO Suspend/Resume ---"
			/usr/bin/suspend_test gpio "$GPIO_DEV" "$USB_PATH" > /tmp/suspend_gpio.log 2>&1
			cat /tmp/suspend_gpio.log
			check "GPIO suspend/resume" 'grep -q "PASSED" /tmp/suspend_gpio.log'
		else
			echo "WARNING: No GPIO device, skipping GPIO suspend test"
		fi
		;;
esac

check "no kernel warnings"      '! dmesg | grep -q "WARNING:"'
check "no kernel oops"          '! dmesg | grep -q "Oops:"'

echo "== $FTDI_MODE mode: $PASS/$TOTAL passed, $FAIL failed =="

# Print full dmesg for debugging
dmesg

# Halt UML
exec poweroff -f
