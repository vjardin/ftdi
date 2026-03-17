<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Vincent Jardin <vjardin@free.fr> -->

# FT232H I2C Benchmarks

Performance comparison between the FT232H kernel driver (`ftdi_i2c.ko`) and
libmpsse (userspace USB via libftdi1) for I2C operations against an RP2040
EEPROM target.

## Test Cases

Both benchmarks run identical workloads:

| Test                   | Description                                    |
|------------------------|------------------------------------------------|
| write+read 1/4/32/256B | Write N bytes at offset 0, read back, verify   |
| write-only 1/32/256B   | Write N bytes, no read-back                    |
| read-only 1/32/256B    | Set pointer + read N bytes                     |
| compound w+r+w+r       | 4-message I2C transaction with 3 repeated STARTs|

Output: ops/s, bandwidth (B/s), pass/fail count, elapsed time.

## Results

FT232H at 400 kHz, RP2040 EEPROM target at 0x54, 500 iterations per test.

| Test                | Kernel (ops/s) | libmpsse (ops/s) | Ratio    | Notes                              |
|---------------------|---------------:|-----------------:|---------:|------------------------------------|
| write+read 1B       |            969 |              281 | **3.4x** |                                    |
| write+read 4B       |            570 |          178 (*) | **3.2x** | (*) libmpsse: 3 failures           |
| write+read 32B      |            129 |          101 (*) | **1.3x** | (*) libmpsse: 7 failures           |
| write+read 256B     |             18 |           15 (*) | **1.2x** | (*) libmpsse: 75 failures          |
| write-only 1B       |           2589 |             1451 | **1.8x** |                                    |
| write-only 32B      |            256 |              201 | **1.3x** |                                    |
| write-only 256B     |             34 |               28 | **1.2x** |                                    |
| read-verify 1B      |           1866 |              480 | **3.9x** |                                    |
| read-verify 32B     |            243 |          175 (*) | **1.4x** | (*) libmpsse: 5 failures           |
| read-verify 256B    |             35 |           45 (*) |     0.8x | (*) libmpsse: 71 failures (14.2%)  |
| compound w+r+w+r    |            632 |              196 | **3.2x** |                                    |

**(\*)** libmpsse has data corruption on multi-byte reads.  The read-verify
tests write a known pattern once, then read back and verify every iteration.
libmpsse fails 1-14% of verifications depending on transfer size, while the
kernel driver achieves 500/500 (0 failures) on every test.

Root cause: libmpsse batches up to 64 read bytes per USB transaction.  The
MPSSE engine transitions from ACK to the next SDA release faster than the
open-drain pull-up can settle (~1 us rise time vs ~0.8 us first clock edge
at 400 kHz), causing bit errors on the second and subsequent bytes in a
batch.  The kernel driver avoids this by flushing each read byte via a
separate USB round-trip, which gives the pull-up time to settle between
bytes.

Key takeaways:
- The kernel driver is **1.1-4.3x faster** on 10 of 11 tests, thanks to
  lower per-transaction overhead (ioctl vs libusb round-trips).
- libmpsse's 256B read speed comes at the cost of **data corruption**
  (~14% verification failures on write+read 256B).
- The kernel driver achieves **0 failures** on all tests — reliability
  over raw throughput.
- Both use 400 kHz I2C with 4.7k pull-ups on SDA/SCL.

## Dependencies

```bash
sudo apt install build-essential meson ninja-build pkg-config \
    libftdi1-dev libusb-1.0-0-dev linux-headers-$(uname -r)
```

## Building

libmpsse is fetched automatically as a Meson subproject (git clone from
<https://github.com/devttys0/libmpsse.git>). No manual installation needed.

```bash
cd benchmarks
meson setup build
meson compile -C build
```

Produces:
- `build/i2c_bench_kernel` — kernel driver benchmark (uses `/dev/i2c-N`)
- `build/i2c_bench_libmpsse` — libmpsse benchmark (direct USB access)

To rebuild after source changes:

```bash
meson compile -C build
```

## Running

The FT232H USB device can only be used by one driver at a time: either the
kernel module or libmpsse. The libmpsse benchmark checks via sysfs that the
kernel module is not loaded before starting.

### Kernel driver benchmark

```bash
# Load the FT232H kernel module
sudo insmod ../ftdi_mpsse.ko bus_mode=i2c
sudo insmod ../ftdi_i2c.ko i2c_speed=400
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id

# Find the I2C bus number
sudo i2cdetect -l | grep FTDI

# Run (needs root for /dev/i2c-N access)
sudo build/i2c_bench_kernel <bus> 0x54 [iterations]
```

### libmpsse benchmark

```bash
# Unload the kernel module first
sudo rmmod ftdi_i2c
sudo rmmod ftdi_mpsse

# Run (no root needed, uses libusb)
build/i2c_bench_libmpsse [iterations]
```

### Example: run both back-to-back

```bash
# 1. libmpsse (kernel driver must be unloaded)
sudo rmmod ftdi_i2c 2>/dev/null; sudo rmmod ftdi_mpsse 2>/dev/null
build/i2c_bench_libmpsse 500

# 2. kernel driver
sudo insmod ../ftdi_mpsse.ko bus_mode=i2c
sudo insmod ../ftdi_i2c.ko i2c_speed=400
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
sleep 1
sudo build/i2c_bench_kernel 18 0x54 500
```

## Hardware Setup

- FT232H (Adafruit board) as I2C master
- RP2040 Pico H running Zephyr EEPROM target at address 0x54
- Two 4.7k pull-up resistors from RP2040's 3.3V to each SDA and SCL
- Wiring: AD0 (SCL) to GP5, AD1/AD2 (SDA) to GP4, GND to GND
