# Hardware Test Plan — Phase 0 Gates

**Prerequisites:** Both RP2040-Zero boards wired per wiring guide, USB-TTL debug adapter connected to RP2040 #2 GPIO 16.

**Tools needed:**
```bash
# Terminal 1: UART debug monitor
screen /dev/tty.usbserial-XXXX 115200
# Exit: Ctrl-A, K, Y

# Terminal 2: USB device inspection
system_profiler SPUSBDataType
```

**Flashing:**
```bash
# Hold BOOT on RP2040, plug into Mac via USB-C
# Drive "RPI-RP2" appears in Finder
cp firmware-g29/build/firmware_g29.uf2 /Volumes/RPI-RP2/
cp firmware-xbox/build/firmware_xbox.uf2 /Volumes/RPI-RP2/
```

---

## Test 0a: Individual Board Boot (No Peripherals)

**Goal:** Verify each board boots and prints to UART/USB.

### RP2040 #2 (firmware-g29)
1. Flash `firmware_g29.uf2`
2. Connect UART adapter to GPIO 16
3. Open `screen` at 115200

**Expected output:**
```
=== G29 Bridge — RP2040 #2 (G29 + Auth) ===
Build: Mar 24 2026 XX:XX:XX

[INIT] G29 USB Host...
[G29] TinyUSB host initialized on PIO, GPIO 0/1
[INIT] Auth Relay...
[AUTH] Root port 1 added on GPIO 2/3
[INIT] SPI Slave...
[SPI] Slave initialized on SPI1, DMA channels X/X
[INIT] Ready. Entering main loop.

[STATUS] G29:-- Auth:-- Steer=0000 Gas=00 Brake=00
```

**PASS:** Startup banner appears. Status shows G29:-- Auth:-- (nothing connected).

**FAIL scenarios:**
- No output → check UART wiring (GPIO 16 = TX → adapter RX), baud rate 115200
- Hangs at init → reflash, try different USB-C cable
- Watchdog resets (repeating banner) → something in init is hanging >100ms

### RP2040 #1 (firmware-xbox)
1. Flash `firmware_xbox.uf2`
2. Connect to Mac via USB-C (this board uses native USB, no UART)
3. Check USB enumeration:
```bash
system_profiler SPUSBDataType | grep -A 5 "045E\|Xbox\|Vendor"
```

**Expected:** Device with VID 0x045E appears (may show as unknown/vendor-specific device on Mac — this is fine, Mac doesn't have GIP drivers).

**PASS:** USB device enumerates with correct VID/PID.

**FAIL:** No USB device → check descriptor code, try `dmesg` for USB errors.

---

## Test 0b: PIO Dual Root Port

**Goal:** Verify two USB devices enumerate simultaneously on RP2040 #2's PIO0.

### Setup
- RP2040 #2 flashed with `firmware_g29.uf2`
- Connect ANY two USB devices to the breakout boards (doesn't need to be G29/auth — a keyboard + mouse works for this test)
- UART monitor running

**Expected output:**
```
[USB] Device mounted: addr=1 VID=XXXX PID=XXXX
[USB] Device mounted: addr=2 VID=XXXX PID=XXXX
```

**PASS:** Both devices enumerate with different addresses. No USB errors in UART output.

**FAIL scenarios:**
- Only one device enumerates → PIO0 dual root port may not work with these pins. Check wiring, try swapping D+/D-.
- CRC errors or timeouts → 68Ω resistors missing or wrong value, signal integrity issue
- Second device causes first to drop → PIO state machine exhaustion. This is the blocking risk from Marcus's review.

### Workaround if dual port fails
Try enumerating each device individually (unplug the other). If both work alone but not together, the dual-root-port approach needs investigation. Fall back to time-multiplexing or USB hub.

---

## Test 0c: SPI Bus Between Boards

**Goal:** Verify 1kHz 32-byte frame exchange with CRC-8 validation.

### Setup
- Both boards flashed
- SPI wires connected: SCK, MOSI, MISO, CS, GND (5 wires)
- UART on RP2040 #2

**Expected UART output (after 5 seconds):**
```
[STATUS] G29:-- Auth:-- Steer=0000 Gas=00 Brake=00
```

And on RP2040 #1 (if you can see its output — may need to read from RP2040 #2's SPI error counters):
```
[STATUS] Xbox:-- SPI:OK Steer=0000 Gas=00 Brake=00
```

**Verification:** The SPI status should show "OK" (valid CRC frames received). If CRC errors accumulate, check:
- Wire connections (MOSI→MOSI? or MOSI→MISO? See note below)
- GND shared between boards
- Wire length (<10cm)
- No loose connections

**SPI wiring note:** The signal names can be confusing. In our setup:
- RP2040 #1 GPIO 19 (MOSI = master out) → RP2040 #2 GPIO 11 (MOSI = slave in on SPI1)
- RP2040 #1 GPIO 16 (MISO = master in) ← RP2040 #2 GPIO 8 (MISO = slave out on SPI1)
- The Pico SDK names MOSI/MISO from the peripheral's perspective, so MOSI on master goes to MOSI on slave (they're the same SPI signal, not crossed).

**PASS:** SPI frames exchange at 1kHz. Zero CRC errors over 60 seconds. Status fields propagate (RP2040 #2's G29 status appears in RP2040 #1's output).

**FAIL:** All CRC errors → wires probably swapped. MOSI↔MISO or SCK wiring wrong.

---

## Test 0d: G29 Mode Switch + HID Reports

**Goal:** Verify G29 enumerates, mode-switches, and sends HID reports.

### Setup
- RP2040 #2 flashed with `firmware_g29.uf2`
- G29 plugged into USB-A breakout #1 (GPIO 0/1)
- G29 mode switch on back of wheel set to **PS3** (NOT PS4)
- G29's 12V power adapter plugged in (needed for FFB, not for input)
- UART monitor running

### Phase 1: Initial Enumeration

**Expected UART output:**
```
[USB] Device mounted: addr=1 VID=046D PID=C294
```

The G29 initially enumerates as a legacy Driving Force device (PID 0xC294 or similar).

**If PID is already 0xC24F:** The G29 is already in native mode. Skip to Phase 3.

### Phase 2: Mode Switch (if needed)

The firmware should automatically send the mode switch command and wait for re-enumeration:

**Expected UART output:**
```
[G29] Legacy Logitech detected (PID=C294), sending mode switch
[USB] Device unmounted: addr=1
[G29] Disconnected during mode switch — waiting for re-enumeration
[USB] Device mounted: addr=1 VID=046D PID=C24F
[G29] Native G29 detected (PID=C24F)
```

**10-second timeout:** If the G29 doesn't re-enumerate within 10 seconds, the firmware retries once.

**FAIL:** If re-enumeration never succeeds, the G29 may need a physical power cycle (unplug USB + 12V, wait 5s, replug). If it consistently fails, we may need to operate in legacy mode.

### Phase 3: HID Report Verification

Once enumerated, the firmware polls HID reports. Enable raw hex dump for verification:

**Expected UART output:**
```
[HID] Mounted: addr=1 inst=0 VID=046D PID=C24F
[HID] Report reception started
[STATUS] G29:OK Auth:-- Steer=8000 Gas=FF Brake=FF
```

**Physical verification** — perform each action and watch UART:

| Action | Expected Change |
|--------|----------------|
| Wheel centered, all released | Steer≈0x8000, Gas=0xFF, Brake=0xFF |
| Wheel full left | Steer≈0x0000 |
| Wheel full right | Steer≈0xFFFF |
| Gas pedal fully pressed | Gas=0x00 |
| Brake pedal fully pressed | Brake=0x00 |
| Press Cross (X) button | buttons0 bit 4 set |
| Press Circle (O) button | buttons0 bit 6 set |
| Press PS button | buttons3 bit 4 set |
| D-pad up | buttons0 low nibble = 0x00 |
| D-pad released | buttons0 low nibble = 0x08 |

**PASS:** All inputs produce expected values. Steering full range is 0x0000-0xFFFF (or 0x0000-0x3FFF if 14-bit). Record actual range.

**IMPORTANT:** If steering values don't match expected (e.g., limited range, wrong center), dump raw hex bytes:
```
[RAW] XX XX XX XX XX XX XX XX XX XX XX XX
```
Post the raw dumps in the Claude Code session. I can diff baseline vs. each input and fix the parser byte offsets.

---

## Test 0e: Auth Board Enumeration

**Goal:** Verify the PowerA auth board enumerates on PIO0 root port 1.

### Setup
- Auth board extracted from PowerA controller
- Wired to USB-A breakout #2 (GPIO 2/3 via 68Ω resistors)
- UART monitor running

**Expected UART output:**
```
[USB] Device mounted: addr=2 VID=XXXX PID=XXXX
```

**PASS:** Auth board enumerates. Record the VID/PID — we need these for the auth relay code.

**FAIL scenarios:**
- No enumeration → check wiring, try on Mac first (`system_profiler SPUSBDataType` with the auth board plugged into a USB-A port on the Mac directly)
- Enumerates then drops → power issue (auth board may need more current than VBUS provides)

---

## Test 0f: Full Chain (Both Boards + G29 + SPI)

**Goal:** End-to-end: G29 input → RP2040 #2 → SPI → RP2040 #1 → verify in UART.

### Setup
- Both boards flashed and wired (SPI + USB breakouts)
- G29 connected to breakout #1
- RP2040 #1 connected to Mac (or Xbox for quick test)
- UART monitor on RP2040 #2

**Expected behavior:**
1. G29 enumerates on RP2040 #2
2. RP2040 #2 parses HID reports, sends via SPI
3. RP2040 #1 receives SPI frames, shows G29 state in its status output
4. Turning the wheel changes the Steer value on both boards

**PASS:** Steering changes propagate through the SPI link. <5ms latency (not perceptible). No CRC errors.

---

## Test Matrix

Copy and fill in as you test:

```
Date: ___________
RP2040 #1 board: ___  RP2040 #2 board: ___

0a-1 RP2040 #2 boot:     [ PASS / FAIL ] _______________
0a-2 RP2040 #1 USB enum:  [ PASS / FAIL ] VID=_____ PID=_____
0b   Dual root port:      [ PASS / FAIL ] Dev1=_____ Dev2=_____
0c   SPI bus (60s):        [ PASS / FAIL ] CRC errors: _____
0d-1 G29 initial enum:    [ PASS / FAIL ] PID=_____
0d-2 G29 mode switch:     [ PASS / FAIL / SKIPPED ] New PID=_____
0d-3 G29 HID reports:     [ PASS / FAIL ] Steer range: _____-_____
0d-4 G29 buttons:         [ PASS / FAIL ] Verified: ___/10
0d-5 G29 pedals:          [ PASS / FAIL ] Gas range: ___-___
0e   Auth board enum:     [ PASS / FAIL ] VID=_____ PID=_____
0f   Full chain:          [ PASS / FAIL ] Steer propagates: [ YES / NO ]

Notes: ___________________________________________________________
```

---

## If Something Fails

**General debug steps:**
1. Check UART output first — it tells you exactly what failed
2. Paste the UART output into Claude Code — I can diagnose from the log
3. If USB issues: try `system_profiler SPUSBDataType` on the Mac with the device plugged in directly
4. If SPI issues: verify GND is shared, wires aren't swapped
5. If watchdog resets: look for the boot banner repeating — something is hanging

**Known risks from architecture review:**
- Dual root port may not work → fall back to testing each USB port individually
- G29 re-enumeration may hang → try operating in legacy mode without mode switch
- SPI MOSI/MISO naming can be confusing → if all CRC errors, swap MOSI↔MISO wires
