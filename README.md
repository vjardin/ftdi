# FTDI MPSSE Kernel Driver

[![Build & Test](https://github.com/vjardin/ftdi/actions/workflows/test.yml/badge.svg)](https://github.com/vjardin/ftdi/actions/workflows/test.yml)
[![Static Analysis](https://github.com/vjardin/ftdi/actions/workflows/check.yml/badge.svg)](https://github.com/vjardin/ftdi/actions/workflows/check.yml)

Out-of-tree Linux kernel driver suite for FTDI Hi-Speed USB chips
(FT232H, FT2232H, FT4232H and variants), built as a USB interface
driver with child platform devices for UART, SPI, I2C, and GPIO.

Compared to userspace alternatives (libmpsse / libftdi), the kernel
driver delivers **1.1-4.3x higher throughput** with lower latency and
**zero verification failures** across all I2C transfer sizes at 400 kHz.
See [`benchmarks/`](benchmarks/) for reproducible numbers and methodology.

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

MPSSE-capable channels (FT232H A, FT2232H A/B, FT4232H A/B):
  -> ftdi-uart | ftdi-spi | ftdi-i2c  (one per channel, selected by bus_mode)
  -> ftdi-gpio                        (MPSSE SET_BITS_LOW/HIGH, 16 pins)

Non-MPSSE channels (FT4232H C/D):
  -> ftdi-uart                        (always, hardware-fixed)
  -> ftdi-gpio-bitbang                (async bit-bang, 8 pins, shared with UART)
```

The core module (`ftdi_mpsse.ko`) owns the USB interface. On probe it:

 - Detects the chip type from `bcdDevice`
 - Determines MPSSE capability per channel (FT4232H C/D lack MPSSE hardware)
 - Reads and decodes the on-chip EEPROM (128 words / 256 bytes)
 - Selects the channel mode (see [Mode Selection](#mode-selection) below);
   non-MPSSE channels are forced to UART mode
 - Creates child platform devices via `mfd_add_hotplug_devices()`
 - Exposes EEPROM data via sysfs (see [EEPROM Sysfs](#eeprom-sysfs) below)

MPSSE-capable children communicate through exported transport functions
(`ftdi_mpsse_xfer`, `ftdi_mpsse_write`, etc.), serialised by a mutex in
the core. Non-MPSSE children (UART, bit-bang GPIO) use vendor control
messages (`ftdi_mpsse_cfg_msg`) and direct bulk I/O via the endpoint
accessors.

## Module Structure

| Module                 | Source Files                         | Description                                                 |
| ---------------------- | ------------------------------------ | ----------------------------------------------------------- |
| `ftdi_mpsse.ko`        | `ftdi_mpsse_core.c`, `ftdi_eeprom.c` | USB probe, EEPROM decode, MPSSE transport, mode setup       |
| `ftdi_uart.ko`         | `ftdi_uart.c`                        | `serial_core` `uart_port`, `/dev/ttyFTDIx`                  |
| `ftdi_spi.ko`          | `ftdi_spi.c`                         | `spi_controller` with MPSSE clocking commands               |
| `ftdi_i2c.ko`          | `ftdi_i2c.c`                         | `i2c_adapter` with 3-phase I2C protocol                     |
| `ftdi_gpio.ko`         | `ftdi_gpio.c`                        | `gpio_chip` on MPSSE AD/AC pins (16 pins)                   |
| `ftdi_gpio_bitbang.ko` | `ftdi_gpio_bitbang.c`                | `gpio_chip` via async bit-bang (8 pins, non-MPSSE channels) |

Shared definitions live in `ftdi_mpsse.h` (platform data, transport API,
MPSSE opcodes). Internal EEPROM definitions live in `ftdi_eeprom.h`
(constants, `struct ftdi_eeprom`, decode functions).

## Supported Hardware

| Chip     | Channels | MPSSE | C/D Modes            | PID      |
| -------- | -------- | ----- | -------------------- | -------- |
| FT232H   | 1        | A     | --                   | `0x6014` |
| FT2232H  | 2        | A, B  | --                   | `0x6010` |
| FT4232H  | 4        | A, B  | UART + bit-bang GPIO | `0x6011` |
| FT4232HA | 4        | A, B  | UART + bit-bang GPIO | `0x6048` |
| FT232HP  | 1        | A     | --                   | `0x6045` |
| FT233HP  | 1        | A     | --                   | `0x6044` |
| FT2232HP | 2        | A, B  | --                   | `0x6042` |
| FT2233HP | 2        | A, B  | --                   | `0x6040` |
| FT4232HP | 4        | A, B  | UART + bit-bang GPIO | `0x6043` |
| FT4233HP | 4        | A, B  | UART + bit-bang GPIO | `0x6041` |

All chips share the FTDI vendor ID `0x0403`.

FT4232H channels A and B have full MPSSE capability (SPI, I2C, UART,
GPIO). Channels C and D lack the MPSSE engine -- they support UART
(native async serial) and GPIO via async bit-bang mode only. This is a
silicon-level constraint.

## Building

```sh
make                                  # build against running kernel
make KDIR=/path/to/kernel/build       # cross-compile or target different headers
make tools                            # build userspace tools
make clean
```

Standard kbuild variables (`CROSS_COMPILE`, `ARCH`, etc.) work.

## Usage

### Loading

The PID table in `ftdi_mpsse.ko` is empty by default to avoid
conflicting with `ftdi_sio`. Four methods to bind the driver:

#### Method 1: `new_id` (runtime, no reboot)

```sh
sudo modprobe ftdi_mpsse bus_mode=spi
# Add FTDI PID at runtime
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
```

#### Method 2: `driver_override` (per-device, most flexible)

```sh
sudo modprobe ftdi_mpsse bus_mode=spi
# Unbind ftdi_sio from the specific interface
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
# Override to ftdi_mpsse
echo ftdi_mpsse | sudo tee /sys/bus/usb/devices/1-2:1.0/driver_override
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind
```

#### Method 3: udev rule (persistent, automatic)

```sh
# /etc/udev/rules.d/99-ftdi-mpsse.rules
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="0403", \
  ATTR{idProduct}=="6014", \
  RUN+="/bin/sh -c 'echo ftdi_mpsse > %S%p/%k:1.0/driver_override'"
```

#### Method 4: EEPROM custom PID (permanent)

Use `ftdi_eeprom` to program a custom PID into the device EEPROM,
then add that PID to a static table or use `new_id`.

After binding, load child drivers:
```sh
sudo modprobe ftdi_gpio
sudo modprobe ftdi_spi       # if bus_mode includes SPI
sudo modprobe ftdi_i2c       # if bus_mode includes I2C
sudo modprobe ftdi_uart      # if bus_mode=uart
```

### Mode Selection

The driver decides which child devices to create by evaluating two
sources, in order of priority:

1. `bus_mode` module parameter -- always wins if set to a known value
2. EEPROM channel type -- consulted when `bus_mode` is empty (auto)

#### Priority 1 - `bus_mode` module parameter (static override)

Set at module load time. Applies identically regardless of what the
EEPROM is set:

```sh
sudo modprobe ftdi_mpsse bus_mode=spi     # force SPI + GPIO
sudo modprobe ftdi_mpsse bus_mode=uart    # force UART + GPIO
sudo modprobe ftdi_mpsse bus_mode=i2c     # force I2C + GPIO
sudo modprobe ftdi_mpsse                  # auto (see below)
```

Comma-separated values select per-interface modes on multi-channel
chips (FT2232 and FT4232, not FT232):

```sh
# FT2232H: channel A = SPI, channel B = UART
sudo modprobe ftdi_mpsse bus_mode=spi,uart

# FT4232H: all 4 channels explicit (C/D only accept uart)
sudo modprobe ftdi_mpsse bus_mode=spi,i2c,uart,uart
```

FT4232H accepts up to 4 comma-separated values. Channels C and D only
support `uart` -- if a non-UART value is specified for C/D, the driver
emits a warning and forces UART mode.

| `bus_mode=`  | Child Devices Created        | Mode                | Use Case                        |
| ------------ | ---------------------------- | ------------------- | ------------------------------- |
| `uart`       | ftdi-uart, ftdi-gpio         | Reset (UART native) | Serial port + MPSSE GPIO        |
| `spi`        | ftdi-spi, ftdi-gpio          | MPSSE               | SPI master + GPIO               |
| `i2c`        | ftdi-i2c, ftdi-gpio          | MPSSE               | I2C adapter + GPIO              |
| `uart` (C/D) | ftdi-uart, ftdi-gpio-bitbang | Reset (UART native) | Serial port + bit-bang GPIO     |
| *(empty)*    | auto -- see below            | depends             | EEPROM-driven or MPSSE fallback |

#### Auto mode (EEPROM-driven)

When `bus_mode` is empty (the default), the driver reads the EEPROM
and applies a multi-level detection algorithm:

##### Step 1: Channel type check

| EEPROM Channel Type | Value  | Behaviour                                       |
| ------------------- | ------ | ----------------------------------------------- |
| `UART` (factory)    | `0x00` | Enters UART mode, creates ftdi-uart + ftdi-gpio |
| `FIFO`              | `0x01` | Rejected (`-ENODEV`): mode not supported        |
| `OPTO`              | `0x02` | Rejected (`-ENODEV`): mode not supported        |
| `CPU`               | `0x04` | Rejected (`-ENODEV`): mode not supported        |
| `FT1284`            | `0x08` | Rejected (`-ENODEV`): mode not supported        |
| *(empty/unknown)*   | --     | Proceeds to protocol detection (Step 2)         |

##### Step 2: Protocol detection (for unknown/empty channel type)

When the channel type is unknown or empty, the driver applies heuristics
in priority order in order to guess the user's settings:

| Priority | Source                    | Detection Logic                                   |
| -------- | ------------------------- | ------------------------------------------------- |
| 1        | Protocol hint byte (0x1A) | `'S'`=SPI, `'I'`=I2C, `'U'`=UART                  |
| 2        | VCP driver bit            | If VCP enabled -> UART (device is a COM port)     |
| 3        | Product string            | Contains "SPI" -> SPI; contains "I2C" -> I2C      |
| 4        | Electrical settings       | Slow slew + Schmitt -> I2C (open-drain optimized) |
| 5        | Default                   | SPI (most common MPSSE use case)                  |

The default is currently SPI-only. Use the protocol hint byte (`'I'`) or `bus_mode=i2c`
parameter to select I2C mode instead.

The EEPROM channel type is set per-channel by FTDI programming tools
(`ftdi_eeprom`).  Most factory-fresh devices ship with `cha_type=UART` or
with an empty EEPROM.

The FT4232H is a special case: channels A and B store their channel type
in the EEPROM (byte 0x00 and 0x01, same as FT2232H) and can be configured
as UART/FIFO/OPTO/CPU/FT1284. Channels C and D are hardware-fixed to
UART regardless of EEPROM contents -- by design, they lack the MPSSE engine entirely.

#### EEPROM Protocol Hint (user area byte at 0x1A)

The FTDI EEPROM has no native field for SPI vs I2C mode -- both use the
MPSSE engine. To provide explicit control, program a single byte at
offset `0x1A` in the EEPROM user area:

| Value | Char | Mode                              |
| ----- | ---- | --------------------------------- |
| 0x53  | `S`  | SPI only (ftdi-spi + ftdi-gpio)   |
| 0x49  | `I`  | I2C only (ftdi-i2c + ftdi-gpio)   |
| 0x55  | `U`  | UART only (ftdi-uart + ftdi-gpio) |
| other | --   | Auto-detect via heuristics        |

##### Programming the protocol hint with ftdi_eeprom

1. Create a 1-byte file containing the hint character:
   ```sh
   echo -n 'S' > protocol_hint.bin   # For SPI mode
   # or
   echo -n 'I' > protocol_hint.bin   # For I2C mode
   ```

2. Add to your `ftdi_eeprom` configuration:
   ```ini
   # ft232h.conf
   vendor_id=0x0403
   product_id=0x6014
   manufacturer="ACME"
   product="SPI Adapter"     # Also detected: "SPI" in product string

   # Protocol hint in user area
   user_data_addr=0x1a
   user_data_file="protocol_hint.bin"

   # Recommended electrical settings for SPI
   group0_schmitt=true
   group0_slew=fast
   group1_schmitt=true
   group1_slew=fast

   filename=ft232h.bin
   ```

3. Flash the EEPROM:
   ```sh
   sudo ftdi_eeprom --flash-eeprom ft232h.conf
   ```

##### Alternatively, use the product string

If you don't want to use the user area, include "SPI" or "I2C" in the
product string:

```ini
product="My SPI Adapter"    # Driver detects "SPI" -> SPI mode
product="I2C Bridge v2"     # Driver detects "I2C" -> I2C mode
```

##### Electrical settings heuristic

If no protocol hint or product string keyword is found, the driver
examines the ADBUS electrical settings:

- Slow slew + Schmitt trigger -> likely a I2C mode (open-drain optimized)
- Fast slew -> likely a SPI mode (high-frequency clock optimized)

This heuristic reflects best practices: I2C benefits from slow edges
and noise immunity, while SPI benefits from fast edges for higher
clock rates.

#### Tuning the EEPROM to influence auto mode

To have the driver automatically choose UART mode without passing
`bus_mode=uart`, program the EEPROM channel type to UART:

```ini
# ft232h.conf for ftdi_eeprom
cha_type=UART
```

To have it use SPI mode automatically, either:
- Set the protocol hint byte to `'S'` (0x53) at offset 0x1A
- Include "SPI" in the product string
- Use fast slew rate settings (driver will default to SPI)

To have it use I2C mode automatically, either:
- Set the protocol hint byte to `'I'` (0x49) at offset 0x1A
- Include "I2C" in the product string
- Use slow slew + Schmitt trigger settings

For predictable behaviour across different EEPROM states, always set
`bus_mode` explicitly.

#### Multi-Channel Configuration

On multi-channel chips, each channel is configured independently:

| Chip    | Channels | MPSSE-Capable | Configuration                                     |
| ------- | -------- | ------------- | ------------------------------------------------- |
| FT232H  | 1        | A             | Single mode: SPI xor I2C xor UART                 |
| FT2232H | 2        | A, B          | Per-channel: e.g., A=SPI, B=I2C                   |
| FT4232H | 4        | A, B          | A/B: SPI/I2C/UART; C/D: UART + bit-bang GPIO only |

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

The EEPROM protocol hint at offset 0x1A applies to the channel associated
with each USB interface (interface 0 = channel A, interface 1 = channel B).
Channels C and D ignore EEPROM protocol hints and are always UART.

### UART Capabilities

The UART child driver (`ftdi_uart.ko`) provides a full-featured
serial port on `/dev/ttyFTDIx`:

- Baud rates: 300 to 12 Mbaud (Hi-Speed divisor for >= 1200, BM divisor for < 1200)
- Data bits: 7, 8 (CS5 accepted as smartcard quirk; CS6 rejected)
- Parity: None, Odd, Even, Mark, Space (`CMSPAR`)
- Stop bits: 1, 2
- Flow control: RTS/CTS, DTR/DSR (sysfs), XON/XOFF (termios)
- Modem signals: DTR, RTS output; CTS, DSR, RI, DCD input
- RS-485: hardware TXDEN via CBUS pin (configured in EEPROM, auto-detected at probe; `TIOCSRS485` ioctl reports error if no TXDEN pin is configured)
- CBUS GPIO: FT232H exposes up to 4 CBUS pins as a `gpio_chip` (pin availability determined by EEPROM configuration)
- Double-buffered writes: 2 write URBs for pipelined TX

sysfs attributes (under the platform device):

| Attribute       | Type   | Default | Description                          |
| --------------- | ------ | ------- | ------------------------------------ |
| `latency_timer` | 1-255  | 16      | RX buffer timeout in ms              |
| `dtr_dsr_flow`  | 0 or 1 | 0       | Enable DTR/DSR hardware flow control |

### EEPROM Sysfs

The core module exposes EEPROM data via sysfs on the USB interface
device (e.g. `/sys/bus/usb/devices/1-2:1.0/`). Attributes are
created at probe time and are read-only.

Each USB interface exposes the EEPROM attributes for its own channel.
The EEPROM is physically one per chip (256 bytes shared by all channels),
but since each interface probes independently, the attributes are
per-interface:

- `eeprom_channel_type` on interface 0 shows channel A's type
- `eeprom_channel_type` on interface 1 shows channel B's type
- `eeprom_channel_type` on interface 2 shows channel C's type (FT4232H)
- `eeprom_channel_type` on interface 3 shows channel D's type (FT4232H)

Chip-wide fields (`eeprom_vid`, `eeprom_pid`, the raw `eeprom` binary,
etc.) are identical on all interfaces -- they read the same physical
EEPROM.

#### Text attributes

| Attribute              | Format     | Example                                    | Description                                                            |
| ---------------------- | ---------- | ------------------------------------------ | ---------------------------------------------------------------------- |
| `eeprom_valid`         | `%d\n`     | `1`                                        | 1 if EEPROM was read successfully                                      |
| `eeprom_empty`         | `%d\n`     | `0`                                        | 1 if all bytes are `0xFF` (blank EEPROM)                               |
| `eeprom_checksum`      | `%s\n`     | `ok`                                       | `ok`, `bad`, or `n/a` (empty/invalid)                                  |
| `eeprom_vid`           | `0x%04x\n` | `0x0403`                                   | Vendor ID from EEPROM                                                  |
| `eeprom_pid`           | `0x%04x\n` | `0x6014`                                   | Product ID from EEPROM                                                 |
| `eeprom_channel_type`  | `%s\n`     | `uart`                                     | This interface's channel type: `uart`, `fifo`, `opto`, `cpu`, `ft1284` |
| `eeprom_self_powered`  | `%d\n`     | `0`                                        | 1 if self-powered flag set                                             |
| `eeprom_remote_wakeup` | `%d\n`     | `0`                                        | 1 if remote wakeup flag set                                            |
| `eeprom_max_power`     | `%u\n`     | `500`                                      | Maximum power draw in mA                                               |
| `eeprom_cbus`          | multi-line | `CBUS0: tristate\nCBUS1: txled\n`          | Per-pin CBUS function (FT232H: 10 pins)                                |
| `eeprom_drive`         | multi-line | `group0: 4mA slew=fast schmitt=off\n`      | Drive strength, slew rate, Schmitt trigger                             |
| `eeprom_config`        | INI format | `vendor_id=0x0403\nproduct_id=0x6014\n...` | `ftdi_eeprom`-compatible config with `#` warnings                      |

```sh
# Quick read of all EEPROM attributes
for f in /sys/bus/usb/devices/1-2:1.0/eeprom_*; do
    echo "$(basename $f): $(cat $f)"
done
```

#### Binary attribute

| Attribute | Size  | Description                                                |
| --------- | ----- | ---------------------------------------------------------- |
| `eeprom`  | 256 B | Raw EEPROM contents (little-endian, as read from hardware) |

The binary dump is compatible with `ftdi_eeprom`:

```sh
# Dump raw EEPROM to file
cat /sys/bus/usb/devices/1-2:1.0/eeprom > eeprom_backup.bin
hexdump -C eeprom_backup.bin

# Compare with ftdi_eeprom decode
ftdi_eeprom --read-eeprom ft232h.conf
diff <(xxd eeprom_backup.bin) <(xxd ft232h.bin)
```

#### INI config attribute (`eeprom_config`)

The `eeprom_config` attribute outputs an INI-format configuration compatible
with the `ftdi_eeprom` tool's config file parser.  Lines starting with `#`
are warnings for sub-optimal settings (Schmitt trigger off, fast slew rate):

```sh
cat /sys/bus/usb/devices/1-2:1.0/eeprom_config
```

Example output (FT232H):
```ini
vendor_id=0x0403
product_id=0x6014
self_powered=false
remote_wakeup=false
max_power=90
use_serial=true
suspend_pull_downs=false
cha_type=UART
cha_vcp=true
group0_drive=4
group0_schmitt=true
group0_slew=slow
group1_drive=4
group1_schmitt=false
# WARNING: Schmitt trigger off on group1 (ADBUS)
group1_slew=fast
# WARNING: slew rate not limited on group1 (ADBUS)
cbush0=TRISTATE
...
```

Example output (FT4232H, from interface 2 = channel C):
```ini
vendor_id=0x0403
product_id=0x6011
self_powered=false
remote_wakeup=false
max_power=500
use_serial=true
suspend_pull_downs=false
chc_type=UART
chc_vcp=true
chc_rs485=false
group0_drive=4
group0_schmitt=true
group0_slew=slow
group1_drive=4
group1_schmitt=true
group1_slew=slow
```

#### I2C EEPROM validation

When the I2C child driver (`ftdi_i2c.ko`) probes, it checks the EEPROM
drive settings on the data bus (ADBUS) and emits kernel messages for
sub-optimal configurations:

| Condition                  | Severity     | Message                                                  |
| -------------------------- | ------------ | -------------------------------------------------------- |
| Schmitt trigger off        | `dev_warn`   | Schmitt trigger OFF on data bus -- I2C requires Schmitt  |
| Drive current <= 4 mA      | `dev_notice` | data bus drive current is N mA -- consider 8 or 16 mA    |
| Fast slew rate             | `dev_notice` | fast slew rate on data bus -- slow slew recommended      |
| Suspend pull-downs enabled | `dev_notice` | suspend_pull_downs enabled -- may conflict with pull-ups |

Check with: `dmesg | grep "EEPROM.*I2C\|EEPROM.*data bus\|EEPROM.*suspend"`

#### UART EEPROM validation

When the UART child driver (`ftdi_uart.ko`) probes, it checks the EEPROM
drive settings on the data bus (ADBUS) and emits kernel messages for
sub-optimal configurations:

| Condition                  | Severity     | Message                                             |
| -------------------------- | ------------ | --------------------------------------------------- |
| Schmitt trigger off        | `dev_warn`   | Schmitt trigger OFF -- UART requires Schmitt for RX |
| Drive current < 8 mA       | `dev_notice` | data bus drive current is N mA -- 8 mA recommended  |
| Suspend pull-downs enabled | `dev_notice` | suspend_pull_downs -- may cause break on TXD        |
| VCP not enabled (FT232H)   | `dev_notice` | VCP not enabled -- may not appear as serial port    |

At runtime, `set_termios` checks the slew rate against the baud rate
(once per device instance):

| Condition                    | Severity     | Message                                           |
| ---------------------------- | ------------ | ------------------------------------------------- |
| Slow slew at baud > 1 Mbaud  | `dev_notice` | slow slew rate -- consider fast slew for >1 Mbaud |
| Fast slew at baud <= 1 Mbaud | `dev_notice` | fast slew rate -- slow slew recommended           |

Check with: `dmesg | grep "EEPROM.*UART\|EEPROM.*data bus\|EEPROM.*slew\|EEPROM.*VCP\|EEPROM.*suspend"`

#### SPI EEPROM validation

When the SPI child driver (`ftdi_spi.ko`) probes, it checks the EEPROM
drive settings on the data bus (ADBUS) and emits kernel messages for
sub-optimal configurations:

| Condition                  | Severity     | Message                                                 |
| -------------------------- | ------------ | ------------------------------------------------------- |
| Schmitt trigger off        | `dev_warn`   | Schmitt trigger OFF -- SPI requires Schmitt for MISO    |
| Suspend pull-downs enabled | `dev_warn`   | suspend_pull_downs -- all CS# pulled low during suspend |
| VCP driver enabled         | `dev_notice` | VCP enabled -- may prevent MPSSE access for SPI         |

At runtime, `transfer_one_message` checks slew rate and drive current
against the SPI clock speed (once per controller):

| Condition                       | Severity     | Message                                          |
| ------------------------------- | ------------ | ------------------------------------------------ |
| Slow slew at speed > 15 MHz     | `dev_notice` | slow slew rate -- consider fast slew for >15 MHz |
| Fast slew at speed <= 5 MHz     | `dev_notice` | fast slew rate -- slow slew recommended          |
| Drive < 12 mA at speed > 20 MHz | `dev_notice` | drive N mA -- 12+ mA recommended for >20 MHz     |

Check with: `dmesg | grep "EEPROM.*SPI\|EEPROM.*data bus\|EEPROM.*slew\|EEPROM.*CS"`

#### CBUS function names in `eeprom_cbus`

| Name       | Value | Description                |
| ---------- | ----- | -------------------------- |
| `tristate` | 0x00  | High-impedance (default)   |
| `txled`    | 0x01  | Pulses low during TX       |
| `rxled`    | 0x02  | Pulses low during RX       |
| `txrxled`  | 0x03  | Pulses low during TX or RX |
| `pwren`    | 0x04  | Low when USB active        |
| `sleep`    | 0x05  | Low during USB suspend     |
| `drive_0`  | 0x06  | Output driven low          |
| `drive_1`  | 0x07  | Output driven high         |
| `iomode`   | 0x08  | GPIO (bit-bang mode)       |
| `txden`    | 0x09  | RS-485 transmit enable     |
| `clk30`    | 0x0A  | 30 MHz clock output        |
| `clk15`    | 0x0B  | 15 MHz clock output        |
| `clk7_5`   | 0x0C  | 7.5 MHz clock output       |

### Pinmap Sysfs

The core module exposes a read-only `pinmap` attribute on the USB
interface device (alongside the `eeprom_*` attributes) that shows the
kernel's active pin configuration at a glance:

```sh
cat /sys/bus/usb/devices/1-2:1.0/pinmap
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

gpio: gpiochip496
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

gpio: gpiochip496
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

gpio: gpiochip496
  AD0  reserved (ttyFTDI0)
  AD1  reserved (ttyFTDI0)
  ...
  AD7  reserved (ttyFTDI0)
  AC0  available
  AC1  available
  ...
  AC7  available
```

#### Example: FT4232H channel C (non-MPSSE, `bus_mode=spi,i2c,uart,uart`)

```
chip: FT4232H
channel: C
mode: uart (non-MPSSE)

uart: ttyFTDI2
  DBUS0  TXD
  DBUS1  RXD
  DBUS2  RTS
  DBUS3  CTS
  DBUS4  DTR
  DBUS5  DSR
  DBUS6  DCD
  DBUS7  RI

gpio-bitbang: gpiochip504
  DBUS0  reserved (ttyFTDI2)
  DBUS1  reserved (ttyFTDI2)
  DBUS2  available
  DBUS3  available
  DBUS4  available
  DBUS5  available
  DBUS6  available
  DBUS7  available
```

### EEPROM Configuration

Some UART features (RS-485 TXDEN, CBUS GPIO) depend on CBUS pin
assignments stored in the device EEPROM. The EEPROM can be
programmed with `ftdi_eeprom`.

The FT232H has 10 ACBUS pins (ACBUS0-ACBUS9). Each pin can be
assigned one of these functions in the EEPROM:

| Function   | Value | Description                                      |
| ---------- | ----- | ------------------------------------------------ |
| `TRISTATE` | 0x00  | Input, pull-up (default)                         |
| `TXLED`    | 0x01  | Pulses low during TX                             |
| `RXLED`    | 0x02  | Pulses low during RX                             |
| `TXRXLED`  | 0x03  | Pulses low during TX or RX                       |
| `PWREN`    | 0x04  | Low during USB suspend                           |
| `SLEEP`    | 0x05  | Low during USB suspend                           |
| `DRIVE_0`  | 0x06  | Output low                                       |
| `DRIVE_1`  | 0x07  | Output high                                      |
| `IOMODE`   | 0x08  | GPIO (bit-bang mode) -- used by CBUS GPIO driver |
| `TXDEN`    | 0x09  | RS-485 transmit enable -- asserted during TX     |
| `CLK30`    | 0x0A  | 30 MHz clock output                              |
| `CLK15`    | 0x0B  | 15 MHz clock output                              |
| `CLK7_5`   | 0x0C  | 7.5 MHz clock output                             |

The driver reads the full EEPROM at probe and:
- Uses the channel type for auto-mode selection (see [Mode Selection](#mode-selection))
- Exposes decoded fields and raw dump via sysfs (see [EEPROM Sysfs](#eeprom-sysfs))
- Provides CBUS config to the UART child (pins set to `IOMODE` become GPIO,
  pins set to `TXDEN` enable RS-485 support)

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

# Channel configuration
cha_type=UART
cha_rs485=true

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

# GPIO
gpioinfo | grep ftdi

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

- SPI modes 0/1/2/3 (all CPOL/CPHA combinations)
- Clock speed: 30 MHz max, ~457 Hz min (60 MHz base, 16-bit divisor)
- Full duplex, TX-only, and RX-only transfers
- Sub-byte transfers: 1-8 bits per word (MPSSE bit-mode clocking)
- SPI_CS_HIGH: per-device active-high chip select polarity
- Configurable chip selects: 1-5 CS lines on AD3-AD7, per-channel on multi-channel chips (see `spi_cs` below)
- Multi-transfer batching: `transfer_one_message` assembles MPSSE commands into a single buffer for fewer USB round-trips
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
`spi_cs=A:3,4,5;B:3` gives channel A three CS lines and channel B one.
If a channel is not listed, it defaults to CS0=AD3.

### I2C Capabilities

The I2C child driver (`ftdi_i2c.ko`) provides an `i2c_adapter` with:

- Clock speed: 10 kHz to 3.4 MHz (continuous, not just 100/400 kHz)
- 7-bit and 10-bit addressing (`I2C_FUNC_10BIT_ADDR`)
- Hardware open-drain on FT232H (MPSSE opcode 0x9E); software open-drain emulation on FT2232H/FT4232H
- Command batching: one USB round-trip per I2C message
- Clock stretching via MPSSE adaptive clocking (requires AD3 wired to SCL)
- Proper repeated START between multi-message transfers
- Bus recovery: 9-pulse SCL toggle via `i2c_generic_scl_recovery`
- SMBus emulation (`I2C_FUNC_SMBUS_EMUL`)

Module parameters (set at load time):

| Parameter          | Type | Default | Description                                   |
| ------------------ | ---- | ------- | --------------------------------------------- |
| `i2c_speed`        | uint | 100     | I2C bus speed in kHz (10-3400)                |
| `clock_stretching` | bool | 0       | Enable adaptive clocking for clock stretching |

### GPIO Capabilities

#### MPSSE GPIO (`ftdi_gpio.ko`) -- MPSSE-capable channels

The MPSSE GPIO driver provides a 16-pin `gpio_chip` on channels with
MPSSE hardware (FT232H A, FT2232H A/B, FT4232H A/B):

- 16 pins: AD0-AD7 (low byte) + AC0-AC7 (high byte)
- Hardware state sync: pin values read from MPSSE at probe (no stale-zero writes or spurious IRQ edges)
- Open-drain on FT232H: per-pin `PIN_CONFIG_DRIVE_OPEN_DRAIN` / `PIN_CONFIG_DRIVE_PUSH_PULL` via `set_config`
- Reserved pin masking: the parent (`ftdi_mpsse.ko`) passes a per-mode `gpio_reserved_mask` via platform_data
  when creating the GPIO child -- `0x0007` for I2C (AD0-AD2), `0x0000` for UART, and a dynamic mask for SPI
  covering AD0-AD2 (SCK/MOSI/MISO) plus each CS pin (e.g. `0x000f` with CS0=AD3, `0x003f` with CS0-CS2=AD3-AD5).
  `init_valid_mask` hides reserved pins from the GPIO core so userspace cannot access bus-driver pins.
- Polled edge-triggered IRQ: 1 ms delayed_work polling, rising/falling/both
- Per-device quirks: custom pin names and direction overrides
- Transport: MPSSE `SET_BITS_LOW`/`SET_BITS_HIGH` and `GET_BITS_LOW`/`GET_BITS_HIGH` opcodes

#### Bit-Bang GPIO (`ftdi_gpio_bitbang.ko`) -- non-MPSSE channels

The bit-bang GPIO driver provides an 8-pin `gpio_chip` on FT4232H
channels C and D (which lack MPSSE hardware):

- 8 pins: DBUS0-DBUS7 (one byte, no high byte)
- Async bit-bang mode: uses `SET_BITMODE` vendor command (bitmode 0x01) with direct bulk writes, not MPSSE opcodes
- Reserved pin masking: TXD/RXD (DBUS0-DBUS1) are always reserved; DBUS2-DBUS7 are available as GPIO
- Shares bulk endpoints with the UART driver on the same channel
- Lower bandwidth than MPSSE GPIO (one USB round-trip per read/write vs batched MPSSE commands)

### Pin Mapping

#### MPSSE channels (FT232H A, FT2232H A/B, FT4232H A/B)

```
Low byte (AD0-AD7):
  UART mode:  AD0=TXD  AD1=RXD      AD2=RTS     AD3=CTS  AD4=DTR  AD5=DSR  AD6=DCD  AD7=RI
  SPI mode:   AD0=SCK  AD1=MOSI     AD2=MISO    AD3=CS0  AD4-AD7=GPIO/CS1-CS4
  I2C mode:   AD0=SCL  AD1=SDA_OUT  AD2=SDA_IN  AD3-AD7=GPIO

High byte (AC0-AC7):
  All modes:  AC0-AC7 available as GPIO (MPSSE SET_BITS_HIGH)
```

#### Non-MPSSE channels (FT4232H C/D)

```
DBUS0-DBUS7 (8 pins per channel, no high byte):
  UART:       DBUS0=TXD  DBUS1=RXD  DBUS2=RTS  DBUS3=CTS
              DBUS4=DTR  DBUS5=DSR  DBUS6=DCD  DBUS7=RI
  Bit-bang:   DBUS0-DBUS7 (pins used by UART are reserved)
```

Channels C and D have no AC-bus pins. Bit-bang GPIO pins that overlap
with active UART signals are masked as reserved.

## Known Limitations

- GPIO cache coherency: If a `set_bank` USB write fails, the software cache diverges
  from hardware. The next successful write corrects both values.
- Per-interface mode via EEPROM only: The `bus_mode` parameter
  supports comma-separated per-interface values (`spi,uart`), and
  auto mode reads the EEPROM channel type per-channel.
- GPIO clobbers sibling pins: `set_bank` rewrites the entire 8-bit
  bank, overwriting pins owned by SPI/I2C. Mitigated by
  `init_valid_mask` preventing direct GPIO access to reserved pins.
- Bit-bang GPIO bandwidth: On FT4232H channels C/D, GPIO uses async
  bit-bang mode over USB bulk endpoints. Each read or write is a
  separate USB round-trip, unlike MPSSE GPIO which can batch multiple
  pin operations into one transfer.
- Bit-bang GPIO / UART endpoint sharing: On FT4232H channels C/D,
  bit-bang GPIO and UART share the same bulk endpoints. The bus_lock
  mutex serialises access.

## Relationship to Upstream

This driver suite is independent of the in-kernel `ftdi_sio` USB serial
driver. It targets the same FTDI Hi-Speed chips but exposes their MPSSE
engine as SPI, I2C, and GPIO subsystem devices -- functionality that
`ftdi_sio` does not provide. For FT4232H, this driver also handles the
non-MPSSE channels C/D (UART + bit-bang GPIO), providing unified
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

- SPI: edge-triggered clocking (opcodes 0x10-0x36), CLK/5 disabled for 60 MHz base clock
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
      2. Build MPSSE command buffer:
         DISABLE_CLK_DIV5              -- 60 MHz base clock
         SET_CLK_DIVISOR(div)          -- freq = 60M / ((1+div)*2)
         SET_BITS_LOW(val, dir)        -- configure SCK/MOSI/CS pins
         CS assert (SET_BITS_LOW)      -- drive CS# low
         CLK_BYTES_OUT/IN/INOUT        -- clock data (opcode+len+data)
         CS deassert (SET_BITS_LOW)    -- drive CS# high
      3. ftdi_mpsse_xfer(tx_buf, tx_len, rx_buf, rx_len)
           -> ftdi_mpsse_bulk_write()   -- USB bulk OUT
           -> ftdi_mpsse_bulk_read()    -- USB bulk IN (strips 2 status bytes)
      4. ftdi_mpsse_bus_unlock()
```

The main optimisation: `transfer_one_message` batches all MPSSE commands for
an entire `spi_message` (multiple transfers) into a single buffer before
flushing, minimising USB round-trips.

### Data Flow: I2C Transfer

```
userspace ioctl(fd, I2C_RDWR, &msgs)
  -> i2c_transfer() -> i2c_algorithm.xfer()
    -> ftdi_i2c_xfer()  [ftdi_i2c.c]
      1. ftdi_mpsse_bus_lock()
      2. For each i2c_msg:
         START or repeated-START condition (SET_BITS_LOW sequence)
         Address byte (CLK_BITS_OUT + read ACK via CLK_BITS_IN)
         Data bytes:
           Write: CLK_BITS_OUT + read ACK
           Read:  CLK_BITS_IN + send ACK/NACK
         SEND_IMMEDIATE
      3. ftdi_mpsse_xfer() -> single USB round-trip per message
      4. Parse response: check ACK bits, copy read data
      5. STOP condition (SET_BITS_LOW sequence)
      6. ftdi_mpsse_bus_unlock()
```

The I2C driver uses 3-phase clocking (opcode 0x8C) which inserts an
extra clock phase for the I2C ACK/NACK bit. Open-drain is handled
differently per chip:
- FT232H: hardware open-drain via MPSSE opcode 0x9E (`DRIVE_ZERO_ONLY`)
- FT2232H/FT4232H: software open-drain -- drive low for '0', switch
  pin to input (tristate) for '1', relying on external pull-ups

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
   if (disconnect) wake_up(disconnect_wq)
```

Disconnect uses `usb_poison_urb()` (not `usb_kill_urb()`) because
poison is permanent -- it cancels in-flight URBs AND causes all future
`usb_submit_urb()` to return `-EPERM`. This closes the TOCTOU race
where a transport function passes the disconnect check, then submits a
URB after kill has completed.

Suspend uses `usb_kill_urb()` (not poison) because suspend is
reversible -- URBs need to be submittable again after resume.

### Synchronisation Primitives

| Primitive           | Location            | Description                                |
| ------------------- | ------------------- | ------------------------------------------ |
| `bus_lock` (mutex)  | ftdi_mpsse_core.c   | Whole-transaction exclusion across drivers |
| `io_lock` (mutex)   | ftdi_mpsse_core.c   | Serialises USB bulk transfers              |
| `disconnect_lock`   | ftdi_mpsse_core.c   | Guards disconnect/suspend state            |
| `write_lock` (spin) | ftdi_uart.c         | Protects UART tx_outstanding flag          |
| `fg->lock` (mutex)  | ftdi_gpio.c         | Serialises GPIO cache + MPSSE I/O          |
| `fg->lock` (mutex)  | ftdi_gpio_bitbang.c | Serialises bit-bang GPIO + bulk I/O        |

Lock ordering: `bus_lock` -> `fg->lock` -> `io_lock` -> `disconnect_lock`

On non-MPSSE channels (FT4232H C/D), `bus_lock` serialises access between
the UART driver and the bit-bang GPIO driver, since both share the same
bulk endpoints.

### EEPROM Auto-Detection Cascade

When `bus_mode` is empty (auto mode), the driver evaluates these sources
in order to decide which child devices to create:

```
1. bus_mode module parameter     -> if set: use directly
2. EEPROM channel type byte      -> UART(0x00), FIFO/OPTO/CPU/FT1284 rejected
3. EEPROM protocol hint (0x1A)   -> 'S'=SPI, 'I'=I2C, 'U'=UART
4. VCP driver bit                -> if set: UART
5. Product string keywords       -> contains "SPI" or "I2C"
6. Electrical characteristics    -> slow slew + Schmitt = I2C
7. Default                       -> SPI (most common MPSSE use case)
```

Steps 3-7 are implemented in `ftdi_mpsse_detect_protocol()`.  The
cascade exits at the first match.

### Module Parameter Reference

All module parameters across the driver suite:

| Module       | Parameter          | Type   | Default | Description                                                                                                                                                                                                                                                     |
| ------------ | ------------------ | ------ | ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ftdi_mpsse` | `bus_mode`         | string | `""`    | Channel mode: `uart`, `spi`, `i2c`, or empty for auto. Comma-separated for per-interface (`spi,uart` for FT2232H; `spi,i2c,uart,uart` for FT4232H). Non-MPSSE channels (FT4232H C/D) only accept `uart`; other values produce a warning and are forced to UART. |
| `ftdi_mpsse` | `spi_cs`           | string | `"3"`   | SPI chip-select AD pin numbers (3-7). Global: `3,4,5`. Per-channel: `A:3,4,5;B:3`.                                                                                                                                                                              |
| `ftdi_mpsse` | `autosuspend`      | bool   | `0`     | Enable USB autosuspend. Chip loses all state during suspend                                                                                                                                                                                                     |
| `ftdi_spi`   | `register_spidev`  | bool   | `1`     | Auto-register `/dev/spidevX.Y` devices for userspace access                                                                                                                                                                                                     |
| `ftdi_i2c`   | `i2c_speed`        | uint   | `100`   | I2C bus speed in kHz (10-3400)                                                                                                                                                                                                                                  |
| `ftdi_i2c`   | `clock_stretching` | bool   | `0`     | Enable adaptive clocking (requires AD3 wired to SCL)                                                                                                                                                                                                            |

sysfs attributes (runtime-tuneable):

| Module       | Attribute       | Path            | Description                                             |
| ------------ | --------------- | --------------- | ------------------------------------------------------- |
| `ftdi_uart`  | `latency_timer` | platform device | RX buffer timeout in ms (1-255, default 16)             |
| `ftdi_uart`  | `dtr_dsr_flow`  | platform device | Enable DTR/DSR hardware flow control (0 or 1)           |
| `ftdi_mpsse` | `pinmap`        | USB interface   | Read-only: active pin assignments and GPIO availability |
| `ftdi_mpsse` | `eeprom_*`      | USB interface   | Read-only: decoded EEPROM fields                        |
| `ftdi_mpsse` | `eeprom`        | USB interface   | Read-only: raw 256-byte EEPROM binary                   |

## Development

### Prerequisites

- Linux kernel headers (for module build): `apt install linux-headers-$(uname -r)`
- For UML-based testing (x86 only):
  - Linux kernel source tree (default: `~/dev/linux`, override: `LINUX_SRC=`)
  - Static BusyBox: `apt install busybox-static`
  - GCC, `cpio`, `gzip`
- For static analysis:
  - `sparse` (endian/lock checking)
  - `checkpatch.pl` (from kernel source)
  - `coccinelle` for semantic patches

### Build and Test Iteration Loop

The fast development cycle is:

```sh
# One-time setup: build the UML kernel (~2-5 min)
make -C tests uml-kernel

# Edit driver source, then test (~5 sec per test):
make -C tests test-spi        # rebuilds modules + initramfs automatically

# Run all default tests (SPI + I2C + UART):
make test

# Run everything including multi-channel:
make -C tests test-all

# Static analysis before submitting:
make check                     # sparse + checkpatch
```

The `tests/Makefile` dependency chain handles everything: if you edit
`ftdi_spi.c` and run `make -C tests test-spi`, it rebuilds the modules
against the UML kernel, rebuilds the initramfs, and runs the test -- all
in one command.

### Running a Single Test Interactively

For debugging, you can run UML directly:

```sh
# Build everything first
make -C tests initramfs

# Run UML with console output and the test mode you want
tests/.uml-build/linux \
    initrd=tests/initramfs.cpio.gz \
    mem=256M \
    ftdi_mode=spi \
    con=null con0=fd:0,fd:1
```

The full kernel dmesg is printed at the end.  The `ftdi_mode=` parameter
selects the test (see `tests/init.sh` for all modes).

### Running the Emulator Standalone

For protocol-level debugging outside UML:

```sh
# Terminal 1: start the emulator
ftdi-emulation/ftdi_usbip --chip ft232h --mode spi

# Terminal 2: attach via USB/IP
sudo modprobe vhci-hcd
sudo usbip attach -r 127.0.0.1 -b 1-1

# Load the driver
sudo insmod ftdi_mpsse.ko bus_mode=spi spi_cs=3,4,5
sudo insmod ftdi_spi.ko
sudo insmod ftdi_gpio.ko
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id

# Now you can test with real tools (spidev_test, gpioinfo, etc.)
```

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
   // Use ftdi_mpsse_xfer(), ftdi_mpsse_write(), ftdi_mpsse_read()
   // Always wrap multi-step operations in bus_lock/bus_unlock
   ftdi_mpsse_bus_lock(pdev);
   ret = ftdi_mpsse_xfer(pdev, tx, tx_len, rx, rx_len);
   ftdi_mpsse_bus_unlock(pdev);
   ```

3. **Add to `Makefile`**: append `ftdi_foo.o` to `obj-m`

4. **Add MFD cell** in `ftdi_mpsse_core.c`: create `mfd_cell` and
   `ftdi_mpsse_pdata` entries, add to the appropriate mode's cell
   array in the probe function

5. **Add to test initramfs**: update `tests/Makefile` and `tests/init.sh`

Key invariants:
- Always hold `bus_lock` around multi-step MPSSE transactions
- Transport functions check `disconnect`/`suspended` internally
- `can_sleep = true` for GPIO chips -- all I/O goes over USB
- Use `devm_*` allocation where possible

For **non-MPSSE children** (e.g. `ftdi_gpio_bitbang`): use
`ftdi_mpsse_cfg_msg()` for vendor control messages and
`ftdi_mpsse_get_endpoints()` for direct bulk I/O, instead of
`ftdi_mpsse_xfer()`.  Check `ftdi_mpsse_is_mpsse_capable()` at
probe to adapt behaviour.

### How to Add a New Test

1. **Add a test mode** to `tests/init.sh`:
   - Add a `case` entry in the `ftdi_mode` parser (line ~35)
   - Add test assertions using the `check` function

2. **Add a Makefile target** to `tests/Makefile`:
   ```make
   test-foo: $(INITRAMFS)
   	$(call run_test,foo)
   ```

3. **(Optional) Add a C test tool** if userspace I/O testing is needed:
   - Create `tests/foo_test.c`
   - Must compile with `-static` (runs in initramfs)
   - Add build rule and include in `$(INITRAMFS)` dependencies

4. **Emulator error injection** (for error path tests):
   - The emulator supports `--error TYPE --error-count N`
   - Available modes: `i2c-nak`, `i2c-stuck`, `usb-stall`, `usb-timeout`
   - To add a new mode: add enum in `ftdi_emu.h`, handle in
     `ftdi_emu_bulk_out()` in `ftdi_emu.c`

### Test Log Files

Test output is saved to `tests/test-<mode>.log`.  Each log contains:
- PASS/FAIL results for all assertions
- Debug output from test tools
- Full kernel dmesg (useful for driver debug messages and stack traces)

### Test Coverage

| Component                   | Status | Notes                                  |
| --------------------------- | ------ | -------------------------------------- |
| USB probe/detect            | Tested | FT232H, FT2232H, FT4232H               |
| UART ttyFTDI creation       | Tested | With data transfer (uart_test)         |
| EEPROM decode               | Tested | Checksum verified                      |
| GPIO registration + I/O     | Tested | gpio_test reads/writes pin 8 (AC0)     |
| SPI transfers               | Tested | spi_test via spidev                    |
| I2C transfers               | Tested | i2c_test via i2c-dev                   |
| Hot-unplug                  | Tested | SPI + GPIO during disconnect           |
| Suspend/resume              | Tested | SPI + GPIO across USB suspend          |
| Multi-channel (FT2232H)     | Tested | Both channels probed, per-channel GPIO |
| Multi-channel (FT4232H A/B) | Tested | A+B MPSSE (SPI/I2C), per-channel GPIO  |
| FT4232H C/D UART            | Tested | uart_test on non-MPSSE channels        |
| FT4232H C/D bit-bang GPIO   | Tested | gpio_test on bit-bang GPIO pins        |
| I2C NAK error path          | Tested | Error injection via emulator           |
| I2C bus stuck recovery      | Tested | Frozen SDA simulation                  |

## Troubleshooting

### `ftdi_sio` Claims the Device

The most common issue: the in-kernel `ftdi_sio` driver binds to FTDI
devices before `ftdi_mpsse` can.  Solutions:

```sh
# Method 1: unbind ftdi_sio, use driver_override
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
echo ftdi_mpsse | sudo tee /sys/bus/usb/devices/1-2:1.0/driver_override
echo 1-2:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind

# Method 2: blacklist ftdi_sio
echo "blacklist ftdi_sio" | sudo tee /etc/modprobe.d/ftdi.conf

# Method 3: use new_id (runtime, triggers reprobe)
echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
```

### Device Not Probed (No Child Devices)

Check dmesg for the reason:
```sh
dmesg | grep ftdi_mpsse
```

Common causes:
- `unsupported FTDI chip` -- the `bcdDevice` value doesn't match any
  known chip type
- `EEPROM channel type 0xNN not supported` -- FIFO/OPTO/CPU/FT1284 modes
  are not implemented
- `unknown bus_mode` -- typo in the module parameter
- `bus_mode 'spi' not supported on non-MPSSE channel` -- FT4232H C/D
  only support UART; the driver warns and forces UART mode

### Empty EEPROM (All 0xFF)

Factory-fresh FTDI chips may have an empty EEPROM.  The driver handles
this gracefully:
- Reports `EEPROM: empty (all 0xFF)` in dmesg
- Falls through to default SPI mode (or respects `bus_mode` parameter)
- Use `tools/ftdi_eeprom_check` to verify: `sudo tools/ftdi_eeprom_check`

### SPI/I2C Not Working

Verify the mode was set correctly:
```sh
cat /sys/bus/usb/devices/1-2:1.0/pinmap    # shows active mode and pin mapping
dmesg | grep "EEPROM hints"                 # shows auto-detection result
```

If in the wrong mode, either set `bus_mode` explicitly or program the
EEPROM protocol hint byte.

### UML Test Failures

```sh
# Check the full log
cat tests/test-<mode>.log

# Look for kernel oops or warnings
grep -E "FAIL:|WARNING:|Oops:" tests/test-<mode>.log

# Rebuild everything clean
make -C tests clean
make -C tests uml-kernel    # if kernel source changed
make -C tests test-spi
```

Common test issues:
- `UML requires an x86 host` -- UML only works on x86_64/i686
- Missing `busybox-static` -- `apt install busybox-static`
- Missing kernel source -- set `LINUX_SRC=/path/to/linux`

## License

GPL-2.0+
