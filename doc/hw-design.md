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

The FT232H supports hardware open-drain via the EEPROM DRIVE_ZERO_ONLY
setting (address 0xAC = 0x9E). The kernel driver sends `MPSSE_DRIVE_ZERO_ONLY`
(command 0x9E) at init time to enable this on AD0-AD2:

- Writing 0 -> drives pin LOW (active)
- Writing 1 -> pin floats (pulled HIGH by external resistor)

This is the correct mode for I2C. Without it, the pins are push-pull and
will fight the slave's open-drain output.

### Adaptive Clocking (Clock Stretching)

The MPSSE supports adaptive clocking via the `MPSSE_ENABLE_ADAPTIVE` command.
When enabled, the MPSSE monitors **AD7 (GPIOL3)**  -- not AD0 (SCL)  -- to detect
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
implications for Repeated START detection (see below).

### Repeated START and PIO Slave Timing

The MPSSE generates Repeated START (Sr) using GPIO `SET_BITS_LOW` commands
(not clock commands). The Sr sequence:

1. Release SDA (SET_BITS_LOW: SDA=1)  -- ~100ns per command
2. Raise SCL (SET_BITS_LOW: SCL=1, SDA=1)  -- 4x repetitions for rise time
3. Pull SDA LOW while SCL HIGH (SET_BITS_LOW: SCL=1, SDA=0)  -- Sr condition
4. Pull SCL LOW (SET_BITS_LOW: SCL=0, SDA=0)  -- hold

Each `SET_BITS_LOW` takes approximately **80-100 ns** on the FT232H (measured
empirically, not the theoretical 17 ns at 60 MHz MPSSE clock). This is likely
due to USB command parsing overhead in the MPSSE engine.

**Timing from SCL HIGH to SDA fall (Sr condition):**

With 4x Phase 2 (SCL HIGH) + 1x Phase 3 start (SDA fall):
~5 commands x 100 ns = **~500 ns** (measured range: 400-800 ns)

**Impact on PIO-based I2C slaves:**

A PIO slave must detect the Sr by polling SDA while SCL is HIGH. The polling
window is limited by the I2C clock frequency:

| I2C Speed | SCL HIGH Time | Sr SDA Fall | PIO Can Detect? |
|-----------|---------------|-------------|-----------------|
| 100 kHz   | ~5 us         | ~500 ns     | Yes (plenty of margin) |
| 400 kHz   | ~1.25 us      | ~500 ns     | Marginal (window ~750 ns) |
| 1 MHz     | ~500 ns       | ~500 ns     | No (window is zero) |

At 400 kHz, the PIO polling window (~1 us safe maximum) barely covers the
MPSSE's Sr timing (~500 ns). In practice, this is **unreliable**  -- the MPSSE
processing speed varies, and the PIO risks false-triggering on data bit=1
if the window exceeds SCL HIGH time.

**Recommendation for compound I2C transactions at 400 kHz:**

Wire AD7 to SCL and enable adaptive clocking. This allows the PIO slave to
hold SCL LOW (clock stretching) after each byte, giving unlimited time to
detect Repeated START before releasing SCL.

Without AD7  -- recommended operating modes:

**100 kHz (best for PIO slaves):** All operations work including compound
transactions with Repeated START. Verified with 4-byte compound reads.

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

1. MPSSE writes AD0=1 (release in open-drain)  -- bus SCL stays LOW (slave holds)
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
trace/jumper wire. Enable adaptive clocking in the kernel driver with
`MPSSE_ENABLE_ADAPTIVE` (command 0x96). This allows the MPSSE to wait for
SCL HIGH before sampling SDA, enabling full clock stretching support.

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
2. Enable adaptive clocking in the kernel driver:
   ```c
   buf[pos++] = MPSSE_ENABLE_ADAPTIVE;  /* 0x96 */
   ```
3. The newer board revision has a physical I2C switch that connects D1<->D2
   (SDA out<->SDA in), so no manual wiring needed for SDA.

With this single jumper wire, the MPSSE monitors the actual SCL bus level
and waits for the slave to release SCL before proceeding. This enables:
- CPU-driven ACK/NACK (PIO slave holds SCL while CPU decides)
- All protocol-level fault injection (NACK address, clock stretch, slow ACK)
- Reliable compound transactions at any I2C speed (400 kHz, 1 MHz)

### Auto-Detection of AD7 Wiring

The kernel driver automatically detects if AD7 is wired to SCL at probe time.
It toggles SCL (AD0) and reads AD7 via `GET_BITS_LOW`. If AD7 follows AD0,
adaptive clocking is enabled automatically  -- no module parameter needed.

```
dmesg output when AD7 is wired:
  ftdi-i2c: AD7<->SCL wiring detected, adaptive clocking enabled
  ftdi-i2c: FTDI MPSSE I2C adapter at 400 kHz, HW open-drain, clock stretching (AD7)

dmesg output when AD7 is NOT wired:
  ftdi-i2c: FTDI MPSSE I2C adapter at 100 kHz, HW open-drain
```

The detection is safe: it only toggles SCL briefly during init (before any I2C
transactions). Manual override is still available via `clock_stretching=1` module
parameter.

### EEPROM Configuration

The FT232H EEPROM should be configured for I2C operation:

| Address | Value | Purpose |
|---------|-------|---------|
| 0xAC    | 0x9E  | DRIVE_ZERO_ONLY: open-drain on AD0-AD2 |

The kernel driver sends `MPSSE_DRIVE_ZERO_ONLY` at runtime, so EEPROM
programming is not strictly required. However, EEPROM configuration ensures
correct behavior even before the driver initializes.

### Bus Recovery

The kernel driver implements I2C bus recovery via `i2c_generic_scl_recovery()`:

- `get_scl`: reads AD0 via `GET_BITS_LOW`
- `set_scl`: drives AD0 via `SET_BITS_LOW`
- `get_sda`: reads AD2 via `GET_BITS_LOW`

Bus recovery generates 9 SCL pulses to release a stuck slave, then sends STOP.
This is triggered automatically after transfer failures.

### Reference Wiring Diagram

See `doc/ft232h-i2c-pico-wiring.svg` for the complete FT232H-to-RP2350
wiring diagram.

### FTDI Application Notes

| Document | Topic |
|----------|-------|
| AN_108   | MPSSE command reference |
| AN_113   | I2C with FT2232H (timing, SET_BITS_LOW repetitions) |
| AN_255   | I2C with FT232H + FT201X |
| AN_135   | MPSSE basics |
| AN_177   | LibMPSSE I2C API |
