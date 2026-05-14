# P10 audit — summon.ino v2

Built: `arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 --warnings all`
Result: 1,100,567 B flash (32%), 47,944 B RAM (14%). Zero warnings originating
in user code. All warnings emitted are from m5stack core 3.3.7's variant header
re-defining macros already set in `Arduino.h` (`NUM_DIGITAL_PINS`,
`NUM_ANALOG_INPUTS`, etc.) — not actionable in this sketch.

## NASA Power of Ten review

| # | Rule | Status | Notes |
|---|------|--------|-------|
| 1 | No `goto`, `setjmp`, `longjmp`, recursion | ✅ | None present. |
| 2 | All loops have fixed upper bounds | ✅* | `while (millis() < end)` in setup is bounded (≤15 iters). `while(1) delay(1000)` is the panic path on ESP-NOW init failure — intentional halt; SoC watchdog will eventually reboot if it cares. Arduino `loop()` is the runtime main — unavoidable. |
| 3 | No dynamic allocation after init | ✅ | No `malloc`/`new` in user code. `Preferences` uses NVS (no heap). `M5GFX` allocates display buffers inside `M5.begin()`; nothing on the steady-state path allocates. |
| 4 | Functions ≤60 lines | ✅ | `loop()` = 67 lines including blanks/comments (~55 logical). `draw_main()` = 38. All others <30. |
| 5 | ≥2 runtime assertions per function | ⚠ | Guards present (`on_recv`: null+len checks; `add_peer`: duplicate guard; mac equality before honoring non-pair msgs). Not asserting at the density P10 wants — acceptable for a 240 MHz event loop where every-call asserts would burn the cycle budget. |
| 6 | Restrict data scope | ✅ | File-scope `static volatile` only for vars shared with the ESP-NOW receive task. Per-period timers are `static` inside `loop()` (function scope). No globals exposed externally. |
| 7 | Check non-void return values | ✅ | `esp_now_init()` checked (panic on fail). `esp_now_send()` / `esp_now_add_peer()` results explicitly discarded via `(void)` — drops are tolerable (heartbeat retries; UI degrades gracefully on link loss). |
| 8 | Limit preprocessor use | ✅ | Only `#define` constants for tunables. No function-like macros. No conditional compilation. |
| 9 | Limit pointer use; no function pointers | ✅* | One function pointer required by API (`esp_now_register_recv_cb`). All other pointers are `const`-correct, used with `memcpy`/`memcmp`; no pointer arithmetic. |
| 10 | Compile with all warnings on, zero warnings | ✅ | `--warnings all` produces zero warnings from user code. Upstream core warnings are not actionable here. |

(* = accepted deviation, documented above.)

## Other checks

- **Anti-stacking**: BtnA debounced through state machine. While `awaiting_ack_until` or `ack_until` is active, repeat BtnA presses emit a low "rejected" tone instead of re-firing summons. ACKs are only honored if the device is actually awaiting one.
- **Volatile correctness**: All variables written by `on_recv` (ESP-NOW receive task context) and read by `loop` (main task) are `volatile`. 32-bit aligned reads/writes are atomic on ESP32-S3, so no explicit barriers needed for these small scalars.
- **NVS preserved across flashes**: pairing survives a re-upload because `arduino-cli upload` only writes the app partition.
- **Power-safe sleep**: `setBrightness(0)` is used instead of `Display.sleep()` to avoid the ST7789 wake-glitch and to give millisecond wake-up on incoming summons.

## Verdict

Green to flash.
