# Getting Started — Hardware Assembly & First Tests

## What You Need On Your Desk

- [ ] 2x DWEII RP2040-Zero boards (labeled "Board 1" and "Board 2" with tape)
- [ ] 2x Keesoo USB-A female breakout boards
- [ ] 4x 68 ohm resistors
- [ ] Hookup wire (22-24 AWG, solid core preferred for breadboard/perfboard)
- [ ] Soldering iron + solder
- [ ] USB-TTL serial adapter (FTDI or CH340)
- [ ] Logitech G29 wheel + pedals + 12V power adapter
- [ ] PowerA controller (intact — don't open it until Test 0e)
- [ ] 2x USB-C cables (Mac to each RP2040)
- [ ] Your MacBook Pro

## Step 1: Label Your Boards

Take two RP2040-Zero boards. Put a piece of tape on each:

- **Board 1** = "XBOX" — this connects to the Xbox (later) and runs the Equation VM
- **Board 2** = "G29" — this connects to the G29 wheel and the auth board

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

## Step 3: Connect UART Debug Adapter to Board 2

You need to see debug output from Board 2. This requires 2 wires soldered (or use header pins + jumper wires if you have them):

| Board 2 Pin | Wire To | Adapter Pin |
|-------------|---------|-------------|
| GPIO 16 | → | RX on USB-TTL adapter |
| GND | → | GND on USB-TTL adapter |

Once wired:
1. Plug the USB-TTL adapter into your Mac
2. Plug Board 2 into your Mac via USB-C (to power it)
3. Run:
   ```bash
   ./tools/scripts/monitor.sh
   ```
4. You should see:
   ```
   === G29 Bridge — RP2040 #2 (G29 + Auth) ===
   ```

**If you see nothing:** Check that GPIO 16 goes to the adapter's **RX** pin (not TX). Check the baud rate is 115200. Try `ls /dev/tty.usb*` to see available serial ports.

**If you see the banner repeating every second:** The watchdog is resetting. Something in the init is taking too long. Paste the output into Claude Code.

**Checkpoint: You should see the boot banner and status lines in your terminal. The status will show G29:-- Auth:-- because nothing is plugged into the USB breakouts yet. This is correct.**

## Step 4: Wire USB-A Breakout #1 (for G29)

This breakout lets the G29 plug into Board 2. You need to solder 4 wires + 2 resistors:

```
Keesoo USB-A Breakout          68Ω Resistors        Board 2
┌─────────────────┐                               ┌──────────┐
│ VBUS (red)      │──────────── direct ──────────→│ VBUS/5V  │
│ D+   (green)    │────[68Ω]─────────────────────→│ GPIO 0   │
│ D-   (white)    │────[68Ω]─────────────────────→│ GPIO 1   │
│ GND  (black)    │──────────── direct ──────────→│ GND      │
└─────────────────┘                               └──────────┘
```

**Critical details:**
- The 68Ω resistors go on **D+ and D- only** (not VBUS or GND)
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

**If the G29 doesn't enumerate:** Check wiring. Try swapping D+ and D- (breakout pin labeling may differ from the standard). Make sure the 68Ω resistors are present.

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
│ GPIO 18 (SCK)│──────────────→  │ GPIO 10 (SCK)│
│ GPIO 19 (MOSI)│─────────────→  │ GPIO 11 (MOSI)│
│ GPIO 16 (MISO)│←────────────   │ GPIO 8 (MISO)│
│ GPIO 17 (CS) │──────────────→  │ GPIO 9  (CS) │
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

## Step 9: Wire USB-A Breakout #2 (for Auth Board) — LATER

Don't wire this until you're ready to open the PowerA controller. Same wiring pattern as breakout #1 but on different GPIOs:

```
Breakout #2 → 68Ω → Board 2 GPIO 2 (D+)
Breakout #2 → 68Ω → Board 2 GPIO 3 (D-)
Breakout #2 → Board 2 VBUS (5V)
Breakout #2 → Board 2 GND
```

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
   - G29 not detected → D+/D- swapped, or 68Ω resistors missing
   - SPI all errors → MOSI/MISO swapped
   - Board keeps rebooting → watchdog reset, something hanging in init
4. **To reflash:** Hold BOOT, plug in, run `./tools/scripts/flash.sh g29` or `xbox`
