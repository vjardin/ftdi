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
$ sudo insmod ../ftdi_spi.ko
$ sudo insmod ../ftdi_i2c.ko
$ sudo insmod ../ftdi_gpio.ko

# The USB ID table is empty by default (avoids ftdi_sio conflict).
# Bind the device via new_id:
$ echo "0403 6014" | sudo tee /sys/bus/usb/drivers/ftdi_mpsse/new_id

$ dmesg | tail
[  42.123] usb 3-1: new high-speed USB device number 2 using vhci_hcd
[  42.456] ftdi_mpsse 3-1:1.0: detected FT232H on channel A
[  42.789] ftdi_mpsse 3-1:1.0: EEPROM checksum OK

$ ls /dev/ttyFTDI*
/dev/ttyFTDI0

$ cat /sys/bus/usb/devices/3-1:1.0/eeprom_*
```

When done, detach and clean up:

```
$ sudo usbip detach -p 0
$ sudo rmmod ftdi_spi ftdi_uart ftdi_gpio ftdi_mpsse
```

## Example: emulate an FT2232H with a captured EEPROM

Dump the EEPROM from a real device first (e.g. with `ftdi_eeprom`),
then replay it:

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
--mode MODE       Protocol mode: spi (default), i2c, or uart
                  Sets the EEPROM protocol hint byte at 0x1A
--eeprom FILE     Load 256-byte binary EEPROM image
--port PORT       TCP listen port (default: 3240)
--error TYPE      Error injection: i2c-nak, i2c-stuck, usb-stall, usb-timeout
--error-count N   Number of errors to inject (default: 1, 0=infinite)
--help            Show usage
```
