# Session Handoff — G29-to-Xbox Bridge

## Date: March 25, 2026

## Current Hardware State

```
BOARD G (RP2040-Zero labeled "G") — WORKING
├── USB-C → Mac (CDC debug output via screen /dev/tty.usbmodem* 115200)
├── USB-A Breakout #1 → G29 wheel (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Slave (GPIO 8=TX, 9=CS, 10=SCK, 11=RX) → Board X
├── GPIO 3 ← Board S GPIO 14 (UART RX for sniffer debug relay)
├── GND shared with Board S
└── Firmware: firmware_g29.uf2 (Build: Mar 25 2026 19:02:04)

BOARD X (RP2040-Zero labeled "X") — RUNNING, XBOX NOT DETECTING
├── USB-C → Xbox Series X (native USB device, GIP protocol)
├── USB-A Breakout #2 → PowerA controller (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Master (GPIO 2=SCK, 3=MOSI, 4=MISO, 5=CS) → Board G
└── Firmware: firmware_xbox.uf2 (Build: Mar 25 2026 ~19:30)
    Clock: 120MHz, Watchdog: 2s, Core 1: PIO-USB host + VM

BOARD S (3rd RP2040-Zero labeled "S") — NOT YET FLASHED
├── GPIO 14 (UART TX) → Board G GPIO 3 (UART RX) — WIRED
├── GND → Board G GND — WIRED
├── USB-A Breakout needed on GPIO 0/1 for PowerA (reuse from Board X)
├── USB-C → Xbox Series X (will be native USB device)
└── Firmware: needs firmware-sniffer/ built and flashed

G29 WHEEL
├── Plugged into Board G USB-A Breakout #1
├── Mode switch: PS3
├── 12V power adapter: connected
└── Status: G29:OK in legacy mode (PID C294, 8-bit steering)

POWERА CONTROLLER
├── Currently plugged into Board X USB-A Breakout #2
├── Confirmed working directly on Xbox (tested before session)
└── For sniffer: move breakout from Board X to Board S
```

## What Works
- ✅ G29 input capture: steering (8-bit legacy), gas, brake, buttons all reading correctly
- ✅ SPI link: 1kHz frame exchange, zero CRC errors, data propagates both directions
- ✅ Board X USB device: enumerates on Mac as "G29 Xbox Bridge" (VID=0x0E6F PID=0x02A4)
- ✅ Board X PIO-USB host: PowerA enumerated successfully on GPIO 0/1 (tested earlier)
- ✅ Board X Core 1: PIO-USB host + cooperative VM ticks running
- ✅ CDC debug on Board G: reliable, shows G29 status + SPI stats

## What Doesn't Work
- ❌ Xbox Series X does not detect Board X AT ALL — no brief detection, nothing in Accessories
- ❌ G29 mode switch (PID C294→C24F) — command sends but G29 doesn't switch. 8-bit steering only.
- ❌ G29 cold-boot detection — wheel must be unplugged/replugged after Board G powers on
- ❌ WS2812 LED on Board G — PIO1 init conflicts with PIO0 USB host, disabled for now

## Next Step: Build USB Sniffer (Board S)

### Purpose
Capture the exact byte-level GIP handshake between Xbox Series X and the PowerA controller.
This will show us what the Xbox sends first, what the controller responds with, the authentication
flow, and the exact timing — so we can replicate it in our firmware.

### Sniffer Architecture
```
Board S (RP2040 #3):
  Native USB (device) → Xbox Series X
    Presents PowerA's exact USB descriptors (VID=0x20D6 PID=0x205B)
    Forwards all Xbox→PowerA traffic

  PIO-USB (host, GPIO 0/1) → PowerA controller
    Hosts the PowerA, forwards all PowerA→Xbox traffic

  UART TX (GPIO 14) → Board G GPIO 3 (already wired)
    Dumps every packet in hex with timestamps
    Board G prints to CDC on Mac
```

### Physical Setup for Sniffing
1. Move the PowerA USB-A breakout from Board X GPIO 0/1 → Board S GPIO 0/1
2. Plug Board S USB-C → Xbox Series X
3. Board G USB-C → Mac (receives sniffer output via UART relay)
4. Board X is NOT involved during sniffing (can be disconnected)

### Firmware to Build
New directory: `firmware-sniffer/`
- Copies PowerA's USB descriptors (VID=0x20D6, PID=0x205B, class=0xFF/0x47/0xD0)
- PIO-USB host on GPIO 0/1 for PowerA
- Bidirectional transparent forwarding of all USB traffic
- UART TX on GPIO 14 at 115200 baud — hex dump every packet with direction + timestamp
- Board G firmware update: read UART RX on GPIO 3, print to CDC

### Key Questions the Sniffer Will Answer
1. What does the Xbox send FIRST after USB enumeration?
2. Does the Xbox request the MS OS descriptor?
3. What is the exact ANNOUNCE (0x02) format from a real controller?
4. What metadata does the PowerA send?
5. When does authentication start relative to the GIP handshake?
6. What are the exact timing requirements?

## Known Bugs (Lower Priority)
1. **G29 mode switch** — PID C294→C24F doesn't work via PIO-USB. Leaves wheel in 8-bit steering legacy mode.
2. **G29 cold-boot detection** — PIO-USB host doesn't detect devices connected before power-on. Requires replug.
3. **WS2812 LED** — PIO1 initialization interferes with PIO0 USB host. Needs investigation (timing? pin conflict?).
4. **SPI downstream frames empty** — Board X sends type=00 seq=0 because SPI frame population code runs but no GIP data flows yet.
5. **Cable sensitivity** — Board X requires specific USB-C cable; some cables cause boot failures or no enumeration.

## Build Commands
```bash
export PICO_SDK_PATH=/Users/bradleycage/pico-sdk

# Board G
cd ~/g29proj/g29-xbox-bridge/firmware-g29/build && cmake .. && make -j4

# Board X
cd ~/g29proj/g29-xbox-bridge/firmware-xbox/build && cmake .. && make -j4

# Flash (hold BOOT, plug in, release)
cp firmware-g29/build/firmware_g29.uf2 /Volumes/RPI-RP2/
cp firmware-xbox/build/firmware_xbox.uf2 /Volumes/RPI-RP2/
```

## Git Status
Latest commit: `93e7c88` — "Hardware testing: EP OUT fix, passive ANNOUNCE, SPI debug, bring-up fixes"
Branch: main, pushed to origin

## Key Files
- `firmware-xbox/src/usb_descriptors.c` — USB descriptors + MS OS
- `firmware-xbox/src/gip_protocol.c` — GIP state machine
- `firmware-xbox/src/auth_host.c` — PowerA PIO-USB host + auth relay
- `firmware-xbox/src/main.c` — Board X main loop (Core 0 + Core 1)
- `firmware-g29/src/main.c` — Board G main loop with SPI debug
- `firmware-g29/src/g29_host.c` — G29 PIO-USB host
- `firmware-g29/src/g29_parser.c` — Legacy DFGT 27-byte report parser
- `shared/spi_protocol.h` — SPI frame format (synced to both firmware includes)
- `tasks/lessons.md` — All accumulated learnings and bugs

## DWEII RP2040-Zero Pinout Reference
- Right side: GPIO 0–8 (Pins 1–11)
- Bottom: GPIO 9–13 (Pins 12–17)
- Left side: GPIO 14–15, 26–29 (Pins 19–20, 31–35)
- Top: 3V3, GND, 5V (Pins 37–39)
- Internal only: GPIO 16 (RGB LED), GPIO 17–25 (NO PADS)
