<!-- SPDX-License-Identifier: GPL-2.0 -->
# FTDI MPSSE Kernel Driver

[![Build & Test](https://github.com/vjardin/ftdi/actions/workflows/test.yml/badge.svg)](https://github.com/vjardin/ftdi/actions/workflows/test.yml)
[![Static Analysis](https://github.com/vjardin/ftdi/actions/workflows/check.yml/badge.svg)](https://github.com/vjardin/ftdi/actions/workflows/check.yml)

Out-of-tree Linux kernel driver suite for FTDI Hi-Speed USB chips
(FT232H, FT2232H, FT4232H and variants), built as a USB interface
driver with child platform devices for UART, SPI, I2C, and GPIO.

In the I2C benchmark of [`benchmarks/`](benchmarks/) (one FT232H at
400 kHz, an RP2040 target), the kernel driver is 1.1-4.2x faster than
libmpsse on 10 of the 11 tests and verifies every byte it reads back;
libmpsse is faster on 256-byte reads, of which it corrupts 14 %.

## Architecture

```
                          USB
                           |
                    ftdi_mpsse.ko          USB probe, EEPROM decode, mode setup,
                     (usb_driver)          MPSSE + bulk I/O API, module param bus_mode
                           |
          +-------+--------+--------+-------+
          |       |        |        |       |
     ftdi-uart  ftdi-spi ftdi-i2c ftdi-gpio ftdi-gpio-bitbang
    (platform) (platform)(platform)(platform)  (platform)
          |       |        |        |            |
  /dev/ttyFTDIx  spi_ctrl i2c_adap gpio_chip  gpio_chip
                 spidevX.Y i2c-N   (MPSSE)    (bit-bang)

One channel = one USB interface; per channel, by mode:
  spi      -> ftdi-spi  + ftdi-gpio   (MPSSE; GPIO on the free AD/AC pins)
  i2c      -> ftdi-i2c  + ftdi-gpio   (MPSSE)
  uart     -> ftdi-uart               (any channel; default on FT4232H C/D)
  bitbang  -> ftdi-gpio-bitbang       (async bit-bang, 8 pins, any channel)

FT4232H C/D have no MPSSE: uart or bitbang only. ftdi-gpio has 16 lines
(AD0-7, AC0-7), 8 on FT4232H A/B (no high byte).
```

The core module (`ftdi_mpsse.ko`) owns the USB interface. On probe it:

 - Detects the chip type from `bcdDevice`
 - Determines MPSSE capability per channel (FT4232H C/D lack MPSSE hardware)
 - Reads and decodes the EEPROM next to the chip (93C46/56/66, up to 256
   bytes)
 - Selects the channel mode (see [Mode Selection](#mode-selection) below);
   non-MPSSE channels run as UART unless `bus_mode` asks for bit-bang
 - Creates child platform devices via `mfd_add_hotplug_devices()`
 - Exposes EEPROM data via sysfs and debugfs (see
   [EEPROM in sysfs and debugfs](#eeprom-in-sysfs-and-debugfs) below)

MPSSE children communicate through exported transport functions
(`ftdi_mpsse_xfer`, `ftdi_mpsse_write`, etc.), serialised by a mutex in
the core. The bit-bang GPIO child uses the same bulk transport plus
vendor control messages (`ftdi_mpsse_cfg_msg`, `ftdi_mpsse_read_pins`);
the UART runs its own URBs on the endpoints it gets from
`ftdi_mpsse_get_endpoints()`.

## Module Structure

| Module                 | Source Files                         | Description                                                 |
| ---------------------- | ------------------------------------ | ----------------------------------------------------------- |
| `ftdi_mpsse.ko`        | `ftdi_mpsse_core.c`, `ftdi_eeprom.c` | USB probe, EEPROM decode, MPSSE transport, mode setup       |
| `ftdi_uart.ko`         | `ftdi_uart.c`                        | `serial_core` `uart_port`, `/dev/ttyFTDIx`                  |
| `ftdi_spi.ko`          | `ftdi_spi.c`                         | `spi_controller` with MPSSE clocking commands               |
| `ftdi_i2c.ko`          | `ftdi_i2c.c`                         | `i2c_adapter` with 3-phase I2C protocol                     |
| `ftdi_gpio.ko`         | `ftdi_gpio.c`                        | `gpio_chip` on MPSSE AD/AC pins (16 pins, 8 on FT4232H)     |
| `ftdi_gpio_bitbang.ko` | `ftdi_gpio_bitbang.c`                | `gpio_chip` via async bit-bang (8 pins, `bus_mode=bitbang`) |

Shared definitions live in `ftdi_mpsse.h` (platform data, transport API,
MPSSE opcodes). Internal EEPROM definitions live in `ftdi_eeprom.h`
(constants, `struct ftdi_eeprom`, decode functions).

## Supported Hardware

| Chip     | Channels | MPSSE | C/D Modes             | PID      |
| -------- | -------- | ----- | --------------------- | -------- |
| FT232H   | 1        | A     | --                    | `0x6014` |
| FT2232H  | 2        | A, B  | --                    | `0x6010` |
| FT4232H  | 4        | A, B  | UART or bit-bang GPIO | `0x6011` |
| FT4232HA | 4        | A, B  | UART or bit-bang GPIO | `0x6048` |
| FT232HP  | 1        | A     | --                    | `0x6045` |
| FT233HP  | 1        | A     | --                    | `0x6044` |
| FT2232HP | 2        | A, B  | --                    | `0x6042` |
| FT2233HP | 2        | A, B  | --                    | `0x6040` |
| FT4232HP | 4        | A, B  | UART or bit-bang GPIO | `0x6043` |
| FT4233HP | 4        | A, B  | UART or bit-bang GPIO | `0x6041` |

All chips share the FTDI vendor ID `0x0403`. The -HP/-HA chips are
taken as their base chip for the EEPROM layout, but without the FT232H
open-drain outputs (opcode 0x9E) and CBUS GPIO, which have not been
checked against their datasheets: on FT232HP/FT233HP, I2C releases its
lines as on FT2232H, without clock stretching.

FT4232H channels A and B have full MPSSE capability (SPI, I2C, UART,
GPIO). Channels C and D lack the MPSSE engine -- they are either a UART
(native async serial) or 8 GPIOs in async bit-bang mode, not both at
once. This is a silicon-level constraint.

## Building

```sh
make                                  # build against running kernel
make KDIR=/path/to/kernel/build       # cross-compile or target different headers
sudo make modules_install && sudo depmod -a   # for modprobe below
make tools                            # build userspace tools
make clean
```

Standard kbuild variables (`CROSS_COMPILE`, `ARCH`, etc.) work. Without
`modules_install`, load the modules with `insmod` from the build
directory, `ftdi_mpsse.ko` first.

## Usage

### Loading

The USB ID table of `ftdi_mpsse.ko` is empty on purpose, so that it
coexists with `ftdi_sio` (as `vfio-pci` does with PCI drivers). Devices
are handed over with `new_id`: the USB bus has no `driver_override`.

#### Method 1: `new_id` (runtime, no reboot)

```sh
sudo modprobe ftdi_mpsse bus_mode=spi
# Bind every interface with this VID:PID that no driver owns yet
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
```

#### Method 2: take an interface from `ftdi_sio`

`new_id` leaves interfaces that `ftdi_sio` already claimed alone. Move
them one at a time; `bind` only accepts a driver that matches, so the
`new_id` above is still needed:

```sh
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind
```

#### Method 3: udev rule (persistent, automatic)

```sh
# /etc/udev/rules.d/99-ftdi-mpsse.rules
# new_id keeps every ID written to it, duplicates too: add it once. The
# other steps may fail harmlessly: interface not owned by ftdi_sio, or
# already bound to ftdi_mpsse by new_id.
ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_interface", \
  ENV{PRODUCT}=="403/6014/*", \
  RUN+="/bin/sh -c 'exec 2>/dev/null; modprobe ftdi_mpsse; \
    grep -qx 0403.6014 /sys/bus/usb/drivers/ftdi_mpsse/new_id || \
    echo 0403 6014 > /sys/bus/usb/drivers/ftdi_mpsse/new_id; \
    echo %k > /sys/bus/usb/drivers/ftdi_sio/unbind; \
    echo %k > /sys/bus/usb/drivers/ftdi_mpsse/bind'"
```

#### Method 4: EEPROM custom PID (permanent)

Use `ftdi_eeprom` to program a custom PID that `ftdi_sio` does not know,
then add that PID with `new_id`.

The child drivers are loaded by udev through their `platform:ftdi-*`
module aliases when the core creates their devices. By hand:
```sh
sudo modprobe ftdi_spi           # SPI channels
sudo modprobe ftdi_i2c           # I2C channels
sudo modprobe ftdi_gpio          # MPSSE GPIO next to SPI / I2C
sudo modprobe ftdi_uart          # UART channels
sudo modprobe ftdi_gpio_bitbang  # bus_mode=bitbang channels
```

### Mode Selection

The driver decides which child devices to create from three sources, in
order of priority:

1. `bus_mode` module parameter -- always wins if set to a known value
2. `bus_mode=` tag in the EEPROM user area -- the same setting, stored
   in the device
3. The rest of the EEPROM (product string, VCP driver bit, drive
   settings) -- consulted when neither is present (auto)

#### Priority 1 - `bus_mode` module parameter (static override)

Set at module load time. Applies identically regardless of what the
EEPROM is set:

```sh
sudo modprobe ftdi_mpsse bus_mode=spi      # SPI + MPSSE GPIO
sudo modprobe ftdi_mpsse bus_mode=i2c      # I2C + MPSSE GPIO
sudo modprobe ftdi_mpsse bus_mode=uart     # UART only
sudo modprobe ftdi_mpsse bus_mode=bitbang  # 8 bit-bang GPIOs, no UART
sudo modprobe ftdi_mpsse                   # auto (see below)
```

Comma-separated values select per-interface modes on multi-channel
chips (FT2232 and FT4232, not FT232); a missing entry means auto:

```sh
# FT2232H: channel A = SPI, channel B = UART
sudo modprobe ftdi_mpsse bus_mode=spi,uart

# FT4232H: A = SPI, B = I2C, C = UART, D = bit-bang GPIO
sudo modprobe ftdi_mpsse bus_mode=spi,i2c,uart,bitbang
```

FT4232H accepts up to 4 comma-separated values. Channels C and D have
no MPSSE: they accept `uart` and `bitbang`; any other value logs a
warning and the channel runs as a UART.

| `bus_mode=`        | Child devices       | Channel mode        | Use case                          |
| ------------------ | ------------------- | ------------------- | --------------------------------- |
| `spi`              | ftdi-spi, ftdi-gpio | MPSSE               | SPI master + GPIO                 |
| `i2c`              | ftdi-i2c, ftdi-gpio | MPSSE               | I2C adapter + GPIO                |
| `uart`             | ftdi-uart           | Reset (UART native) | Serial port                       |
| `bitbang` (`gpio`) | ftdi-gpio-bitbang   | Async bit-bang      | 8 GPIOs on xDBUS0-7, any channel  |
| *(empty)*          | auto -- see below   | depends             | EEPROM-driven or SPI fallback     |

A UART channel gets no GPIO child: in UART mode the chip sends whatever
arrives on bulk OUT as serial data, so MPSSE GPIO commands cannot be
used there. Bit-bang mode replaces the UART of its channel ("UART or
bit-bang", DS_FT4232H), hence the bit-bang child is alone on it.

#### Auto mode (EEPROM-driven)

When `bus_mode` is empty for an interface (the default), the driver
reads the EEPROM. Nothing is taken from an image that is blank,
unreadable or has a bad checksum: such a channel comes up as SPI (a
UART on FT4232H C/D, which have no MPSSE).

For a valid EEPROM, the first match decides:

| Priority | Source                        | Result                                                       |
| -------- | ----------------------------- | ------------------------------------------------------------ |
| 1        | `bus_mode=` tag (user area)   | The mode it names, per channel (see below)                   |
| 2        | Channel type FIFO/OPTO/CPU/FT1284 | Probe refused (`-ENODEV`): the channel is not wired for serial or MPSSE use |
| 3        | Product string                | Contains "SPI" -> SPI; contains "I2C" -> I2C                 |
| 4        | VCP driver bit of the channel | Set -> UART (the channel is meant to be a serial port)       |
| 5        | Drive settings of the data bus | Slow slew + Schmitt trigger -> I2C (open-drain settings)    |
| 6        | Default                       | SPI (most common MPSSE use case)                             |

The log tells which one applied, for example
`auto: EEPROM hints I2C mode (product string)`, and the last line of
the probe ends with `(auto)`.

Notes:

- The EEPROM channel type does not select MPSSE: SPI and I2C are
  entered at run time from a channel of type UART, the type of every
  UART or MPSSE design. Only the other hardware modes are refused.
- The VCP bit is set by default by `ftdi_eeprom` (`cha_vcp=true`) and
  FT_PROG ("Virtual COM Port"), so an EEPROM that says nothing else
  gives a serial port, as with `ftdi_sio`. Clear it ("D2XX Direct",
  `cha_vcp=false`) for a channel used through the MPSSE.
- FT4232H has no channel type field: its bytes 0x00/0x01 only hold the
  four VCP bits. Channels C and D have no MPSSE and are UARTs unless
  `bus_mode` (parameter or tag) asks for `bitbang`.
- The drive settings are those of the channel's own data bus: ADBUS on
  FT232H (`group1` of `ftdi_eeprom`), AL / BL on FT2232H (`group0`,
  `group2`), A / B on FT4232H (`group0`, `group1`).

#### EEPROM `bus_mode=` tag (user area)

The FTDI EEPROM has no native field for SPI vs I2C -- both use the MPSSE
engine. To state it in the device itself, store the ASCII text
`bus_mode=` followed by a value in the unused part of the EEPROM. The
value has the syntax of the module parameter:

| Tag                    | Meaning                                              |
| ---------------------- | ---------------------------------------------------- |
| `bus_mode=i2c`         | Every channel is I2C                                 |
| `bus_mode=spi,uart`    | Channel A SPI, channel B UART                        |
| `bus_mode=i2c,,uart,bitbang` | A I2C, B auto, C UART, D bit-bang GPIO (FT4232H) |

The value ends at the first byte that is not an ASCII lower-case letter
(`a`-`z`), a digit or a comma: a NUL, or the `0xFF` of unused EEPROM
cells. An unknown value, or a tag without a value, is reported and
ignored.

The driver finds the tag by its text, wherever it is: the unused area
depends on the chip and on the EEPROM size, so there is no fixed
address. (Earlier versions read a one-byte hint at offset `0x1A`; that
byte is the ACBUS4/ACBUS5 function on FT232H and is no longer read.)
`eeprom_bus_mode` in sysfs shows what the driver found.

##### Programming the tag with ftdi_eeprom

1. Create a file with the tag:
   ```sh
   printf 'bus_mode=i2c\0' > bus_mode.bin
   ```

2. Add it to your `ftdi_eeprom` configuration as user data. With a
   93C56 or 93C66, libftdi leaves `0x19`-`0x99` unused on
   FT2232H/FT4232H and `0x1f`-`0x9f` on FT232H; `0x20` is free on all:
   ```ini
   # ft232h.conf
   vendor_id=0x0403
   product_id=0x6014
   manufacturer="ACME"
   product="Sensor bridge"
   cha_vcp=false             # MPSSE channel, not a COM port

   # bus_mode tag in the user area
   user_data_addr=0x20
   user_data_file="bus_mode.bin"

   # ADBUS (group1 on FT232H) with slow slew and Schmitt trigger: what
   # auto mode takes for I2C when there is no tag (see below)
   group1_drive=8MA
   group1_schmitt=true
   group1_slew=slow

   filename=ft232h.bin
   ```

3. Flash the EEPROM, give the interface back to `ftdi_mpsse` and check
   what the driver reads. `ftdi_eeprom` takes interface A from whatever
   driver has it (libftdi's default, `AUTO_DETACH_SIO_MODULE`, detaches
   `ftdi_mpsse` too), then resets the device: usbcore binds each
   interface to the first registered driver that matches it, usually
   `ftdi_sio`. Move it as in Method 2:
   ```sh
   sudo ftdi_eeprom --flash-eeprom ft232h.conf
   echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
   echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind
   cat /sys/bus/usb/devices/1-2:1.0/eeprom_bus_mode    # i2c
   ```

A 93C46 (128 bytes) has no room before the strings; use the product
string there.

##### Alternatively, use the product string

If you don't want to use the user area, include "SPI" or "I2C" in the
product string. It is checked before the VCP bit, so it works with the
default `cha_vcp=true` of `ftdi_eeprom` (`tools/ft232h-i2c.conf` is such
a configuration):

```ini
product="My SPI Adapter"    # Driver detects "SPI" -> SPI mode
product="I2C Bridge v2"     # Driver detects "I2C" -> I2C mode
```

##### Electrical settings heuristic

If there is no tag and no product string keyword, and the VCP bit is
clear, the driver examines the drive settings of the channel's data bus:

- Slow slew + Schmitt trigger -> likely a I2C mode (open-drain optimized)
- Anything else -> SPI

This heuristic reflects best practices: I2C benefits from slow edges
and noise immunity, while SPI benefits from fast edges for higher
clock rates.

#### Tuning the EEPROM to influence auto mode

| To get        | Program                                                                 |
| ------------- | ----------------------------------------------------------------------- |
| UART          | Nothing: VCP driver bit set (`cha_vcp=true`, the default)               |
| SPI           | `bus_mode=spi` tag, or "SPI" in the product string, or just `cha_vcp=false` |
| I2C           | `bus_mode=i2c` tag, or "I2C" in the product string, or `cha_vcp=false` with slow slew + Schmitt on the data bus |
| Bit-bang GPIO | `bus_mode=bitbang` tag                                                  |

For predictable behaviour across different EEPROM states, always set
`bus_mode` explicitly, as a parameter or as the EEPROM tag.

#### Multi-Channel Configuration

On multi-channel chips, each channel is configured independently:

| Chip    | Channels | MPSSE-Capable | Configuration                                     |
| ------- | -------- | ------------- | ------------------------------------------------- |
| FT232H  | 1        | A             | One mode: SPI, I2C, UART or bit-bang              |
| FT2232H | 2        | A, B          | Per-channel: e.g., A=SPI, B=I2C                   |
| FT4232H | 4        | A, B          | A/B: SPI/I2C/UART/bit-bang; C/D: UART or bit-bang |

SPI and I2C are mutually exclusive per channel because they use different
MPSSE clocking modes (SPI uses edge-triggered, I2C uses 3-phase clocking).
However, different channels on the same chip can use different protocols.

Example: FT2232H with channel A for SPI and channel B for I2C:

```sh
sudo modprobe ftdi_mpsse bus_mode=spi,i2c
```

Example: FT4232H with A=SPI, B=I2C, C/D=UART:

```sh
sudo modprobe ftdi_mpsse bus_mode=spi,i2c,uart,uart
```

The EEPROM `bus_mode=` tag takes the same per-interface list
(`bus_mode=spi,i2c`). Channels C and D of an FT4232H are UARTs unless
`bus_mode`, parameter or tag, asks for `bitbang`.

### UART Capabilities

The UART child driver (`ftdi_uart.ko`) provides a full-featured
serial port on `/dev/ttyFTDIx`:

- Baud rates: 300 to 12 Mbaud (Hi-Speed divisor for >= 1200, BM divisor
  for < 1200); above 6 Mbaud the chip only has 8 and 12 Mbaud (AN_120
  4.4): another rate there gives the nearest of 6, 8 and 12 Mbaud, and
  termios reports it
- Data bits: 7, 8 ("7/8 bit data", DS_FT232H); `CS5` and `CS6` keep the
  previous size, or give 8
- Parity: None, Odd, Even, Mark, Space (`CMSPAR`)
- Stop bits: 1, 2
- Flow control, done by the chip: RTS/CTS (`CRTSCTS`), DTR/DSR (sysfs
  `dtr_dsr_flow`), XON/XOFF (`IXON`, with `VSTART`/`VSTOP`). When the
  tty's input buffer fills, the driver stops reading the chip: its RX
  buffer fills in turn and it drops RTS or DTR by itself ("will drop RTS
  if it cannot receive any more", AN232B-04); with `IXOFF` serial_core
  sends the XOFF. Without flow control the chip's buffer overflows
  (`oe:` in `/proc/tty/driver/ftdi_uart`); bytes the tty buffer could not
  take count as `bo:`
- `CREAD` off: received data is dropped
- Modem signals: DTR, RTS output; CTS, DSR, RI, DCD input. The changes
  are sent in order, and data written after a change waits until the
  chip has it (an RTS-driven transceiver, a reset pulse on DTR)
- RS-485: the chip drives the transmit enable (TXDEN) by itself while it
  sends. `TIOCSRS485` is accepted when the channel has a TXDEN pin, which
  the driver names at probe (`RS-485: TXDEN on ACBUS4`) and in `pinmap`:
  - FT232H: the ACBUS pin given the `TXDEN` function in the EEPROM
    (ACBUS0-6, 8 or 9);
  - FT2232H: ACBUS0 / BCBUS0, always there on a UART channel;
  - FT4232H: xDBUS7 of the channels for which the EEPROM selects TXDEN
    instead of RI# (`cha_rs485` ... `chd_rs485` for `ftdi_eeprom`).
  
  Without such a pin the ioctl fails with `EOPNOTSUPP`.
- CBUS GPIO: FT232H exposes ACBUS5, 6, 8 and 9 ("ACBUS Bit Bang",
  DS_FT232H table 3.5) as a `gpio_chip` (`ftdi-cbus-<interface>`); a
  pin is a line if the EEPROM gives it the `IOMODE` function. The first
  request enters CBUS bit-bang mode, every pin an input; unbinding the
  UART driver makes every pin an input again and leaves the mode (leaving
  it alone does not release the pins)
- Double-buffered writes: 2 write URBs (4 KiB each) for pipelined TX
- Draining: `close()`, `tcdrain()`, `tcsendbreak()` and `TCSADRAIN` wait
  for the data still in the write URBs and in the chip's TX buffer (the
  reported FIFO size covers both): up to twice the time it takes at the
  current baud rate, or without limit with `CRTSCTS` (serial_core).
  While reading is stopped (input buffer full) the chip sends no line
  status: the driver then counts its TX buffer as sent once the time it
  takes at the line rate has passed
- A halted bulk endpoint is cleared and its transfers restarted
- A break still on at close is ended

sysfs attributes (under the platform device):

| Attribute       | Type   | Default | Description                                                         |
| --------------- | ------ | ------- | ------------------------------------------------------------------- |
| `latency_timer` | 1-255  | 16      | RX buffer timeout in ms                                             |
| `dtr_dsr_flow`  | 0 or 1 | 0       | DTR/DSR flow control when `CRTSCTS` is off; an open port takes it at once |

### EEPROM in sysfs and debugfs

The core module exposes the EEPROM on the USB interface device (e.g.
`/sys/bus/usb/devices/1-2:1.0/`), one value per sysfs attribute, and
its multi-line reports in debugfs, in
`/sys/kernel/debug/ftdi_mpsse/<interface>/` (e.g. `.../1-2:1.0/`).
Everything is created at probe time and is read-only.

Each USB interface exposes the EEPROM attributes for its own channel.
The EEPROM is physically one per chip (256 bytes shared by all channels),
but since each interface probes independently, the attributes are
per-interface:

- `eeprom_channel_type` on interface 0 shows channel A's type
- `eeprom_channel_type` on interface 1 shows channel B's type (FT2232H)
- FT4232H has no channel type in its EEPROM: every interface shows `uart`

Chip-wide fields (`eeprom_vid`, `eeprom_pid`, the raw `eeprom` binary,
etc.) are identical on all interfaces -- they read the same physical
EEPROM.

#### Text attributes

| Attribute              | Format     | Example                                    | Description                                                            |
| ---------------------- | ---------- | ------------------------------------------ | ---------------------------------------------------------------------- |
| `eeprom_valid`         | `%d\n`     | `1`                                        | 1 if EEPROM was read successfully                                      |
| `eeprom_empty`         | `%d\n`     | `0`                                        | 1 if it was read and all bytes are `0xFF` (blank EEPROM)               |
| `eeprom_checksum`      | `%s\n`     | `ok`                                       | `ok`, `bad`, or `n/a` (empty/invalid)                                  |
| `eeprom_vid`           | `0x%04x\n` | `0x0403`                                   | Vendor ID from EEPROM                                                  |
| `eeprom_pid`           | `0x%04x\n` | `0x6014`                                   | Product ID from EEPROM                                                 |
| `eeprom_channel_type`  | `%s\n`     | `uart`                                     | This interface's channel type: `uart`, `fifo`, `opto`, `cpu`, `ft1284` |
| `eeprom_self_powered`  | `%d\n`     | `0`                                        | 1 if self-powered flag set                                             |
| `eeprom_remote_wakeup` | `%d\n`     | `0`                                        | 1 if remote wakeup flag set                                            |
| `eeprom_max_power`     | `%u\n`     | `500`                                      | Maximum power draw in mA                                               |
| `eeprom_bus_mode`      | `%s\n`     | `i2c`                                      | Value of the `bus_mode=` tag of the user area, `none`, or `n/a`        |

```sh
# Quick read of all EEPROM attributes
for f in /sys/bus/usb/devices/1-2:1.0/eeprom_*; do
    echo "$(basename $f): $(cat $f)"
done
```

#### debugfs files

| File           | Content                                                                 |
| -------------- | ----------------------------------------------------------------------- |
| `eeprom_cbus`  | Per-pin CBUS function, FT232H (`CBUS4: TXDEN`), as `ftdi_eeprom` names it |
| `eeprom_drive` | Drive strength, slew rate and Schmitt trigger of every group of the chip |
| `eeprom_config`| The decoded settings as an `ftdi_eeprom` configuration                  |
| `pinmap`       | Pins of the channel and who uses them (see [Pinmap](#pinmap-debugfs))   |

```sh
sudo mount -t debugfs none /sys/kernel/debug    # if not mounted
sudo cat /sys/kernel/debug/ftdi_mpsse/1-2:1.0/eeprom_drive
```

`eeprom_drive` lists the drive groups of the chip, numbered as in
`ftdi_eeprom` (`group0_drive` ...) and named after the pins they drive:

| Chip    | group0 | group1 | group2 | group3 |
| ------- | ------ | ------ | ------ | ------ |
| FT232H  | ACBUS  | ADBUS  | --     | --     |
| FT2232H | AL (ADBUS) | AH (ACBUS) | BL (BDBUS) | BH (BCBUS) |
| FT4232H | A      | B      | C      | D      |

```
$ cat /sys/kernel/debug/ftdi_mpsse/1-2:1.1/eeprom_drive      # FT2232H
group0 (AL): 4mA slew=fast schmitt=off
group1 (AH): 4mA slew=fast schmitt=off
group2 (BL): 8mA slew=slow schmitt=on
group3 (BH): 4mA slew=fast schmitt=off
```

Auto mode reads the group of the channel's own data bus (see
[Mode Selection](#mode-selection)).

#### Binary attribute

| Attribute | Size  | Description                                                           |
| --------- | ----- | --------------------------------------------------------------------- |
| `eeprom`  | 256 B | Raw EEPROM contents (little-endian, as read from hardware); root only |

The binary dump is compatible with `ftdi_eeprom`:

```sh
# Dump raw EEPROM to file
sudo cat /sys/bus/usb/devices/1-2:1.0/eeprom > eeprom_backup.bin
hexdump -C eeprom_backup.bin

# Compare with ftdi_eeprom decode; it takes interface A from ftdi_mpsse
# and leaves it without a driver: bind it again afterwards
ftdi_eeprom --read-eeprom ft232h.conf
diff <(xxd eeprom_backup.bin) <(xxd ft232h.bin)
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind
```

#### `eeprom_config`

`eeprom_config` prints the decoded settings with the key names and value
syntax of the `ftdi_eeprom` configuration file: strings quoted, drive
strengths as `4MA` ... `16MA`, CBUS functions as `ftdi_eeprom` spells
them (`DRIVE1`). Lines starting with `#` hold what `ftdi_eeprom` has no
key for: a CBUS function it does not know, and the `bus_mode=` tag of
the user area, which is written with `user_data_addr`/`user_data_file`.
An EEPROM that is missing, blank or has a bad checksum gives a single
`#` line.

```sh
sudo cat /sys/kernel/debug/ftdi_mpsse/1-2:1.0/eeprom_config
```

Example output (FT232H):
```ini
vendor_id=0x0403
product_id=0x6014
manufacturer="ACME"
product="Sensor bridge"
serial="FT1234"
self_powered=false
remote_wakeup=false
max_power=90
use_serial=true
suspend_pull_downs=false
cha_type=UART
cha_vcp=false
group0_drive=4MA
group0_schmitt=true
group0_slew=slow
group1_drive=8MA
group1_schmitt=true
group1_slew=slow
cbush0=TRISTATE
...
# user area tag: bus_mode=i2c
```

FT4232H has no channel type: interface 2 (channel C) shows `chc_vcp`
and `chc_rs485` instead of `cha_type`/`cha_vcp`, and four drive groups.

#### EEPROM settings the children warn about

One setting is reported at probe, with the effect the datasheets give
it: "Pull down I/O pins in USB suspend" (`suspend_pull_downs=true`,
DS_FT232H 7.2, DS_FT2232H and DS_FT4232H 7.1). While the chip sleeps it pulls
the I/O pins low:

| Child       | Message (`dmesg \| grep EEPROM:`)                                              |
| ----------- | ------------------------------------------------------------------------------ |
| `ftdi-spi`  | `I/O pins pulled down in USB suspend: every CS# is asserted while the chip sleeps` |
| `ftdi-i2c`  | `I/O pins pulled down in USB suspend: SCL and SDA are held low while the chip sleeps` |
| `ftdi-uart` | `I/O pins pulled down in USB suspend: TXD sends a break while the chip sleeps` |

#### CBUS function names in `eeprom_cbus`

| Name       | Value | Description (DS_FT232H table 3.5)                        |
| ---------- | ----- | -------------------------------------------------------- |
| `TRISTATE` | 0x00  | Input, pulled up (TriSt-PU, the default)                 |
| `TXLED`    | 0x01  | Pulses low during TX                                     |
| `RXLED`    | 0x02  | Pulses low during RX                                     |
| `TXRXLED`  | 0x03  | Pulses low during TX or RX                               |
| `PWREN`    | 0x04  | PWREN#: low once configured by USB, high in USB suspend  |
| `SLEEP`    | 0x05  | SLEEP#: low during USB suspend                           |
| `DRIVE_0`  | 0x06  | Output driven low                                        |
| `DRIVE1`   | 0x07  | Output driven high                                       |
| `IOMODE`   | 0x08  | GPIO (CBUS bit-bang, ACBUS5/6/8/9)                       |
| `TXDEN`    | 0x09  | RS-485 transmit enable                                   |
| `CLK30`    | 0x0A  | 30 MHz clock output                                      |
| `CLK15`    | 0x0B  | 15 MHz clock output                                      |
| `CLK7_5`   | 0x0C  | 7.5 MHz clock output                                     |

### Pinmap (debugfs)

The core module exposes a read-only `pinmap` file in the debugfs
directory of the USB interface (next to `eeprom_cbus`, `eeprom_drive`
and `eeprom_config`) that shows the kernel's active pin configuration
at a glance:

```sh
sudo cat /sys/kernel/debug/ftdi_mpsse/1-2:1.0/pinmap
```

The output includes the chip type, channel letter, active mode, bus-specific
pin assignments with registered subsystem device names, and per-pin GPIO
availability.

#### Example: `bus_mode=spi spi_cs=3,4`

```
chip: FT232H
channel: A
mode: spi

spi: spi2
  AD0  SCK
  AD1  MOSI
  AD2  MISO
  AD3  CS0
  AD4  CS1

gpio: gpiochip1
  AD0  reserved (spi2)
  AD1  reserved (spi2)
  AD2  reserved (spi2)
  AD3  reserved (spi2)
  AD4  reserved (spi2)
  AD5  available
  AD6  available
  AD7  available
  AC0  available
  ...
  AC7  available
```

#### Example: `bus_mode=i2c`

```
chip: FT232H
channel: A
mode: i2c

i2c: i2c-8
  AD0  SCL
  AD1  SDA_OUT
  AD2  SDA_IN

gpio: gpiochip1
  AD0  reserved (i2c-8)
  AD1  reserved (i2c-8)
  AD2  reserved (i2c-8)
  AD3  available
  AD4  available
  AD5  available
  AD6  available
  AD7  available
  AC0  available
  ...
  AC7  available
```

With I2C clock stretching (AD7 wired to SCL, see I2C Capabilities) AD7
belongs to the I2C adapter: the `i2c:` section gains
`  AD7  SCL_SENSE (clock stretching)` and the `gpio:` section shows
`  AD7  in use (i2c-8)` instead of `available`.

#### Example: `bus_mode=uart`

```
chip: FT232H
channel: A
mode: uart

uart: ttyFTDI0
  AD0  TXD
  AD1  RXD
  AD2  RTS
  AD3  CTS
  AD4  DTR
  AD5  DSR
  AD6  DCD
  AD7  RI

gpio: - (UART mode)
```

Pins are named after the channel, as in the datasheets: `BD0` for
BDBUS0 on channel B; FT4232H channels C/D in UART mode print `CDBUS0`
... and `mode: uart (non-MPSSE)`. A channel with an RS-485 transmit
enable shows it: `  ACBUS4  TXDEN` after the list on FT232H and FT2232H,
and `TXDEN` in place of `RI` on pin 7 on FT4232H. On an FT232H whose
EEPROM gives CBUS pins the I/O mode, the `gpio:` line names the CBUS chip
of the UART: `gpio: gpiochip2 (CBUS)`.

#### Example: FT4232H channel D (non-MPSSE, `bus_mode=spi,i2c,uart,bitbang`)

```
chip: FT4232H
channel: D
mode: bitbang (non-MPSSE)

gpio-bitbang: gpiochip3
  DDBUS0  available
  DDBUS1  available
  DDBUS2  available
  DDBUS3  available
  DDBUS4  available
  DDBUS5  available
  DDBUS6  available
  DDBUS7  available
```

### EEPROM Configuration

Some UART features (RS-485 TXDEN, CBUS GPIO) depend on CBUS pin
assignments stored in the device EEPROM. The EEPROM can be
programmed with `ftdi_eeprom`.

The FT232H has 10 ACBUS pins (ACBUS0-ACBUS9). Each pin is given one of
the functions listed in
[CBUS function names](#cbus-function-names-in-eeprom_cbus) in the
EEPROM.

The driver reads the full EEPROM at probe and:
- Refuses a channel whose type is FIFO, OPTO, CPU or FT1284, and takes
  the mode from the EEPROM in auto mode (see [Mode Selection](#mode-selection))
- Exposes decoded fields and the raw dump (see
  [EEPROM in sysfs and debugfs](#eeprom-in-sysfs-and-debugfs))
- Provides the CBUS functions to the UART child (ACBUS5, 6, 8 and 9 set
  to `IOMODE` become GPIO; any of ACBUS0-6, 8 and 9 set to `TXDEN`
  enables RS-485 support)

#### Programming with ftdi_eeprom

Create a configuration file (e.g. `ft232h.conf`):

```ini
vendor_id=0x0403
product_id=0x6014
manufacturer="FTDI"
product="FT232H"
serial="12345678"
max_power=500
self_powered=false

# CBUS pin assignments (FT232H uses cbush0-cbush9)
cbush0=TRISTATE
cbush1=TRISTATE
cbush2=TRISTATE
cbush3=TRISTATE
cbush4=TXDEN          # RS-485 transmit enable
cbush5=IOMODE         # GPIO pin 0
cbush6=IOMODE         # GPIO pin 1
cbush7=TRISTATE
cbush8=IOMODE         # GPIO pin 2
cbush9=IOMODE         # GPIO pin 3

# Channel configuration (TXDEN comes from cbush4 above)
cha_type=UART

filename=ft232h.bin
```

Flash the EEPROM (requires `ftdi_sio` to be unloaded):

```sh
sudo ftdi_eeprom --flash-eeprom ft232h.conf
```

Unplug and replug the device for the new configuration to take
effect.

### Tools

`tools/ftdi_eeprom_check` checks whether the EEPROM of connected FTDI
devices is empty (erased). It uses raw Linux USB devfs ioctls and
has no userspace library dependencies (no libusb, no libftdi).
Build with `make tools`.

```sh
sudo tools/ftdi_eeprom_check
```

### Verifying

```sh
# Child devices
ls /sys/bus/platform/devices/ftdi-*

# GPIO (labels ftdi-gpio-<interface>, ftdi-bitbang-<interface>,
# ftdi-cbus-<interface>)
gpiodetect | grep ftdi

# SPI
ls /sys/class/spi_master/

# I2C
i2cdetect -l
sudo i2cdetect -y <bus_number>

# UART
ls /dev/ttyFTDI*
```

### SPI Capabilities

The SPI child driver (`ftdi_spi.ko`) provides a `spi_controller` with:

- SPI modes 0 and 2, which the MPSSE provides (AN_114), and modes 1 and 3
  (CPHA = 1), built on top of them: once the chip select is asserted SCK
  is moved to its active level, the data is then shifted half a clock
  cycle ahead, and SCK returns to idle after the chip select is released.
  The scheme follows AN_108 (clock and data edges) and is verified
  against the emulator's model of it, not on a scope; a notice is logged
  the first time a device uses CPHA = 1.
- Clock speed: 30 MHz max, 458 Hz min (60 MHz base, 16-bit divisor)
- Chip select and delays as the SPI core defines them: `cs_change`,
  `cs_off`, the transfer `delay`, `cs_change_delay`, `word_delay` and the
  device's `cs_setup`, `cs_hold` and `cs_inactive`. A delay starts when
  the wire got there, not when the USB write completed: the driver first
  waits for the MPSSE to finish clocking (one USB round trip), then
  sleeps. A message is over (`spi_sync()` returns) when its data has
  left the pins.
- Full duplex, TX-only, and RX-only transfers
- Sub-byte transfers: 1-8 bits per word (MPSSE bit-mode clocking)
- SPI_CS_HIGH: per-device active-high chip select polarity
- Configurable chip selects: 1-5 CS lines on AD3-AD7, per-channel on multi-channel chips (see `spi_cs` below)
- Multi-transfer batching: `transfer_one_message` assembles the MPSSE
  commands of a message into one buffer; reads take one round trip per
  chunk of the chip's FIFO (one per word below 8 bits per word)
- Per-transfer speed: clock divisor updated only when speed changes
- LSB-first bit order (`SPI_LSB_FIRST`)
- Loopback (`SPI_LOOP`) for self-test

Module parameters (set at load time on `ftdi_mpsse.ko`):

| Parameter | Type   | Default | Description                                                                              |
| --------- | ------ | ------- | ---------------------------------------------------------------------------------------- |
| `spi_cs`  | string | `"3"`   | SPI CS AD pin numbers (3-7). Global or per-channel. Remaining AD pins available as GPIO. |

Global syntax applies to all SPI channels: `spi_cs=3,4,5` assigns
CS0=AD3, CS1=AD4, CS2=AD5; AD6-AD7 become GPIO pins.

Per-channel syntax for multi-channel chips (FT2232H, FT4232H):
`spi_cs='A:3,4,5;B:3'` gives channel A three CS lines and channel B one
(quoted for the shell, which would split at `;`). If a channel is not
listed, it defaults to CS0=AD3.

Userspace access through spidev: load `ftdi_spi` with `register_spidev=1`
for a `/dev/spidevX.Y` on every chip select, or, with
`CONFIG_SPI_DYNAMIC`, instantiate one on a chip select:
`echo spidev 0 | sudo tee /sys/class/spi_master/spi2/new_device`.

An invalid value (a pin outside 3-7, a pin given twice, more than five
pins, a malformed channel entry) is logged (`invalid spi_cs parameter`)
and the SPI channels fail to probe.

### I2C Capabilities

The I2C child driver (`ftdi_i2c.ko`) provides an `i2c_adapter` with:

- Clock speed: 10 kHz to 400 kHz (`i2c_speed`, or `i2c_speed_khz` in sysfs at runtime)
- Bus timing within the I2C-bus specification (UM10204): START, repeated
  START and STOP setup and hold times for the mode of the speed (standard
  up to 100 kHz, fast above), and the 4.0 µs SCL high time of standard
  mode. The MPSSE keeps SCL high for a third of a bit, so SCL runs at
  83 kHz at most in standard mode: `i2c_speed` 84 to 100 gives 83 kHz
  (logged at probe). The setup times of a repeated START and a STOP
  count from the end of the largest rise time allowed (tr, 1000 ns in
  standard mode, 300 ns in fast mode); the SCL high time of the data
  bits counts from the release of SCL, so a bus near that rise time gets
  less above VIH. `strict_timing=0` restores the short sequences of
  AN_113 and the clock at the requested rate.
- 7-bit addressing
- Open drain: FT232H has open-drain outputs (MPSSE opcode 0x9E). On
  FT2232H/FT4232H the driver releases SCL and SDA by making them inputs
  when they are not driven low ("set to input when not driving the
  lines", AN_255 2.4), but the MPSSE drives SCL, and the data bits it
  clocks out, push-pull: clock stretching and several masters need an
  FT232H
- One USB round trip per byte (the ACK/NAK bit is read back before the next byte)
- Clock stretching via MPSSE adaptive clocking, FT232H only: SCL must also
  be wired to AD7 (GPIOL3); detected at probe, per adapter (with SCL held
  low by a slave a wired AD7 reads low, like one that is held low
  otherwise: the wiring is then checked at the first transfer on a free
  bus, as after a detection that failed). AD7 is then a pin of the I2C
  adapter and no longer a GPIO line: a request for it fails with `EBUSY`.
  Conversely, while a GPIO consumer holds AD7, the wiring is not probed,
  and `clock_stretching=1` is refused. It then also covers a slave that
  holds SCL before a repeated START or a STOP: the driver reads SCL once
  it had time to rise, before the setup time (with `strict_timing=0`
  just before SDA moves), and redoes the condition once the slave has
  let go (up to the adapter timeout, 1 s).
- Proper repeated START between multi-message transfers
- Busy bus: the bus is read before the START; with SCL or SDA held low
  the transfer fails with `EBUSY`. Unless the previous transfer left the
  bus seen free, it is read on its own, before anything is clocked: a
  slave left in the middle of a byte would take a START and an address as
  data. After a transfer that left the bus free, the lines are sampled
  in the buffer of the START instead, which saves a USB round trip: a
  line pulled low since then still gives `EBUSY`, but the address byte
  (9 clocks) has gone out by the time the sample is read.
  `check_bus_free=1` reads the bus on its own before every transfer, at
  the price of that round trip; what is left is the time of one round
  trip between the read and the START. It is read again after the STOP:
  SDA still low (the ACK checks cannot tell a held SDA from an ACK), or
  SCL still held by a slave that stretched the clock while the MPSSE
  clocked on (no AD7 wire: the bits and the STOP went past it), fails
  the transfer with `EIO`; the driver then waits for SCL and sends the
  STOP again. A held SDA is followed by a bus recovery
- Bus recovery (`i2c_generic_scl_recovery`): up to 9 SCL pulses, SDA
  following SCL so that each one ends with a STOP
- Errors: address not acknowledged `ENXIO`, data byte not acknowledged
  `EIO`, USB timeout or SCL held past the adapter timeout `ETIMEDOUT`
- A read of zero bytes is refused (`EOPNOTSUPP`, `I2C_AQ_NO_ZERO_LEN_READ`):
  the addressed slave would drive SDA where the STOP goes
- SMBus emulation (`I2C_FUNC_SMBUS_EMUL`)
- At most 4096 bytes per message (`EOPNOTSUPP` above, the I2C core's
  adapter quirks)
- Counters in debugfs, `/sys/kernel/debug/i2c/i2c-N/i2c_stats`:
  `xfer_ok`, `xfer_fail`, `nack` (address), `data_nack`, `timeout` (USB),
  `scl_stretch` (SCL held low too long), `bus_recovery`, `bytes_tx`,
  `bytes_rx`; writing `0` resets them

Module parameters (set at load time):

| Parameter          | Type | Default | Description                                   |
| ------------------ | ---- | ------- | --------------------------------------------- |
| `i2c_speed`        | uint | 100     | I2C bus speed in kHz (10-400)                 |
| `clock_stretching` | bool | 0       | Force adaptive clocking on FT232H (SCL wired to AD7) |
| `strict_timing`    | bool | 1       | I2C-bus specification timing; 0 = AN_113 timing |
| `check_bus_free`   | bool | 0       | Read SCL and SDA on their own before every transfer (one more USB round trip): nothing clocked into a busy bus |
| `i2c_bus_nr_map`   | string | `""`  | Fixed adapter numbers by USB path             |
| `i2c_delay_us_map` | string | `""`  | Delay (0-10000 µs) before each repeated START and after the STOP, by USB path |

Both maps are lists of `usbpath:value`. The path is that of the USB
device (`3-2`, as in `dmesg`), which stands for every I2C channel of the
chip, or the name of the USB interface of one channel (`3-2:1.0` is
channel A, `3-2:1.1` channel B), which comes first. On a chip with two
I2C channels the bus numbers need the interface form, since a number can
only be given once:

```sh
insmod ftdi_i2c.ko i2c_bus_nr_map=3-2:1.0:40,3-2:1.1:41 i2c_delay_us_map=3-2:300
```

A value that is not a number in range (bus numbers 0-1048575, the
minors of i2c-dev; delays up to 10000 µs, below the 25 ms SMBus clock
low timeout since SCL stays low during the delay between two messages)
is logged with its key and ignored.

### GPIO Capabilities

#### MPSSE GPIO (`ftdi_gpio.ko`) -- MPSSE-capable channels

The MPSSE GPIO driver provides a `gpio_chip` next to the SPI or I2C
controller of channels with MPSSE hardware (FT232H A, FT2232H A/B,
FT4232H A/B):

- 16 lines AD0-AD7 (low byte) + AC0-AC7 (high byte); 8 lines (AD0-AD7)
  on FT4232H, whose MPSSE channels have no high byte
- Hardware state sync: pin values read from MPSSE at probe (no stale-zero writes or spurious IRQ edges)
- One pin state per channel: GPIO, SPI and I2C update a shared shadow
  in the core under `bus_lock`, so a GPIO write never clobbers a
  bus-driver pin and an SPI/I2C transaction keeps the GPIO outputs
- Open-drain on FT232H: per-pin `PIN_CONFIG_DRIVE_OPEN_DRAIN` / `PIN_CONFIG_DRIVE_PUSH_PULL` via `set_config`
  (MPSSE opcode `0x9E`, shared with the I2C pins)
- Reserved pin masking: the parent (`ftdi_mpsse.ko`) passes a per-mode `gpio_reserved_mask` via platform_data
  when creating the GPIO child -- `0x0007` for I2C (AD0-AD2) and a dynamic mask for SPI
  covering AD0-AD2 (SCK/MOSI/MISO) plus each CS pin (e.g. `0x000f` with CS0=AD3, `0x003f` with CS0-CS2=AD3-AD5).
  `init_valid_mask` hides reserved pins from the GPIO core so userspace cannot access bus-driver pins.
- Polled edge-triggered IRQ: 1 ms delayed_work polling, rising/falling/both
- Transport: MPSSE `SET_BITS_LOW`/`SET_BITS_HIGH` and `GET_BITS_LOW`/`GET_BITS_HIGH` opcodes

#### Bit-Bang GPIO (`ftdi_gpio_bitbang.ko`) -- `bus_mode=bitbang`

The bit-bang GPIO driver turns a whole channel into an 8-line
`gpio_chip`. It is the only GPIO option on FT4232H channels C and D
(no MPSSE), and works on any other channel too:

- 8 lines: xDBUS0-7 (DBUS0-7 on FT4232H C/D, AD0-7 elsewhere)
- Async bit-bang (AN2232C-02): `SET_BITMODE` (`0x01`) carries the
  direction mask, a byte written on bulk OUT drives the outputs, the
  `READ_PINS` vendor request returns the pin levels
- Replaces the UART of the channel: no `ttyFTDI` on a bit-bang channel
- Bit-bang mode is left (bitmode reset) when the driver unbinds. From
  suspend to resume an access fails (`ESHUTDOWN`), as transfers do while
  the device is suspended: the parent's resume leaves the channel in
  UART mode, where a byte written would go out on TXD. If entering
  bit-bang mode at resume fails, the next access enters it first; after
  a failed write whose rollback failed too, the next access sends the
  value and the directions again
- One USB round trip per read or write

### Pin Mapping

#### MPSSE channels (FT232H A, FT2232H A/B, FT4232H A/B)

```
Low byte (AD0-AD7):
  UART mode:  AD0=TXD  AD1=RXD      AD2=RTS     AD3=CTS  AD4=DTR  AD5=DSR  AD6=DCD  AD7=RI
  SPI mode:   AD0=SCK  AD1=MOSI     AD2=MISO    AD3=CS0  AD4-AD7=GPIO/CS1-CS4
  I2C mode:   AD0=SCL  AD1=SDA_OUT  AD2=SDA_IN  AD3-AD6=GPIO
              AD7=GPIO, or SCL sense with clock stretching (FT232H)

  Bit-bang:   AD0-AD7 = GPIO 0-7 (bus_mode=bitbang)

High byte (AC0-AC7, not on FT4232H):
  SPI / I2C:  AC0-AC7 available as GPIO (MPSSE SET_BITS_HIGH)
```

#### Non-MPSSE channels (FT4232H C/D)

```
DBUS0-DBUS7 (8 pins per channel, no high byte):
  UART:       DBUS0=TXD  DBUS1=RXD  DBUS2=RTS  DBUS3=CTS
              DBUS4=DTR  DBUS5=DSR  DBUS6=DCD  DBUS7=RI
  Bit-bang:   DBUS0-DBUS7 = GPIO 0-7 (bus_mode=bitbang, no UART)
```

Channels C and D have no AC-bus pins.

## Known Limitations

- GPIO writes that fail: the line keeps the direction and value it had
  (the driver's cache, and on MPSSE channels the pin shadow, are put
  back, and the old state is sent again in case the chip took the new
  one). If that second write fails too, the chip may hold the new state
  until the next write, or until a resume or an MPSSE recovery replays
  the state.
- Bit-bang GPIO bandwidth: each read or write is a separate USB round
  trip, unlike MPSSE GPIO which can batch pin operations.
- No runtime PM: the driver does not set `supports_autosuspend`, so
  usbcore keeps the device active while it is bound. The suspend/resume
  callbacks (re-entering MPSSE/UART and restoring the children's state)
  only run on system sleep. They are
  tested by calling them in the order of a system sleep, with the
  emulated chip losing its state in between (`make -C tests test-pm`),
  not by a real sleep: see `tests/README.md`.
- I2C speed limited to 400 kHz (fast mode). Higher modes are TODO:
  - **Fast-mode plus (Fm+, 1 MHz)**: requires pull-up resistors < 1 kΩ
    and higher bus drive current (20 mA). The MPSSE clock divisor
    supports it but the driver has not been validated at this speed.
  - **High-speed mode (HS, 3.4 MHz)**: requires a master code byte
    (00001xxx) sent in fast mode before switching to HS clock. The
    driver does not implement the HS-mode entry protocol.
- I2C 10-bit addressing (`I2C_M_TEN`) is rejected with `-EAFNOSUPPORT`
  and `I2C_M_RECV_LEN` with `-EOPNOTSUPP`; neither is advertised.
- I2C clock stretching needs an FT232H with AD7 wired to SCL. Without
  it the MPSSE clocks on while a slave holds SCL: the transfer fails if
  SCL is still held at the STOP (the driver then waits for SCL before it
  returns), but a shorter stretch can corrupt the data without an error.
  Do not set `clock_stretching=1` on a board without the wire: the MPSSE
  waits for AD7 and every transfer times out.
- SPI modes 1 and 3 and the I2C bus timing follow the FTDI application
  notes and the I2C-bus specification, and are tested against an emulator
  built from the same documents. They have not been measured on a logic
  analyser.
- SPI delays cost USB round trips: a delay that follows data, and the end
  of a write-only message, first wait for the chip to finish clocking.

## Relationship to Upstream

This driver suite is independent of the in-kernel `ftdi_sio` USB serial
driver. It targets the same FTDI Hi-Speed chips but exposes their MPSSE
engine as SPI, I2C, and GPIO subsystem devices -- functionality that
`ftdi_sio` does not provide. For FT4232H, this driver also handles the
non-MPSSE channels C/D (UART or bit-bang GPIO), providing unified
management of all 4 interfaces under a single driver.

## Design Internals

This section explains how the driver works internally, for developers
who want to understand, modify, or extend the code.

### MPSSE Hardware Model

FTDI Hi-Speed chips contain an MPSSE (Multi-Protocol Synchronous Serial
Engine) -- a command-response processor inside the USB device. The host
communicates with it by:

1. Writing MPSSE opcodes + arguments to the USB bulk OUT endpoint
2. Reading results from the USB bulk IN endpoint

Every bulk IN packet from FTDI hardware is prefixed with 2 modem-status
bytes (byte 0 = modem status, byte 1 = line status). These appear in
every response, even status-only packets with no payload data. The core
transport (`ftdi_mpsse_bulk_read` in `ftdi_mpsse_core.c`) strips these
bytes transparently -- child drivers never see them.

The MPSSE engine supports different clocking modes that are mutually
exclusive per channel:

- SPI: edge-triggered clocking (opcodes 0x10-0x3E), CLK/5 disabled for 60 MHz base clock
- I2C: 3-phase clocking (opcode 0x8C), open-drain (opcode 0x9E on FT232H), with SCL/SDA on AD0/AD1-AD2
- UART: not MPSSE at all -- the chip's native async serial path, set by resetting bitmode to 0x00

By design, a single MPSSE channel cannot run SPI and I2C simultaneously because they
require different clocking configurations.

### Data Flow: SPI Transfer

```
userspace write(/dev/spidevX.Y, buf, len)
  -> spi_sync() -> spi_controller.transfer_one_message()
    -> ftdi_spi_transfer_one_message()  [ftdi_spi.c]
      1. ftdi_mpsse_bus_lock()         -- acquire cross-driver mutex
      2. Build the MPSSE command buffer:
         DISABLE_CLK_DIV5, DISABLE_ADAPTIVE, DISABLE_3PHASE
         SET_CLK_DIVISOR(div)          -- freq = 60M / ((1+div)*2)
         SET_BITS_LOW(val, dir)        -- SCK idle level, CS pins deasserted
         LOOPBACK_ON/OFF               -- SPI_LOOP
         CS assert (SET_BITS_LOW)
         per transfer: SET_CLK_DIVISOR if the speed changes,
           CLK_BYTES_OUT/INOUT (or CLK_BITS_* below 8 bits per word),
           delays and cs_change as the SPI core defines them
         CS deassert (SET_BITS_LOW)
      3. ftdi_mpsse_xfer_busy(tx_buf, tx_len, rx_buf, rx_len, busy_ms)
           -> ftdi_mpsse_bulk_write()   -- USB bulk OUT
           -> ftdi_mpsse_bulk_read()    -- USB bulk IN (strips 2 status bytes)
         once per read chunk (the chip's FIFO), and before every delay
      4. End of the message: GET_BITS_LOW + SEND_IMMEDIATE, read back
         when clocking is still queued, so that spi_sync() returns once
         the data has left the pins
      5. ftdi_mpsse_bus_unlock()
```

The commands go out in 4 KiB buffers; a write-only message without
delays needs no round trip until its end. Reads take one round trip per
chunk of the chip's FIFO (per word below 8 bits per word), and every
delay waits for the clocking queued before it.

### Data Flow: I2C Transfer

```
userspace ioctl(fd, I2C_RDWR, &msgs)
  -> i2c_transfer() -> i2c_algorithm.xfer()
    -> ftdi_i2c_xfer()  [ftdi_i2c.c]
      1. ftdi_mpsse_bus_lock(); SCL and SDA read on their own (one
         round trip) unless the last transfer left the bus seen free,
         always with check_bus_free=1: a line held low ends here, EBUSY
      2. First message: engine setup (60 MHz, 3-phase, adaptive or not,
         divisor, loopback off), bus free (tBUF), GET_BITS_LOW (bus
         busy?), START, then the address byte and its ACK: one round trip
      3. Other messages: repeated START and address byte: one round trip
         (two with adaptive clocking, which reads SCL before SDA falls)
      4. Every data byte: one round trip
           Write: CLK_BITS_OUT 8 bits, SDA released, CLK_BITS_IN 1 bit (ACK)
           Read:  SDA released, CLK_BITS_IN 8 bits, CLK_BITS_OUT 1 bit (ACK/NACK)
      5. STOP, bus free, GET_BITS_LOW: SDA must be high
      6. ftdi_mpsse_bus_unlock(); bus recovery if SDA was held low
```

The I2C driver uses 3-phase clocking (opcode 0x8C): every bit has a
setup, a clock high and a hold phase (AN_108 6.3), so that SDA changes
while SCL is low. Open drain is handled differently per chip:
- FT232H: hardware open drain via MPSSE opcode 0x9E (`DRIVE_ZERO_ONLY`)
  on AD0-AD2
- FT2232H/FT4232H: SCL and SDA are driven low for '0' and made inputs
  (released, pulled up) for '1' by the pin writes (AN_255 2.4); the
  clocking commands drive SCL and the data bits push-pull

### Data Flow: UART

```
userspace write(/dev/ttyFTDIx, buf, len)
  -> tty_write() -> uart_write() -> start_tx()
    -> ftdi_uart_start_tx()  [ftdi_uart.c]
      1. Dequeue data from tty tx FIFO
      2. usb_fill_bulk_urb() -> usb_submit_urb()  -- direct USB bulk OUT
         (no MPSSE encoding -- the chip is in native UART mode)
```

UART mode bypasses MPSSE entirely. The chip is set to bitmode 0x00
(reset) which activates native async serial. Data is sent raw over
USB bulk endpoints. The FTDI chip handles baud rate generation, start/stop
bits, and parity in hardware.

This data flow is identical on MPSSE-capable channels (FT232H A,
FT2232H A/B, FT4232H A/B) and non-MPSSE channels (FT4232H C/D).
Both use direct bulk URBs and vendor control messages -- no MPSSE
opcodes are involved.

Read path: a continuously-resubmitted read URB receives bulk IN packets.
Each packet starts with the 2-byte FTDI status header. The callback
(`ftdi_uart_read_complete`) processes modem/line status changes and
pushes received data into the tty flip buffer.

### Transfer Errors and MPSSE Resynchronisation

The MPSSE is a command stream: the driver sends opcodes and reads back
exactly the bytes they produce. When a transfer fails (a timeout, a USB
error, or more response bytes than expected) the engine may still be
executing a command, or waiting for the payload of one that was cut
short. Purging the USB buffers does not stop it: the command finishes
later and its answer would be read as the response to the next command,
after which every response is the previous one, reported as success.

So `ftdi_mpsse_xfer()` never leaves the engine in that state. On any
failure that is not a cancellation (unbind, unplug, suspend), still
under `io_lock`, `ftdi_mpsse_resync()`:

1. resets the bitmode -- the MPSSE reset (AN_135 §4.2) -- and re-enters
   MPSSE mode
2. purges both buffers and checks the bad-command echo (`0xAA` ->
   `FA AA`, AN_135 §4.3.1)
3. replays the open-drain mask and the pin values/directions from the
   channel's pin shadow

The failed transfer still returns its error. If the resynchronisation
itself fails, `need_resync` stays set and the next transfer retries it
before sending anything. A transfer that ended with a STALL (`-EPIPE`)
has both bulk endpoints cleared first (`CLEAR_FEATURE(ENDPOINT_HALT)`,
500 ms): a halt stays until the host clears it, and one met during the
resynchronisation is cleared at the next attempt. Since the recovery replays the pin shadow,
every MPSSE transfer, even a single command, runs with `bus_lock` held:
the shadow is then consistent.

The reset puts the engine setup (clock divisor, divide-by-5, 3-phase and
adaptive clocking, loopback) back to the chip defaults, as resume does.
The children send theirs with every transaction: `ftdi_spi` with every
message, `ftdi_i2c` at the head of every START; `ftdi_gpio` only uses
pins.

Waiting for a response is bounded in time, not in packets: 2 s without
any response data, re-armed whenever data arrives, whatever the latency
timer.

### Disconnect and Suspend Synchronisation

The core uses a pattern inspired by the DLN2 USB-to-I2C driver to handle
hot-unplug and suspend safely:

```
struct ftdi_mpsse_dev {
    spinlock_t disconnect_lock;  -- protects disconnect + suspended flags
    bool disconnect;             -- set once in disconnect callback
    bool suspended;              -- set/cleared in suspend/resume
    int active_transfers;        -- refcount of in-progress I/O
    wait_queue_head_t disconnect_wq;  -- waited on by disconnect/suspend
};
```

Every transport function (`ftdi_mpsse_xfer`, `ftdi_mpsse_cfg_msg`) follows this pattern:

```
1. spin_lock(disconnect_lock)
   if (disconnect || suspended) return -ESHUTDOWN
   active_transfers++
   spin_unlock(disconnect_lock)

2. Do USB I/O under io_lock mutex

3. spin_lock(disconnect_lock)
   active_transfers--
   spin_unlock(disconnect_lock)
   if (disconnect || suspended) wake_up(disconnect_wq)
```

Disconnect uses `usb_poison_urb()` (not `usb_kill_urb()`) because
poison is permanent -- it cancels in-flight URBs AND causes all future
`usb_submit_urb()` to return `-EPERM`. This closes the TOCTOU race
where a transport function passes the disconnect check, then submits a
URB after kill has completed.

Disconnect (and unbind, rmmod) runs in this order:

1. set `disconnect`: new transfers fail with `-ESHUTDOWN`
2. poison the core's bulk URBs, wait up to 10 s for `active_transfers`
   to reach 0
3. `mfd_remove_devices()`: the children unregister; `ftdi-uart` poisons
   its own read/write URBs
4. reset the bitmode over ep0, leaving the channel in its default (UART)
   mode for the next driver (fails harmlessly after an unplug)

Since the driver drains every URB itself, it sets `soft_unbind`:
usbcore does not flush the endpoints before calling `disconnect` on an
unbind. That flush spins with IRQs off until each URB is unlinked; on
`vhci_hcd` (USB/IP) an URB whose completion is in progress cannot be
unlinked and a uniprocessor kernel livelocks.

Suspend poisons the URBs as well, and resume unpoisons them: a transfer
that passed the `suspended` check just before cannot submit one after
the kill. A transfer that is in flight at that moment is cut short: its
URB is cancelled, it submits no further one (a response is one bulk IN
per packet), and it fails with `-ESHUTDOWN` like the transfers that
follow. The SPI core lets a message in flight finish first; an I2C
transfer is interrupted. Resume re-enters MPSSE (or UART) and replays
the channel's pin shadow and open-drain mask; if that fails,
`need_resync` makes the next transfer try again. The children then
restore their own state: `ftdi_gpio` re-reads its inputs and restarts its
polling (its outputs are in the shadow), `ftdi_gpio_bitbang` its mode and
lines (`fg->lock`), `ftdi_uart` its port
through serial_core (startup, termios, modem lines) and its CBUS mode
(`cbus_lock`); `ftdi_spi` and `ftdi_i2c` send their engine setup with
the next transfer, and `ftdi_i2c` reads the bus before its next START,
the state the slaves were left in being unknown.

### Synchronisation Primitives

| Primitive           | Location            | Description                                |
| ------------------- | ------------------- | ------------------------------------------ |
| `bus_lock` (mutex)  | ftdi_mpsse_core.c   | Whole-transaction exclusion across drivers |
| `io_lock` (mutex)   | ftdi_mpsse_core.c   | Serialises USB bulk transfers              |
| `disconnect_lock`   | ftdi_mpsse_core.c   | Guards disconnect/suspend state            |
| `port->lock` (spin) | ftdi_uart.c         | UART: write URBs, RX/TX state, modem control queue |
| `cbus_lock` (mutex) | ftdi_uart.c         | UART: CBUS GPIO state                      |
| `fg->lock` (mutex)  | ftdi_gpio.c         | Serialises GPIO cache + MPSSE I/O          |
| `fg->lock` (mutex)  | ftdi_gpio_bitbang.c | Serialises bit-bang GPIO + bulk I/O        |

Lock ordering: `bus_lock` -> `fg->lock` -> `io_lock` -> `disconnect_lock`

The channel's pin shadow (`ftdi_mpsse_pins_set()`/`_get()`, open-drain
`_od_set()`) is only touched with `bus_lock` held (lockdep-asserted).

### EEPROM Auto-Detection Cascade

`ftdi_mpsse_select_mode()` decides what a channel is used for, most
explicit source first:

```
1. bus_mode module parameter       -> if set for this interface: use it
2. EEPROM "bus_mode=" tag          -> same syntax, in the user area
3. Channel without MPSSE (FT4232H C/D) -> UART (or bit-bang if asked)
4. EEPROM channel type             -> FIFO/OPTO/CPU/FT1284 rejected
5. Product string keywords         -> contains "SPI" or "I2C"
6. VCP driver bit of the channel   -> if set: UART
7. Drive settings of its data bus  -> slow slew + Schmitt = I2C
8. Default                         -> SPI (most common MPSSE use case)
```

Steps 2 and 4-7 need an EEPROM that was read, is not blank and has a
valid checksum; otherwise the channel goes straight to the default.
Steps 5-8 are `ftdi_mpsse_detect_protocol()`. The drive nibble of a
channel's data bus comes from `ftdi_eeprom_dbus_drive()`, which knows
the layout of bytes `0x0c`/`0x0d` for each chip.

### Module Parameter Reference

All module parameters across the driver suite:

| Module       | Parameter          | Type   | Default | Description                                                                                                                                                                                                                                                     |
| ------------ | ------------------ | ------ | ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ftdi_mpsse` | `bus_mode`         | string | `""`    | Channel mode: `uart`, `spi`, `i2c`, `bitbang` (alias `gpio`), or empty for auto. Comma-separated for per-interface (`spi,uart` for FT2232H; `spi,i2c,uart,bitbang` for FT4232H), a missing entry is auto. FT4232H C/D accept `uart` and `bitbang`; other values log a warning and give a UART. |
| `ftdi_mpsse` | `spi_cs`           | string | `"3"`   | SPI chip-select AD pin numbers (3-7). Global: `3,4,5`. Per-channel: `'A:3,4,5;B:3'`.                                                                                                                                                                              |
| `ftdi_mpsse` | `mpsse_latency`    | uint   | `16`    | MPSSE latency timer in ms (1-255, AN_255); writable, applied whenever a channel enters MPSSE (probe, resume, recovery after a failed transfer)                                                                                                                  |
| `ftdi_spi`   | `register_spidev`  | bool   | `0`     | Register a spidev device (`/dev/spidevX.Y`) on every chip select; without it, instantiate clients with the controller's `new_device` (`CONFIG_SPI_DYNAMIC`)                                                                                                     |
| `ftdi_i2c`   | `i2c_speed`        | uint   | `100`   | I2C bus speed in kHz (10-400)                                                                                                                                                                                                                                   |
| `ftdi_i2c`   | `clock_stretching` | bool   | `0`     | Force adaptive clocking on every FT232H adapter (SCL wired to AD0 and AD7); auto-detected per adapter; ignored on FT2232H/FT4232H (SCL not open drain)                                                                                                        |
| `ftdi_i2c`   | `strict_timing`    | bool   | `1`     | Meet the I2C-bus specification minimum times (START/STOP setup and hold, SCL high: 83 kHz on the bus for 84-100 kHz); `0` = four pin writes per step as in AN_113, clock at the requested rate                                                                  |
| `ftdi_i2c`   | `bus_checks`       | bool   | `0`     | SDA loopback / SCL-SDA short checks at probe (bring-up only)                                                                                                                                                                                                    |
| `ftdi_i2c`   | `check_bus_free`   | bool   | `0`     | Read SCL and SDA in a USB round trip of its own before every transfer, so that nothing is clocked into a bus that is not free; `0` = only at probe, after a failed transfer and after a resume, otherwise sampled with the START (the address byte is then clocked before `EBUSY`) |
| `ftdi_i2c`   | `i2c_bus_nr_map`   | string | `""`    | `usbpath:nr,...`: fixed adapter numbers, per USB device (`3-2:40`) or per channel by interface name (`3-2:1.0:40,3-2:1.1:41`)                                                                                                                                   |
| `ftdi_i2c`   | `i2c_delay_us_map` | string | `""`    | `usbpath:us,...`: delay of 0-10000 µs before each repeated START and after the STOP, per USB device or per channel (interface name); the channel entry wins                                                                                                      |
| `ftdi_gpio`  | `gpio_offset_compat` | bool | `0`     | Legacy offsets: 0-7 = AC, 8-11 = AD4-7; ignored on FT4232H, which has no AC pins                                                                                                                                                                              |
| `ftdi_gpio`  | `gpio_base`        | int    | `-1`    | Fixed base of the first gpiochip, -1 = dynamic                                                                                                                                                                                                                  |

sysfs attributes:

| Module       | Attribute       | Path            | Description                                             |
| ------------ | --------------- | --------------- | ------------------------------------------------------- |
| `ftdi_uart`  | `latency_timer` | platform device | RX buffer timeout in ms (1-255, default 16)             |
| `ftdi_uart`  | `dtr_dsr_flow`  | platform device | DTR/DSR flow control (0 or 1), applied at once          |
| `ftdi_i2c`   | `i2c_speed_khz` | platform device | I2C speed in kHz (10-400), from the next transfer; `-EINVAL` outside the range |
| `ftdi_mpsse` | `eeprom_*`      | USB interface   | Read-only: decoded EEPROM fields, one value each        |
| `ftdi_mpsse` | `eeprom`        | USB interface   | Read-only (root): raw 256-byte EEPROM binary            |

debugfs files (`/sys/kernel/debug`):

| Module       | File                                    | Description                                             |
| ------------ | --------------------------------------- | ------------------------------------------------------- |
| `ftdi_i2c`   | `i2c/i2c-N/i2c_stats`                   | Transfer counters; writing `0` resets them              |
| `ftdi_mpsse` | `ftdi_mpsse/<interface>/pinmap`         | Active pin assignments and GPIO availability            |
| `ftdi_mpsse` | `ftdi_mpsse/<interface>/eeprom_cbus`, `eeprom_drive`, `eeprom_config` | Multi-line EEPROM reports |

## Development

### Prerequisites

- A recent kernel tree or headers: the code follows current mainline
  APIs and has no compatibility code for older kernels (it does not
  build against 6.8 distro headers)
- For UML-based testing (x86 only):
  - Linux kernel source tree (default: `~/dev/linux`, override: `LINUX_SRC=`)
  - Static BusyBox: `apt install busybox-static`
  - GCC, `cpio`, `gzip`
- For static analysis:
  - `sparse` (endian/lock checking), `smatch`
  - `checkpatch.pl` (from kernel source)
  - `coccinelle` for semantic patches

### Build and Test Iteration Loop

```sh
# One-time setup: build the UML kernel (~2-5 min)
make -C tests uml-kernel LINUX_SRC=/path/to/linux

# Edit driver source, then test (~5 sec per test):
make -C tests test-spi        # rebuilds modules + initramfs automatically

make test                     # SPI + I2C + UART
make -C tests test-all        # + multi-channel, FT4232H C/D, AD7 wiring
make -C tests test-errors     # error injection
make -C tests test-bus        # on the wire: I2C stretching and timing, SPI modes, delays, CS
make -C tests test-pm         # suspend/resume callbacks, chip power-cycled in between
make -C tests test-stress     # concurrency, unbind, hotplug, parameters

# Debug kernel: KASAN, lockdep, kmemleak, UBSAN, failslab
make -C tests uml-kernel-debug LINUX_SRC=/path/to/linux
make -C tests test-stress DEBUG=1

# Static analysis before submitting (SPARSE and SMATCH default to
# ~/bin/sparse and ~/bin/smatch, KDIR to the running kernel):
make check KDIR=tests/.uml-build ARCH=um SPARSE=$(which sparse) SMATCH=$(which smatch) \
	CHECKPATCH=/path/to/linux/scripts/checkpatch.pl
```

`tests/README.md` lists every test mode. A run passes only if all its
checks pass and the kernel log has no WARN/BUG/KASAN/UBSAN/lockdep
splat (`tests/check_log.sh`); splats of known upstream bugs are listed
in `tests/known_splats.awk` and named instead of failing the run.

### Running a Single Test Interactively

```sh
make -C tests initramfs
tests/.uml-build/linux \
    initrd=tests/.uml-build/initramfs.cpio.gz \
    mem=256M \
    ftdi_mode=spi \
    con=null con0=fd:0,fd:1
```

The emulator log and the full kernel dmesg are printed at the end. The
`ftdi_mode=` parameter selects the test (see `tests/init.sh`).

### Running the Emulator Standalone

For protocol-level debugging outside UML (needs a host kernel the
modules build against):

```sh
# Terminal 1: start the emulator
ftdi-emulation/ftdi_usbip --chip ft232h --mode spi

# Terminal 2: attach via USB/IP
sudo modprobe vhci-hcd
sudo usbip attach -r 127.0.0.1 -b 1-1

# Load the driver; 0403:6014 is in ftdi_sio's table: take the interface
# from it if it is loaded (the interface name is in dmesg)
sudo insmod ftdi_mpsse.ko bus_mode=spi spi_cs=3,4,5
sudo insmod ftdi_spi.ko register_spidev=1
sudo insmod ftdi_gpio.ko
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
echo 3-1:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
echo 3-1:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind

# Now you can test with real tools (spidev_test, gpioinfo, etc.)
```

`ftdi-emulation/README.md` describes what the emulator models and the
protocol violations it reports.

### How to Add a New Child Driver

All child drivers follow the same pattern (see `ftdi_spi.c` for a
clean example):

1. **Create `ftdi_foo.c`** with a `platform_driver`:
   ```c
   static struct platform_driver ftdi_foo_driver = {
       .driver = { .name = "ftdi-foo" },
       .probe = ftdi_foo_probe,
   };
   module_platform_driver(ftdi_foo_driver);
   MODULE_ALIAS("platform:ftdi-foo");
   ```

2. **In probe**, get platform data and parent transport:
   ```c
   const struct ftdi_mpsse_pdata *pdata = dev_get_platdata(&pdev->dev);
   // Use ftdi_mpsse_xfer(), ftdi_mpsse_write(): bus_lock held around
   // every transfer (lockdep-asserted) and around whole transactions
   ftdi_mpsse_bus_lock(pdev);
   ret = ftdi_mpsse_xfer(pdev, tx, tx_len, rx, rx_len);
   ftdi_mpsse_bus_unlock(pdev);
   ```

3. **Add to `Makefile`**: append `ftdi_foo.o` to `obj-m`

4. **Add MFD cell** in `ftdi_mpsse_core.c`: name the child in the case
   of its mode in `ftdi_mpsse_add_children()`, which builds the cells and
   their `ftdi_mpsse_pdata` on the stack for `mfd_add_hotplug_devices()`
   (grow `cells[]` and `pd[]` for a third child of one mode)

5. **Add to test initramfs**: update `tests/Makefile` and `tests/init.sh`

Key invariants:
- Hold `bus_lock` around every MPSSE transfer and whole transactions;
  send the engine setup (clock, 3-phase, adaptive) with every
  transaction: the core resets the engine after a failed transfer
- Transport functions check `disconnect`/`suspended` internally
- `can_sleep = true` for GPIO chips -- all I/O goes over USB
- Use `devm_*` allocation where possible

For **non-MPSSE children** (e.g. `ftdi_gpio_bitbang`): use
`ftdi_mpsse_cfg_msg()` for vendor control messages,
`ftdi_mpsse_write()` for bulk OUT data and `ftdi_mpsse_read_pins()` for
`READ_PINS`; a child running its own URBs (`ftdi_uart`) gets the raw
endpoints from `ftdi_mpsse_get_endpoints()` and must poison them in its
remove (a killed URB can be submitted again). Pins shared with siblings go through the core's shadow
(`ftdi_mpsse_pins_set()`/`_get()`) under `bus_lock`.

### How to Add a New Test

See "Adding a Test Mode" in `tests/README.md`. Test output is saved to
`tests/test-<mode>.log`: assertion results, tool output, the emulator
log and the full kernel dmesg.

### Test Coverage

| Component                    | Status      | Notes                                               |
| ---------------------------- | ----------- | --------------------------------------------------- |
| USB probe/detect             | Tested      | FT232H, FT2232H, FT4232H                            |
| EEPROM decode                | Tested      | Checksum, blank and corrupt images, drive groups per chip |
| EEPROM-driven mode selection | Tested      | Tag, product string, VCP bit, drive settings, channel type, 93C46 (mirrored), another product ID (`test-eeprom`) |
| SPI transfers                | Tested      | spidev, loopback, bits per word, SCK rate measured; modes 0-3 edge by edge against per-mode slaves (`spi-modes`); one device per chip select, `SPI_CS_HIGH` (`spi-cs-high`) |
| SPI timing on the wire       | Tested      | End of message, delays, `cs_change`, `cs_off`, CS setup/hold/inactive, a slow write the chip's buffer holds up for longer than the USB timeout (`spi-timing`, `spi-cs`) |
| I2C transfers                | Tested      | i2c-dev against bus slaves (address, START/STOP, data read back), 100/400 kHz, AD7 adaptive clocking |
| I2C timing on the wire       | Tested      | UM10204 minimums measured in every I2C mode; clock stretching before repeated START and STOP (`i2c-ad7-stretch`) |
| UART                         | Tested      | Loopback data, modem lines, baud changes, 7 data bits, break, 64 KB, drain on close/tcdrain; RTS/CTS, DTR/DSR and XON/XOFF against a port nobody reads, overrun counted without them (`uart-flow`) |
| MPSSE GPIO                   | Tested      | Values, directions; edge events from a pin the emulated board moves; edge requests under stress |
| Bit-bang GPIO                | Tested      | FT4232H channel D (`bus_mode=bitbang`)              |
| Multi-channel                | Tested      | FT2232H A/B, FT4232H A-D                            |
| RS-485 (TXDEN)               | Tested      | Pin found per chip and EEPROM, `TIOCSRS485` accepted or refused (`uart-rs485`, `uart-rs485-ft4232h`, `ft2232h-uart`); the emulated chip names the same pin from its EEPROM, the timing of the signal is not emulated |
| I2C maps per channel         | Tested      | Interface-name keys on FT2232H (`i2c-map-chan`)     |
| AD7 ownership                | Tested      | I2C clock stretching against the GPIO line, both orders (`i2c-ad7-gpio`) |
| Error paths                  | Tested      | NAK, stuck bus, clock stretch, USB timeout, sync    |
| MPSSE error recovery         | Tested      | Stalled command, stray response byte, USB timeout   |
| Hot-unplug / unbind / rmmod  | Tested      | Storms under concurrent I/O                         |
| Allocation failures          | Tested      | failslab (debug kernel)                             |
| Memory / locking             | Tested      | KASAN, lockdep, kmemleak, UBSAN (debug kernel)      |
| Suspend/resume               | Tested      | Callbacks run in system-sleep order by a test module, chip power-cycled in between: SPI, I2C with clock stretching, UART with CBUS GPIO, FT4232H in four modes (`test-pm`).  Not a real system sleep (`vhci_hcd` refuses it, and it would freeze the emulator) |
| Multi-channel data paths     | Tested      | SPI and I2C transfers on every MPSSE channel of FT2232H and FT4232H, mixed modes on one chip (`test-all`) |

## Troubleshooting

### `ftdi_sio` Claims the Device

The most common issue: the in-kernel `ftdi_sio` driver binds to FTDI
devices before `ftdi_mpsse` can. Solutions:

```sh
# Method 1: add the ID, then move the interface over
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind

# Method 2: blacklist ftdi_sio
echo "blacklist ftdi_sio" | sudo tee /etc/modprobe.d/ftdi.conf
```

### Device Not Probed (No Child Devices)

Check dmesg for the reason:
```sh
dmesg | grep ftdi_mpsse
```

Causes of a failed probe:
- `unsupported FTDI chip` -- the `bcdDevice` value doesn't match any
  known chip type
- `EEPROM channel type 0xNN not supported` -- FIFO/OPTO/CPU/FT1284 modes
  are not implemented; `bus_mode` (parameter or EEPROM tag) overrides it
- `invalid spi_cs parameter` -- see [SPI Capabilities](#spi-capabilities)
- `MPSSE sync failed` -- the chip did not answer the bad-command check
  when entering MPSSE (`failed to set channel mode`)

Not a failure, but a different mode than expected:
- `unknown bus_mode` -- typo in the parameter or the EEPROM tag: the
  channel falls back to auto mode
- `bus_mode 'spi' not supported on non-MPSSE channel` -- FT4232H C/D
  only support `uart` and `bitbang`; the driver warns and forces UART

### Empty EEPROM (All 0xFF)

Factory-fresh FTDI chips may have an empty EEPROM. The driver handles
this gracefully:
- Reports `EEPROM: empty (all 0xFF)` in dmesg
- Reports `auto: no usable EEPROM, defaulting to SPI` and comes up as SPI
  (or respects the `bus_mode` parameter)
- Use `tools/ftdi_eeprom_check` to verify: `sudo tools/ftdi_eeprom_check`

### SPI/I2C Not Working

Verify the mode was set correctly:
```sh
sudo cat /sys/kernel/debug/ftdi_mpsse/1-2:1.0/pinmap   # active mode and pin mapping
dmesg | grep "EEPROM hints"                            # auto-detection result
```

If in the wrong mode, either set `bus_mode` explicitly or program the
EEPROM (`bus_mode=` tag, product string or VCP bit; `eeprom_bus_mode`
and `eeprom_config` show what the driver decoded).

### UML Test Failures

```sh
# Check the full log
cat tests/test-<mode>.log

# Verdict and the reason for it
sh tests/check_log.sh <mode> tests/test-<mode>.log
grep -E "FAIL:|WARNING:|Oops:" tests/test-<mode>.log

# Rebuild everything clean
make -C tests clean
# After a change of the kernel source: the kernel is rebuilt only when
# tests/uml.config is newer than it
touch tests/uml.config && make -C tests uml-kernel LINUX_SRC=/path/to/linux
make -C tests test-spi
```

Common test issues:
- `UML requires an x86 host` -- UML only works on x86_64/i686
- Missing `busybox-static` -- `apt install busybox-static`
- Missing kernel source -- set `LINUX_SRC=/path/to/linux`

## License

Each file carries its SPDX identifier: the driver sources (`ftdi_*.c`)
and the test kernel modules are GPL-2.0+, the driver headers and
`tools/ftdi_eeprom_check.c` GPL-2.0, the emulator, the test suite and
the Makefiles GPL-2.0-only; the documents, the UML configuration
fragments and `tools/ft232h-i2c.conf` have the kernel's GPL-2.0, and
`.gitignore` its GPL-2.0-only; `benchmarks/` is AGPL-3.0-or-later, its
libmpsse build files included (`config.h` is built into the benchmark).
