<!-- SPDX-License-Identifier: GPL-2.0 -->
# FT232H Hardware Design Notes

## I2C Bus Design

### Pin Mapping

| FT232H Pin | MPSSE Name | I2C Function | Direction |
|------------|------------|--------------|-----------|
| AD0        | TCK/SK     | SCL          | Output (open-drain with 0x9E) |
| AD1        | TDI/DO     | SDA out      | Output (open-drain with 0x9E) |
| AD2        | TDO/DI     | SDA in       | Input (wired to AD1) |
| AD7        | GPIOL3     | SCL sense    | Input (optional, for adaptive clocking) |

**AD1 and AD2 must be wired together** on the PCB or breakout board. AD1 is
the SDA output (open-drain), AD2 is the SDA input. The MPSSE writes via AD1
and reads via AD2. Without this connection, the master can send data but
cannot read ACK from slaves.

### Pull-Up Resistors

4.7 kohm pull-ups to 3.3V on both SDA and SCL. The pull-ups should be located
close to the slave device (RP2350 side), not the FT232H side, to minimize
capacitive loading on the bus.

### Hardware Open-Drain Mode (0x9E)

The FT232H has hardware open-drain outputs: the MPSSE command 0x9E
("Set I/O to only drive on a '0' and tristate on a '1'", AN_108 §7.1)
selects the pins. It is a run-time command, not an EEPROM setting, and
the FT2232H and FT4232H do not have it. The kernel driver sends
`MPSSE_DRIVE_ZERO_ONLY` (0x9E) at init time for AD0-AD2:

- Writing 0 -> drives pin LOW (active)
- Writing 1 -> pin floats (pulled HIGH by external resistor)

This is the correct mode for I2C. On FT2232H and FT4232H the driver
releases SCL and SDA by making them inputs instead ("set to input when
not driving the lines", AN_255 2.4), but the MPSSE drives SCL, and the
data bits it clocks out, push-pull: a slave that stretches the clock
fights it, so clock stretching needs an FT232H.

### Adaptive Clocking (Clock Stretching)

The MPSSE supports adaptive clocking via the `MPSSE_ENABLE_ADAPTIVE` command.
When enabled, the MPSSE monitors **AD7 (GPIOL3)** -- not AD0 (SCL) -- to detect
clock stretching. The slave holds SCL LOW to pause the master.

**Required wiring for clock stretching:**

```
AD0 (SCL output) --+-- I2C bus SCL --+-- 4.7k -- 3V3
                    |                 |
AD7 (SCL sense) ---+                 +-- Slave SCL
```

AD7 must be connected to the same SCL wire as AD0. The MPSSE reads AD7 to
know when the slave has released SCL.

**Without AD7 wired**: adaptive clocking does not work. The MPSSE clocks at
the configured frequency regardless of the slave's state. This has
implications for Repeated START detection (see below). Do not force it
(`clock_stretching=1`) on a board without the wire: AD7 floats HIGH, never
follows the clock LOW, and the MPSSE stops in the first byte; every transfer
then fails on the 2 s response timeout.

### Bus Timing: START, Repeated START, STOP

The MPSSE generates START, Repeated START (Sr) and STOP with GPIO
`SET_BITS_LOW` commands (not clock commands). Each step is one pin state,
repeated to last long enough. The Sr sequence:

1. Release SDA while SCL is LOW (SET_BITS_LOW: SCL=0, SDA=1)
2. Raise SCL (SET_BITS_LOW: SCL=1, SDA=1) -- setup time of the Sr
3. Pull SDA LOW while SCL HIGH (SET_BITS_LOW: SCL=1, SDA=0) -- Sr condition
4. Pull SCL LOW (SET_BITS_LOW: SCL=0, SDA=0) -- hold

Each `SET_BITS_LOW` takes approximately **80-100 ns** on the FT232H (measured
empirically, not the theoretical 17 ns at 60 MHz MPSSE clock). This is likely
due to USB command parsing overhead in the MPSSE engine.

The data bits use 3-phase clocking (AN_108 6.3): SCL is LOW for two thirds of
a bit and HIGH for one third. At 100 kHz that is 3.33 us HIGH, at 400 kHz
0.83 us.

**`strict_timing=1` (default): I2C-bus specification timing.**  The number of
repetitions is computed from the minimum times of UM10204 (table 11) at 80 ns
per command, and in standard mode the clock is slowed down so that SCL stays
HIGH for 4.0 us. The setup times of a Sr and a STOP start after the largest
rise time allowed (tr: 1 us in standard mode, 0.3 us in fast mode), counted
from the command that releases SCL:

| `i2c_speed` | Mode     | SCL on the bus | SCL HIGH | Sr: SCL released to SDA fall | START hold | STOP: SCL released to SDA rise |
|-------------|----------|----------------|----------|------------------------------|------------|--------------------------------|
| 10-83 kHz   | standard | as requested   | >= 4.0 us | >= 5.8 us (13 + 59 commands) | >= 4.0 us (50 commands) | >= 5.0 us (13 + 50 commands) |
| 84-100 kHz  | standard | 83 kHz         | 4.0 us   | >= 5.8 us                    | >= 4.0 us  | >= 5.0 us                      |
| 101-400 kHz | fast     | as requested   | >= 0.83 us | >= 0.96 us (4 + 8 commands) | >= 0.64 us (8 commands) | >= 0.96 us (4 + 8 commands) |

The driver logs `SCL runs at 83 kHz` at probe when the clock is limited.

**`strict_timing=0`: AN_113 timing.**  Four commands per step, as in AN_113
("to ensure the minimum period", which the measured command time does not
give), and the clock at the requested rate: Sr about 320-500 ns after SCL
HIGH at every speed, START hold and STOP setup about the same, SCL HIGH for
3.33 us at 100 kHz. This was the only behaviour of earlier versions.

**Impact on PIO-based I2C slaves:**

A slave built on a PIO state machine may detect the Sr by polling SDA while
SCL is HIGH, for a limited window. A slave that follows the specification
detects a START whenever SDA falls while SCL is HIGH, however long SCL has
been HIGH; a slave with a window must keep it open longer than the setup
time the master uses:

| Driver setting              | Sr: SCL HIGH to SDA fall | Window the slave needs |
|-----------------------------|--------------------------|------------------------|
| `strict_timing=1`, <= 100 kHz | 4.8-7.2 us             | > 7.2 us               |
| `strict_timing=1`, > 100 kHz  | 0.66-1.2 us            | > 1.2 us               |
| `strict_timing=0`             | 0.3-0.8 us (measured)  | > 0.8 us               |

(80 to 100 ns per command; the lower bound with SCL rising in the whole
rise time allowed.)

A slave whose window is shorter than the SCL HIGH time of a data bit (so
that it does not mistake a data bit for a START) cannot see a Sr of 4.8 us
or more at 100 kHz: load the driver with `strict_timing=0` for such a slave, or give
the slave a real START detector.

With `strict_timing=0` at 400 kHz, the window (about 0.8 us, the SCL HIGH
time) barely covers the Sr timing (about 500 ns): in practice this is
**unreliable**.

**Recommendation for compound I2C transactions at 400 kHz:**

Wire AD7 to SCL and enable adaptive clocking. This allows the PIO slave to
hold SCL LOW (clock stretching) while its CPU works.

**Clock stretching and Sr / STOP.**  Adaptive clocking only holds the clock
commands (the data bits). The pin writes of a Sr or a STOP do not wait for
SCL. If the slave is still holding SCL LOW when they are sent, SDA moves
while SCL is LOW and the slave sees neither a Sr nor a STOP. With adaptive
clocking the driver therefore reads SCL (through AD7), in the same USB
buffer, once it released SCL and gave it the rise time (UM10204 tr, 1 us
in standard mode), before the setup time (tSU;STA, tSU;STO). With
`strict_timing=0` it reads SCL just before it moves SDA, which keeps the
short AN_113 sequences above:

- SCL was HIGH: only the master pulls it low again, the condition was
  generated with the timing above.
- SCL was LOW (the slave was stretching): the driver takes the bus back,
  releases SCL, polls it until it is HIGH (up to the I2C adapter timeout,
  1 s), then moves SDA. In that case SDA moves at least one USB round trip
  (250 us or more) after SCL went HIGH. If the slave let SCL go during
  the setup time of the first attempt, that condition came too early (the
  slave may have seen it or not); the one that follows is in time.

Without AD7  -- recommended operating modes:

**100 kHz (best for PIO slaves):** All operations work including compound
transactions with Repeated START. Verified with 4-byte compound reads
(with the AN_113 timing, i.e. `strict_timing=0` today).

**400 kHz (writes + separate reads only):** Writes work at full speed.
Compound reads detect Sr but multi-byte read data is garbled (tx_poll_thread
timing marginal at 400kHz). Use separate transactions instead:

```bash
# Instead of: sudo i2ctransfer -y 18 w1@0x54 0x00 r4@0x54
# Use:
sudo i2ctransfer -y 18 w1@0x54 0x00    # set EEPROM pointer
sleep 5                                  # wait for slave watchdog
sudo i2ctransfer -y 18 r4@0x54          # read data
```

For maximum compatibility without AD7: **use 100 kHz**.

### Clock Stretching Limitation Without AD7

I2C clock stretching (slave holds SCL LOW to pause the master) does **not work**
with the FT232H MPSSE unless AD7 is wired to SCL.

The MPSSE does not monitor the actual SCL bus level. It toggles AD0 based on its
internal clock divider and reads SDA immediately, regardless of whether SCL
actually went HIGH on the bus. When the slave drives SCL LOW:

1. MPSSE writes AD0=1 (release in open-drain) -- bus SCL stays LOW (slave holds)
2. MPSSE reads AD2 (SDA) at the same instant  -- doesn't wait for SCL HIGH
3. MPSSE writes AD0=0  -- proceeds to next command

The slave's clock stretching is completely invisible to the MPSSE. The SDA read
happens at the wrong time, producing incorrect ACK/NACK results.

**Impact on PIO slaves:**

Without AD7, the PIO slave cannot implement CPU-driven ACK/NACK. The PIO must
use auto-ACK (drive SDA LOW immediately in hardware, ~20ns response). This means:

| Feature | Without AD7 | With AD7 |
|---------|-------------|----------|
| Auto-ACK (hardware) | Yes | Yes |
| CPU-driven ACK/NACK | No | Yes |
| NACK address fault | No (auto-ACK prevents) | Yes |
| Clock stretch fault | No | Yes |
| Slow ACK fault | No | Yes |
| Stretch forever fault | No | Yes |
| Bus-level faults (SDA/SCL stuck) | Yes | Yes |
| Data corruption fault | Yes | Yes |
| Stop-after-N fault | Yes | Yes |

**Recommendation:** Wire AD7 to the same SCL net as AD0. This is a single
trace/jumper wire. On an FT232H the driver detects it at probe and enables
adaptive clocking (`MPSSE_ENABLE_ADAPTIVE`, command 0x96) for that adapter;
`clock_stretching=1` forces it. "After the TCK output has changed the
controller waits until RTCK is sampled to be the same before it changes
TCK again" (AN_108 §6.9): with SCL on RTCK, the MPSSE holds the clock
while a slave holds SCL low, which is clock stretching support for the
data bits.

### Adafruit FT232H Breakout (Product 2264)

The Adafruit FT232H breakout exposes all ADBUS pins (D0-D7) on the header:

| Header Pin | FT232H Pin | I2C Function |
|------------|------------|--------------|
| D0         | AD0        | SCL (output, open-drain) |
| D1         | AD1        | SDA out (open-drain) |
| D2         | AD2        | SDA in (board has I2C switch to connect D1<->D2) |
| D7         | AD7/GPIOL3 | SCL sense (for adaptive clocking) |

**To enable clock stretching on the Adafruit board:**

1. Connect D7 to D0 with a jumper wire (one wire, both on the same header)
2. Load the driver: it detects the wire and enables adaptive clocking (see
   "Auto-Detection of AD7 Wiring" below)
3. The newer board revision has a physical I2C switch that connects D1<->D2
   (SDA out<->SDA in), so no manual wiring needed for SDA.

With this single jumper wire, the MPSSE monitors the actual SCL bus level
and waits for the slave to release SCL before it moves the clock again.
This enables:
- CPU-driven ACK/NACK (PIO slave holds SCL while CPU decides)
- All protocol-level fault injection (NACK address, clock stretch, slow ACK)
- Reliable compound transactions at every speed the driver supports
  (10 to 400 kHz)

### Auto-Detection of AD7 Wiring

The kernel driver automatically detects if AD7 is wired to SCL at probe time.
It toggles SCL (AD0) and reads AD7 via `GET_BITS_LOW`. If AD7 follows AD0,
adaptive clocking is enabled automatically  -- no module parameter needed.

```
dmesg output when AD7 is wired (i2c_speed=400):
  ftdi-i2c ftdi-i2c.0.auto: AD7<->SCL wiring detected, adaptive clocking enabled
  ftdi-i2c ftdi-i2c.0.auto: FTDI MPSSE I2C adapter at 400 kHz, HW open-drain, clock stretching (AD7)

dmesg output when AD7 is NOT wired (default i2c_speed=100):
  ftdi-i2c ftdi-i2c.0.auto: AD7 not wired to SCL: no clock stretching (it needs AD7 wired to AD0)
  ftdi-i2c ftdi-i2c.0.auto: FTDI MPSSE I2C adapter at 100 kHz, HW open-drain
  ftdi-i2c ftdi-i2c.0.auto: SCL runs at 83 kHz: the standard-mode SCL high time (4.0 us) with 3-phase clocking
```

The detection is safe: it only toggles SCL briefly during init (before any I2C
transactions). Manual override is still available via `clock_stretching=1` module
parameter.

### EEPROM Configuration

Open drain needs no EEPROM setting: the driver sends 0x9E at run time.
What the EEPROM can do for I2C (`tools/ft232h-i2c.conf` is an example for
`ftdi_eeprom`):

- select I2C mode in the driver's auto mode: "I2C" in the product string,
  or the text `bus_mode=i2c` in the user area (README, "EEPROM bus_mode= tag");
- set slow slew rate and Schmitt trigger on the ADBUS drive group
  (`group1_*` for ftdi_eeprom), which auto mode also takes for I2C when
  there is no tag nor product string and the VCP driver bit is clear
  (`cha_vcp=false`; `ftdi_eeprom` sets it by default, and the channel is
  then a UART; README, Mode Selection).

### Bus Recovery

The kernel driver implements I2C bus recovery via `i2c_generic_scl_recovery()`:

- `get_scl`: reads AD0 via `GET_BITS_LOW` (SCL is released when high,
  so AD0 reads the bus)
- `set_scl`, `set_sda`: drive AD0 / AD1 low or release them via
  `SET_BITS_LOW`
- `get_sda`: reads AD2 via `GET_BITS_LOW`

Bus recovery generates up to 9 SCL pulses, until the slave releases SDA;
SDA follows SCL half a cycle later, so that every pulse ends with a STOP.
It runs whenever a transfer ends with SDA held low: found before its
START (the transfer fails with `EBUSY`), after a failed byte, or after
its STOP (`EIO`). SCL held low is not recovered: the transfer fails
(`EBUSY` before the START, `EIO` at the STOP, `ETIMEDOUT` with adaptive
clocking) once the driver has waited for it.

A line found low before the START: after a transfer that ended well, the
driver samples SCL and SDA in the same USB buffer as the START and its
address byte, so the 9 clocks of the address have reached the bus when
it returns `EBUSY`. On a board where that matters (a slave that must
not see stray clocks, another master), load `ftdi_i2c` with
`check_bus_free=1`: the lines are then read in a round trip of their own
before every transfer and nothing is clocked into a bus that is not
free. Every transfer is one USB round trip longer, and a line pulled
low during that round trip, after the read, is still only seen with the
START.

### FTDI Application Notes

| Document | Topic |
|----------|-------|
| AN_108   | MPSSE command reference |
| AN_113   | I2C with FT2232H (timing, SET_BITS_LOW repetitions) |
| AN_255   | I2C with FT232H + FT201X |
| AN_135   | MPSSE basics |
| AN_177   | LibMPSSE I2C API |
