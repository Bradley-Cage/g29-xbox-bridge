# Lessons Learned

## Session Log
*Updated as corrections and discoveries accumulate.*

### Known Risks (from project docs)
1. **Pico-PIO-USB API mismatch** — Function signatures in g29_parser.c and auth_passthrough.c are based on an older library version. MUST check actual headers in `lib/Pico-PIO-USB/src/` after cloning and update call sites.
2. **G29 HID byte layout** — Parser byte offsets are from documentation, not verified against this specific wheel. Raw hex dump (TEST-P1-02) must happen before trusting the parser.
3. **Xbox GIP descriptors** — If Xbox Series X rejects the device, pull descriptors from GP2040-CE project's Xbox Series X code path.
4. **Dual PIO host** — Running two PIO USB host instances may need library patches or a USB hub fallback.
5. **Header inconsistencies** — main.c includes `"config.h"`, `"g29_host.h"`, `"xbox_device.h"`, `"input_mapper.h"` but the actual headers are `bridge_config.h`, and there are no `g29_host.h`, `xbox_device.h`, or `input_mapper.h` headers. These need to be created or includes need fixing.
6. **Type mismatches** — `bridge_config.h` defines `xbox_state_t` but `xbox_device.c` uses `xbox_report_t`. `input_mapper.c` references types/constants not in `bridge_config.h` (e.g., `mapper_config_t` with different fields, `xbox_report_t`, `XBOX_BTN_*` constants, `G29_DPAD_*` enums). These all need reconciliation.
7. **Missing function declarations** — Functions like `g29_host_init()`, `g29_host_task()`, `g29_get_state()`, `xbox_device_task()`, `xbox_is_connected()`, `xbox_set_rumble_callback()`, `xbox_get_auth_challenge()`, `xbox_submit_auth_response()` are called but not declared in any header.

### Architecture Decisions
8. **Single RP2040, Option A** — Core 0: USB device (Xbox) + GIP protocol. Core 1: FFB VM + auth. G29 PIO host uses DMA for non-blocking reads. Target 200MHz (certified), 250MHz only if measured headroom is insufficient. Approved fallback: Option B (two-chip split via SPI).
9. **Graceful degradation is mandatory** — The device must NEVER crash, hang, or enter a failed state under any compute load. Shed FFB fidelity (skip equation iterations) or drop intermediate input frames instead. All memory is static. Hardware watchdog 100ms (not 2s — too coarse).
10. **Xbox FFB is a bytecode VM** — Not simple rumble. The "Equation Language" runs 16 equation slots at 1kHz with IEEE 754 float math. RP2040 ROM float routines (Qfplib) make this feasible: fmul=62cy, fsin=584cy. Full spec in docs/GIPDocs/.
11. **GIP wheel protocol is fully documented** — Downloaded from aka.ms/gipdocs. Contains wheel state report (0x20) byte layout, static config report (0x21), equation language spec, metadata JSON format, and C# metadata compiler source. No reverse engineering needed for protocol — only for validation.

### Architecture Review Findings (Devil's Advocate — Session 1)
21. **Dual PIO USB host is unproven and high-risk** — Preferred mitigation: USB hub on PIO0 connecting both G29 and auth board through a single PIO host instance. Eliminates dual-PIO risk entirely. Test dual-PIO as optimization only.
22. **G29 mode switch causes USB disconnect/re-enumeration** — Must be tested with Pico-PIO-USB. Fallback: operate in pre-mode-switch legacy mode if re-enum fails.
23. **Core 0 priority inversion** — TinyUSB ISRs preempt G29 polling. Fix: use PIO0 DMA for G29 report reception (hardware buffers without CPU). Core 0 reads DMA buffer when ready.
24. **VM budget: trig-heavy equations blow the budget** — 16 trig-heavy equations need ~960K cycles at 250MHz, but budget is 250K. Fix: adaptive tick rate (1-4ms), run slower under heavy load rather than skip equations. Copy VM interpreter to SRAM to eliminate flash cache jitter.
25. **Shared axis state torn reads** — Multi-byte float writes from Core 0 can be half-read by Core 1. Fix: double-buffered axis state with atomic flag flip.
26. **Overclock: start at 200MHz (certified), only go to 250MHz if measured** — Test across all 3 boards at operating temperature. Architecture must work at 200MHz.
27. **Watchdog must be 100ms, not 2s** — 2s of lockup = game crash. 100ms gives fast recovery. Reset path must be defined (full chip reset → Xbox re-enumeration).
28. **G29 FFB init: must clear all slots** — Send 0xF3 (stop all), 0xF5 (disable autocenter), then set range. Otherwise default autocenter spring fights VM output.
29. **GIP keepalive required** — Protocol requires periodic heartbeat responses. Must implement keepalive timer in GIP state machine. Missed keepalive = Xbox drops device.
30. **FFB latency: 15-25ms loop vs native hardware effects** — Acknowledged for V1 (constant force only). Reserve G29 slots 1-3 for V2 hybrid mode with native spring/periodic effects.
31. **USB error recovery is mandatory** — G29: 3 retries then disconnect. Auth: 500ms timeout, 3 retries. TinyUSB: use built-in retry. Log all errors to SRAM ring buffer.
32. **VBUS power: G29 has its own 12V PSU** — USB VBUS only powers G29 MCU (~100mA). Auth board <100mA. Total well within Xbox USB port budget.

### Toolchain & Build (Session 1)
33. **Pico-PIO-USB standalone API is dead** — The header declares `pio_usb_host_init()`, `pio_usb_host_task()` etc but they're not implemented. Current versions integrate through TinyUSB: use `tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg)` + `tuh_init(1)` + `tuh_task()`. Callbacks: `tuh_mount_cb`, `tuh_hid_mount_cb`, `tuh_hid_report_received_cb`.
34. **HCD PIO USB source must be explicitly included** — Add `${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c` to target sources AND define `PIO_USB_USE_TINYUSB`.
35. **System clock must be multiple of 12MHz for USB** — Use `set_sys_clock_khz(120000, true)` (120MHz) on RP2040 #2. PIO USB needs ≥120MHz.
36. **Homebrew gcc-arm-embedded cask needs sudo** — Use `brew install arm-none-eabi-gcc` formula (no newlib!) or download official ARM toolchain tarball directly to ~/arm-toolchain/ (includes newlib).
37. **CFG_TUH_VENDOR broken** — TinyUSB vendor host class has `pipe_handle_t` undefined. Use raw control transfers for auth board instead.

### Performance Optimization (Kenji Tanaka Review — Session 1)
38. **VM dispatch: use computed goto, NOT switch** — Cortex-M0+ has no `TBH`, GCC may emit linear scan for large switch. Computed goto (`&&label`) costs 8 cycles/dispatch vs 15-40 for switch. Thread the dispatch (each handler ends with `goto *dispatch_table[*pc++]`). Saves 19K-25K cycles/tick. EASY.
39. **Trig: use 256-entry quarter-wave LUT + lerp** — 1KB table gives 0.004% accuracy (40x better than needed). Cost ~290 cycles vs ROM SIN at 584. Combined SIN+COS for ~100 cycles vs 1179 from ROM. EASY, ~30 lines.
40. **SPI master must use DMA** — Current `spi_write_read_blocking` stalls Core 0 for 5120 cycles (2.56% budget). DMA frees those cycles for TinyUSB/GIP. EASY, ~25 lines.
41. **Memory bank placement matters** — VM interpreter+dispatch in bank 0 (SRAM). VM registers in bank 4 (Core 1 exclusive). Axis double-buffer in bank 2. SPI buffers in bank 3. Equation bytecode in bank 1. Prevents cross-core bank contention stalls.
42. **GIP_CMD_LED (0x0A) == wheel request command** — Both are command ID 0x0A. For wheels, 0x0A is used for wheel-specific requests (angle, power, vibration), not LED control. Handled in wheel request dispatcher.

### G29 Protocol Verification (Session 1)
12. **G29 PID is 0xC24F in PS3 mode, NOT 0xC294** — bridge_config.h has wrong PID. The G29 initially enumerates as a legacy Driving Force device and must be mode-switched to G29 native mode (PID 0xC24F). PID 0xC294 may be the initial/legacy PID before mode switch.
13. **G29 input report is 12 bytes, not 27** — bridge_config.h defines G29_REPORT_SIZE as 27, but PS3 mode reports are 12 bytes. The entire parser byte map is wrong.
14. **g29_parser.c button mapping is WRONG** — Byte 0 contains BOTH d-pad (low nibble) AND face buttons (high nibble). Byte 1 contains R1,L1,R2,L2,Share,Options,R3,L3. The current parser has all button bits in the wrong bytes with wrong bitmasks.
15. **G29 requires initialization/mode-switch** — Must send F8 0A 00... (revert) then F8 09 05 01 01 00 00 (switch to G29 mode). The wheel re-enumerates after this. Firmware must handle USB disconnect/reconnect.
16. **G29 legacy FFB has NO native periodic effect** — Periodic effects (sine, square, etc.) must be emulated by rapidly updating a constant force slot at 4-5ms intervals. The build_periodic() function in ffb_translator.c is completely wrong.
17. **FFB slot encoding is (slot_mask << 4) | cmd_op** — Current code uses 0x11, 0x21, 0x31, 0x41 which mixes slot masks with command ops incorrectly. Slot 0=0x1_, slot 1=0x2_, slot 2=0x4_, slot 3=0x8_. Download-and-play=0x_1, update=0x_C, stop=0x_3.
18. **FFB effect type IDs are wrong** — Spring is 0x0B (not 0x01), damper is 0x0C (not 0x02), friction is 0x0E (not implied). The WiiBrew simplified encoding (0x01=spring, 0x02=friction, 0x0D=damper) is a different simpler variant.
19. **G29 interrupt OUT bInterval is 5ms** — Do not send FFB commands faster than every 4-5ms or they'll be dropped.
20. **PS3 mode is required** — PS4 mode (0xC260) uses a completely different 64-byte report format and HID++ FFB protocol. The G29 mode switch on the back of the wheel must be set to PS3.
