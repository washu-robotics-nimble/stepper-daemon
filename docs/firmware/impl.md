# firmware: impl

How the firmware meets [spec.md](spec.md). Update this file in the same commit as any structural change to the firmware.

## Current state (2026-11-01)

Build plan step 7: safe boot state, host-link framing, the global commands `identify`, `list_objects`, `settings_done`, `set_link`, `ping`, `estop`, and the stepper commands `set_motion`, `move` and `move_to` (trapezoid ramps), `stop`, `set_enable`, `set_position` and `get_state`, with `move_start` / `move_end` / `position` events and `fault` events for late steps; soft limits, and link-loss handling with `link_recovered`.

| File | Role |
|---|---|
| `board.h`, `board_table.c` | Board table (dev board: one stepper `m0`, pins as in `docs/build-plan.md`) with each stepper's safe defaults (F-R34): 200 full steps/s, 400 full steps/s^2, 20 us pulse width, full step, `dir_polarity` `H`. At most 4 steppers: each takes a PIO0 state machine (step generator) and a PIO1 state machine (step counter); see Known gaps. Pure C |
| `stepgen.pio`, `stepgen.c` / `.h` | Step pulse generator, one PIO0 state machine per stepper, fed by DMA from core 1, and a step counter on PIO1 (details below). `stepgen.h` is pure C; host tests use `tests/dummy_stepgen.c` in place of `stepgen.c` |
| `planner.c` / `.h` | Per-step trapezoid planner (F-R5): turns a move into PIO words, a chunk at a time. Pure C |
| `stepper.c` / `.h` | Stepper objects: motion settings and state, the stepper commands, emergency stop, `move_start` / `move_end` / `position` events (F-R1, F-R10, F-R21, F-R24, F-R32–F-R34). Pure C; host tests use `tests/dummy_board.c` for the driver pins. It gets the time from its caller (`stepper_poll(now_ms)`) |
| `board.c` | `board_safe_init()`: driver pins to their safe state. `board_set_enable()`: driver ENABLE, waking the driver first (below). `board_set_microsteps()`: MS1–MS3 |
| `linebuf.c` / `.h` | Line assembler (F-R31). A line is at most 1024 bytes including its `\n`; an over-length line is dropped and reported once at its `\n`. Pure C |
| `linequeue.c` / `.h` | Received lines waiting for dispatch, 8 slots (F-R24, below). Pure C |
| `hostlink.c` / `.h` | Frame decoding, dispatch tables (global commands; per-type object commands), replies and `rx_error` events (F-R31, F-R33, F-R35), the test for an `estop` line, the link settings (`settings_applied`, `report_period`, `timeout_ms`) and link-loss state (F-R22). Writes through a callback. Pure C |
| `version.h` | Firmware version for `identify` |
| `main.c` | Calls `board_safe_init()` first (F-R23), then sets up the step generators (which starts core 1) and runs core 0's main loop: TinyUSB, received bytes through the line assembler into the line queue, one queued line to the dispatcher, then `stepper_poll()` |
| `usb_descriptors.c`, `tusb_config.h` | TinyUSB used directly (not the SDK's `stdio_usb`), one CDC interface (F-R30). Development VID/PID `2E8A:000A`. The USB serial number is the flash unique ID, so `/dev/serial/by-id/` gives each board a stable path |

Host-link details:
- Frames are parsed with `cJSON_ParseWithOpts(..., require_null_terminated)`, so trailing non-whitespace makes a frame malformed (plain `cJSON_Parse` would ignore it). Trailing whitespace such as a terminal's `\r` is allowed. A line with an embedded NUL is malformed.
- `seq` is read through `valuedouble` (exact for every uint32; `valueint` clamps at `INT_MAX`). Replies are printed into a fixed buffer with `cJSON_PrintPreallocated`.
- The partial line is discarded on connect and disconnect (DTR edges). Unread bytes and queued lines are dropped only on disconnect, since on connect the FIFO may already hold the host's first frame.
- The pure-C modules are linked into the host unit tests (`tests/test_*.c`). `tests/hostlink.py` is a stdlib-only client for manual tests on the board. With `--wait`, it sends each frame only after the previous one's reply and the `move_end` of any move it started. `tests/check_commands.py` runs the handshake and checks every command's replies and error cases on the board; `tests/flood.py` floods `ping` during a move.

Step generation details:
- PIO word formats (`stepgen.pio`), tag in bit 0: a **single** word is one step, with the low delay in bits 31..1; a **repeat** word is `count - 1` in bits 31..1, followed by a word with the low delay. The high delay sits in the state machine's ISR, loaded by the CPU while the state machine is disabled.
- Cycle counts: high time = high delay + 2; low time = low delay + 3 in a repeat word, + 6 for a single word (it includes fetching the next word). `stepgen.h` holds these as `STEPGEN_*_OVERHEAD`. The state machine runs at the full system clock, read at runtime (`clock_get_hz`).
- Ramp steps are single words, one per step; the cruise is one repeat pair (per 2^31 steps). The pulse rate is `speed * microsteps` and the ramp rate `accel * microsteps`.
- The TX FIFO is joined (8 words), as slack against interrupt latency.
- A move is rejected with `bad_param` if the high time is under 1 us, or the low time would be under 1 us at this speed (F-R2), or if the target leaves the int32 position range. `steps: 0` is accepted before any of the timing checks, even at settings that would reject a real move. It queues nothing and marks the axis moving; the axis is already idle, so the next poll sends `move_start` and `move_end` together.
- DIR (F-R4): `stepgen_set_dir()` waits 1 us after changing DIR, before any step is queued. DIR only changes while the axis is idle, and "idle" means the state machine is back at `top` with an empty FIFO, so the last step's low time (at least 1 us) has already run.
- Finished moves are detected by `stepper_poll()` from that idle state, once core 1 has also handed over the move's last word. `move_start` is also sent from the poll, so it always follows the command's reply.
- A move doesn't touch ENABLE: it pulses STEP and DIR whether or not the driver is enabled.

Planner details (`planner.c`):
- Periods follow Austin's recurrence (see Intended approach), with the 0.676 correction on the first period. That first period is about 32% shorter than the exact `sqrt(2n / a)` profile, and the second about 2%; from the third on, periods are within 0.75%. Absolute timing doesn't matter, so the shift this gives the profile is accepted.
- `c[0]` is computed once per move in floating point; each step after it costs one 32-bit division and no floating point.
- Periods are fixed point, renormalised every step to keep 30 significant bits. Without that, the per-step change `2c / (4n + 1)` loses precision deep into long ramps; at 20 kHz the error reached 1% after about 380000 steps.
- The ramp length isn't taken from the formula `v^2 / (2a)`: acceleration runs until the recurrence's period reaches the cruise period, or until half the move is done (a triangle, F-R5). The 0.676 start leaves the recurrence slightly behind the formula, and ending the ramp by the formula left a jump to cruise speed of up to 1.8 times `accel`. Deceleration replays the same periods backwards, so both ramps have the same length; an odd triangle has one middle step at the peak period.
- No step is faster than the cruise period, so checking the cruise period against F-R2 covers the whole move.
- Host tests (`tests/test_planner.c`): exact step totals; every period from the third on within 1% of the exact profile, capped at cruise speed; step-to-step acceleration never above `accel`, allowing each period half a cycle of rounding; any chunk size gives the same words; long ramps up to 500000 steps.

Feeding (`stepgen.c`, core 1):
- Core 0 validates a move, plans its parameters and hands the plan to core 1 through a per-axis slot and an `active` flag. Core 1 then owns the planner state until the move is fully in the FIFO.
- Each axis has one DMA channel, paced by its state machine's TX FIFO, and two 64-word buffers. The DMA completion interrupt (`DMA_IRQ_1`, on core 1) starts the next buffer if it is ready, and core 1's main loop refills the drained one. If the next buffer isn't ready yet, the main loop starts it as soon as it is.
- The two DMA channels aren't chained to each other: a late refill would then replay a stale buffer as extra steps. With one channel per axis, a late refill only pauses the state machine, and the pause is reported.
- Underruns (F-R3): the PIO's sticky TX-stall flag is read each time DMA resumes during a move; a set flag means the state machine ran out of words since the last resume, so a step came out late. The stall at `top` while idle is cleared once a new move's first words are in the FIFO. Each underrun is counted, and `stepper_poll()` sends one `fault` event (`code: step_underrun`) per change in the count. The move goes on: no step is lost, so the position stays exact.

Stop and emergency stop (F-R1, F-R21, F-R24):
- The emitted step count comes from a step counter: a second PIO program (`stepcount`, on PIO1, one state machine per stepper) counts rising edges on the STEP pin's input. It counts what reached the pin, whatever words were queued; a repeat word's remaining count can't be read back from the step generator. The CPU reads the counter with `mov isr, x` and `push` executed on its state machine.
- `stepgen_stop()` (core 0) halts the step generator's state machine at once, so no step starts after it. If STEP is high at that moment, it is held high for the full high time and then driven low, so the driver takes the step the counter has already counted. The cut is thus within 1 step.
- Core 0 then asks core 1 to drop the move (a `stop_req` flag); core 1 aborts the DMA channel (with its interrupt masked, as the SDK advises for RP2040-E13) and forgets the buffers. Core 0 clears the FIFO by toggling the TX join (`pio_sm_clear_fifos()` toggles the RX join, which would briefly join both), restarts the state machine at `top`, and reloads its high delay, which the restart clears.
- With `microsteps` above 1, `stop` then ends the move on the next full step (spec): after halting, it starts a second step generator run of the missing `microsteps - 1` steps or fewer, planned from rest with the move's settings. The position is thus always a whole full step, so nothing needs rounding. At 1/16 and the default 400 full steps/s^2, those steps take at most about 22 ms.
- `stop` replies with the final `pos`, then `move_end` (`reason: stop`) follows from the poll once the axis is idle. The axis counts as moving until that `move_end` is out, so a `move` in between gets `busy`. A move of 0 steps can be stopped too, with nothing to halt.
- `estop` halts every moving axis the same way (`reason: estop`), but at once, without finishing the full step (also cutting a `stop`'s run to its full step); its position is then rounded toward the move's start. It disables every driver and marks every axis's position invalid. A `stop` already pending keeps its reason. Nothing is latched: after `set_enable 1`, moves work again.
- Ahead of queued messages (F-R24): received bytes are assembled into lines as soon as they arrive, while the 8-slot line queue has room; an `estop` line runs the moment it is complete, ahead of every line still queued. Other lines (and over-length markers, so `rx_error` keeps its place) are queued and dispatched one per pass of the main loop, so `stepper_poll()` sends pending events between commands. A line is spotted as `estop` by a substring test, then a parse; an `estop` without a valid `seq` is still dropped with `no_seq`.
  - Limits: see Known gaps.

Settings and state (F-R10, F-R32–F-R34):
- `settings_applied` is a flag in `hostlink.c`: false from boot, set by `settings_done`, and never cleared. Before it, `move` and `move_to` get `not_configured`; every other command works.
- Each object starts with the board table's defaults (F-R34). `set_motion` checks every field, then the resulting combination with the same timing check a move uses, before applying anything; while the axis is moving it is refused with `busy`. A new `microsteps` value drives MS1–MS3 at once (the axis is idle).
- Soft limits (F-R25) are part of the motion settings: a flag and a value per side, so `0` is a real limit and `null` clears one. A move checks, in this order: `not_configured`, `busy`, `pos_invalid` (any limit set and the position invalid), then the target against the int32 range and the limits (`bad_param`).
- The live position (`get_state` and `position` events) is the move's start plus the steps counted so far, in whole full steps toward the start. `position` events go out from `stepper_poll()` every `report_period` ms after `move_start`, with no lower bound on the period.
- `move_to` is a relative move of `pos` minus the current position, with the same checks.
- Positions are int32 full steps, and one move is at most 2^31 steps (one repeat word), so at 1/16 a move can't exceed 2^27 full steps.

Link loss (F-R22):
- `main.c` reports the link as up when the USB device is mounted, not suspended, and the host has the port open (`tud_cdc_connected()`, i.e. DTR). It does so on every pass of the main loop, before any line is read, so `link_recovered` comes ahead of every reply. A DTR drop seen through the line-state callback is reported as down first, so a close and reopen between two passes is still a loss.
- `hostlink.c` keeps the link state, pure C. It gets the time from `hostlink_tick()`, once per pass. A loss calls `stepper_link_lost()`, which stops each moving axis through the same path as `stop` (`reason: link_lost`); a `stop` already pending keeps its reason. `link_recovered` is sent once per loss, however many causes overlapped (e.g. a timeout, then the port closed).
- The `timeout_ms` clock runs from the last frame that got a reply, and starts over when the port opens. While the link is down for another reason, the timeout doesn't apply.
- Host tests: `tests/test_link.c`. On the board, `tests/check_commands.py` checks the timeout, and closes and reopens the port during a move.

Driver enable (F-R10): `set_enable 1` first wakes the driver if SLEEP or RESET is low: RESET and SLEEP high, then 1 ms (the A4988's wake-up time before stepping), then ENABLE low. `set_enable 0` and `estop` only drive ENABLE high; SLEEP and RESET stay high from then on. Both also mark the position invalid (spec).

Safe state at boot: STEP low and ENABLE high (F-R23). F-R23 doesn't specify the other driver pins; they are driven low, which holds the A4988 asleep (SLEEP) and in reset (RESET), with DIR low and full-step microstepping (MS1–MS3 low). `stepper_init()` then sets MS1–MS3 to the board table's default microsteps.

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
  - During acceleration and deceleration, one word is one step, so the ramp is smooth rather than a staircase of constant-speed segments (the lg limitation in the old `src/ramp.c`, since removed).
  - During cruise, the period is constant, so the planner sends a single "N steps at period P" word to save CPU.
  - DIR is set, and held, around the move with the A4988 setup and hold margins (F-R4).
- Ramps (F-R5): a new per-step planner runs on core 1. Core 0 handles USB, framing and command dispatch.
  - DMA feeds the PIO, not the CPU: core 1 computes periods in chunks (about 64 words) into two alternating buffers, and one DMA channel per axis streams them into the TX FIFO. Core 1 is woken only at each chunk boundary to refill the buffer just drained. This gives about a chunk of slack against stalls caused by the CPU, instead of the FIFO's 4–8 words.
  - Periods come from David Austin's integer recurrence ("Generate stepper-motor speed profiles in real time", 2005): `c[n] = c[n-1] - 2*c[n-1] / (4n + 1)`, with the usual correction factor (0.676) on the first period. No square roots and no floating point per step.
  - Nothing carries over from `src/ramp.c`: even the trapezoid / triangle decision now comes from the recurrence itself (see Planner details above).
  - Speeds are in full steps/s, so the planner scales by the microstep setting to get the pulse rate.
  - The planner is pure C with no SDK dependency, so it compiles on the host and is unit-tested there against the exact formula `t[n] = sqrt(2n / a)`.
- FIFO underrun (F-R3): if core 1 ever misses a chunk refill, the PIO stalls and that step comes out late. The firmware checks the PIO's latched TX-stall flag each time DMA resumes during a move, and reports any stall as a `fault` event (`code: step_underrun`), so a late step is never silent.
- Stop (F-R1, F-R21): `stop` halts the state machine, aborts the DMA channel and clears the TX FIFO, which keeps the cut within 2 steps. The emitted count comes from a step counter on the STEP pin (see Stop and emergency stop above), so the reported position stays exact; working it out from the DMA's remaining transfer count fails during cruise, where one repeat word stands for many steps.
- Endstop (F-R20): the step PIO program checks the endstop pin (`jmp pin`) before every step and halts itself when it is active, so the axis stops within 1 step with no CPU involvement.
  - The check applies only to moves toward the endstop; moves away from it (e.g. leaving the switch after homing) ignore it. The planner marks this per move.
  - On halting, the PIO raises an IRQ. Core 1 then aborts the DMA, works out the exact emitted count as for `stop`, and core 0 sends the `endstop` and `move_end` (`reason: endstop`) events as before (F-R32).
  - Endstop changes while the axis is idle are still reported, through a GPIO interrupt that only reports and never stops anything.
- Emergency stop (F-R24): handled in the USB receive path, before the line queue (see above).
- PWM and servo (F-R7, F-R41): RP2040 hardware PWM slices.
- HX711 (F-R43): a PIO program, so reading never competes with step timing.

## Known gaps

All of it beyond build plan step 7. Limitations of what is built:

- **PIO state machines.** Each stepper takes one PIO0 state machine (step generator) and one PIO1 state machine (step counter). With 4 steppers PIO1 is full, which leaves none for the HX711 reader (build plan step 17). This board has one stepper.
- **`estop` and unread input (F-R24).** `estop` overtakes only the lines already read from USB. With the 8-slot line queue full, nothing more is read until one queued line is dispatched, so an `estop` behind them waits for that. While a reply is blocked on a full USB TX buffer (a host that doesn't read), no input is read at all.
- **Underrun reports (F-R3)** are covered by host tests only; they have never been triggered on hardware, since the board doesn't fall behind in normal operation.
- **Position after `estop` with microstepping.** `estop` halts at once, so with `microsteps` above 1 the axis may rest part-way through a full step, and the reported position is rounded toward the move's start. Later moves are whole full steps, so the axis keeps that offset of under one full step from then on. Position is invalid after `estop` anyway (spec), so the host has to re-reference it: homing defines a new reference at the endstop, whereas `set_position` only renames the current position.