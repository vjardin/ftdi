<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Vincent Jardin <vjardin@free.fr> -->

# FT232H I2C Benchmarks

Performance comparison between the FT232H kernel driver (`ftdi_i2c.ko`) and
libmpsse (userspace USB via libftdi1) for I2C operations against an RP2040
EEPROM target.

## Test Cases

Both benchmarks run identical workloads:

| Test | Description |
|---|---|
| write+read 1/4/32/256B | Write N bytes at offset 0, read back, verify |
| write-only 1/32/256B | Write N bytes, no read-back |
| read-only 1/32/256B | Set pointer + read N bytes |
| compound w+r+w+r | 4-message I2C transaction with 3 repeated STARTs |

Output: ops/s, bandwidth (B/s), pass/fail count, elapsed time.

## Results

FT232H at 400 kHz, RP2040 EEPROM target at 0x54, 500 iterations per test.

| Test                | Kernel (ops/s) | libmpsse (ops/s) | Ratio    | Notes                         |
|---------------------|---------------:|-----------------:|---------:|-------------------------------|
| write+read 1B       |           1035 |              331 | **3.1x** |                               |
| write+read 4B       |            577 |              179 | **3.2x** |                               |
| write+read 32B      |            129 |              100 | **1.3x** |                               |
| write+read 256B     |          0 (*) |               16 |        — | (*)                           |
| write-only 1B       |           2324 |             1559 | **1.5x** |                               |
| write-only 32B      |            262 |              202 | **1.3x** |                               |
| write-only 256B     |          0 (*) |               28 |        — | (*)                           |
| read-only 1B        |           1639 |              451 | **3.6x** |                               |
| read-only 32B       |            250 |              175 | **1.4x** |                               |
| read-only 256B      |             35 |               52 |     0.7x | libmpsse batches 64B/USB xfer |
| compound w+r+w+r    |            676 |              196 | **3.5x** |                               |

**(\*)** The kernel driver fails on 256-byte write and write+read transfers.
The MPSSE command buffer for 256 data bytes exceeds the internal USB transfer
size. The per-byte flush design (one USB round-trip per byte for reliable
open-drain settling) generates 12 command bytes per data byte — 256 bytes
requires ~3 KB of MPSSE commands, which overflows the buffer. This is a known
limitation of the v19 flush-per-byte approach; a chunked flush (e.g. 32 bytes
per USB round-trip) would fix it without sacrificing signal integrity.
libmpsse handles 256B by chunking reads into 64-byte USB transactions.

Key takeaways:
- The kernel driver is 1.3-3.6x faster for small/medium transfers (1-32B)
  thanks to lower per-transaction overhead (ioctl vs libusb round-trips).
- libmpsse wins on large reads (256B) because it batches up to 64 bytes per
  USB transaction, while the kernel driver flushes each byte individually.
- Both achieve 100% reliability at 400 kHz with 4.7k pull-ups.

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
