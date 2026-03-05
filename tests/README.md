# UML-based Test Framework

Automated integration tests for the FTDI kernel modules using
User Mode Linux (UML).  Runs as an unprivileged process -- no root,
no hardware, no host kernel tainting, but x86 only !

## How It Works

A UML kernel boots with an initramfs containing:

- ftdi_usbip -- the FTDI USB/IP device emulator (from `ftdi-emulation/`)
- usbip-core / vhci-hcd -- the kernel's USB/IP virtual host controller
- usbip_attach -- minimal helper that imports the emulated device
- FTDI modules -- `ftdi_mpsse.ko`, `ftdi_uart.ko`, `ftdi_spi.ko`, `ftdi_i2c.ko`, `ftdi_gpio.ko`, `ftdi_gpio_bitbang.ko`
- Test tools -- `spi_test`, `i2c_test`, `uart_test`, `gpio_test`, `hotplug_test`, `error_test`, `suspend_test`

Everything communicates over localhost inside UML.  The init script
loads the modules, verifies device detection, and reports PASS/FAIL.

## Prerequisites

- Linux kernel source tree (default: `~/dev/linux`)
- x86 host (UML only supports x86)
- GCC
- Static BusyBox binary (`apt install busybox-static` on Debian/Ubuntu)
- `cpio`, `gzip`

## Usage

```sh
# One-time: build the UML kernel (~2-5 min)
make -C tests uml-kernel

# Run the default tests: SPI + I2C + UART (~5 sec each)
make -C tests test

# Or from the top-level directory:
make test
```

## Test Output

Example output for the SPI test (`make -C tests test-spi`):

```
PASS: FTDI MPSSE core probed
PASS: GPIO driver registered
PASS: EEPROM checksum OK
PASS: gpiochip device exists
PASS: SPI mode detected
PASS: SPI controller registered
PASS: SPI 3 CS pins reserved
PASS: spidev devices created
PASS: spidev device exists
PASS: SPI read/write test
PASS: GPIO read/write test
PASS: No I2C adapter (SPI mode)
PASS: no kernel warnings
PASS: no kernel oops
== spi mode: 14/14 passed, 0 failed ==
```

Full kernel dmesg is printed after the test results and saved to
`tests/test-<mode>.log` (e.g. `test-spi.log`, `test-i2c.log`).

## Customisation

|   Variable  |     Default        | Description                     |
|-------------|--------------------|---------------------------------|
| `LINUX_SRC` | `~/dev/linux`      | Kernel source tree              |
| `UML_BUILD` | `tests/.uml-build` | UML out-of-tree build directory |
| `BUSYBOX`   | `/usr/bin/busybox` | Path to static BusyBox binary   |

## Cleaning Up

```sh
make -C tests clean
```

This removes the initramfs, copied modules, and userspace binaries.
The UML build directory (`.uml-build/`) is preserved to avoid
rebuilding the kernel.  Remove it manually if needed:

```sh
rm -rf tests/.uml-build
```
