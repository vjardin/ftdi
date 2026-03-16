# FTDI MPSSE Protocol Reference

The Multi-Protocol Synchronous Serial Engine (MPSSE) is a hardware block
in FTDI FT2232D, FT2232H, FT4232H, and FT232H chips. All communication
with the MPSSE happens over the USB bulk endpoint: the host sends
command bytes (opcodes + parameters) via USB write, and reads back
response data via USB read. Commands and data are mixed in a single
byte stream.

This document describes the MPSSE command set — the actual bytes sent
to and received from the FTDI chip.

Reference: FTDI AN\_108 "Command Processor for MPSSE and MCU Host Bus
Emulation Modes", FTDI AN\_135 "FTDI MPSSE Basics".

## Activation

The MPSSE is not active by default. It must be enabled after opening
the USB device:

1. Reset the device (`FT_SetBitMode(handle, 0x0, 0x00)`
   or `ftdi_set_bitmode(&ftdi, 0, 0)`)
2. Enable MPSSE mode (`FT_SetBitMode(handle, 0x0, 0x02)`
   or `ftdi_set_bitmode(&ftdi, 0, 2)`)

Once in MPSSE mode, every byte written to the device is interpreted
as a command opcode or command parameter by the MPSSE engine.
Responses are placed in the read buffer and retrieved with USB read.

## Command Format

Every MPSSE command is a 1-byte opcode optionally followed by
parameters. Multiple commands can be packed into a single USB write
buffer. The MPSSE processes them sequentially.

General structure:

```
[opcode] [param1] [param2] ... [opcode] [param1] ...
```

For data transfer commands, the format is:

```
[opcode] [LengthL] [LengthH] [data0] [data1] ... [dataN]
```

Where Length is a 16-bit little-endian value. The actual transfer
size is `Length + 1` bytes (i.e., length 0x0000 = 1 byte,
0xFFFF = 65536 bytes).

For bit-mode transfers:

```
[opcode] [BitLength] [data]
```

Where BitLength is the number of bits minus 1 (0 = 1 bit, 7 = 8 bits).

## Data Shifting Opcodes

The data shifting opcodes are built by combining bit flags:

| Bit | Mask | Name              | Meaning                          |
|-----|------|-------------------|----------------------------------|
|   0 | 0x01 | `MPSSE_WRITE_NEG` | Clock out on negative edge       |
|   1 | 0x02 | `MPSSE_BITMODE`   | Transfer bits (not bytes)        |
|   2 | 0x04 | `MPSSE_READ_NEG`  | Sample in on negative edge       |
|   3 | 0x08 | `MPSSE_LSB`       | LSB first (default MSB first)    |
|   4 | 0x10 | `MPSSE_DO_WRITE`  | Write (data out on TDI/DO)       |
|   5 | 0x20 | `MPSSE_DO_READ`   | Read (data in from TDO/DI)       |
|   6 | 0x40 | `MPSSE_WRITE_TMS` | Write TMS/CS instead of TDI/DO   |

These flags are OR'd together to form the opcode. For example:

### Byte-Mode Transfers

Write only, MSB first, clock on falling edge:
```
opcode = MPSSE_DO_WRITE | MPSSE_WRITE_NEG = 0x11
Wire:  [0x11] [LenL] [LenH] [byte0] ... [byteN]
```

Read only, MSB first, sample on rising edge:
```
opcode = MPSSE_DO_READ = 0x20
Wire:  [0x20] [LenL] [LenH]
Response: [byte0] [byte1] ... [byteN]
```

Simultaneous read+write, MSB, write falling, read rising:
```
opcode = MPSSE_DO_WRITE | MPSSE_DO_READ
       | MPSSE_WRITE_NEG = 0x31
Wire:  [0x31] [LenL] [LenH] [byte0] ... [byteN]
Response: [byte0] ... [byteN]
```

Write only, LSB first, clock on falling edge:
```
opcode = MPSSE_DO_WRITE | MPSSE_WRITE_NEG
       | MPSSE_LSB = 0x19
```

### Bit-Mode Transfers

Set bit 1 (`MPSSE_BITMODE = 0x02`) to transfer bits instead of
bytes. The length field is then a single byte (not two), and
indicates the number of bits minus 1.

Write 3 bits, MSB first, clock on falling edge:
```
opcode = MPSSE_DO_WRITE | MPSSE_WRITE_NEG
       | MPSSE_BITMODE = 0x13
Wire:  [0x13] [0x02] [data_byte]
```
The 3 most significant bits of `data_byte` are clocked out
(MSB mode).

Read 1 bit:
```
opcode = MPSSE_DO_READ | MPSSE_BITMODE = 0x22
Wire:  [0x22] [0x00]
Response: 1 byte with the bit in the MSB position
```

### TMS Shifts

Bit 6 (`MPSSE_WRITE_TMS = 0x40`) routes data to the TMS/CS pin
instead of TDI/DO. Used for JTAG state machine navigation. TMS
data is always bit-mode.

```
opcode = MPSSE_WRITE_TMS | MPSSE_WRITE_NEG
       | MPSSE_BITMODE = 0x4B
Wire:  [0x4B] [BitLength] [data_byte]
```

### Common Opcode Summary

| Opcode | Flags                             | Description              |
|--------|-----------------------------------|--------------------------|
| 0x10   | DO_WRITE                          | W bytes, MSB, +ve edge   |
| 0x11   | DO_WRITE \| WRITE_NEG             | W bytes, MSB, −ve edge   |
| 0x18   | DO_WRITE \| LSB                   | W bytes, LSB, +ve edge   |
| 0x19   | DO_WRITE \| WRITE_NEG \| LSB      | W bytes, LSB, −ve edge   |
| 0x20   | DO_READ                           | R bytes, MSB, +ve edge   |
| 0x24   | DO_READ \| READ_NEG               | R bytes, MSB, −ve edge   |
| 0x28   | DO_READ \| LSB                    | R bytes, LSB, +ve edge   |
| 0x2C   | DO_READ \| READ_NEG \| LSB        | R bytes, LSB, −ve edge   |
| 0x31   | DO_WRITE \| DO_READ \| WRITE_NEG  | R+W bytes, MSB, W↓ R↑    |
| 0x34   | DO_WRITE \| DO_READ \| READ_NEG   | R+W bytes, MSB, W↑ R↓    |
| 0x13   | DO_WRITE \| WRITE_NEG \| BITMODE  | W bits, MSB, −ve edge    |
| 0x22   | DO_READ \| BITMODE                | R bits, MSB, +ve edge    |
| 0x33   | DO_WRITE \| DO_READ               | R+W bits, MSB, W↓ R↑     |
|        | \| WRITE_NEG \| BITMODE           |                          |

## GPIO Commands

The MPSSE controls two 8-bit GPIO ports: the low byte
(ADBUS0-7) and the high byte (ACBUS0-7). Each port has
separate direction and value registers.

### Set Data Bits Low Byte -- `0x80`

Sets the direction and value of the 8 low-byte pins (ADBUS0-7).
These include the four MPSSE signals (SK, DO, DI, CS) and four
GPIO pins (GPIOL0-3).

```
Wire:  [0x80] [Value] [Direction]
```

- Value: output state for each pin (1 = high, 0 = low)
- Direction: pin direction (1 = output, 0 = input)

Pin mapping (low byte):

| Bit | Pin    | SPI        | I2C     |
|-----|--------|------------|---------|
|   0 | ADBUS0 | SK (clock) | SCK     |
|   1 | ADBUS1 | DO (MOSI)  | SDA out |
|   2 | ADBUS2 | DI (MISO)  | SDA in  |
|   3 | ADBUS3 | CS         | --      |
|   4 | ADBUS4 | GPIOL0     | GPIOL0  |
|   5 | ADBUS5 | GPIOL1     | GPIOL1  |
|   6 | ADBUS6 | GPIOL2     | GPIOL2  |
|   7 | ADBUS7 | GPIOL3     | GPIOL3  |

Example -- SPI idle state (SK high, DO low, CS high, GPIOs low):
```
[0x80] [0x09] [0xFB]
         |       +-- Direction: all output except DI (bit 2)
         +---------- Value: SK=1, DO=0, DI=0, CS=1
```

### Read Data Bits Low Byte -- `0x81`

Reads the current state of the low-byte pins. No parameters.

```
Wire:     [0x81]
Response: [1 byte with pin states]
```

### Set Data Bits High Byte -- `0x82`

Sets direction and value of the 8 high-byte pins (ACBUS0-7 /
GPIOH0-7). Only available on FT2232H, FT4232H, FT232H.

```
Wire:  [0x82] [Value] [Direction]
```

### Read Data Bits High Byte -- `0x83`

```
Wire:     [0x83]
Response: [1 byte with pin states]
```

## Clock Configuration

### Set Clock Divisor -- `0x86`

Sets the TCK/SK clock frequency using a 16-bit divisor.

```
Wire:  [0x86] [DivisorL] [DivisorH]
```

FT2232D (12 MHz base clock):
```
Frequency = 12 MHz / ((1 + Divisor) * 2)
```

FT2232H/FT4232H/FT232H with divide-by-5 enabled (default, 12 MHz effective):
```
Frequency = 12 MHz / ((1 + Divisor) * 2)
```

FT2232H/FT4232H/FT232H with divide-by-5 disabled (60 MHz base):
```
Frequency = 60 MHz / ((1 + Divisor) * 2)
```

Divisor range: 0x0000-0xFFFF. Frequency range depends on base clock.

Examples (60 MHz base, divide-by-5 disabled):

| Divisor | Frequency |
|---------|-----------|
| 0x0000  | 30 MHz    |
| 0x0001  | 10 MHz    |
| 0x0004  | 6 MHz     |
| 0x05DB  | 1 MHz     |
| 0xFFFF  | ~458 Hz   |

### Disable Divide-by-5 -- `0x8A` (H-type only)

Switches to 60 MHz base clock. Required for frequencies above 6 MHz.

```
Wire:  [0x8A]
```

Alias: `TCK_X5`, `DIS_DIV_5`

### Enable Divide-by-5 -- `0x8B` (H-type only)

Switches back to 12 MHz effective base clock (default state).

```
Wire:  [0x8B]
```

Alias: `TCK_D5`, `EN_DIV_5`

## Clocking Without Data Transfer

### Clock N Bits -- `0x8E`

Generate clock pulses without transferring data. Useful for I2C/JTAG timing.

```
Wire:  [0x8E] [Length]
```
Clocks `Length + 1` bits (0 = 1 clock, 7 = 8 clocks).

### 6.2 Clock N Bytes -- `0x8F`

```
Wire:  [0x8F] [LengthL] [LengthH]
```
Clocks `(Length + 1) * 8` bits.

### Clock Until GPIO High -- `0x94`

Clock continuously until GPIOL1 goes high.

```
Wire:  [0x94]
```

### Clock Until GPIO Low -- `0x95`

Clock continuously until GPIOL1 goes low.

```
Wire:  [0x95]
```

### Clock N Bytes Until GPIO High -- `0x9C`

Clock up to N bytes, stopping early if GPIOL1 goes high.

```
Wire:  [0x9C] [LengthL] [LengthH]
```

### Clock N Bytes Until GPIO Low -- `0x9D`

Clock up to N bytes, stopping early if GPIOL1 goes low.

```
Wire:  [0x9D] [LengthL] [LengthH]
```

## Three-Phase Clocking (I2C)

For I2C, data must be available on both clock edges. Three-phase
clocking adds an extra phase where the data line is set up before
the clock rises.

### Enable 3-Phase Clocking -- `0x8C`

```
Wire:  [0x8C]
```

The clock cycle becomes: data setup, clock high, clock low
(3 phases instead of 2). Required for correct I2C timing on
FT2232H and FT4232H.

### Disable 3-Phase Clocking -- `0x8D`

```
Wire:  [0x8D]
```

## Adaptive Clocking (JTAG)

For ARM JTAG targets that use a return clock (RTCK). The MPSSE
waits for the RTCK signal before proceeding.

### Enable Adaptive Clocking -- `0x96`

```
Wire:  [0x96]
```

### Disable Adaptive Clocking -- `0x97`

```
Wire:  [0x97]
```

## Loopback

Connects TDI/DO internally to TDO/DI for diagnostics -- data
written to DO is looped back to DI without external wiring.

### Enable Loopback -- `0x84`

```
Wire:  [0x84]
```

### Disable Loopback -- `0x85`

```
Wire:  [0x85]
```

## Flow Control

### Send Immediate -- `0x87`

Forces any buffered read data to be sent back to the host
immediately, without waiting for the USB latency timer to expire.

```
Wire:  [0x87]
```

Used after read commands when low-latency responses are needed.

### 10.2 Wait On I/O High -- `0x88`

Pauses MPSSE execution until GPIOL1 goes high.

```
Wire:  [0x88]
```

### 10.3 Wait On I/O Low -- `0x89`

Pauses MPSSE execution until GPIOL1 goes low.

```
Wire:  [0x89]
```

## Open Collector / Tristate -- `0x9E` (FT232H only)

Enables open-drain (open-collector) outputs on selected pins.
Each bit set to 1 makes that pin open-drain instead of push-pull.

```
Wire:  [0x9E] [LowByte] [HighByte]
```

## Bad Command Detection

If the MPSSE receives an unknown opcode, it responds with:

```
Response: [0xFA] [bad_opcode]
```

This is used for synchronization: send a known-bad opcode
(e.g., `0xAB`), then read back and look for `0xFA 0xAB` to
confirm the MPSSE is in sync with the host.

## Protocol Recipes

### SPI Mode 0 (CPOL=0, CPHA=0)

Clock idles low. Data propagated on falling edge, sampled on rising edge.

```
Setup:
  [0x8A]                    # Disable divide-by-5
  [0x97]                    # Disable adaptive clocking
  [0x86] [divL] [divH]      # Set clock divisor
  [0x80] [0x08] [0xFB]      # SK=0, CS=1, DO=0, DI=input

Start (assert CS):
  [0x80] [0x00] [0xFB]      # CS=0, SK=0

Write 4 bytes:
  [0x11] [0x03] [0x00]      # Write 4 bytes, MSB, fall
  [b0] [b1] [b2] [b3]

Read 256 bytes:
  [0x20] [0xFF] [0x00]      # Read 256 bytes, MSB, rise
  -> response: 256 bytes

Stop (deassert CS):
  [0x80] [0x08] [0xFB]      # CS=1, SK=0
```

### SPI Mode 3 (CPOL=1, CPHA=1)

Clock idles high. Data propagated on falling edge, sampled on
rising edge.

```
Setup:
  [0x80] [0x0B] [0xFB]      # SK=1, CS=1, DO=0

Start:
  [0x80] [0x02] [0xFB]      # CS=0, SK=1
  [0x80] [0x00] [0xFB]      # SK=0 (prevent glitch)

Write/Read: same opcodes as SPI0 (0x11, 0x20)

Stop:
  [0x80] [0x00] [0xFB]      # SK=0 first
  [0x80] [0x08] [0xFB]      # CS=1, SK=0
  [0x80] [0x0B] [0xFB]      # idle: SK=1, CS=1
```

### I2C

Clock idles high, SDA idles high. Three-phase clocking enabled.
Each byte is transferred individually with ACK/NACK bit handling.

```
Setup:
  [0x8C]                    # Enable 3-phase clocking
  [0x97]                    # Disable adaptive clocking
  [0x86] [divL] [divH]      # Set clock divisor
  [0x80] [0x03] [0x0B]      # SK=1, SDA=1 (idle)

Start condition (SDA low while SCK high):
  [0x80] [0x01] [0x0B]      # SDA=0, SK=1 -> start
  [0x80] [0x00] [0x0B]      # SDA=0, SK=0 -> ready

Write 1 byte (address):
  [0x80] [0x00] [0x0B]      # SK=0, DO=output
  [0x11] [0x00] [0x00]      # Write 1 byte, fall edge
  [addr]

Read ACK (1 bit from slave):
  [0x80] [0x00] [0x09]      # DO as input (release SDA)
  [0x22] [0x00]              # Read 1 bit, rise edge
  [0x87]                    # Send immediate
  -> response: 1 byte (bit 0 = ACK/NACK)

Write ACK (after master reads):
  [0x80] [0x00] [0x0B]      # DO as output
  [0x13] [0x00] [0x00]      # Write 1 bit = ACK/NACK

Stop condition (SDA high while SCK high):
  [0x80] [0x00] [0x0B]      # SDA=0, SK=0
  [0x80] [0x01] [0x0B]      # SDA=0, SK=1
  [0x80] [0x03] [0x0B]      # SDA=1, SK=1 -> stop
```

### GPIO Only

No serial protocol -- just set/read pins via `0x80`/`0x81`/
`0x82`/`0x83` commands.

```
Set GPIOL0 high:
  [0x80] [0x18] [0xFB]      # Set bit 4 (GPIOL0) high

Read low byte:
  [0x81]
  -> response: 1 byte with all 8 pin states
```

## Pin Assignments

The four dedicated MPSSE signals occupy the lowest bits of the
low byte:

| Bit | ADBUS  | Signal | SPI  | I2C       | JTAG |
|-----|--------|--------|------|-----------|------|
|   0 | ADBUS0 | TCK/SK | SCLK | SCK       | TCK  |
|   1 | ADBUS1 | TDI/DO | MOSI | SDA out   | TDI  |
|   2 | ADBUS2 | TDO/DI | MISO | SDA in    | TDO  |
|   3 | ADBUS3 | TMS/CS | CS   | --        | TMS  |
|   4 | ADBUS4 | GPIOL0 | GPIO | GPIO      | GPIO |
|   5 | ADBUS5 | GPIOL1 | GPIO | WAIT/STOP | GPIO |
|   6 | ADBUS6 | GPIOL2 | GPIO | GPIO      | GPIO |
|   7 | ADBUS7 | GPIOL3 | GPIO | GPIO      | RTCK |

High byte (ACBUS0-7) on FT2232H/FT4232H/FT232H: all available as GPIO (GPIOH0-7).

For I2C, DO and DI must be wired together externally (with
pull-up resistors to VCC) to form the bidirectional SDA line.
The MPSSE switches DO direction between output and input to
avoid bus contention.

## Chip Variants

| Chip    | MPSSE ch. | Max clk | High GPIO | 3-phase | Adaptive |
|---------|-----------|---------|-----------|---------|----------|
| FT2232D | 1         | 6 MHz   | No        | No      | No       |
| FT2232H | 2         | 30 MHz  | Yes 8pin  | Yes     | Yes      |
| FT4232H | 2         | 30 MHz  | Yes 8pin  | Yes     | Yes      |
| FT232H  | 1         | 30 MHz  | Yes 8pin  | Yes     | Yes      |

The H-type devices have a 60 MHz internal clock with an optional
divide-by-5 (enabled by default for backward compatibility with
FT2232D). Disable divide-by-5 (`0x8A`) to reach 30 MHz.

## Opcode Quick Reference

| Opcode | Parameters        | Description                       |
|--------|-------------------|-----------------------------------|
| 0x10   | LenL LenH Data... | Write bytes, MSB, +ve edge        |
| 0x11   | LenL LenH Data... | Write bytes, MSB, -ve edge        |
| 0x13   | BitLen Data        | Write bits, MSB, -ve edge        |
| 0x20   | LenL LenH         | Read bytes, MSB, +ve edge         |
| 0x22   | BitLen             | Read bits, MSB, +ve edge         |
| 0x24   | LenL LenH         | Read bytes, MSB, -ve edge         |
| 0x31   | LenL LenH Data... | R+W bytes, MSB, W-ve R+ve         |
| 0x34   | LenL LenH Data... | R+W bytes, MSB, W+ve R-ve         |
| 0x80   | Value Direction    | Set low byte GPIO                |
| 0x81   | --                 | Read low byte GPIO (1 byte back) |
| 0x82   | Value Direction    | Set high byte GPIO               |
| 0x83   | --                 | Read high byte GPIO (1 byte back)|
| 0x84   | --                 | Enable loopback                  |
| 0x85   | --                 | Disable loopback                 |
| 0x86   | DivL DivH          | Set clock divisor                |
| 0x87   | --                 | Send immediate (flush read buf)  |
| 0x88   | --                 | Wait on I/O high                 |
| 0x89   | --                 | Wait on I/O low                  |
| 0x8A   | --                 | Disable divide-by-5 (60 MHz)     |
| 0x8B   | --                 | Enable divide-by-5 (12 MHz)      |
| 0x8C   | --                 | Enable 3-phase clocking          |
| 0x8D   | --                 | Disable 3-phase clocking         |
| 0x8E   | BitLen             | Clock N+1 bits (no data)         |
| 0x8F   | LenL LenH          | Clock (N+1)*8 bits (no data)     |
| 0x94   | --                 | Clock until GPIOL1 high          |
| 0x95   | --                 | Clock until GPIOL1 low           |
| 0x96   | --                 | Enable adaptive clocking         |
| 0x97   | --                 | Disable adaptive clocking        |
| 0x9C   | LenL LenH          | Clock N bytes or GPIOL1 high     |
| 0x9D   | LenL LenH          | Clock N bytes or GPIOL1 low      |
| 0x9E   | LowByte HighByte   | Set open-drain mode (FT232H)     |
| 0xFA   | (response)         | Bad command echo (sync detect)   |
