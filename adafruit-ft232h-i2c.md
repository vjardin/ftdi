<!-- SPDX-License-Identifier: GPL-2.0 -->
# Adafruit USB-C to I2C FT232H Board

The Adafruit FT232H Breakout (product 2264, revised in February 2020
with a USB-C connector, an I2C switch and a STEMMA QT connector) is a
convenient development board for I2C with the `ftdi_mpsse` driver.

## Board Layout (I2C relevant)

```
                    USB-C
                      |
                   FT232H
                      |
         AD0 (D0) ----+---------------------- SCL --+
         AD1 (D1) ----+---------------------- SDA --+-- STEMMA QT
         AD2 (D2) ----+--[I2C switch]-- AD1         |   connector
                                        3.3V -------+
                                        GND --------+
```

## STEMMA QT Cable Pinout

| Wire   | Signal | Description      |
|--------|--------|------------------|
| Black  | GND    | Ground           |
| Red    | 3.3V   | Power            |
| Blue   | SDA    | I2C data (D1)    |
| Yellow | SCL    | I2C clock (D0)   |

## The I2C Switch

The MPSSE has no bidirectional data pin: it drives SDA through AD1 (DO)
and reads it through AD2 (DI), so the two must be joined (AN_113, AN_255).
The I2C switch next to the STEMMA QT connector does that: it connects D1
to D2 on the board.

### Switch ON

D1 and D2 are joined: SDA can be driven and read. This is the position
for I2C, and for anything on the STEMMA QT connector.

### Switch OFF

D2 is not on SDA. The driver then reads the pull-up instead of the
slave's ACK bit, so every address looks unanswered and transfers fail
with `ENXIO`. This is the position for SPI, where D1 (MOSI) and D2
(MISO) are separate signals.

The original board (micro-USB) has no switch: join D1 and D2 with a
wire.

### Pull-ups

I2C needs pull-ups on SCL and SDA. STEMMA QT and Qwiic sensor boards
usually carry them; with a bare device on the header, add them (4.7 kΩ
to 3.3 V, as Adafruit's guide for the original board recommends).

## Usage with `ftdi_mpsse`

### Quick Start

```sh
# Load the core first: the child drivers use its symbols
insmod ftdi_mpsse.ko bus_mode=i2c
insmod ftdi_i2c.ko
insmod ftdi_gpio.ko

# ftdi_sio claims 0403:6014: take the interface from it (here the
# board is USB device 1-2, so its interface is 1-2:1.0)
echo 1-2:1.0 > /sys/bus/usb/drivers/ftdi_sio/unbind

# Bind the Adafruit board (VID:PID 0403:6014) to ftdi_mpsse
echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id

# The I2C adapter appears as /dev/i2c-N; the pinmap (debugfs) names it
grep '^i2c:' /sys/kernel/debug/ftdi_mpsse/1-2:1.0/pinmap
# i2c: i2c-N

# Scan for devices on that bus
i2cdetect -y N
```

### Wiring Example with a STEMMA QT Sensor

Connect a STEMMA QT cable between the Adafruit FT232H board and an
I2C sensor (e.g. BME280, SHT40, or any Qwiic/STEMMA QT device):

```
Adafruit FT232H          STEMMA QT cable         I2C sensor
  STEMMA QT  ----------- Yellow (SCL) ----------- SCL
  connector  ----------- Blue   (SDA) ----------- SDA
             ----------- Red    (3.3V) ---------- VIN
             ----------- Black  (GND)  ---------- GND
```

Set the I2C switch to ON.

### Pin Assignment

In I2C mode, the FT232H uses:

| Pin | Function | Notes                                          |
|-----|----------|------------------------------------------------|
| AD0 | SCL      | I2C clock                                      |
| AD1 | SDA_OUT  | I2C data out                                   |
| AD2 | SDA_IN   | I2C data in (joined to AD1 by the I2C switch)  |

With the switch ON, AD1 and AD2 are joined -- this is the wiring FTDI
I2C needs, the MPSSE engine using separate output (DO = AD1) and input
(DI = AD2) pins for the bidirectional SDA line. The FT232H's hardware open-drain mode (0x9E) handles the
open-drain signalling automatically.

AD3-AD7 and AC0-AC7 are available as GPIO (directly on the header
pins, not on the STEMMA QT connector).

### Driver Pin Map

The driver exposes the full pin assignment in debugfs. Example with
the Adafruit board on USB path `1-2`:

```
$ sudo cat /sys/kernel/debug/ftdi_mpsse/1-2:1.0/pinmap
chip: FT232H
channel: A
mode: i2c

i2c: i2c-18
  AD0  SCL
  AD1  SDA_OUT
  AD2  SDA_IN

gpio: gpiochip1
  AD0  reserved (i2c-18)
  AD1  reserved (i2c-18)
  AD2  reserved (i2c-18)
  AD3  available
  AD4  available
  AD5  available
  AD6  available
  AD7  available
  AC0  available
  AC1  available
  AC2  available
  AC3  available
  AC4  available
  AC5  available
  AC6  available
  AC7  available
```

AD0-AD2 are reserved by the I2C adapter (`i2c-18`) -- the GPIO core
will not allow userspace to request them. The remaining 13 pins
(AD3-AD7, AC0-AC7) are available as GPIO on the board's header pins.

The `gpioinfo` tool confirms the same view from the GPIO subsystem:

```
$ gpioinfo gpiochip1
gpiochip1 - 16 lines:
    line   0:    unnamed             input consumer=kernel    # AD0 - SCL (reserved)
    line   1:    unnamed             input consumer=kernel    # AD1 - SDA_OUT (reserved)
    line   2:    unnamed             input consumer=kernel    # AD2 - SDA_IN (reserved)
    line   3:    unnamed             input                    # AD3 - available
    line   4:    unnamed             input                    # AD4 - available
    line   5:    unnamed             input                    # AD5 - available
    line   6:    unnamed             input                    # AD6 - available
    line   7:    unnamed             input                    # AD7 - available
    line   8:    unnamed             input                    # AC0 - available
    line   9:    unnamed             input                    # AC1 - available
    line  10:    unnamed             input                    # AC2 - available
    line  11:    unnamed             input                    # AC3 - available
    line  12:    unnamed             input                    # AC4 - available
    line  13:    unnamed             input                    # AC5 - available
    line  14:    unnamed             input                    # AC6 - available
    line  15:    unnamed             input                    # AC7 - available
```

Lines 0-2 show `consumer=kernel` because the I2C driver has reserved
them. Lines 3-15 have no consumer and can be requested by userspace
via `gpioset`/`gpioget` or `/dev/gpiochipN`.
