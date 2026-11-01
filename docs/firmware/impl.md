# firmware: impl

How the firmware meets [spec.md](spec.md). Update this file in the same commit as any structural change to the firmware.

## Current state (2026-10-31)

Build plan step 3: safe boot state, host-link framing, the global commands `identify`, `list_objects`, `ping`, and a constant-speed relative `move` with `move_start` / `move_end` events.

| File | Role |
|---|---|
| `board.h`, `board_table.c` | Board table (dev board: one stepper `m0`, pins as in `docs/build-plan.md`) with each stepper's safe defaults (F-R34): 200 full steps/s, 20 us pulse width. At most 4 steppers, one per PIO0 state machine. Pure C |
| `stepgen.pio`, `stepgen.c` / `.h` | Step pulse generator, one PIO0 state machine per stepper (details below). `stepgen.h` is pure C; host tests use `tests/dummy_stepgen.c` in place of `stepgen.c` |
| `stepper.c` / `.h` | Stepper objects: motion state, the `move` command, `move_start` / `move_end` events (F-R1, F-R32). Pure C |
| `board.c` | `board_safe_init()`: driver pins to their safe state |
| `linebuf.c` / `.h` | Line assembler (F-R31). A line is at most 1024 bytes including its `\n`; an over-length line is dropped and reported once at its `\n`. Pure C |
| `hostlink.c` / `.h` | Frame decoding, dispatch tables (global commands; per-type object commands), replies and `rx_error` events (F-R31, F-R33, F-R35). Writes through a callback. Pure C |
| `version.h` | Firmware version for `identify` |
| `main.c` | Calls `board_safe_init()` first (F-R23), then sets up the step generators and runs a main loop: TinyUSB, received bytes through the line assembler to the dispatcher, then `stepper_poll()` |
| `usb_descriptors.c`, `tusb_config.h` | TinyUSB used directly (not the SDK's `stdio_usb`), one CDC interface (F-R30). Development VID/PID `2E8A:000A`. The USB serial number is the flash unique ID, so `/dev/serial/by-id/` gives each board a stable path |

Host-link details:
- Frames are parsed with `cJSON_ParseWithOpts(..., require_null_terminated)`, so trailing non-whitespace makes a frame malformed (plain `cJSON_Parse` would ignore it). Trailing whitespace such as a terminal's `\r` is allowed. A line with an embedded NUL is malformed.
- `seq` is read through `valuedouble` (exact for every uint32; `valueint` clamps at `INT_MAX`). Replies are printed into a fixed buffer with `cJSON_PrintPreallocated`.
- The partial line is discarded on connect and disconnect (DTR edges). Unread bytes are flushed only on disconnect, since on connect the FIFO may already hold the host's first frame.
- The pure-C modules are linked into the host unit tests (`tests/test_*.c`). `tests/hostlink.py` is a stdlib-only client for manual tests on the board. With `--wait`, it sends each frame only after the previous one's reply and the `move_end` of any move it started.

Step generation details:
- PIO word formats (`stepgen.pio`), tag in bit 0: a **single** word is one step, with the low delay in bits 31..1; a **repeat** word is `count - 1` in bits 31..1, followed by a word with the low delay. The high delay sits in the state machine's ISR, loaded by the CPU while the state machine is disabled.
- Cycle counts: high time = high delay + 2; low time = low delay + 3 in a repeat word, + 6 for a single word (it includes fetching the next word). `stepgen.h` holds these as `STEPGEN_*_OVERHEAD`. The state machine runs at the full system clock, read at runtime (`clock_get_hz`).
- So far only repeat pairs are used: a move is one pair, pushed by core 0 straight into the FIFO (DMA and per-step words come with the planner, step 4). The pulse rate is `speed * microsteps`; microsteps is fixed at 1 until step 6, since MS1–MS3 stay low.
- A move is rejected with `bad_param` if the high time is under 1 us, or the low time would be under 1 us at this speed (F-R2), or if the target leaves the int32 position range. `steps: 0` is accepted before any of the timing checks, even at settings that would reject a real move. It queues nothing and marks the axis moving; the axis is already idle, so the next poll sends `move_start` and `move_end` together.
- DIR (F-R4): `stepgen_set_dir()` waits 1 us after changing DIR, before any step is queued. DIR only changes while the axis is idle, and "idle" means the state machine is back at `top` with an empty FIFO, so the last step's low time (at least 1 us) has already run.
- Finished moves are detected by `stepper_poll()` from that idle state. `move_start` is also sent from the poll, so it always follows the command's reply.
- `move` doesn't check `not_configured` yet; that check comes with `settings_done` in step 6. ENABLE, SLEEP and RESET stay in their boot state, so a move pulses STEP and DIR with the driver still disabled.

Safe state at boot: STEP low and ENABLE high (F-R23). F-R23 doesn't specify the other driver pins; they are driven low, which holds the A4988 asleep (SLEEP) and in reset (RESET), with DIR low and full-step microstepping (MS1–MS3 low).

- Pico SDK **2.3.1**, board `pico` (RP2040), toolchain `arm-none-eabi-gcc` from Debian. The SDK lives outside the repo, checked out with its submodules (TinyUSB is one).
- Build: `cmake -S src/firmware -B src/firmware/build -DPICO_SDK_PATH=<pico-sdk> -DPICOTOOL_FETCH_FROM_GIT_PATH=<picotool-dir>`, then `cmake --build src/firmware/build`. Output: `src/firmware/build/stepper_fw.uf2`. `<pico-sdk>` is the SDK checkout (alternatively, set the `PICO_SDK_PATH` environment variable); `<picotool-dir>` is any directory outside the build tree. Both are cached in the build directory, so they're only needed on the first configure.
- Without a system `picotool`, the SDK downloads and builds one (needed to produce the `.uf2`); `PICOTOOL_FETCH_FROM_GIT_PATH` keeps that one copy outside the build directory, so clean builds reuse it.

## Intended approach (not yet committed)

- Toolchain: Pico SDK, C, CMake, in `src/firmware/`. Same language as the daemon.
- USB: TinyUSB CDC, bundled with the Pico SDK (F-R30). USB disconnect, suspend and DTR drop are fed into link-loss handling (F-R22).
- Host link codec (F-R31): cJSON, as in the daemon.
- Board layout (F-R34, F-R35): a compiled-in table of the board's objects (`oid`, type, pins, wiring, safe default settings; for steppers also the attached endstop, homing direction and homing speed). One table per board build.
  - **Never rename or reuse an existing `oid`** in this table: clients and scripts address objects by it. Retire an object by deleting its entry; a new object gets a new `oid`.
- Step generation (F-R1–F-R4, F-R6): one PIO state machine per axis, timing **every step individually**. For each step it pulls a word from its TX FIFO, drives STEP high for the fixed high time (`pulse_width_us`), drives it low, then counts down that step's period. Resolution is one PIO clock (8 ns at 125 MHz), and timing doesn't depend on the CPU or USB traffic (F-R3).
  - During acceleration and deceleration, one word is one step, so the ramp is smooth rather than a staircase of constant-speed segments (the lg limitation in the old `src/ramp.c`).
  - During cruise, the period is constant, so the planner sends a single "N steps at period P" word to save CPU.
  - DIR is set, and held, around the move with the A4988 setup and hold margins (F-R4).
- Ramps (F-R5): a new per-step planner runs on core 1. Core 0 handles USB, framing and command dispatch.
  - DMA feeds the PIO, not the CPU: core 1 computes periods in chunks (about 64 words) into two alternating buffers, and one DMA channel per axis streams them into the TX FIFO. Core 1 is woken only at each chunk boundary to refill the buffer just drained. This gives about a chunk of slack against stalls caused by the CPU, instead of the FIFO's 4–8 words.
  - Periods come from David Austin's integer recurrence ("Generate stepper-motor speed profiles in real time", 2005): `c[n] = c[n-1] - 2*c[n-1] / (4n + 1)`, with the usual correction factor (0.676) on the first period. No square roots and no floating point per step.
  - Only the trapezoid / triangle decision carries over from `src/ramp.c`; its segment planner is replaced.
  - Speeds are in full steps/s, so the planner scales by the microstep setting to get the pulse rate.
  - The planner is pure C with no SDK dependency, so it compiles on the host and is unit-tested there against the exact formula `t[n] = sqrt(2n / a)`.
- FIFO underrun (F-R3): if core 1 ever misses a chunk refill, the PIO stalls and that step comes out late. The firmware checks the PIO's latched TX-stall flag after every move segment, and reports any stall as a `fault` event (`code: step_underrun`), so a late step is never silent.
- Stop (F-R1, F-R21): up to 4 periods (8 with joined FIFOs) can be queued. `stop` aborts the DMA channel, clears the TX FIFO and halts the state machine, which keeps the cut within 2 steps. The emitted count comes from the DMA's remaining transfer count, minus words left unconsumed in the FIFO, corrected for a step that was in progress, so the reported position stays exact.
- Endstop (F-R20): the step PIO program checks the endstop pin (`jmp pin`) before every step and halts itself when it is active, so the axis stops within 1 step with no CPU involvement.
  - The check applies only to moves toward the endstop; moves away from it (e.g. leaving the switch after homing) ignore it. The planner marks this per move.
  - On halting, the PIO raises an IRQ. Core 1 then aborts the DMA, works out the exact emitted count as for `stop`, and core 0 sends the `endstop` and `move_end` (`reason: endstop`) events as before (F-R32).
  - Endstop changes while the axis is idle are still reported, through a GPIO interrupt that only reports and never stops anything.
- Emergency stop (F-R24): handled in the USB receive path, before the command queue.
- PWM and servo (F-R7, F-R41): RP2040 hardware PWM slices.
- HX711 (F-R43): a PIO program, so reading never competes with step timing.

## Known gaps

All of it.