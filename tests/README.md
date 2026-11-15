<!-- SPDX-License-Identifier: GPL-2.0 -->
# UML-based Test Framework

Automated integration tests for the FTDI kernel modules using
User Mode Linux (UML). Runs as an unprivileged process -- no root,
no hardware, no host kernel tainting, but x86 only !

## How It Works

A UML kernel boots with an initramfs containing:

- ftdi_usbip -- the FTDI USB/IP device emulator (from `ftdi-emulation/`)
- usbip-core / vhci-hcd -- the kernel's USB/IP virtual host controller
- usbip_attach -- minimal helper that imports the emulated device
- FTDI modules -- `ftdi_mpsse.ko`, `ftdi_uart.ko`, `ftdi_spi.ko`, `ftdi_i2c.ko`, `ftdi_gpio.ko`, `ftdi_gpio_bitbang.ko`
- Test tools -- `spi_test`, `spi_echo_test`, `i2c_test`, `uart_test`, `uart_drain_test`, `uart_flow_test`, `rs485_test`, `gpio_test`, `hotplug_test`, `error_test`, `stress_test`
- `known_splats.awk` -- kernel splats of known upstream (non-ftdi) bugs

Everything communicates over localhost inside UML. `init.sh` (the
initramfs `/init`) starts the emulator, attaches it, loads the modules,
binds the device through `new_id`, runs the assertions of the selected
`ftdi_mode`, then checks the emulator log and the kernel log. The
`pinmap`, `eeprom_*` reports and `i2c_stats` are read in debugfs, which
`uml.config` enables and `init.sh` mounts.

Most modes do not pass `bus_mode`: the emulator's `--mode` shapes the
EEPROM of the emulated chip (a `bus_mode=` tag for SPI/I2C, the VCP
driver bit for UART) and the driver's auto mode reads it. The `ee-*`
modes build other images with the emulator's `--ee-*` options.

## Prerequisites

- Linux kernel source tree, recent mainline (default: `~/dev/linux`,
  override with `LINUX_SRC=`)
- x86 host (UML only supports x86)
- GCC
- Static BusyBox binary (`apt install busybox-static` on Debian/Ubuntu)
- `cpio`, `gzip`

## Usage

```sh
# One-time: build the UML kernel (~2-5 min)
make -C tests uml-kernel LINUX_SRC=/path/to/linux

# Run the default tests: SPI + I2C + UART (~5 sec each)
make -C tests test            # or `make test` from the top level

make -C tests test-all        # + FT2232H, FT4232H, FT4232H C/D, AD7 wiring and ownership, I2C maps, UART drain and flow control, RS-485, latency timers
make -C tests test-errors     # error injection: NAK, stuck SDA/SCL, a slave stuck in a byte, stretch, USB timeout and halt, MPSSE stall/desync, sync
make -C tests test-bus        # on the wire: I2C stretching and timing, SPI modes, delays, chip select
make -C tests test-pm         # suspend/resume callbacks, the chip power-cycled in between
make -C tests test-stress-short  # 5 s of the main stress modes (what CI runs)
make -C tests test-eeprom     # EEPROM-driven mode selection, one image per path
make -C tests test-i2c-speeds # i2c-100k + i2c-400k
make -C tests test-stress     # concurrency, unbind/rmmod, hotplug, parameter storms
make -C tests test-<mode>     # any ftdi_mode below, e.g. test-stress-uart
```

The modules are rebuilt when a driver source changes; the emulator and
the initramfs are rebuilt on every run.

### Debug kernel

A second UML kernel with KASAN, lockdep, kmemleak, UBSAN, debug objects,
failslab and the hung task detector (`uml.config` + `uml-debug.config`,
uniprocessor) catches memory and locking bugs the plain kernel misses:

```sh
make -C tests uml-kernel-debug LINUX_SRC=/path/to/linux
make -C tests test-stress test-stress-failslab DEBUG=1
```

`DEBUG=1` selects `tests/.uml-build-debug` and 1 GB of guest memory. On
it, every run also scans kmemleak and fails on a leak from an ftdi
module.

### SMP kernel

A third kernel runs 4 CPUs (`uml.config` + `uml-smp.config`, with UBSAN,
debug objects and the hung task detector), for the races a uniprocessor
kernel cannot show:

```sh
make -C tests uml-kernel-smp LINUX_SRC=/path/to/linux
make -C tests test-stress SMP=1
```

`SMP=1` selects `tests/.uml-build-smp`, 512 MB of guest memory and
`seccomp=on`, which UML needs for SMP. It has no `CONFIG_PM_SLEEP`: the
`suspend*` modes report SKIP there. KASAN is left to the uniprocessor
debug kernel, UML SMP crashes with it.

## Test Modes

| Mode | Chip | What it checks |
|------|------|----------------|
| `spi`, `i2c`, `uart` | FT232H | probe, child devices, I/O through spidev / i2c-dev / tty, GPIO.  `spi`: SCK rate measured, a chip select with no device reads `0xFF`, ACBUS7 reads its pull-down, GPIO edge events from a pin the board moves.  `i2c`: an address refused after a repeated START.  `uart`: 7 data bits, a break between data |
| `ft2232h`, `ft2232h-{spi,i2c,uart}` | FT2232H | both channels, per-channel children; on each channel SPI or I2C transfers (`spi_test` / `i2c_test`), UART loopback and RS-485 for `-uart` |
| `ft2232h-mixed` | FT2232H | `bus_mode=spi,i2c`: SPI on A and I2C on B, one after the other and at the same time |
| `ft4232h`, `ft4232h-{spi,i2c,uart}` | FT4232H | A/B MPSSE (8 GPIO lines, no high byte) with SPI or I2C transfers on each (UART loopback for `-uart`), C/D UART |
| `ft4232h-cd` | FT4232H | `bus_mode=spi,spi,uart,bitbang`: SPI on A and B, C UART, D bit-bang GPIO |
| `ft4232h-mixed` | FT4232H | `bus_mode=i2c,spi,uart,bitbang`: one mode per channel, each with its I/O test |
| `i2c-100k`, `i2c-400k` | FT232H | `i2c_speed`: every data bit measured by the emulator at 83.3 kHz in standard mode (SCL high 4.0 µs), at 400 kHz in fast mode |
| `i2c-ad7` | FT232H | AD7 wired to SCL: adaptive clocking detected |
| `i2c-ad7-stretch` | FT232H | AD7 wired, the slave stretches SCL for 20 ms after every byte: data, repeated START and STOP all get through (memory device read back) |
| `i2c-ad7-stretch-short` | FT232H | AD7 wired, the slave lets SCL go 2 µs after the master, inside the setup time of a repeated START or STOP: no address follows a condition it spoiled (the condition is redone), the data gets through |
| `i2c-ad7-late` | FT232H | AD7 wired, SCL held low while the adapter is set up: the wiring is checked at the first transfer on a free bus, then clock stretching works |
| `i2c-ad7-missing` | FT232H | `clock_stretching=1` without the AD7 wire: the MPSSE stops on RTCK, the transfer times out, the engine is reset |
| `i2c-an113` | FT232H | `strict_timing=0`: transfers work, the emulator measures the short AN_113 timing |
| `i2c-map` | FT232H | `i2c_bus_nr_map` / `i2c_delay_us_map` entry in the middle of the list |
| `i2c-map-chan` | FT2232H | both channels I2C: map entries keyed by USB interface (`1-1:1.1:31`), device entry as fallback, I/O on both adapters |
| `i2c-ad7-gpio` | FT232H | AD7 wired to SCL: GPIO line 7 refused while the adapter uses it; no clock stretching while a GPIO consumer holds it; `clock_stretching=1` refused then |
| `uart-rs485` | FT232H | TXDEN on ACBUS4 in the EEPROM: found, `TIOCSRS485` accepted; the chip's own reading of its EEPROM names the same pin and asserts it while sending |
| `uart-rs485-ft4232h` | FT4232H | TXDEN instead of RI# on channels B and C only: RS-485 accepted there, refused on A and D |
| `uart-flow` | FT232H | 16 KiB sent to a port nobody reads (emulator with `--uart-pace`): with RTS/CTS, DTR/DSR or XON/XOFF the chip holds its sender and every byte is read back; without flow control the chip's receiver overruns and the driver counts it |
| `i2c-nak` | FT232H | address not acknowledged: `ENXIO` |
| `i2c-stuck` | FT232H | SDA held low for good: every read fails with `EBUSY` (no START on a held bus), the bus recovery pulses SCL 9 times per read on the wire (counted by the emulator).  Then SDA let go, a good transfer, SDA held again: `EBUSY` from the START itself, one address byte clocked at most |
| `i2c-check-free` | FT232H | `check_bus_free=1`: transfers work; SDA held after a good transfer gives `EBUSY` with nothing clocked into the bus (9 clocks without the parameter, `i2c-stuck`) |
| `i2c-stretch` | FT232H | a slave stretches SCL, AD7 not wired: the transfer fails, the driver waits for SCL without calling it stuck, the bus works afterwards |
| `i2c-scl-stuck` | FT232H | a slave holds SCL low for good: failure reported as a stuck SCL |
| `i2c-stuck-byte` | FT232H | a slave stuck in a byte holds SDA low and lets go during the bus recovery: the read fails, the recovery ends with a STOP, the bus works again |
| `spi-modes` | FT232H | SPI modes 0 to 3, MSB and LSB first, against one shift-register slave per mode (`--spi-echo`): data intact only on the right clock edges |
| `spi-timing` | FT232H | a write returns once on the wire, also when the chip's buffer holds the USB write up for longer than its timeout (4 KiB at 4 kHz); transfer delay, `cs_change` and word delay measured by the emulator at 10 kHz |
| `spi-cs-high` | FT232H | `SPI_CS_HIGH`: a device selected by a high level on CS1 answers, and the active-low one on CS0 answers alone afterwards (`--spi-cs-high`, `spi_cs=3,4`) |
| `spi-cs` | FT232H | `cs_setup`, `cs_hold`, `cs_inactive`, `cs_change_delay` and `cs_off`, sent by a test SPI driver (`kmod/ftdi_spi_cstest.c`) bound in place of spidev |
| `spi-error` | FT232H | a bulk IN URB that never completes during a transfer: timeout, MPSSE resynchronised, data in step afterwards |
| `spi-halt` | FT232H | the bulk IN endpoint halts during a transfer: the transfer fails, the core clears the halt and resynchronises the MPSSE, data in step afterwards |
| `uart-halt` | FT232H | the read URB of the port gets a STALL: the UART clears the halt and the loopback works |
| `spi-stall`, `i2c-stall` | FT232H | an MPSSE read command hangs for 3 s and answers late: the core resets the engine, no late response, data in step afterwards |
| `mpsse-desync` | FT232H | a stray response byte is refused (`-EPROTO`), then resynchronisation |
| `mpsse-sync-fail` | FT232H | MPSSE sync failure at probe |
| `uart-drain` | FT232H | `tcdrain()` and `close()` wait for the data in the write URBs and the chip (emulator with `--uart-pace`) |
| `ee-product-i2c`, `ee-vcp-uart`, `ee-slew-i2c`, `ee-default-spi` | FT232H | auto mode: product string, VCP driver bit, drive settings of ADBUS, default |
| `ee-cbus-1a` | FT232H | byte `0x1a` (ACBUS4/5 function) is not taken for a protocol hint |
| `ee-fifo`, `ee-blank`, `ee-badsum` | FT232H | FIFO channel type refused; blank or corrupt EEPROM gives SPI |
| `ee-tag-ff` | FT232H | a tag followed by erased cells (`0xFF`) instead of a NUL is read up to them |
| `ee-param-wins` | FT232H | the `bus_mode` parameter beats the EEPROM tag |
| `ee-ft2232h-chan`, `ee-ft2232h-drive` | FT2232H | per-channel `bus_mode=` tag; each channel reads its own drive group |
| `ee-ft4232h-vcp` | FT4232H | no channel type on FT4232H, VCP bit per channel |
| `ee-93c46` | FT2232H | the 64-word EEPROM, read back mirrored: checksum over 64 words, strings behind the fixed part, mode from the product string |
| `ee-custom-pid` | FT232H | an EEPROM with another product ID: the device enumerates with it, binds through `new_id`, the chip type still comes from `bcdDevice` |
| `hotplug` | FT232H | the emulator is killed (an unplug) while SPI and GPIO I/O run: data right until then, the unplug ends the I/O with `ESHUTDOWN` or `ENODEV` |
| `latency` | FT2232H | `mpsse_latency` reaches the chip and is clamped to 1-255 when a channel enters MPSSE; a UART's `latency_timer` reaches it, 0 and 256 refused |
| `suspend` | FT232H | suspend and resume of an SPI channel and its GPIO chip, the chip power-cycled in between: a message in flight finishes first, I/O is refused while suspended, MPSSE mode, pins and open-drain mask come back, I/O works again |
| `suspend-i2c` | FT232H | same for I2C with clock stretching (AD7): a transfer in flight is cut short (`ESHUTDOWN`); clock, 3-phase and adaptive clocking come back |
| `suspend-uart` | FT232H | same for a UART held open, with a CBUS GPIO output: baud rate, latency timer and CBUS mode come back |
| `suspend-ft4232h` | FT4232H | same for SPI on A, I2C on B, UART on C and bit-bang GPIO on D at once |
| `stress-spi`, `stress-i2c`, `stress-uart` | FT232H | parallel randomized workers, data verified |
| `stress-uart-errors` | FT232H | UART under injected line errors |
| `stress-ft2232h`, `stress-ft4232h` | FT2232H, FT4232H | all channels at once |
| `stress-unbind` | FT232H | unbind/bind and rmmod/insmod loops under I/O |
| `stress-hotplug` | FT232H | detach/re-import loop, then an emulator crash, under I/O |
| `stress-params` | FT232H | reload the stack with malformed and out-of-range module parameters |
| `stress-failslab` | FT232H | rebinds with the N-th slab allocation of the bind failed (`/proc/self/fail-nth`, N swept), then SPI I/O without injection (debug kernel) |

Kernel command line knobs: `ftdi_mode=MODE`, `ftdi_stress_secs=N`
(length of the stress phase), `ftdi_watchdog=SECS` (after SECS, dump
the blocked tasks, the locks held (lockdep kernels) and the CPU
backtraces with sysrq).

Faults that must hit a running device, not its probe, are armed from
`init.sh` through the emulator's control FIFO:
`echo "error TYPE [COUNT [SKIP [CHAN]]]" > /tmp/emu.ctl`; a pin is
driven from the board side with `echo "pin A C1 0" > /tmp/emu.ctl` (see
`ftdi-emulation/README.md`).

### What the I/O tests talk to

The emulator puts an I2C bus or SPI slaves behind the MPSSE pins and
decodes what the driver does on them (`ftdi-emulation/README.md`):

- I2C: counter devices at `0x20`, `0x22`, `0x50`, `0x51` (each byte read
  is the previous one plus 1), a memory device at `0x57` (`i2c_test`
  writes it and reads it back through a repeated START), nothing at any
  other address (`i2c_test` expects `ENXIO` at `0x30`).
- SPI: a counter device behind the first chip select (AD3), in SPI
  modes 0 and 3, and nothing behind the others (they read `0xFF`); with
  `--spi-cs-high` (`spi-cs-high`) a second one behind AD4, selected by a
  high level; with `--spi-echo` (`spi-modes`, `spi-timing`, `spi-cs`,
  loaded with `spi_cs=3,4,5,6,7`), `spidevB.1` to `spidevB.4` are
  shift-register slaves for SPI modes 0 to 3 that send back each byte
  one byte later.
- UART: a loopback plug. The chip's buffers have their real size: a
  port that is written to and not read loses nothing only while the
  emulated wire has no rate (the default); with `--uart-pace`
  (`uart-drain`, `uart-flow`) the receiver overruns unless flow control
  holds the sender.

`spi-cs` needs a kernel-side client: `tests/kmod/ftdi_spi_cstest.c` is
built against the UML kernel with the drivers, loaded only in that mode
and bound through `driver_override`.

### Suspend and resume

A real system sleep cannot be run in this environment: `vhci_hcd` refuses
to sleep with a device attached, and a sleep freezes every process of the
guest, the emulator included, while the resume path needs it to answer.
The drivers have no runtime PM either. `tests/kmod/ftdi_pm_test.c`
therefore calls the suspend and resume callbacks itself, in the order the
PM core and usbcore use: for each USB interface, last one first, the
`suspend` of every child device and then of the interface's USB driver;
on resume the reverse.

```sh
echo "suspend 1-1" > /sys/module/ftdi_pm_test/parameters/cmd
echo "power-cycle" > /tmp/emu.ctl      # the chip loses its mode and pins
echo "resume 1-1"  > /sys/module/ftdi_pm_test/parameters/cmd
```

The `suspend*` modes compare what the emulated chip holds (bit mode,
driven pins, directions, open-drain masks, CBUS pins, baud rate, latency
timer: the `emu: state chX ...` line) before the suspend and after the
resume, and run the I/O tests again. What this does not cover: the
freezing of user space, the suspend of the USB device itself by usbcore,
`reset_resume`, and the PM core's ordering against other devices. On a
kernel without `CONFIG_PM_SLEEP` (UML SMP) the modes report SKIP.

## Verdict

`check_log.sh MODE LOG` decides, not the UML exit code. A run passes
only if:

- `init.sh` printed its summary line (no crash, panic or timeout)
- the summary reports 0 failed checks
- the kernel log has no splat (WARN, BUG, KASAN, UBSAN, lockdep, hung
  task), apart from known upstream bugs listed in `known_splats.awk`;
  those are named in the verdict line instead

`init.sh` itself also fails on emulator-reported protocol violations (an
MPSSE opcode the chip would reject, MPSSE on a non-MPSSE channel, a
high-byte access on FT4232H, an SPI read with no chip select, a response
larger than the chip's RX FIFO), on a request the emulated chip refuses
or cannot model (a STALLed vendor request, an unhandled control request,
a bit mode it does not emulate, a wait-on-I/O that would block for ever,
an unknown `emu.ctl` command), on data one of its buffers had to drop, on
an URB that reached it before `SET_CONFIGURATION`, on what the bus models
report in every mode (an I2C time below the UM10204 minimum, a write the
memory device did not see end with a STOP, an I2C line driven high
against a slave that holds it low, a START after a bus recovery that
did not end with a STOP, an address after a repeated START that came
too soon after a slave let SCL go, a chip select asserted with SCK at
the wrong level, an MPSSE left waiting for RTCK), and on kmemleak
findings. The modes that provoke one of these on purpose (`i2c-an113`,
`i2c-ad7-missing`, and `i2c-ad7-stretch-short` for the condition that
comes too early before it is redone) expect it instead.

The test tools check what they read, not only that a call returned: the
emulated SPI and I2C counter devices must count on without a gap from one
transfer to the next, a GPIO line switched to input must read the chip's
pull-up, and `hotplug_test` passes only if the unplug ends its loop with
`ESHUTDOWN` or `ENODEV`. The checks that used to pass whatever the
driver did were each seen failing once with a bug planted in a copy of
the driver or of the emulator.

## Running One Mode Interactively

```sh
make -C tests initramfs
tests/.uml-build/linux initrd=tests/.uml-build/initramfs.cpio.gz \
	mem=256M ftdi_mode=spi con=null con0=fd:0,fd:1
```

The emulator log and the full dmesg are printed after the results.
Logs of `make` runs are saved to `tests/test-<mode>.log`.

## Customisation

|   Variable     |     Default        | Description                          |
|----------------|--------------------|--------------------------------------|
| `LINUX_SRC`    | `~/dev/linux`      | Kernel source tree                   |
| `UML_BUILD`    | `tests/.uml-build` | UML out-of-tree build directory      |
| `DEBUG`        | (unset)            | `1`: use the debug kernel            |
| `SMP`          | (unset)            | `1`: use the SMP kernel              |
| `UML_MEM`      | `256M` (`1G` debug, `512M` SMP) | Guest memory            |
| `UML_EXTRA`    | (empty, `seccomp=on ncpus=4` SMP) | Extra UML command line arguments |
| `TEST_TIMEOUT` | `120`              | Seconds before a run is killed       |
| `BUSYBOX`      | `/usr/bin/busybox` | Path to static BusyBox binary        |

## Adding a Test Mode

1. Add a `case` to the `ftdi_mode` parser in `init.sh` (chip, emulator
   protocol, error injection) and the `check "desc" 'cmd'` assertions
   (`skip "why"` reports what cannot run without counting it).
2. `make -C tests test-<mode>` works for any mode; add it to a group
   target (`test-all`, `test-errors`, `BUS_MODES`, `STRESS_MODES`) if it
   belongs there.
3. A new C tool goes into `TEST_TOOLS` in the `Makefile` (built
   `-static -pthread`, copied into the initramfs).
4. A new emulator error injection needs an enum in
   `ftdi-emulation/ftdi_emu.h`, its behaviour in `ftdi_emu.c` (or in
   `ftdi_bus.c` for the I2C bus) and a name in
   `ftdi_emu_error_from_name()`.

## Cleaning Up

```sh
make -C tests clean
```

This removes the initramfs, copied modules, logs and userspace binaries.
The UML build directories (`.uml-build/`, `.uml-build-debug/`,
`.uml-build-smp/`) are preserved to avoid rebuilding the kernel. Remove
them manually if needed.
