# Getting Started — Hardware Assembly & First Tests

## What You Need On Your Desk

- [ ] 2x DWEII RP2040-Zero boards (labeled "Board 1" and "Board 2" with tape)
- [ ] 2x Keesoo USB-A female breakout boards
- [ ] 12x 200 ohm resistors (3 parallel per line ≈67Ω)
- [ ] Hookup wire (22-24 AWG, solid core preferred for breadboard/perfboard)
- [ ] Soldering iron + solder
- [ ] Logitech G29 wheel + pedals + 12V power adapter (set mode switch to **PS3**)
- [ ] PowerA Wired Controller for Xbox Series X|S (stays intact — do not open it)
- [ ] 2x USB-C cables (Mac to each RP2040)
- [ ] Your MacBook Pro

## Step 1: Label Your Boards

Take two RP2040-Zero boards. Put a piece of tape on each:

- **Board 1** = "XBOX" — this connects to the Xbox (later) and runs the Equation VM
- **Board 2** = "G29" — this connects to the G29 wheel only (debug via USB CDC)

## Step 2: Flash Board 2 First (No Wiring Yet)

No soldering needed for this step. Just the bare board and a USB-C cable.

1. Open Terminal on your Mac
2. Navigate to the project:
   ```bash
   cd ~/g29proj/g29-xbox-bridge
   ```
3. Hold the **BOOT** button on Board 2 (it's the small button on the board)
4. While holding BOOT, plug Board 2 into your Mac with a USB-C cable
5. Release BOOT. A drive called **RPI-RP2** should appear in Finder
6. Run:
   ```bash
   ./tools/scripts/flash.sh g29 --flash-only
   ```
   If the drive appeared, it will copy the firmware and the board reboots.
7. **Unplug Board 2 from Mac**

**If RPI-RP2 doesn't appear:** Try a different USB-C cable (some are charge-only). Try a different USB port on the Mac.

## Step 3: Connect to Board 2 Debug Output (USB CDC)

Board 2 outputs debug over USB CDC on its native USB-C port — no separate UART adapter needed.

1. Plug Board 2 into your Mac via USB-C
2. Wait ~2 seconds for the CDC device to appear
3. Run:
   ```bash
   ./tools/scripts/monitor.sh
   ```
   The script auto-detects the `/dev/tty.usbmodem*` port.
4. You should see:
   ```
   === G29 Bridge — RP2040 #2 (G29) ===
   ```

**If you see nothing:** Try `ls /dev/tty.usbmodem*` — if no port appears, try a different USB-C cable (must be data-capable). Press the reset button on the board to trigger the boot banner again.

**If you see the banner repeating every second:** The watchdog is resetting. Something in the init is taking too long. Paste the output into Claude Code.

**Note:** Board 1 (XBOX) cannot use USB CDC for debug because its native USB port is the Xbox GIP device. Board 1 debug info is relayed through the SPI link and appears in Board 2's CDC output.

**Checkpoint: You should see the boot banner and status lines in your terminal. The status will show G29:-- because nothing is plugged into the USB breakout yet. This is correct.**

## Step 4: Wire USB-A Breakout #1 (for G29)

This breakout lets the G29 plug into Board 2. You need to solder 4 wires + 6 resistors (3 parallel per data line):

```
Keesoo USB-A Breakout       3×200Ω Parallel (≈67Ω)   Board 2
┌─────────────────┐                               ┌──────────┐
│ VBUS (red)      │──────────── direct ──────────→│ VBUS/5V  │
│ D+   (green)    │────[3×200Ω]─────────────────────→│ GPIO 0   │
│ D-   (white)    │────[3×200Ω]─────────────────────→│ GPIO 1   │
│ GND  (black)    │──────────── direct ──────────→│ GND      │
└─────────────────┘                               └──────────┘
```

**Critical details:**
- The 3×200Ω parallel groups (≈67Ω) go on **D+ and D- only** (not VBUS or GND)
- VBUS from the breakout connects to Board 2's **VBUS pad** (5V), NOT 3.3V
- Double-check D+ is on GPIO 0 and D- is on GPIO 1 (not swapped)

## Step 5: Test G29 Input (Test 0d)

1. Set the G29's mode switch (on the back/bottom of the wheel base) to **PS3**
2. Plug the G29's USB-A cable into the breakout board you just wired
3. Plug in the G29's 12V power adapter
4. Watch the UART monitor terminal. You should see:
   ```
   [USB] Device mounted: addr=1 VID=046D PID=XXXX
   ```
5. If you see `PID=C24F`: the G29 is already in native mode. Great.
6. If you see `PID=C294` or similar: the firmware will attempt a mode switch. Watch for:
   ```
   [G29] Legacy Logitech detected, sending mode switch
   [USB] Device unmounted: addr=1
   ... (wait up to 10 seconds) ...
   [USB] Device mounted: addr=1 VID=046D PID=C24F
   ```
7. Once the G29 is connected, the status line should show `G29:OK`
8. **Test each input physically:**
   - Turn wheel full left → `Steer` should go toward `0000`
   - Turn wheel full right → `Steer` should go toward `FFFF`
   - Center wheel → `Steer` should be around `8000`
   - Press gas pedal fully → `Gas` should go from `FF` to `00`
   - Press brake pedal → `Brake` should go from `FF` to `00`

**If the G29 doesn't enumerate:** Check wiring. Try swapping D+ and D- (breakout pin labeling may differ from the standard). Make sure the resistor groups are present (3×200Ω parallel per line).

**Paste the UART output into Claude Code** — I can tell you exactly what's happening and fix any issues.

## Step 6: Flash Board 1

1. Hold BOOT on Board 1, plug into Mac via USB-C
2. Run:
   ```bash
   ./tools/scripts/flash.sh xbox --flash-only
   ```
3. Unplug Board 1

## Step 7: Wire SPI Between Boards

Connect 5 wires between the two boards:

```
Board 1 (XBOX)                    Board 2 (G29)
┌──────────────┐                  ┌──────────────┐
│ GPIO 2  (SCK)│──────────────→  │ GPIO 10 (SCK)│
│ GPIO 3 (MOSI)│──────────────→  │ GPIO 11 (MOSI)│
│ GPIO 4 (MISO)│←─────────────   │ GPIO 8  (MISO)│
│ GPIO 5  (CS) │──────────────→  │ GPIO 9  (CS) │
│ GND          │──────────────── │ GND          │
└──────────────┘                  └──────────────┘
```

**Keep wires short** (under 10cm / 4 inches). Longer wires may cause SPI errors.

**MOSI connects to MOSI** (not crossed). Both boards use the Pico SDK naming where MOSI is the same signal on both master and slave.

## Step 8: Test SPI Link (Test 0c)

1. Power both boards (Board 1 via USB-C to Mac, Board 2 via USB-C to a charger or second Mac port)
2. Watch UART monitor on Board 2
3. After 5 seconds, the status line should show that SPI frames are being exchanged:
   ```
   [STATUS] G29:OK Auth:-- Steer=XXXX Gas=XX Brake=XX
   ```
4. If Board 1 is also printing status (via its own debug path), it should show SPI:OK

**If you see SPI CRC errors:** Swap the MOSI and MISO wires between the boards. This is the #1 SPI wiring mistake.

## Step 9: Wire USB-A Breakout #2 (for PowerA Auth Controller)

Same wiring pattern as breakout #1 but on **Board X (Board 1)** GPIO 0/1. The PowerA controller plugs in intact via its USB cable — no need to open it:

```
Breakout #2 → 3×200Ω parallel → Board 1 GPIO 0 (D+)
Breakout #2 → 3×200Ω parallel → Board 1 GPIO 1 (D-)
Breakout #2 → Board 1 VBUS (5V)
Breakout #2 → Board 1 GND
```

**Note:** Board 2 GPIO 2/3 are now free (unused). The PowerA auth controller connects to Board 1 (XBOX) because auth relay runs on Board X.

## Step 10: Verify on Xbox (Test 2j — Later)

Once SPI is working and both boards are communicating:
1. Unplug Board 1 from Mac
2. Plug Board 1 into the **Xbox Series X** via USB-C
3. The Xbox should briefly detect a controller (it may show up for a few seconds in Settings → Devices & connections → Accessories)
4. It will disappear after ~3 minutes because auth isn't working yet. **This is expected.**
5. If the Xbox ignores the device entirely, the GIP descriptors need adjustment — paste any UART output into Claude Code

---

## Quick Reference: Which Terminal to Open

| Terminal | Command | What It Does |
|----------|---------|-------------|
| 1 | `./tools/scripts/monitor.sh --log` | UART debug from Board 2 |
| 2 | `./tools/scripts/usb_watch.sh 046D` | Watch for Logitech USB events |
| 3 | Claude Code | I fix code based on what you see |

## If Something Goes Wrong

1. **Read the UART output** — it tells you what the firmware is doing
2. **Paste it into Claude Code** — I can diagnose from the messages
3. **Common fixes:**
   - No UART output → wrong pin or wrong baud rate
   - G29 not detected → D+/D- swapped, or resistor groups missing (3×200Ω parallel per line)
   - SPI all errors → MOSI/MISO swapped
   - Board keeps rebooting → watchdog reset, something hanging in init
4. **To reflash:** Hold BOOT, plug in, run `./tools/scripts/flash.sh g29` or `xbox`
