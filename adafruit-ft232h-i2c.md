# Adafruit USB-C to I2C FT232H Board

The Adafruit FT232H Breakout (USB-C variant, product ID 5310) is a
convenient development board for I2C with the `ftdi_mpsse` driver.
It features an FT232H chip, a STEMMA QT / Qwiic connector for I2C
peripherals, and a power switch.

## Board Layout (I2C relevant)

```
                    USB-C
                      |
                   FT232H
                      |
         AD0 (SCL) ---+--- 4.7k pull-up ---+-- 3.3V
         AD1 (SDA) ---+--- 4.7k pull-up ---+
         AD2 (SDA) ---+                    |
                      |                [I2C switch]
                      |                    |
                 STEMMA QT connector       |
                 +------------------+      |
                 | Yellow  SCL      |------+
                 | Blue    SDA      |------+
                 | Red     3.3V     |--[switch]-- 3.3V regulator
                 | Black   GND      |------+
                 +------------------+      |
                                          GND
```

## STEMMA QT Cable Pinout

| Wire   | Signal | Description                  |
|--------|--------|------------------------------|
| Black  | GND    | Ground                       |
| Red    | 3.3V   | Power (controlled by switch) |
| Blue   | SDA    | I2C data                     |
| Yellow | SCL    | I2C clock                    |

## The I2C Power Switch

The board has a slide switch labelled "I2C power" (or "pwr") next to
the STEMMA QT connector. It controls the 3.3V supply to the STEMMA QT
connector's Red wire only. It does not disconnect SCL/SDA/GND.

### Switch OFF

- Red wire (3.3V): disconnected -- no power delivered to the
  peripheral through the STEMMA QT connector
- SCL/SDA: still connected to the FT232H (AD0/AD1) and pulled
  up to 3.3V by the on-board 4.7k resistors
- I2C bus: operational -- the FT232H can drive SCL/SDA and
  the pull-ups are active

Use this position when the I2C peripheral has its own power supply
(ex powered from another board, or a sensor with a separate 3.3V
rail). The STEMMA QT cable carries only the I2C signals and ground;
the peripheral must be powered externally.

This is also the safe position when no peripheral is connected --
it prevents the 3.3V rail from being shorted if the cable is
accidentally touched.

### Switch ON

- Red wire (3.3V): connected -- the board's 3.3V regulator
  powers the peripheral through the STEMMA QT connector
- SCL/SDA: same as OFF -- connected and pulled up
- I2C bus: operational, and the peripheral is powered

Use this position when the I2C peripheral needs to be powered from
the FT232H board (e.g. a small sensor breakout with no other power
source). The 3.3V regulator on the Adafruit board can supply up to
~500 mA, which is sufficient for most I2C sensors and small
peripherals.

Important: the switch only controls power delivery through the
STEMMA QT Red wire. The I2C signals work in both positions -- the
switch is not an I2C bus enable/disable toggle.

## Usage with `ftdi_mpsse`

### Quick Start

```sh
# Load modules
insmod ftdi_i2c.ko
insmod ftdi_gpio.ko
insmod ftdi_mpsse.ko bus_mode=i2c

# Bind the Adafruit board (VID:PID 0403:6014)
echo "0403 6014" > /sys/bus/usb/drivers/ftdi_mpsse/new_id

# The I2C adapter appears as /dev/i2c-N
# Find it:
dmesg | grep "FTDI MPSSE I2C"
# ftdi_i2c ftdi-i2c.0: FTDI MPSSE I2C adapter at 100 kHz, HW open-drain

# The bus number (here 18) means /dev/i2c-18 is ready for use
ls -l /dev/i2c-*

# Scan for devices on that bus
i2cdetect -y 18
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

Set the I2C power switch to ON if the sensor needs power from the
board, or OFF if the sensor has its own supply.

### Pin Assignment

In I2C mode, the FT232H uses:

| Pin | Function | Notes                                          |
|-----|----------|------------------------------------------------|
| AD0 | SCL      | I2C clock, 4.7k pull-up to 3.3V on board       |
| AD1 | SDA_OUT  | I2C data out, 4.7k pull-up to 3.3V on board    |
| AD2 | SDA_IN   | I2C data in (directly wired to AD1 on the PCB) |

AD1 and AD2 are shorted together on the Adafruit board -- this is
the correct wiring for FTDI I2C where the MPSSE engine uses separate
output (DO = AD1) and input (DI = AD2) pins for the bidirectional
SDA line. The FT232H's hardware open-drain mode (0x9E) handles the
open-drain signalling automatically.

AD3-AD7 and AC0-AC7 are available as GPIO (directly on the header
pins, not on the STEMMA QT connector).

### Driver Pin Map

The driver exposes the full pin assignment via sysfs. Example with
the Adafruit board on USB path `1-2`:

```
$ cat /sys/bus/usb/devices/1-2:1.0/pinmap
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
