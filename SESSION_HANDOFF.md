# Session Handoff — G29-to-Xbox Bridge

## Date: March 26, 2026 (Evening)

## System Status Summary

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Auth passthrough | FULLY WORKING | 4 rounds, Xbox accepts, CONFIG state |
| Auth safety gating | ALL PROTECTIONS ACTIVE | Gate, mid-session, timeout disconnect |
| SPI master→slave (downstream) | 98.7% reliable | Mode 3 fix resolved CPHA=0 freeze |
| SPI slave→master (upstream) | ~70% reliable | Mode 3 first-byte glitch, needs work |
| G29 USB host (PIO-USB) | NOT WORKING | Cold boot enum fails, deferred |
| Xbox GIP protocol | FULLY WORKING | ANNOUNCE, IDENTIFY, wheel config, auth relay |
| Xbox input reports | NOT STARTED | Blocked on SPI + G29 host |

## What Was Done This Session

### Auth Safety Gating (Lessons #35-38 — ALL DONE)

All four end-user protection mechanisms are now implemented:

1. **Startup gate (#35)**: `tud_init(0)` then immediate `tud_disconnect()`. Polling loop waits up to 15s for `auth_host_connected()` (requires GIP_HOST_READY state). Only then calls `tud_connect()` to enable D+. Gate typically passes in ~609ms.

2. **Startup abort (#36)**: If auth controller not detected within 15s, device halts permanently with D+ low. Logs error, never enables Xbox USB.

3. **Mid-session disconnect (#37)**: Main loop checks `auth_host_connected()` with 3-second debounce. PowerA transiently disconnects after Xbox CONFIG state (~1-1.5s) and during GIP re-handshake (~100ms) — debounce handles both. If auth stays gone >3s, calls `tud_disconnect()`.

4. **Challenge timeout (#38)**: 5s auth relay timeout now calls `tud_disconnect()` instead of silently dropping the challenge.

Additional fixes:
- Cross-core data race fixed: `auth_connected` and `gip_state` made `volatile` in auth_host.c
- Added `auth_host_get_state()` API for diagnostics
- Dev VID/PID changed to 0x1209/0x029B in usb_descriptors.c to reset Xbox fingerprint

### SPI Bug Fix — Root Cause Found and Fixed

**Root cause**: RP2040's PL022 SPI peripheral in slave mode with CPHA=0 (SPH=0) freezes the shift register after the first byte when CS stays low. This is a documented PL022 hardware limitation.

**Fix**: Changed both master and slave from SPI Mode 0 (CPOL=0, CPHA=0) to Mode 3 (CPOL=1, CPHA=1).

- Downstream (master→slave): went from 0% to 98.7% success rate
- Upstream (slave→master): ~70% — still has first-byte corruption issues
- Loopback test confirmed SPI peripheral works perfectly internally
- SPI FIFO flush on init was already in place (not the issue)
- DMA config verified correct via register dumps (INCR_WR=1, INCR_RD=0 for RX)

### Output Quieting

- Board G status: 30s interval (was 5s), removed verbose sub-lines
- Board X status: 30s interval (was 5s), simplified
- AUTH-HB heartbeat: 60s (was 10s)
- AUTH-RX: filtered periodic packets (0x02 ANNOUNCE, 0x03 STATUS, 0x20 INPUT)
- SPI CRC failures: first 5 only on both sides

### G29 PIO-USB (NOT fixed — deferred)

- Cold boot enumeration still fails
- Spurious unmount retry (connected=false after failed enum) implemented but G29 still doesn't mount
- Bus reset approaches tried: GPIO pre-init, PIO port reset, `tuh_rhport_reset_bus` — none worked
- This is a separate PIO-USB issue, not blocking SPI or auth work

## Current Hardware State

```
BOARD G (RP2040-Zero labeled "G") — SPI SLAVE WORKING
├── USB-C → Mac (CDC debug output via screen /dev/tty.usbmodem* 115200)
├── USB-A Breakout #1 → G29 wheel (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Slave (GPIO 8=TX, 9=CS, 10=SCK, 11=RX) → Board X
│   └── SPI Mode 3 (CPOL=1, CPHA=1), 1MHz clock
├── GPIO 5 ← Board X GPIO 8 (UART1 RX for debug relay)
├── GPIO 12 ← Board S GPIO 12 (UART1 — sniffer link, PAUSED)
├── GND shared with Board X and Board S
├── 5V shared rail with Board X
└── Firmware: firmware_g29.uf2 (Build: Mar 26 2026)
    Features: SPI GIP telemetry decode, UART relay

BOARD X (RP2040-Zero labeled "X") — GIP + AUTH FULLY WORKING
├── USB-C → Xbox Series X (native USB device, GIP protocol)
│   └── VID/PID: 0x1209/0x029B (dev identity, resets Xbox fingerprint)
├── USB-A Breakout #2 → Xbox Wireless Controller (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Master (GPIO 2=SCK, 3=MOSI, 4=MISO, 5=CS) → Board G
│   └── SPI Mode 3 (CPOL=1, CPHA=1), 1MHz clock
├── GPIO 8 = UART1 TX (debug output → Board G GPIO 5)
├── 5V shared rail with Board G
└── Firmware: firmware_xbox.uf2 (Build: Mar 26 2026)
    Clock: 120MHz, Watchdog: 2s, Core 1: PIO-USB host + VM
    DMA: SPI on ch 10/11, PIO-USB on low channels
    PIO-USB: Pico-PIO-USB 0.5.3 (GP2040-CE fork)
    Auth safety: startup gate (15s), mid-session debounce (3s), challenge timeout (5s)

AUTH CONTROLLER — Xbox Wireless Controller (VID=045E, PID=0B12)
├── Connected via USB-C to Board X USB-A Breakout #2
├── GIP interface: class=FF/47/D0, endpoints 0x02/0x82
├── Full GIP handshake: ANNOUNCE → IDENTIFY (236 bytes) → Init → Auth
├── LED illuminates, rumbles briefly on init
└── Auth chip: produces valid 58-byte crypto signatures (4 rounds)

BOARD S (3rd RP2040-Zero labeled "S") — DISCONNECTED

G29 WHEEL
├── Can be plugged into Board G USB-A Breakout #1
├── Mode switch: PS3
├── 12V power adapter: connected
└── Status: PIO-USB enumeration fails on cold boot (deferred)
```

## Next Steps (Priority Order)

### 1. Fix SPI Upstream (Slave→Master) Reliability
Currently ~70% success rate vs 98.7% downstream. Known Mode 3 issue: first byte corruption after CS assert because SCK idles HIGH with CPOL=1, causing a clock glitch.

Approaches to try (in order):
1. **Dummy byte prefix** — master sends/expects 1 dummy byte, discards it
2. **Periodic SPI peripheral reset** — slave re-inits SPI between frames
3. **GPIO CS interrupt** — slave uses CS rising edge to reset SPI state
4. **PIO-based SPI slave** (long-term) — bypasses PL022 entirely. Reference: MichaelBell/spi-ram-emu

Diagnostic code (GPIO func checks, SPI-DIAG) should be cleaned up after fix is confirmed.

### 2. Fix G29 PIO-USB Cold Boot Enumeration
G29 does not enumerate on cold boot via PIO-USB. Unplug/replug sometimes triggers calibration but full enumeration still fails. This is a PIO-USB timing/reset issue.

### 3. Send Wheel Input Reports to Xbox
Once SPI and G29 host both work:
- Read G29 wheel data from Board G via SPI
- Map G29 inputs to Xbox wheel report format (cmd 0x20)
- Send periodic input reports via `gip_send_input_report()`

## Key Technical Decisions

### Auth Safety Architecture
Auth gate uses `tud_init(0)` + `tud_disconnect()` pattern: TinyUSB initializes but D+ stays low. Only after `auth_host_connected()` returns true does `tud_connect()` pull D+ high. This prevents Xbox from ever seeing the device before auth is ready, protecting against the 0x82d60003 permanent block.

Mid-session monitoring uses a 3-second debounce because:
- PowerA controller transiently disconnects for ~1-1.5s after Xbox reaches CONFIG state
- GIP re-handshake causes ~100ms disconnect
- Both are normal and should not trigger Xbox disconnection

### SPI Mode 3 (CPOL=1, CPHA=1) Required for PL022 Slave
The PL022 SPI peripheral in slave mode with CPHA=0 freezes the shift register after byte 1 when CS stays low. Only CPHA=1 modes (Mode 1 or Mode 3) support multi-byte slave reception. This is a PL022 hardware limitation documented in the datasheet but not in the Pico SDK docs.

### PIO-USB: Direct Polling (not TinyUSB callbacks)
TinyUSB's callback mechanism does NOT work reliably with PIO-USB in dual-stack (device+host) mode. Both IN and OUT transfers use direct PIO-USB endpoint polling.

## Key Files Modified This Session
- `firmware-xbox/src/main.c` — auth gate, mid-session disconnect, challenge timeout
- `firmware-xbox/src/auth_host.c` — volatile fixes, state getter, quiet AUTH-RX logging, 60s heartbeat
- `firmware-xbox/include/auth_host.h` — `auth_host_get_state()` API
- `firmware-xbox/src/spi_master.c` — SPI Mode 3, quiet CRC logging
- `firmware-xbox/src/usb_descriptors.c` — dev VID/PID 0x1209/0x029B
- `firmware-g29/src/spi_slave.c` — SPI Mode 3, saved DMA configs, full re-configure on restart
- `firmware-g29/include/bridge_config.h` — SPI mode comment update
- `firmware-g29/src/g29_host.c` — unlimited spurious unmount retries
- `firmware-g29/src/main.c` — 30s status, quiet output
- `shared/spi_protocol.h` — SPI clock back to 1MHz

## Build Commands
```bash
export PICO_SDK_PATH=/Users/bradleycage/pico-sdk

# Board G
cd ~/g29proj/g29-xbox-bridge/firmware-g29/build && cmake .. && make -j4

# Board X
cd ~/g29proj/g29-xbox-bridge/firmware-xbox/build && cmake .. && make -j4

# Flash (hold BOOT, plug in, release)
cp firmware-xbox/build/firmware_xbox.uf2 /Volumes/RPI-RP2/
```

## DWEII RP2040-Zero Pinout Reference
- Right side: GPIO 0-8
- Bottom: GPIO 9-12, then jumps to GPIO 26
- Left side: GPIO 26-29
- Top: 3V3, GND, 5V
- **NO PADS:** GPIO 13, 14, 15, 17-25
- Internal only: GPIO 16 (RGB LED)
