# Lessons Learned

## Session Log
*Updated as corrections and discoveries accumulate.*

---

### Architecture Decisions

1. **Two-chip architecture (Option B) is active** — Board G (RP2040 #1): USB device (Xbox via TinyUSB) + GIP protocol on Core 0, FFB VM on Core 1. Board X (RP2040 #2): PIO-USB host for G29 + PIO-USB host for PowerA auth controller. Boards communicate via SPI (Board G = master, Board X = slave). Option A (single-chip) was abandoned due to resource constraints.
2. **Auth relay lives on Board X, not Board G** — The PowerA controller connects to Board X via PIO-USB (not Board G). This eliminates SPI fragmentation for auth traffic and keeps auth latency low. Board X handles both G29 and PowerA USB hosting.
3. **Graceful degradation is mandatory** — The device must NEVER crash, hang, or enter a failed state under any compute load. Shed FFB fidelity (skip equation iterations) or drop intermediate input frames instead. All memory is static.
4. **Xbox FFB is a bytecode VM** — Not simple rumble. The "Equation Language" runs 16 equation slots at 1kHz with IEEE 754 float math. RP2040 ROM float routines (Qfplib) make this feasible: fmul=62cy, fsin=584cy. Full spec in docs/GIPDocs/.
5. **GIP wheel protocol is fully documented** — Downloaded from aka.ms/gipdocs. Contains wheel state report (0x20) byte layout, static config report (0x21), equation language spec, metadata JSON format, and C# metadata compiler source. No reverse engineering needed for protocol — only for validation.

---

### Hardware: DWEII RP2040-Zero Specifics

6. **GPIO 17-25 have NO pads on the DWEII board** — These pins are not broken out. Board X SPI must use GPIO 2-5 (not 16-19 as originally planned). GPIO 16 is the onboard RGB LED.
7. **Board X runs at 144MHz** — Required for PIO-USB timing (must be multiple of 12MHz). Not 120MHz, not 200MHz.
8. **Dual PIO-USB host FAILED on DWEII** — Running two PIO USB host instances on a single RP2040-Zero does not work. The PowerA auth controller was moved to Board X (sharing PIO-USB with G29 via software multiplexing or hub).

---

### G29 Protocol

9. **[BUG — TOP PRIORITY] G29 mode switch (PID C294 to C24F) does NOT work via PIO-USB** — Sending the F8 09 05 01 01 00 00 mode-switch command does not cause re-enumeration on PIO-USB the way it does on native USB. The wheel stays in legacy DFGT mode (PID C294). Currently operating with 8-bit steering resolution. This is the highest priority bug after the core pipeline is functional.
10. **G29 legacy (DFGT) mode sends 27-byte reports** — Not 12 bytes. The parser has been updated to handle 27-byte legacy reports. Previous lesson claiming 12 bytes was incorrect for this mode.
11. **G29 PID C294 is the legacy/DFGT PID, C24F is native G29** — The wheel enumerates as C294 initially. Mode switch command should transition it to C24F with full 16-bit steering. Currently stuck at C294.
12. **PS3 mode is required** — PS4 mode (0xC260) uses a completely different 64-byte report format and HID++ FFB protocol. The G29 mode switch on the back of the wheel must be set to PS3.
13. **G29 interrupt OUT bInterval is 5ms** — Do not send FFB commands faster than every 4-5ms or they'll be dropped.
14. **G29 legacy FFB has NO native periodic effect** — Periodic effects (sine, square, etc.) must be emulated by rapidly updating a constant force slot at 4-5ms intervals.
15. **FFB slot encoding is (slot_mask << 4) | cmd_op** — Slot 0=0x1_, slot 1=0x2_, slot 2=0x4_, slot 3=0x8_. Download-and-play=0x_1, update=0x_C, stop=0x_3.
16. **FFB effect type IDs** — Spring is 0x0B, damper is 0x0C, friction is 0x0E. The WiiBrew simplified encoding (0x01=spring, 0x02=friction, 0x0D=damper) is a different simpler variant.
17. **G29 FFB init: must clear all slots** — Send 0xF3 (stop all), 0xF5 (disable autocenter), then set range. Otherwise default autocenter spring fights VM output.

---

### PowerA Auth Controller

18. **PowerA controller identifiers** — VID=0x20D6, PID=0x205B. GIP interface uses class=0xFF, subclass=0x47, protocol=0xD0.
19. **Auth is USB passthrough only** — No extractable auth board. The intact PowerA controller is used as a USB device for GIP auth passthrough. Proven approach from GP2040-CE.

---

### GIP / Xbox Protocol

20. **GIP keepalive required** — Protocol requires periodic heartbeat responses. Must implement keepalive timer in GIP state machine. Missed keepalive = Xbox drops device.
21. **GIP_CMD_LED (0x0A) == wheel request command** — Both are command ID 0x0A. For wheels, 0x0A is used for wheel-specific requests (angle, power, vibration), not LED control. Handled in wheel request dispatcher.
22. **Xbox GIP descriptors** — If Xbox Series X rejects the device, pull descriptors from GP2040-CE project's Xbox Series X code path.

---

### Toolchain & Build

23. **Pico-PIO-USB standalone API is dead** — Current versions integrate through TinyUSB: use `tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg)` + `tuh_init(1)` + `tuh_task()`. Callbacks: `tuh_mount_cb`, `tuh_hid_mount_cb`, `tuh_hid_report_received_cb`.
24. **HCD PIO USB source must be explicitly included** — Add `${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c` to target sources AND define `PIO_USB_USE_TINYUSB`.
25. **USB CDC debug works on Board G's native USB-C port** — No UART adapter needed. Debug output goes over CDC on the same USB-C connector used for Xbox device connection (or a separate native USB port if available).
26. **CFG_TUH_VENDOR broken** — TinyUSB vendor host class has `pipe_handle_t` undefined. Use raw control transfers for auth board instead.

---

### Performance Optimization

27. **VM dispatch: use computed goto, NOT switch** — Cortex-M0+ has no `TBH`, GCC may emit linear scan for large switch. Computed goto (`&&label`) costs 8 cycles/dispatch vs 15-40 for switch. Saves 19K-25K cycles/tick.
28. **Trig: use 256-entry quarter-wave LUT + lerp** — 1KB table gives 0.004% accuracy (40x better than needed). Cost ~290 cycles vs ROM SIN at 584. Combined SIN+COS for ~100 cycles vs 1179 from ROM.
29. **SPI master must use DMA** — Blocking SPI stalls Core 0 for 5120 cycles. DMA frees those cycles for TinyUSB/GIP.
30. **Memory bank placement matters** — VM interpreter+dispatch in bank 0 (SRAM). VM registers in bank 4 (Core 1 exclusive). Axis double-buffer in bank 2. SPI buffers in bank 3. Equation bytecode in bank 1. Prevents cross-core bank contention stalls.

---

### Reliability

31. **Watchdog: only Core 1 kicks** — Not both cores. Watchdog timeout is 100ms (not 2s — too coarse). Reset path must be defined (full chip reset then Xbox re-enumeration).
32. **USB error recovery is mandatory** — G29: 3 retries then disconnect. Auth: 500ms timeout, 3 retries. TinyUSB: use built-in retry. Log all errors to SRAM ring buffer.
33. **VBUS power: G29 has its own 12V PSU** — USB VBUS only powers G29 MCU (~100mA). Auth board <100mA. Total well within Xbox USB port budget.
34. **Shared axis state torn reads** — Multi-byte float writes from Core 0 can be half-read by Core 1. Fix: double-buffered axis state with atomic flag flip.

---

### Resolved / Historical Items

*These items were issues during development but have been addressed. Kept for reference.*

- **[RESOLVED] Header inconsistencies and missing function declarations** — Early scaffolding had mismatched includes and undeclared functions. Headers have been reconciled.
- **[RESOLVED] Type mismatches (xbox_state_t vs xbox_report_t, mapper_config_t)** — Types and constants have been unified in the current codebase.
- **[RESOLVED] G29 report size confusion (12 vs 27 bytes)** — Legacy mode confirmed as 27-byte reports. Parser updated accordingly.
- **[RESOLVED] Pico-PIO-USB API mismatch** — Switched to TinyUSB integration path. Standalone API calls removed.
- **[RESOLVED] Dual PIO USB host** — Abandoned. Both G29 and PowerA connect to Board X. No dual-PIO needed.
- **[RESOLVED] SPI pin assignment (GPIO 16-19)** — Changed to GPIO 2-5 due to DWEII board lacking pads on GPIO 17-25.
- **[RESOLVED] Auth on Board G via SPI** — Auth moved to Board X. No SPI fragmentation for auth traffic.
- **[HISTORICAL] Option A single-chip architecture** — Replaced by Option B two-chip. Single-chip could not handle all workloads reliably.
- **[HISTORICAL] Overclock to 200-250MHz discussion** — Board G can run at 200MHz+ if needed. Board X is fixed at 144MHz for PIO-USB. Start conservative and measure.
- **[HISTORICAL] Homebrew gcc-arm-embedded needs sudo** — Use `brew install arm-none-eabi-gcc` formula or download official ARM toolchain tarball.
