# Session Handoff — G29-to-Xbox Bridge

## Date: March 26, 2026 (Updated)

## MILESTONE: Auth Relay Pipeline Working End-to-End

### Xbox → Board X → Auth Controller → Board X → Xbox — All Links Proven

After a marathon debugging session, **all three USB links are now operational**:

1. **Xbox → Board X** (native USB device): Full GIP handshake — ANNOUNCE, IDENTIFY,
   Power states, LED/Config, AUTH challenges all flowing correctly.
2. **Board X → Xbox** (native USB device): Chunked metadata response, ACK handling,
   all working on first attempt.
3. **Board X → Auth Controller** (PIO-USB host): Bidirectional GIP communication
   verified — controller sends ANNOUNCE, IDENTIFY response, and accepts commands.

The auth controller (Xbox Wireless, VID=045E PID=0B12) responds to GIP commands
via PIO-USB. Its LED illuminates on Power ON. It sends GIP ANNOUNCE packets and
IDENTIFY descriptor responses.

### Critical Fix: TinyUSB Callback Bug
PIO-USB IN endpoint transfers completed at the hardware level but TinyUSB's
`on_rx_complete` callback never fired. Root cause: TinyUSB's event queue/dispatch
mechanism doesn't work reliably with PIO-USB in dual-stack mode (device + host).
**Fix: bypass TinyUSB callbacks entirely and poll PIO-USB endpoint state directly
via `pio_usb_ll_transfer_start()` + polling `ep->has_transfer` / `ep->actual_len`.**

### Current Blocker: Auth 0xC1 + Controller Crash on Status Query

The full auth relay pipeline works end-to-end. IDENTIFY handshake completes
with proper chunked ACK format. GP2040-CE 4-step init sequence sends correctly.
Both Xbox Wireless (045E:0B12) and PowerA (20D6:205B) controllers accept init.

But BOTH controllers return `0xC1` ("auth waiting/not available") to 58-byte
auth challenges, and **crash/disconnect on the 2-byte auth status query**.

Pattern (repeats 4x until Xbox gives up):
1. Xbox sends 58-byte auth challenge (seq N) → forwarded → 0xC1 response
2. Xbox sends 2-byte auth status query (seq N+1, payload `01 01`) → forwarded
3. Controller USB-disconnects immediately after receiving the 2-byte query
4. Controller remounts, re-initializes, Xbox retries

Status:
1. ~~Receive ANNOUNCE from controller~~ ✅ DONE
2. ~~IDENTIFY handshake with chunked ACK~~ ✅ DONE (proper Xbox ACK format)
3. ~~GP2040-CE 4-step init (power config + ON + LED + rumble)~~ ✅ DONE
4. ~~Forward Xbox's auth challenges to controller~~ ✅ DONE
5. ~~Receive auth responses from controller~~ ✅ DONE (0xC1 = waiting)
6. ~~Relay auth responses back to Xbox~~ ✅ DONE
7. Fix 2-byte auth status query crash ← **NEXT BLOCKER**
8. Get real auth crypto data (not 0xC1) from controller

### Key Discovery: Chunked ACK Format (SOLVED)
Xbox ACK payload (9 bytes) uses raw byte counts:
`00 [cmd] 20 [received_lo] [received_hi] 00 00 [remaining_lo] [remaining_hi]`

XGIP unhash needed to convert hashed total to raw total for first ACK's
remaining field. Both controllers accept this format and advance through
all IDENTIFY chunks without retrying.

### Key Discovery: GP2040-CE Init Sequence
After IDENTIFY, GP2040-CE sends 4 commands (from XBOneDescriptors.h):
1. Power config: `{0x06,0x62,0x45,0xB8,0x77,0x26,0x2C,0x55,0x53,...,0x1F}` (15 bytes)
2. Power ON: `{0x00}` (1 byte)
3. LED on: `{0x00,0x01,0x14}` (3 bytes, cmd 0x0A)
4. Rumble enable: `{0x00,0x0F,...,0xEB}` (9 bytes, cmd 0x09)

### Theories for 0xC1 + Crash

1. **Auth challenges arrive too early** — first challenge is buffered from Xbox
   before controller even mounts. Controller gets a stale challenge immediately
   after init, before auth chip settles. Try: delay auth forwarding 500ms after
   init, or discard pre-init challenges.

2. **2-byte query format wrong** — payload `01 01` might need different GIP
   wrapping or flags when sent to controller vs what Xbox sends.

3. **0xC1 IS the correct "processing" response** — controller needs time to
   compute crypto. The 2-byte query arriving during computation causes crash.
   Try: wait for controller to send the real response on its own, don't forward
   the 2-byte queries.

4. **PIO-USB throughput** — rapid command succession (init + challenge + query)
   may overwhelm PIO-USB. Try: add inter-command delays.

Controller behavior observed:
- PowerA (20D6:205B): 3 GIP interfaces, endpoints 0x01/0x81
- Xbox Wireless (045E:0B12): 6 GIP interfaces, endpoints 0x02/0x82
- Both complete IDENTIFY + init, both return 0xC1, both crash on 2-byte query
- PowerA sends INPUT (0x20), CONFIG (0x21), VIRTUAL_KEY (0x07) — fully alive
- PowerA sends STATUS with `80 00 00 00` (wired, no battery)

## Current Hardware State

```
BOARD G (RP2040-Zero labeled "G") — WORKING
├── USB-C → Mac (CDC debug output via screen /dev/tty.usbmodem* 115200)
├── USB-A Breakout #1 → G29 wheel (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Slave (GPIO 8=TX, 9=CS, 10=SCK, 11=RX) → Board X
├── GPIO 5 ← Board X GPIO 8 (UART1 RX for debug relay)
├── GPIO 12 ← Board S GPIO 12 (UART1 — sniffer link, PAUSED)
├── GND shared with Board X and Board S
├── 5V shared rail with Board X
└── Firmware: firmware_g29.uf2 (Build: Mar 26 2026)
    Features: SPI GIP telemetry decode, UART relay

BOARD X (RP2040-Zero labeled "X") — GIP + PIO-USB BOTH WORKING
├── USB-C → Xbox Series X (native USB device, GIP protocol)
├── USB-A Breakout #2 → Xbox Wireless Controller (GPIO 0=D+, GPIO 1=D-)
│   └── 3×200Ω parallel resistors on D+/D-, VBUS+GND direct
├── SPI Master (GPIO 2=SCK, 3=MOSI, 4=MISO, 5=CS) → Board G
├── GPIO 8 = UART1 TX (debug output → Board G GPIO 5)
├── 5V shared rail with Board G
└── Firmware: firmware_xbox.uf2 (Build: Mar 26 2026)
    Clock: 120MHz, Watchdog: 2s, Core 1: PIO-USB host + VM
    DMA: SPI on ch 10/11, PIO-USB on low channels
    PIO-USB: Pico-PIO-USB 0.5.3 (GP2040-CE fork)

AUTH CONTROLLER — Xbox Wireless Controller (VID=045E, PID=0B12)
├── Connected via USB-C to Board X USB-A Breakout #2
├── GIP interface: class=FF/47/D0, endpoints 0x02/0x82
├── Sends GIP ANNOUNCE, IDENTIFY response
├── LED illuminates on Power ON command
└── Auth chip accessible via GIP cmd 0x06

BOARD S (3rd RP2040-Zero labeled "S") — DISCONNECTED

G29 WHEEL
├── Can be plugged into Board G USB-A Breakout #1
├── Mode switch: PS3
├── 12V power adapter: connected
└── Status: Works in legacy mode (PID C294, 8-bit steering)
```

## What Works

### GIP Protocol (Xbox ↔ Board X)
- ✅ ANNOUNCE accepted by Xbox on first try (GP2040-CE 28-byte blob)
- ✅ IDENTIFY metadata via XGIP chunked protocol (202 bytes, 4 chunks)
- ✅ Xbox sends Power states (0x06 config, 0x05 QUIESCE, 0x00 ON)
- ✅ Xbox sends LED/Config request (cmd 0x0A)
- ✅ Xbox sends AUTH challenges (cmd 0x06, 58-byte payloads, 4 rounds)
- ✅ Proper keepalive (15s, only after auth, correct payload)
- ✅ Xbox wakes from sleep with audible sound on connection

### PIO-USB Host (Board X → Auth Controller)
- ✅ Controller mounts on PIO-USB (VID=045E, PID=0B12)
- ✅ GIP endpoints found and opened (class FF/47/D0, only first pair)
- ✅ OUT transfers work — Power ON lights up controller LED
- ✅ IN transfers work — direct PIO-USB polling (bypasses TinyUSB callbacks)
- ✅ Controller sends GIP ANNOUNCE (cmd=0x02) with VID/PID
- ✅ Controller sends IDENTIFY response (cmd=0x04, chunked)
- ✅ ANNOUNCE handled once (no more Power ON loop)
- ✅ GIP host state machine reaches READY state

### Auth Relay Infrastructure
- ✅ Auth challenges buffered from Xbox (Core 0) to shared buffer
- ✅ Core 1 picks up challenges from shared buffer
- ✅ Challenges forwarded to controller via PIO-USB OUT endpoint
- ✅ Auth responses received from controller and relayed to Xbox
- ✅ Challenge dedup by seq (Xbox retries ignored)
- ✅ Response dedup by seq (controller retries relayed once)
- ❌ Controller responds with 0xC1 "auth not available" (needs IDENTIFY handshake)

### Infrastructure
- ✅ DMA channel conflict FIXED (SPI ch 10/11, PIO-USB low channels)
- ✅ Board X stable — no PANIC / watchdog reboot
- ✅ Board X UART debug on GPIO 8 → Board G UART relay working
- ✅ Duplicate endpoint bug fixed (only open first EP pair)

## What Doesn't Work Yet
- ❌ Auth completion — controller returns 0xC1 "not available" instead of crypto data
- ❌ SPI telemetry from Board X shows stale data (SPI:ERR on Board G side)
- ❌ UART sniffer relay (Board S → Board G) — paused
- ❌ G29 mode switch (PID C294→C24F) — unchanged from prior session

## Next Steps (Priority Order)

### 1. Fix GIP ACK Format for Controller
The controller doesn't accept our ACKs, causing infinite retries. The ACK payload
needs chunk progress bytes (received/remaining counts). Implement the Xbox ACK
format documented above in the "Key Discovery" section. This will fix both
IDENTIFY chunk ACKs and auth response ACKs.

### 2. Complete IDENTIFY Handshake Before Auth
The controller returns 0xC1 "auth not available" because it needs the IDENTIFY
handshake completed first. Sequence should be:
1. Receive ANNOUNCE → send IDENTIFY request (with INTERNAL flag)
2. Receive IDENTIFY chunks → ACK each with proper chunk progress format
3. Send Power ON after IDENTIFY completes
4. Controller auth chip should now be available for challenges

### 3. ACK Xbox Auth Challenges
Xbox sends auth with `needsAck=1` (flags=0x30). We should ACK these to prevent
Xbox retries. Use same ACK format as controller-side.

### 4. SPI Telemetry
Board G shows SPI:ERR. The SPI master/slave link needs debugging separately.

## Key Technical Decisions

### PIO-USB: Direct Polling for BOTH IN and OUT (not TinyUSB callbacks)
TinyUSB's callback mechanism (both `on_rx_complete` and `on_tx_complete`) does NOT
work reliably with PIO-USB in dual-stack (device+host) mode. Transfers complete at
the PIO hardware level but TinyUSB never dispatches callbacks.

**Solution for IN (RX)**: Use `pio_usb_ll_transfer_start()` to submit transfers,
then poll `ep->has_transfer` and `ep->actual_len` in the main loop.

**Solution for OUT (TX)**: Submit via `tuh_edpt_xfer()` (still works for submission),
but poll the OUT endpoint's `has_transfer` flag to detect completion instead of
relying on `on_tx_complete` callback. Without this, `tx_pending` stays true forever
and blocks all subsequent OUT transfers (this was the root cause of auth challenges
never reaching the controller in the previous session).

### Duplicate Endpoint Prevention
Xbox Wireless Controller has multiple GIP interfaces sharing the same endpoint
numbers (e.g., two interfaces both claiming 0x02/0x82). Opening the same endpoint
twice via `tuh_edpt_open()` corrupts PIO-USB's endpoint pool. Fix: only open the
FIRST occurrence of each endpoint address.

## GIP Protocol Fixes Applied This Session

| Fix | File | Detail |
|-----|------|--------|
| ANNOUNCE payload | gip_protocol.c | GP2040-CE 28-byte blob + 3-byte timestamp |
| ANNOUNCE flags | gip_protocol.c | internal=1 (0x20), fixed seq=1 |
| Device descriptor | usb_descriptors.c | bDeviceSubClass/Protocol = 0xFF/0xFF |
| Endpoint bInterval | usb_descriptors.c | 1ms polling |
| Config bmAttributes | usb_descriptors.c | 0xA0 remote wakeup |
| MS OS vendor code | usb_descriptors.c | 0x20 |
| Security string | usb_descriptors.c | "Xbox Security Method 3..." at index 4 |
| Metadata blob | gip_wheel_metadata.h | GP2040-CE 202-byte gamepad descriptor |
| XGIP chunking | gip_protocol.c | Full protocol: 2-byte offset, hashed lengths, end-of-chunk |
| Single IDENTIFY | gip_protocol.c | Only respond to first request |
| Sequence matching | gip_protocol.c | Use request's seq for chunked response |
| Keepalive | gip_protocol.c | 15s interval, after auth, payload {0x80,0,0,0} |
| PIO-USB RX | auth_host.c | Direct PIO polling (bypass TinyUSB callbacks) |
| Single EP open | auth_host.c | Only open first endpoint pair (prevent duplicates) |
| ANNOUNCE once | auth_host.c | Only send Power ON on first ANNOUNCE |
| Power ON flags | auth_host.c | INTERNAL flag (0x20) required — controller ignores without it |
| TX completion | auth_host.c | Poll PIO-USB OUT endpoint directly (on_tx_complete callback broken) |
| ACK queue | auth_host.c | Deferred ACK queue for when tx_pending blocks immediate send |
| Auth challenge dedup | gip_protocol.c | Track last seq, ignore Xbox retries |
| Auth response dedup | auth_host.c | Track last seq, relay each response once |
| Auth flags | auth_host.c | INTERNAL flag on auth challenge forwarding to controller |

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

## Key Files Changed This Session
- `firmware-xbox/src/gip_protocol.c` — ANNOUNCE, XGIP chunking, ACK handling, state machine
- `firmware-xbox/include/gip_protocol.h` — XGIP constants, keepalive fix
- `firmware-xbox/src/usb_descriptors.c` — Device class, bInterval, MS OS, security string
- `firmware-xbox/include/gip_wheel_metadata.h` — GP2040-CE 202-byte gamepad descriptor
- `firmware-xbox/src/auth_host.c` — Direct PIO-USB polling, single EP open, GIP host handshake

## DWEII RP2040-Zero Pinout Reference
- Right side: GPIO 0–8
- Bottom: GPIO 9–12, then jumps to GPIO 26
- Left side: GPIO 26–29
- Top: 3V3, GND, 5V
- **NO PADS:** GPIO 13, 14, 15, 17–25
- Internal only: GPIO 16 (RGB LED)
