# G29-to-Xbox Bridge

Use a **Logitech G29 PlayStation racing wheel** on an **Xbox Series X** — with authentication passthrough and force feedback.

Two RP2040-Zero microcontrollers form an SPI-linked bridge. One board hosts the G29 and a donor PowerA auth board via PIO-based USB. The other presents a legitimate Xbox controller to the console, running Microsoft's GIP protocol with a bytecode force feedback VM.

## Features

- [x] **1kHz input polling** — steering, gas/brake/clutch, 15+ buttons, D-pad, paddle shifters
- [x] **Xbox auth passthrough** — intact PowerA controller handles Microsoft's cryptographic handshake
- [x] **SPI bridge** — 32-byte CRC-8 frames at 10 MHz, DMA-driven, 1kHz exchange rate
- [x] **USB CDC debug** — real-time serial output over RP2040 #2's USB-C port (no extra wiring)
- [x] **Auth relay** — challenge/response fragmented across SPI frames (256-byte payloads, 500ms timeout)
- [ ] **Force feedback** — Equation Language bytecode VM translating Xbox FFB to G29 motor commands
- [ ] **Input mapping profiles** — configurable deadzone, linearity, steering range, FFB intensity

## Hardware Required

| Component | Qty | Notes |
|-----------|-----|-------|
| DWEII RP2040-Zero | 2 | USB-C, Waveshare-compatible ([Amazon B0C89Y3PPJ](https://www.amazon.com/dp/B0C89Y3PPJ)) |
| Logitech G29 wheel + pedals | 1 | PlayStation version, **set to PS3 mode** (switch on wheel base) |
| Xbox Series X | 1 | Target console |
| PowerA Wired Controller | 1 | Xbox Series X|S version — stays intact, used as auth donor |
| Keesoo USB-A female breakout | 2 | 4-pin 2.54mm pitch (VBUS, D+, D-, GND) |
| 200-ohm resistors | 12 | 3 in parallel per data line = 66.7 ohm (4 groups of 3) |
| Hookup wire, solder, perfboard | - | Standard prototyping supplies |

## Quick Start

1. [Flash Board 2](#flash) with `firmware_g29.uf2`
2. [Wire the G29 USB breakout](#board-2-g29--auth) (4 wires + 6 resistors)
3. Verify G29 enumerates via CDC debug output
4. [Flash Board 1](#flash) with `firmware_xbox.uf2`
5. [Wire the SPI link](#spi-link-board-1--board-2) (5 wires between boards)
6. Wire the auth USB breakout (same pattern as G29, different GPIOs)
7. Connect Board 1 to Xbox, Board 2 to power

See [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) for the full walkthrough with expected debug output at each step.

## Wiring

### Board 2 (G29 + Auth)

| GPIO | Function | Connects To |
|------|----------|-------------|
| 0 | PIO USB D+ (Port 0) | 3x200R parallel -> G29 breakout D+ |
| 1 | PIO USB D- (Port 0) | 3x200R parallel -> G29 breakout D- |
| 2 | PIO USB D+ (Port 1) | 3x200R parallel -> Auth breakout D+ |
| 3 | PIO USB D- (Port 1) | 3x200R parallel -> Auth breakout D- |
| 8 | SPI1 TX (MISO) | Board 1 GPIO 4 |
| 9 | SPI1 CSn | Board 1 GPIO 5 |
| 10 | SPI1 SCK | Board 1 GPIO 6 |
| 11 | SPI1 RX (MOSI) | Board 1 GPIO 7 |
| VBUS | 5V out | Both USB-A breakout VBUS pins |
| GND | Ground | Both USB-A breakouts + Board 1 GND |
| USB-C | Native USB | Mac (CDC debug) or power supply |

### Board 1 (Xbox)

| GPIO | Function | Connects To |
|------|----------|-------------|
| 4 | SPI0 RX (MISO) | Board 2 GPIO 8 |
| 5 | SPI0 CSn (manual) | Board 2 GPIO 9 |
| 6 | SPI0 SCK | Board 2 GPIO 10 |
| 7 | SPI0 TX (MOSI) | Board 2 GPIO 11 |
| GND | Ground | Board 2 GND |
| USB-C | Native USB | Xbox Series X USB-A port |

### SPI Link (Board 1 <-> Board 2)

| Signal | Board 1 (Master) | | Board 2 (Slave) |
|--------|------------------|-|-----------------|
| SCK | GPIO 6 | -> | GPIO 10 |
| MOSI | GPIO 7 | -> | GPIO 11 |
| MISO | GPIO 4 | <- | GPIO 8 |
| CS | GPIO 5 | -> | GPIO 9 |
| GND | GND pad | -- | GND pad |

Keep SPI wires short (<10 cm) and away from USB data lines.

### USB-A Breakout Resistors

Each D+ and D- line needs **3x 200-ohm resistors in parallel** (= 66.7 ohm). This is required by the Pico-PIO-USB library for impedance matching.

```
Breakout D+/D- pin --+--[200R]--+-- RP2040 GPIO
                      +--[200R]--+
                      +--[200R]--+
```

Do NOT put resistors on VBUS or GND. Only on D+ and D- (4 groups total, 12 resistors).

## How It Works

**Board 1 (Xbox + VM)** plugs into the Xbox via USB-C and presents itself as a licensed Xbox controller. It runs the GIP (Gaming Input Protocol) state machine, sends 20-byte input reports, handles keepalives, and executes the Equation Language bytecode VM for force feedback. Runs at 200 MHz. SPI master.

**Board 2 (G29 + Auth)** hosts two USB devices simultaneously using PIO-based bit-banged USB (Pico-PIO-USB). Port 0 connects to the G29 wheel, polling HID reports at 1kHz. Port 1 connects to an intact PowerA controller that handles Xbox authentication. Runs at 120 MHz (required multiple of 12 MHz for PIO USB timing). SPI slave.

**Auth Relay** — When the Xbox sends an authentication challenge (GIP command 0x06), Board 1 fragments the payload into 16-byte chunks across SPI frames. Board 2 reassembles the challenge, forwards it to the PowerA controller via USB, waits for the signed response, then fragments the response back over SPI. Board 1 reassembles and sends it to the Xbox. A 256-byte challenge takes ~16ms over SPI — well within the 500ms timeout.

## Building

### Prerequisites (macOS)

```bash
brew install cmake
brew install --cask gcc-arm-embedded

# Verify
arm-none-eabi-gcc --version
```

If `arm-none-eabi-gcc` isn't found after install:
```bash
export PATH="/Applications/ARM/bin:$PATH"
# Add to ~/.zshrc to persist
```

### Clone and Build

```bash
# Pico SDK (if not already installed)
cd ~ && git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init
echo 'export PICO_SDK_PATH="$HOME/pico-sdk"' >> ~/.zshrc && source ~/.zshrc

# PIO USB library
cd g29-xbox-bridge/firmware-g29
mkdir -p lib && cd lib
git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git

# Build Board 2 (G29)
cd ../../ && mkdir -p build && cd build
cmake .. && make -j4
# Output: firmware_g29.uf2 (110 KB)

# Build Board 1 (Xbox)
cd ../../firmware-xbox && mkdir -p build && cd build
cmake .. && make -j4
# Output: firmware_xbox.uf2 (79 KB)
```

### Flash

1. Hold **BOOT** button on the RP2040-Zero
2. Plug into Mac via USB-C — `RPI-RP2` drive appears
3. Copy the UF2: `cp build/firmware_g29.uf2 /Volumes/RPI-RP2/`

Or use the helper: `./tools/scripts/flash.sh g29`

## Debugging

Debug output comes over USB CDC on Board 2's USB-C port (no extra UART adapter needed):

```bash
screen /dev/tty.usbmodem* 115200
# Exit: Ctrl-A, K, Y
```

Expected output when everything is connected:
```
=== G29 Bridge -- RP2040 #2 (G29 + Auth) ===
[INIT] G29 USB Host...
[INIT] Auth Relay...
[USB] Device mounted: addr=1 VID=046D PID=C24F
[G29] Native G29 detected (PID=C24F)
[AUTH] Auth board mounted: addr=2 VID=XXXX PID=XXXX
[STATUS] G29:OK Auth:OK(0) Steer=8000 Gas=FF Brake=FF
```

The [interactive viewer](viewer/index.html) provides wiring diagrams, pin tables, a build checklist, and log file diagnostics. Serve it locally: `python3 -m http.server 8080 -d viewer/`

## Development Status

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Environment setup + hardware verification | Done |
| 1 | G29 input capture via PIO USB host | Done |
| 2 | Xbox GIP device enumeration + metadata | Done |
| 3 | SPI bridge + input translation | Done |
| 4 | Auth passthrough (PowerA relay) | Done |
| 5 | Force feedback (Equation Language VM -> G29 FFB) | In progress |
| 6 | Integration, profiles, performance tuning | Planned |

See [TESTING_PLAN.md](TESTING_PLAN.md) for detailed test cases per phase.

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/GETTING_STARTED.md) | Hardware assembly walkthrough |
| [Architecture](docs/ARCHITECTURE.md) | Why two boards, protocol choices, design rationale |
| [Wiring Reference](docs/WIRING_REFERENCE.md) | Authoritative pin assignments and resistor placement |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Common problems and fixes |
| [Building](docs/BUILDING.md) | Full build guide with prerequisites |
| [GIP Protocol](docs/GIP_PROTOCOL.md) | Xbox Gaming Input Protocol reference |
| [SPI Protocol](docs/SPI_PROTOCOL.md) | Inter-board frame format and auth fragmentation |
| [Equation Language](docs/EQUATION_LANGUAGE.md) | Xbox force feedback bytecode VM |
| [Hardware Test Plan](docs/HARDWARE_TEST_PLAN.md) | Phase 0 hardware gate tests |
| [Changelog](docs/CHANGELOG.md) | Major decisions and design changes |

## Troubleshooting

**G29 not detected** — Check D+/D- aren't swapped. Verify G29 mode switch is set to PS3 (not PS4). Confirm 3x200R resistor groups are on D+/D- lines. Try a different USB cable on the G29.

**Xbox disconnects after ~3 minutes** — Authentication is failing. Check the PowerA controller is connected to USB-A breakout #2 and enumerated (`[AUTH] Auth board mounted` in debug output). If VID/PID shows but auth fails, the USB transfer format may need adjustment — see `AUTH_BREQUEST_OUT` in `auth_relay.c`.

**SPI all CRC errors** — MOSI/MISO wires are likely swapped. Board 1 GPIO 7 (TX) must connect to Board 2 GPIO 11 (RX). Also check GND wire between boards.

**No debug output on Mac** — USB CDC needs ~1.5 seconds to enumerate. Wait for the `tty.usbmodem*` device to appear. If it never appears, try a different USB-C cable (some are charge-only).

**Board keeps rebooting** — Watchdog reset (100ms timeout). Usually caused by a blocking call in the main loop. Check CDC debug output for the last message before reboot.

## Project Structure

```
g29-xbox-bridge/
+-- firmware-g29/           # Board 2: G29 USB host + auth relay + SPI slave
|   +-- src/                # main.c, g29_host.c, auth_relay.c, g29_ffb.c, ...
|   +-- include/            # bridge_config.h, tusb_config.h, spi_protocol.h
|   +-- lib/Pico-PIO-USB/   # PIO USB library (git clone)
|   +-- build/              # firmware_g29.uf2
|
+-- firmware-xbox/          # Board 1: Xbox GIP device + Equation VM + SPI master
|   +-- src/                # main.c, gip_protocol.c, eq_vm.c, spi_master.c, ...
|   +-- include/            # gip_protocol.h, eq_vm.h, spi_protocol.h
|   +-- build/              # firmware_xbox.uf2
|
+-- shared/                 # Shared protocol definitions
|   +-- spi_protocol.h      # 32-byte frame structs, CRC-8, auth fragmentation
|
+-- docs/                   # Documentation
+-- viewer/                 # Interactive wiring diagrams + debug dashboard
+-- tools/scripts/          # flash.sh, monitor.sh, usb_watch.sh
+-- tests/                  # Native test harness for Equation VM
```

## References

- [Pico-PIO-USB](https://github.com/sekigon-gonnec/Pico-PIO-USB) — PIO-based USB host/device for RP2040
- [TinyUSB](https://github.com/hathach/tinyusb) — USB device/host stack
- [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) — Xbox auth passthrough reference
- [GIMX](https://gimx.fr/wiki/) — G29 HID format and FFB protocol documentation
- Linux `hid-lg4ff.c` — G29 force feedback driver source
- Linux `xpad.c` — Xbox controller protocol reference
- [RP2040-Zero Pinout](https://www.waveshare.com/wiki/RP2040-Zero) — Board pin reference

## License

MIT
