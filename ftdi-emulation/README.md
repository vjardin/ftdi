<!-- SPDX-License-Identifier: GPL-2.0 -->
# FTDI USB/IP Device Emulator

Test the `ftdi_mpsse` kernel driver without physical hardware by
emulating an FTDI device over USB/IP.

## Build

```sh
make
```

## Example: emulate an FT232H and probe it with ftdi_mpsse

Open two terminals on the same machine.

### Terminal 1 -- start the emulator:

```
$ ./ftdi_usbip --chip ft232h --mode spi
Emulating ft232h (VID=0403 PID=6014 bcdDevice=0900) mode=spi
usbip: listening on port 3240 (VID=0403 PID=6014 bcdDevice=0900)
```

`--mode` only shapes the EEPROM the emulated chip carries; the driver's
auto mode then reads it as it would on a real board.

### Terminal 2 -- load vhci-hcd, attach the virtual device, load the driver:

```
$ sudo modprobe vhci-hcd

$ usbip list -r 127.0.0.1
Exportable USB devices
======================
 - 127.0.0.1
      1-1: Future Technology Devices International, Ltd (0403:6014)

$ sudo usbip attach -r 127.0.0.1 -b 1-1

$ sudo insmod ../ftdi_mpsse.ko
$ sudo insmod ../ftdi_uart.ko
$ sudo insmod ../ftdi_spi.ko register_spidev=1
$ sudo insmod ../ftdi_i2c.ko
$ sudo insmod ../ftdi_gpio.ko
$ sudo insmod ../ftdi_gpio_bitbang.ko

# The USB ID table is empty by default (avoids ftdi_sio conflict).
# Bind the device via new_id; 0403:6014 is in ftdi_sio's table, so take
# the interface from ftdi_sio if it is loaded:
$ echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id
$ echo 3-1:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind
$ echo 3-1:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/bind

$ dmesg | tail
usb 3-1: new high-speed USB device number 2 using vhci_hcd
ftdi_mpsse 3-1:1.0: EEPROM: VID=0x0403 PID=0x6014 chan_a=0 checksum=ok
ftdi_mpsse 3-1:1.0: auto: EEPROM hints SPI mode (bus_mode tag)
ftdi-spi ftdi-spi.0.auto: registered 1 spidev device(s)
ftdi-gpio ftdi-gpio.1.auto: FTDI MPSSE GPIO: 16 pins, reserved mask 0x000f
ftdi_mpsse 3-1:1.0: FTDI MPSSE core: channel A, mode 'spi' (auto)

$ ls /dev/spidev*
/dev/spidev0.0

$ sudo cat /sys/kernel/debug/ftdi_mpsse/3-1:1.0/pinmap
```

With `--mode uart` the channel comes up as `/dev/ttyFTDI0` instead.

When done, detach and clean up:

```
$ sudo usbip detach -p 0
$ sudo rmmod ftdi_spi ftdi_i2c ftdi_uart ftdi_gpio ftdi_gpio_bitbang ftdi_mpsse
```

## Example: emulate an FT2232H with a captured EEPROM

Dump the EEPROM from a real device first (e.g. with `ftdi_eeprom`, or
the driver's `eeprom` sysfs file), then replay it. The image is used
byte for byte:

```
$ ./ftdi_usbip --chip ft2232h --eeprom my_eeprom.bin
Loaded EEPROM from my_eeprom.bin
Emulating ft2232h (VID=0403 PID=6010 bcdDevice=0700) mode=spi
usbip: listening on port 3240 (VID=0403 PID=6010 bcdDevice=0700)
```

The FT2232H exposes two interfaces, so the driver creates two
independent channels (A and B) each with their own child devices.

## Options

```
--chip TYPE       Chip to emulate: ft232h (default), ft2232h, or ft4232h
--mode MODE       What the generated EEPROM asks for: spi (default),
                  i2c or uart. spi/i2c: "bus_mode=MODE" tag in the user
                  area and D2XX on the MPSSE channels; uart: VCP driver
                  bit on every channel
--ee-tag TEXT     User-area text instead of the --mode default, e.g.
                  "bus_mode=i2c,spi"; "none" for no user data
--ee-product STR  Product string instead of the chip default
--ee-vcp CHANS    Channels with the VCP driver bit, e.g. "ab"; "none"
--ee-set OFF=VAL  Set one EEPROM byte after the image is built
                  (repeatable); the checksum is recomputed
--ee-93c46        A 93C46 instead of a 93C56/66: 64 words, read back
                  mirrored, with no room for the user-area text
--ee-blank        Blank EEPROM (all 0xFF)
--ee-bad-checksum Corrupt the checksum of the image
--eeprom FILE     Use an EEPROM image file as it is: 256 bytes, or 128
                  for a 93C46 (read back mirrored).  --mode and --ee-*
                  do not apply
--port PORT       TCP listen port (default: 3240)
--error TYPE      Error injection, armed at power-on: i2c-nak, i2c-stuck,
                  i2c-stretch, i2c-stretch-short, i2c-scl-stuck,
                  i2c-stuck-byte, usb-stall, usb-timeout, mpsse-sync,
                  mpsse-stall, mpsse-extra, uart-line
--error-count N   Number of errors to inject (default: 1, 0=infinite);
                  i2c-stuck, i2c-scl-stuck and mpsse-sync are states,
                  not events: they hold until "error none"
--error-skip N    Let N occasions for the error pass first (default: 0)
--error-chan C    Channel the error is for, A to D (default: all)
--ctl FIFO        Control FIFO (created if missing) for run-time commands
--wire-ad7        Board has AD7 (GPIOL3) wired to AD0 (SCL): I2C clock
                  stretching through MPSSE adaptive clocking
--spi-echo        Board has four shift-register SPI slaves, one per SPI
                  mode, selected by AD4 (mode 0) to AD7 (mode 3)
--spi-cs-high     Board has a second counter device on AD4, selected by
                  a high level (pull-down on the line)
--uart-pace       UART data leaves the chip at the baud rate; without
                  it, as fast as the receiver has room for it
--help            Show usage
```

Signals: `SIGUSR1` power-cycles the chip (every channel back to its
power-on state, as across a USB suspend; the USB configuration is
kept), `SIGUSR2` prints per-channel statistics to stderr.

With the statistics comes what each channel holds and a power cycle
loses, which the suspend/resume tests compare before and after:

```
emu: state chA bitmode=0x02 lo=28/2b/20 hi=01/03/00 cbus=0/0 baud=9600 latency=16
```

`lo` and `hi` are the two ports as value driven / direction / open-drain
mask (`0x9E`); `cbus` is value / direction of the CBUS bit-bang pins.

`out_held` of the `emu: stats` line counts the bulk OUT transfers the
chip did not take at once (see "USB buffers" below). The UART of every
channel has its own line:

```
emu: uart chA tx=16384 rx_overruns=0 rx_holds=1 breaks=0 flow=0x01 txden=ACBUS4 txden_pulses=2
```

`tx` characters sent on TXD, `rx_overruns` characters lost by a full
receive buffer, `rx_holds` the times the receiver held its sender
through the flow control `flow` (the value of `SET_FLOW_CTRL`), `breaks`
the breaks received, `txden` the pin the EEPROM of this chip puts the
RS-485 transmit enable on (`none` without one), asserted `txden_pulses`
times.

A channel whose board is an I2C bus also prints what the bus model saw:

```
emu: i2c chA starts=13 rstarts=7 stops=13 rx=293 tx=290 naks=1 stretches=0 timing_errs=0 proto_errs=0 scl_hz_max=83333 scl_hz_min=83333 recovery_clocks=0 busy_clocks=0
```

`scl_hz_max`/`scl_hz_min` are the fastest and slowest SCL rate between
two rising edges of one clocking command, from the engine's clock (whole
ns, so 400 kHz reads 400160 Hz); `recovery_clocks` counts the SCL rises
made by pin writes while a slave (or the `i2c-stuck` fault) holds SDA
low, whatever the master drives on SDA (with `set_sda` the recovery
pulls it low before each rise, for a STOP): the pulses of a bus
recovery, and the STOP that ends a failed transfer. The edges of an
MPSSE reset do not count. `busy_clocks` counts the SCL rises the engine
clocks while a fault (`i2c-stuck`, `i2c-stuck-byte`) holds SDA low: bits
a slave left in the middle of a byte would take as data.

A channel whose board is SPI prints the frames its slaves saw and the
fastest and slowest SCK rate inside one clocking command:

```
emu: spi chA frames=4 proto_errs=0 sck_hz_max=1000000 sck_hz_min=1000000
```

A vendor request the chip would refuse is answered with a STALL and
logged (`emu: vendor request STALLed: ...`); `tests/init.sh` fails on it.

### Run-time control (`--ctl`)

While a client is attached, the emulator reads one command per line from
the FIFO:

```sh
echo "error usb-timeout 1" > /tmp/emu.ctl   # arm an injection on every channel
echo "error i2c-nak 1 1 B" > /tmp/emu.ctl   # TYPE [COUNT [SKIP [CHAN]]]
echo "pin A C1 0" > /tmp/emu.ctl            # the board drives ACBUS1 low
echo "pin A C1 z" > /tmp/emu.ctl            # ... and lets it go
echo "power-cycle" > /tmp/emu.ctl           # as SIGUSR1
echo "stats" > /tmp/emu.ctl                 # as SIGUSR2
```

This is how a fault is injected after the driver has probed: an
injection given with `--error` is armed from power-on and is often
consumed by the probe itself. `SKIP` lets that many occasions pass
before the fault: `error i2c-nak 1 1` acknowledges the first address
and refuses the one after the repeated START. `CHAN` (A to D) aims it
at one channel.

`pin CHAN D<n>|C<n> 0|1|z` makes the board drive a pin of the low byte
(`D`) or of the high byte (`C`), or lets it go: a GPIO input then reads
the level, and its edge detection sees the change. The board keeps
driving across a power cycle of the chip.

### Error injections for the driver's recovery

| `--error` / `error` | Effect |
| ------------------- | ------ |
| `usb-timeout`       | one bulk IN URB never completes |
| `usb-stall`         | the next bulk IN URB gets a STALL, and the endpoint stays halted until CLEAR_FEATURE(ENDPOINT_HALT) |
| `mpsse-stall`       | the next MPSSE read command hangs for 3 s (as on RTCK with a slave stretching the clock) and then answers; a purge only drops what the host sent behind it, a bitmode reset aborts it |
| `mpsse-extra`       | one stray byte sits in the RX FIFO ahead of the next response |

### Error injections on the I2C bus

| `--error` / `error` | Effect |
| ------------------- | ------ |
| `i2c-nak`           | the slave does not acknowledge its address (with `SKIP`: the address after that many acknowledged ones, e.g. the one of a repeated START) |
| `i2c-stuck`         | a slave holds SDA low |
| `i2c-stretch`       | the addressed slave holds SCL low for 20 ms after the 9th clock of a byte, COUNT times (0 = every byte) |
| `i2c-stretch-short` | the addressed slave holds SCL low after the 9th clock of a byte until 2 µs after the master lets it go, inside the setup time of a repeated START or STOP in standard mode; COUNT times (0 = every byte) |
| `i2c-scl-stuck`     | a slave holds SCL low for good |
| `i2c-stuck-byte`    | a slave stuck in a byte holds SDA low from the next SCL fall and lets go after three pulses of a bus recovery; a START before the STOP that should follow is an I2C protocol error |

## What is emulated

The behaviour follows the FTDI datasheets (DS_FT232H, DS_FT2232H,
DS_FT4232H) and application notes (AN_108 MPSSE commands, AN2232C-02
bit-bang, AN232B-04 latency timer):

- **Channels**: FT232H has one (A), FT2232H two (A/B, both MPSSE),
  FT4232H four (A/B MPSSE without the ACBUS/BCBUS high byte, C/D UART or
  bit-bang only). Opcode `0x9E` (drive-zero / open drain) and CBUS
  bit-bang exist on FT232H only.
- **EEPROM**: the generated image is laid out like one written by
  `ftdi_eeprom` or FT_PROG: channel type UART, one VCP driver bit per
  channel (bit 4 of byte 0 on FT232H; bit 3 of bytes 0/1 for A/B and
  bit 7 for C/D otherwise), user data at `0x20`, strings at `0xa0`,
  valid checksum. The `--ee-*` options change one aspect at a time so
  that each detection path of the driver's auto mode can be exercised.
  `--ee-93c46` builds the 64-word image of the small EEPROM: strings
  right behind the fixed part, checksum in word 63, read back mirrored
  in words 64-127. `WRITE_EEPROM` and `ERASE_EEPROM` take the word
  address in `wIndex`.
- **USB descriptors**: as the chip does, the device presents the vendor
  and product IDs, the strings, the power attributes and the serial
  number of its EEPROM when the image is valid (programmed, good
  checksum), and its built-in ones otherwise (`0403:6014`/`6010`/`6011`,
  bus powered, 90 mA, no serial number). `bcdDevice` is the chip's.
- **USB device state**: plugging in (every USB/IP import) power-cycles
  the chip and leaves it unconfigured; until `SET_CONFIGURATION`, only
  standard requests are answered and vendor requests or bulk transfers
  end in `-EPROTO`, as an URB sent to a vanished USB address would.
  `vhci_hcd` can deliver URBs of the previous device instance to a new
  import before usbcore has disconnected it; they never reach the chip.
- **Bulk IN**: every 512-byte Hi-Speed packet starts with the modem and
  line status bytes; data waits in a persistent RX FIFO until a packet
  is full, `SEND_IMMEDIATE` (`0x87`) arrives, a modem status line
  changes or the latency timer expires. Only a purge request discards
  unread data. IN URBs stay pending until data is due; `CMD_UNLINK`
  answers `-ECONNRESET`. A transfer with less room than the packet the
  chip sends ends with `-EOVERFLOW` (babble) and the packet is lost.
- **USB buffers**: each direction of a channel has a buffer of the size
  of the chip's (1 KiB on FT232H, 4 KiB on FT2232H, 2 KiB on FT4232H).
  A bulk OUT transfer completes only as the buffer from the host takes
  its data: the MPSSE empties it at the pace of its commands, a UART at
  the pace of its transmitter. The MPSSE stops before a command whose
  answer has no room in the buffer to the host, until the host reads.
  A purge of the buffer from the host (`PURGE_RX`) drops what the engine
  has not taken yet; the command in execution goes on and still answers.
- **MPSSE**: the complete AN_108 command set, commands may span OUT
  transfers; an unknown opcode with bit 7 set answers `0xFA,<op>` and
  parsing resumes at the next byte, like the chip (AN_108 3.1). Below
  `0x80` the chip would decode a shifting command from the bits: the
  emulator gives no answer and reports a bad command all the same.
- **MPSSE data shifting**, edge by edge (AN_108 2.2, 3.3, 6.3): a clock
  pulse toggles the CLK pin twice from the level `SET_BITS_LOW` left it
  at; the first bit is put on DO before the pulse, DO changes on the edge
  the opcode names and DI is sampled just before the edge it names; with
  3-phase clocking DO only changes when the next bit starts. Loopback
  (`0x84`) samples DO. A command pair that does not match the idle
  level of the clock therefore shifts the data by one bit, as documented.
- **MPSSE time**: the engine has a clock in ns. A command without
  clocking takes 80 ns (the low end of what was measured on an FT232H);
  clock commands take their bits at 60 MHz (12 MHz with divide-by-5) /
  ((1 + divisor) * 2), 1.5 times longer with 3-phase clocking; with
  adaptive clocking every clock edge waits for RTCK on AD7. A response
  only reaches the RX FIFO when the engine gets to it: a pin read queued
  behind a slow transfer answers when the transfer is over. Commands
  are taken from the buffer one at a time, a byte-mode shift one data
  byte at a time, never more than 1 ms ahead of real time: what the
  host has sent beyond that is still in the buffer, where a purge or an
  MPSSE reset finds it. Wait-on-I/O (`0x88`, `0x89`) stops the engine
  until GPIOL1 has the level; the clock commands (`0x8E`, `0x8F`,
  `0x94`, `0x95`, `0x9C`, `0x9D`) pulse TCK, the last four until GPIOL1
  has the level asked for.
- **Pins**: inputs have pull-ups and read high unless driven low, except
  ACBUS7 of the FT232H, pulled down (DS_FT232H 3.4); `0x9E` makes a '1'
  output high-impedance. The board can drive any pin (`pin` command).
  CBUS bit-bang only moves the pins the EEPROM gives the I/O function
  (ACBUS5, 6, 8, 9).
- **I2C bus** (from "enable 3-phase clocking" on, i.e. under the I2C
  driver): AD0 is SCL, AD1 and AD2 are tied together to SDA, both lines
  are the wired-AND of the master and the slaves; with `--wire-ad7`, AD7
  senses SCL. The slaves decode START, STOP, the address, the direction
  and the ACK bits from the edges of the lines:
  - counter devices at `0x20`, `0x22`, `0x50` and `0x51`: every byte read
    is the next value of one counter (`0xA0`, `0xA1`, ...);
  - a memory device at `0x57`, like a small EEPROM: the first byte
    written is the offset, the following ones are stored when the STOP
    arrives, reads return the memory from the offset on;
  - no other address is acknowledged.
  
  START hold, repeated START and STOP setup, bus free time, SCL low and
  high times and data setup are measured, in engine time, against the
  standard-mode or fast-mode minimums of UM10204 according to the bit
  rate the divisor gives.
- **SPI** (from "disable 3-phase clocking" on, and at power-on): a
  counter device behind AD3, selected by a low level, shifts `0xA0`,
  `0xA1`, ... out on MISO, MSB first, a bit per falling SCK edge after a
  rising one (SPI modes 0 and 3); MISO is pulled up while nothing is
  selected, so the other chip selects read `0xFF`. With
  `--spi-cs-high` a second counter device sits on AD4 and is selected
  by a high level (the two share the counter and MISO: both selected at
  once is an `SPI protocol` error). With `--spi-echo`, AD4-AD7 instead
  select four shift-register slaves, for SPI modes 0 to 3: an 8-bit
  delay line between MOSI and MISO, clocked on the edges of its mode,
  so that a byte comes back as the next byte only if the master changes
  and samples the data on the right edges. One line per frame of these
  reports the bits clocked and the times around the chip select:
  `SPI frame cs=AD4 mode=0 bits=400 setup_ns=... hold_ns=...
  inactive_ns=... wgap_ns=...` (chip select to first clock edge, last
  clock edge to chip select, chip select high before the frame, shortest
  gap between two 8-bit words).
- **UART**: a loopback plug is attached (TXD->RXD, RTS->CTS,
  DTR->DSR+DCD); the baud divisor is decoded and logged. Data waits in
  the chip's TX buffer and leaves it at the baud rate with
  `--uart-pace`, else as fast as the receive buffer has room (a wire
  without a rate never overruns its receiver); THRE/TEMT are reported
  only when the TX buffer is empty. With 7 data bits the eighth is not
  transmitted. A break is received as a NUL, the last byte of its
  packet, with BI in the line status; TXD carries nothing else while it
  lasts. A receive buffer that is full loses what arrives and reports
  OE. Flow control (AN232B-04 4.3): with RTS/CTS or DTR/DSR the
  transmitter only sends while CTS# or DSR# is active, and the receiver
  drops RTS# or DTR# when its buffer is nearly full (64 bytes left,
  until half of it is free: the documents give no figures); with
  XON/XOFF it sends XOFF and XON instead, which the plug returns to the
  transmitter, and the two characters are taken out of the received
  data. The RS-485 transmit enable is reported with the pin the EEPROM
  puts it on (`emu: uart` statistics).
- **Bit-bang** (bitmode `0x01`): the direction mask comes with
  `SET_BITMODE`, bulk OUT bytes drive the outputs, `READ_PINS` returns
  the pin levels.

Things a real chip would not tolerate are logged to stderr, and the UML
tests fail on them:

| Log message                                           | Meaning                                       |
| ----------------------------------------------------- | --------------------------------------------- |
| `MPSSE bad command 0x..`                              | opcode the chip rejects                       |
| `SET_BITMODE MPSSE ignored -- ... has no MPSSE`        | MPSSE on FT4232H C/D                          |
| `... no high-byte pins on this chip`                   | `SET/GET_BITS_HIGH` on FT4232H                |
| `SPI read with no chip select asserted`                | clocking data in with every CS idle           |
| `RX FIFO full (N bytes) and the buffer from the host too: ... a real chip stalls` | a write whose answers fill the RX FIFO while the host is not reading: the engine and the write wait for each other |
| `I2C timing: tHD;STA 320 ns < 4000 ns (standard mode)` | a time below the UM10204 minimum (also `tSU;STA`, `tSU;STO`, `tBUF`, `tLOW`, `tHIGH`, `tSU;DAT`) |
| `I2C protocol: write to the memory device not ended by a STOP` | the slave lost a STOP (or got a START instead) |
| `I2C protocol: contention: SDA driven high by the master, pulled low by a slave` (also SCL) | a line driven high push-pull (an output at '1' without `0x9E`) against a slave |
| `I2C protocol: START with no STOP after a stuck slave let go of SDA` | a bus recovery that did not end with a STOP (`i2c-stuck-byte`) |
| `I2C protocol: address after a repeated START below tSU;STA, SCL let go by a slave just before` | the master moved SDA without knowing that SCL had been high long enough (`i2c-stretch-short`) |
| `SPI protocol: mode N slave on ADx: selected with SCK at its active level` | chip select asserted with the clock at the wrong idle level |
| `SPI protocol: counter device on ADx: selected while another slave is` | two slaves driving MISO: a chip select at the wrong level |
| `adaptive clocking: RTCK (AD7) never follows TCK` | adaptive clocking without the AD7 wire: the engine stops for good |
| `vendor request STALLed: ...`                         | a request the chip refuses (wrong channel, missing wLength) |
| `unhandled control: ...`                              | a control request the emulator does not know  |
| `bitmode 0x.. not emulated`, `CBUS bit-bang not available on ...` | a bit mode it cannot model, or on the wrong chip |
| `wait-on-I/O 0x..: the engine stops until GPIOL1 is ...` | `0x88`/`0x89` with GPIOL1 at the other level  |
| `... dropping N bytes`, `byte dropped`, `byte(s) dropped` | a buffer written beyond what the chip said it takes, or a bulk IN transfer too small for a packet (babble): data lost |
| `... before SET_CONFIGURATION`                        | an URB of a previous instance of the device   |
| `usbip: unknown ...`, `ctl: unknown ...`              | a USB/IP or control-FIFO command it does not know |
