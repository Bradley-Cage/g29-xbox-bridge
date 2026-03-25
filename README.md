# G29-to-Xbox Bridge

Custom RP2040 firmware that lets a **Logitech G29 PlayStation racing wheel** work natively on an **Xbox Series X** — with full force feedback and authentication passthrough.

The bridge uses two RP2040-Zero microcontrollers linked by SPI. One board hosts the G29 wheel and a harvested Xbox auth board. The other presents itself to the Xbox as a legitimate controller, running Microsoft's GIP protocol with a bytecode force feedback VM.

## Why This Exists

The Logitech G29 is a great racing wheel, but it doesn't work on Xbox. Microsoft's mandatory cryptographic authentication means you can't just translate USB reports — the console will reject any controller that can't prove it's licensed hardware. This project solves that by pairing PIO-based USB host emulation with a real Xbox auth chip harvested from a donor controller.

## How It Works

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │                                                                     │
  │  G29 Wheel ──USB──► RP2040 #2 (G29 Board) ──SPI 1kHz──► RP2040 #1 (Xbox Board) ──USB──► Xbox Series X
  │                      ▲                                    │
  │  Auth Board ──USB────┘                                    │
  │  (PowerA)    (PIO Host)                                   │
  │                                                           │
  │  RP2040 #2 handles:              RP2040 #1 handles:       │
  │   • G29 HID polling (1kHz)        • Xbox GIP device       │
  │   • Input parsing                 • Equation Language VM   │
  │   • Auth board relay              • Force feedback engine  │
  │   • G29 FFB output                • SPI master             │
  └─────────────────────────────────────────────────────────────────────┘
```

**Board 1 (Xbox)** — Connects to the Xbox Series X via its native USB-C port. Speaks Microsoft's GIP (Gaming Input Protocol) including the Equation Language bytecode VM for force feedback. Acts as SPI master.

**Board 2 (G29)** — Hosts two PIO USB ports: one for the G29 wheel (polling at 1kHz) and one for the harvested PowerA auth board. Relays parsed input and auth responses to Board 1 over SPI. Sends FFB commands to the G29.

**SPI Link** — 32-byte frames at 10 MHz / 1kHz rate. CRC-8 on every frame. All values use fixed-point integer encoding (no floats on the wire — Cortex-M0+ doesn't handle misaligned float access in packed structs).

## Features

- **1kHz input polling** — Steering, pedals (gas/brake/clutch), 15+ buttons, D-pad, paddle shifters
- **Xbox authentication passthrough** — Harvested PowerA auth board handles Microsoft's cryptographic handshake via fragmented SPI relay
- **Force feedback translation** — Xbox Equation Language bytecode VM (16 slots, float math) translated to G29 FFB commands
- **Graceful degradation** — Under overload, the bridge sheds FFB/input fidelity instead of crashing
- **Configurable mapping** — Steering deadzone, range (180-900 degrees), linearity curves, pedal sensitivity, FFB intensity
- **USB CDC debug output** — Real-time debug logging over USB without extra hardware

## Hardware Required

| Component | Purpose |
|-----------|---------|
| 2x RP2040-Zero (DWEII or Waveshare-compatible) | Bridge processors |
| Logitech G29 wheel + pedals | Input device |
| Xbox Series X | Target console |
| PowerA Wired Controller for Xbox Series X\|S | Auth board donor |
| 2x USB Type-A female breakout (2.54mm pitch) | G29 + auth board USB connections |
| 4x 68 ohm resistors | USB signal series resistors |
| Hookup wire, solder, perfboard | Assembly |

## Wiring

### Board 2 (G29 Board)

| Pin | Connection | Notes |
|-----|-----------|-------|
| GPIO 0 | G29 USB D+ (via 68R) | PIO USB Host — G29 wheel |
| GPIO 1 | G29 USB D- (via 68R) | |
| GPIO 2 | Auth board D+ (via 68R) | PIO USB Host — PowerA auth |
| GPIO 3 | Auth board D- (via 68R) | |
| VBUS | USB breakout VCC (both) | Powers G29 + auth board |
| GND | USB breakout GND (both) | |
| USB-C | Mac (for flashing/debug) | Native USB — CDC debug output |

### Board 1 (Xbox Board)

| Pin | Connection | Notes |
|-----|-----------|-------|
| USB-C | Xbox Series X USB-A port | Native USB device mode (GIP) |

### SPI Link (Board 1 ↔ Board 2)

| Board 1 (Master) | Board 2 (Slave) | Signal |
|-------------------|-----------------|--------|
| GPIO TBD | GPIO TBD | SCK |
| GPIO TBD | GPIO TBD | MOSI |
| GPIO TBD | GPIO TBD | MISO |
| GPIO TBD | GPIO TBD | CS |
| GND | GND | Common ground |

> **Note:** Verify GPIO positions against the [Waveshare RP2040-Zero pinout](https://www.waveshare.com/wiki/RP2040-Zero) before soldering. The DWEII boards follow the same layout.

## Project Structure

```
g29-xbox-bridge/
├── firmware-g29/               # RP2040 #2 firmware (G29 + auth)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── auth_relay.h        # Auth board communication
│   │   ├── cdc_debug.h         # USB CDC debug output
│   │   ├── spi_protocol.h      # SPI slave driver
│   │   ├── spi_slave.h         # SPI slave interface
│   │   └── tusb_config.h       # TinyUSB config
│   ├── src/
│   │   ├── main.c              # Entry point, core setup
│   │   ├── g29_host.c          # G29 PIO USB host + HID parsing
│   │   ├── auth_relay.c        # Auth board PIO USB host + relay
│   │   ├── cdc_debug.c         # Debug output over USB CDC
│   │   └── cdc_test.c          # CDC test harness
│   └── lib/Pico-PIO-USB/       # PIO USB library (git submodule)
│
├── firmware-xbox/              # RP2040 #1 firmware (Xbox GIP)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── gip_protocol.h      # Xbox GIP protocol + Equation VM
│   │   └── spi_protocol.h      # SPI master driver
│   └── src/
│       ├── main.c              # Entry point, GIP state machine
│       └── gip_protocol.c      # GIP framing, auth, input reports
│
├── shared/
│   └── spi_protocol.h          # SPI frame definitions (32-byte frames, CRC-8)
│
├── include/                    # Shared types and config
│   ├── bridge_config.h         # Pin defs, data structures, constants
│   ├── tusb_config.h           # TinyUSB configuration
│   ├── auth_passthrough.h      # Auth relay interface
│   └── ffb_translator.h        # FFB translation interface
│
├── src/                        # Legacy single-board source (reference)
│   ├── main.c                  # Dual-core entry point
│   ├── g29_parser.c            # G29 HID report parsing
│   ├── xbox_device.c           # Xbox USB device emulation
│   ├── input_mapper.c          # G29 → Xbox input translation
│   ├── auth_passthrough.c      # Auth relay
│   └── ffb_translator.c        # FFB conversion
│
├── tests/
│   └── test_vm.c               # Native macOS test harness for Equation Language VM
│
├── tools/scripts/
│   ├── flash.sh                # Build + flash firmware to RP2040
│   ├── monitor.sh              # USB CDC / UART debug serial monitor
│   └── usb_watch.sh            # USB device hotplug monitor
│
├── docs/
│   ├── GETTING_STARTED.md      # Hardware assembly walkthrough
│   └── HARDWARE_TEST_PLAN.md   # Phase 0 gate tests
│
├── viewer/                     # Debug visualization tools
├── CMakeLists.txt              # Root build configuration
└── TESTING_PLAN.md             # Full 6-phase test plan
```

## Building

### Prerequisites (macOS)

```bash
# Install toolchain
brew install cmake
brew install --cask gcc-arm-embedded

# Verify
arm-none-eabi-gcc --version    # Should show 13.x or 14.x
```

If `arm-none-eabi-gcc` isn't found after install:
```bash
export PATH="/Applications/ARM/bin:$PATH"
# Add to ~/.zshrc to persist
```

### Clone Dependencies

```bash
# Pico SDK
cd ~
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init
echo 'export PICO_SDK_PATH="$HOME/pico-sdk"' >> ~/.zshrc
source ~/.zshrc

# PIO USB library (inside the project)
cd g29-xbox-bridge/firmware-g29
mkdir -p lib && cd lib
git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git
```

### Build Firmware

```bash
# Board 2 (G29)
cd firmware-g29
mkdir -p build && cd build
cmake .. && make -j10

# Board 1 (Xbox)
cd ../../firmware-xbox
mkdir -p build && cd build
cmake .. && make -j10
```

### Flash

1. Hold **BOOT** on the RP2040-Zero
2. Plug into Mac via USB-C — **RPI-RP2** drive appears
3. Copy the `.uf2` file:
   ```bash
   cp build/firmware_g29.uf2 /Volumes/RPI-RP2/
   ```
   Or use the helper script:
   ```bash
   ./tools/scripts/flash.sh g29
   ```

## Debugging

Debug output is available over USB CDC (no extra UART adapter needed):

```bash
./tools/scripts/monitor.sh
```

Monitor USB device changes:
```bash
./tools/scripts/usb_watch.sh
```

## Development Phases

The project follows a strict phase-gated development process. Each phase must pass all tests before advancing. See [TESTING_PLAN.md](TESTING_PLAN.md) for detailed test cases.

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Environment setup + hardware verification | Complete |
| 1 | G29 input capture via PIO USB host | In progress |
| 2 | Xbox GIP device enumeration | Planned |
| 3 | Input translation (G29 → Xbox reports) | Planned |
| 4 | Auth passthrough (PowerA board relay) | Planned |
| 5 | Force feedback (Equation Language VM → G29 FFB) | Planned |
| 6 | Integration + performance tuning | Planned |

## Technical Details

### Xbox GIP Protocol

The Xbox Series X uses Microsoft's Gaming Input Protocol (GIP):
- USB class `0xFF` (Vendor Specific), subclass `0x47`, protocol `0xD0`
- Mandatory cryptographic authentication — the console drops unauthenticated controllers after ~3 minutes
- 20-byte input reports with GIP framing
- Force feedback uses the Equation Language — a bytecode VM with 16 slots, float math, and combinators (not simple rumble motor values)

### SPI Wire Protocol

The two RP2040 boards communicate over SPI at 10 MHz with fixed 32-byte frames at 1kHz:

- **Upstream (G29 → Xbox):** Steering (16-bit), pedals, buttons, connection status, auth response fragments
- **Downstream (Xbox → G29):** Force values (Q1.14 fixed-point), power level, angle settings, auth challenge fragments
- CRC-8 on every frame with 50ms timeout for error detection
- Auth payloads fragmented across multiple frames (16 bytes/frame downstream, 10 bytes/frame upstream)

### G29 Input Mapping

| G29 | Xbox |
|-----|------|
| Steering wheel | Left stick X |
| Gas pedal | Right trigger (0-1023) |
| Brake pedal | Left trigger (0-1023) |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| L1 / R1 | LB / RB |
| Share / Options | View / Menu |
| PS button | Guide |
| D-pad | D-pad |
| Paddle shifters | Configurable |

## Key References

- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) — PIO-based USB host for RP2040
- [TinyUSB](https://github.com/hathach/tinyusb) — USB device/host stack
- [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) — Xbox Series X|S auth reference implementation
- [GIMX](https://gimx.fr/wiki/) — G29 HID format, FFB protocol, Xbox auth documentation
- Linux `hid-lg4ff.c` — G29 force feedback driver source
- Linux `xpad.c` — Xbox controller protocol reference
- [RP2040-Zero Pinout](https://www.waveshare.com/wiki/RP2040-Zero) — Board pin reference

## Status

This project is under active development. The two-board architecture is finalized and SPI communication protocol is implemented. Currently working through the phase-gated test plan to bring up each subsystem.
