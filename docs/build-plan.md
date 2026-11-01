# Build plan (proposal)

MVP first: the thinnest path that runs end to end, from the CLI through the daemon to the Pico, with one stepper axis.
Every later step widens it.
Each step ends with something that compiles and can be tested, and updates the matching `impl.md` in the same change (docs rule 1).

Hardware testing uses a bare RPi Pico and a logic analyzer.
Inputs without hardware (endstop, HX711 DOUT) are simulated with jumper wires to GND or 3V3.
Analyzer checks are done by hand (8 channels, pins and channels as in the table below); they are not automated, since they depend on this specific bench setup.

| Signal | Pico GPIO | Analyzer channel |
|---|---|---|
| STEP | 2 | 0 |
| DIR | 3 | 1 |
| ENABLE | 4 | 2 |
| MS1 / MS2 / MS3 | 5 / 6 / 7 | 3 (MS1 only) |
| SLEEP / RESET | 8 / 9 | — |
| Endstop (input, pull-up, active low) | 10 | 4 |
| HX711 SCK / DOUT | 11 / 12 | 5 / 6 |
| Debug toggle (timing measurements) | 15 | 7 |

Questions are at the end; answer them as before.

---

## Phase 0: groundwork

### 0. Toolchain and layout

- Install `arm-none-eabi-gcc`, the latest Pico SDK 2.x and `picotool`. None are on this machine yet; `cmake` and `pulseview` are. Record the SDK version used in `firmware/impl.md`.
- Vendor cJSON into `lib/cJSON/`, as inih is.
- New directories: `src/firmware/`, `src/daemon/`, `src/cli/`, `tests/`. The pre-migration code in `src/*.c` stays untouched until step 18.

**Done when:** an empty firmware project (`main` that returns) builds to a `.uf2`; `make test` runs an empty test suite on the host.

---

## Phase 1: firmware, one stepper (Pico + analyzer)

### 1. Boot safe state, USB echo

- Board table with one stepper `m0` (pins as in Q5).
- At boot: STEP low, ENABLE high, before anything else runs (F-R23).
- TinyUSB CDC echoes lines back.

**Test:** with the analyzer on ENABLE, reset the Pico: ENABLE goes high, STEP never pulses. A serial terminal on the board's CDC device echoes typed lines.

### 2. Framing and global commands

- Line assembler (1024-byte cap, drop partial line on connect), cJSON codec, dispatch table.
- `identify`, `list_objects`, `ping`; errors `unknown_cmd`, `unknown_oid`, `bad_param`; `rx_error` event (F-R31, F-R33, F-R35).
- The line assembler and dispatch have no SDK dependencies, so they also compile on the host.

**Test:** host unit tests for the assembler and dispatcher (over-length, partial lines, garbage, every error code). On the Pico, send frames with `tests/hostlink.py` and check the replies.

### 3. Constant-speed `move` on PIO

- The per-step PIO program (firmware impl): one FIFO word per step, plus the "N steps at period P" cruise word. Only the cruise word is used in this step, pushed into the FIFO directly by the CPU; DMA comes in step 4.
- `move` (relative only) at a fixed speed; `move_start` / `move_end` events.

**Test (analyzer):** pulse count equals `steps` for several values (1, 2, 400, 2000; at the 200 full steps/s default, 2000 steps take 10 s). High time ≥ `pulse_width_us`, low time ≥ 1 µs (F-R2). DIR settles ≥ 200 ns before the first edge and holds ≥ 200 ns after the last (F-R4). F-R1 is met as long as the count is exact.

### 4. Trapezoid ramps

- New per-step planner using Austin's integer recurrence, on core 1 (F-R5). Keeps only the trapezoid / triangle decision from `src/ramp.c`.
- DMA chunk feeding: core 1 fills two alternating buffers of about 64 periods, and DMA streams them into the PIO FIFO.
- TX-stall check each time DMA resumes; a stall is reported as a `fault` event with `step_underrun` (F-R3).
- The planner is pure C and also compiles on the host.

**Test:** host unit tests: step total is exact, every period from the third on is within 1% of the exact `sqrt(2n / a)` profile (the 0.676 correction makes the first two shorter), step-to-step acceleration never exceeds `accel` (allowing for 1-cycle rounding), and short moves give a triangular profile. On the analyzer: the period shrinks step by step, holds, then grows, with no flat segments during ramps. Measure jitter while a host script floods `ping` (F-R3, ≤ 1 µs), and check that no `step_underrun` fault appears.

### 5. Stop, estop, enable

- `stop` (returns `pos`), `estop` (all outputs halted, ENABLE high), `set_enable` (F-R21, F-R24, F-R10).
- Both abort the DMA, clear the FIFO and compute the exact emitted count (firmware impl).
- `estop` is handled in the receive path, ahead of queued commands.

**Test (analyzer):** during a long move, `stop` cuts pulses within 2 steps, and the reported `pos` matches the analyzer's pulse count. `estop` does the same and also raises ENABLE.

### 6. Settings and state

- `set_motion`, `settings_done`, `get_state`, `set_position`, `move_to`; `not_configured` before `settings_done`; `settings_applied` in `identify` and in state reports (F-R33, F-R34).
- Built-in safe defaults per object.
- Periodic `position` events every `report_period` ms (default 250, 0 = off); `set_link`.
- Microstep changes rejected while moving (F-R10).

**Test:** a Python script (`tests/check_commands.py`) runs the full handshake, then checks every command's reply and error case. On the analyzer, the MS1–MS3 levels follow `microsteps`.

### 7. Link loss and soft limits

- Link loss on USB disconnect, suspend, DTR drop, or the `timeout_ms` expiring: stop motion, keep ENABLE as it is, send `link_recovered` when the link returns (F-R22).
- Soft limits (F-R25), including the `pos_invalid` rule.

**Test:** during a long move, close `screen`: pulses stop and ENABLE is unchanged. Reopen it: `link_recovered` arrives. Soft-limit logic gets host unit tests.

---

## Phase 2: daemon and CLI, end-to-end MVP

### 8. Daemon link and handshake

- `src/daemon/`, build target `stepperd`.
- New INI schema: `[daemon]`, `[link]`, `[m0]` (D-R43). New sample `motor.ini`.
- Open the serial device exclusively and refuse to start if it is already held (D-R1).
- Handshake: `identify`, `list_objects`, per-object settings, `settings_done` (D-R3). Log every frame in debug mode.
- `tests/dummy_mcu.py`: a stdlib-only Python dummy MCU on a pty, implementing the catalogue well enough to test the daemon without the Pico.

**Test:** `stepperd` against `dummy_mcu.py`, then against the Pico. The log shows the handshake and the settings sent. A second `stepperd` instance refuses to start.

### 9. Client socket, end to end (**MVP milestone 1**)

- Unix socket at the configured path, JSON client protocol (D-R40–D-R42), one dispatch path.
- Commands: `status`, `pos`, `rel`, `abs`, `stop`, `estop`. No lease yet: every client may write.

**Test:** `socat - UNIX-CONNECT:/run/stepper-daemon/control.sock`, then `{"seq":1,"dev":"m0","cmd":"rel","steps":400}`: the analyzer shows 400 ramped pulses and the reply comes back.

### 10. CLI client (**MVP milestone 2**)

- `src/cli/` (`stepper-cli`): typed lines (`m0 rel 400`) become JSON requests. The device is always named (D-R14). `help` and `quit` are local.
- The status bar is driven by polling `status` for now.

**Test:** interactive session against the Pico. Moves show on the analyzer, and the status bar follows.

---

## Phase 3: complete the daemon spec

### 11. Leases and multiple clients

- Command classes, a single global lease with expiry (5 s), release on disconnect (D-R10, D-R13, D-R16–D-R18).
- The CLI takes the lease just before a write and releases it right after.
- Remaining motor commands: `speed`, `accel`, `micro`, `zero`, `lock`, `unlock`.

**Test:** host unit tests for the arbiter (expiry, disconnect, write vs read vs session vs emergency). Manually: two `socat` sessions, where the one without the lease gets `lease_held`, and `estop` works from either.

### 12. Watch and status events

- `watch` / `unwatch`; `status` events built from firmware reports only, one snapshot per event (D-R20–D-R22).
- The CLI status bar switches from polling to `watch`.

**Test:** `dummy_mcu.py` sends scripted events; a watching client gets exactly one consistent `status` event per change.

### 13. Link failure and reconnect

- 500 ms response timeout, position invalid, `link_down` to clients (D-R30).
- Reconnect backoff 250 ms doubling to 2 s (D-R31); runtime settings re-sent instead of the INI values (D-R3).
- Reset detection through `settings_applied`.

**Test:** unplug the Pico during a move: clients get `link_down`, and plugging it back in restores service with the runtime `speed` kept. `dummy_mcu.py` covers the timeout and reset cases.

### 14. Process behaviour

- SIGTERM/SIGINT stops motion, then closes the link (D-R2).
- `estop` jumps the outgoing queue (D-R17).
- systemd unit with `RuntimeDirectory=`, socket mode `0660`, group `stepper` (D-R12).

**Test:** `systemctl stop` during a move stops the pulses. The socket permissions are correct. `estop` sent behind a burst of queued commands takes effect first (`dummy_mcu.py` logs arrival order).

---

## Phase 4: more devices

### 15. Endstop

- `endstop` object in the board table, attached to `m0`; `get_state`.
- The step PIO program checks the endstop pin before every step, only on moves toward the endstop, and halts itself (F-R20). Its IRQ leads to the `endstop` and `move_end` events with the exact position.
- A report-only GPIO interrupt sends `endstop` events while the axis is idle (F-R32).

**Test:** jumper the endstop pin to GND during a move toward it: pulses stop within 1 step on the analyzer, and `move_end` reports `reason: endstop` with `pos` matching the pulse count. With the jumper still in place, a move away from the endstop runs normally. Toggling the jumper while idle sends `endstop` events.

### 16. Homing

- `home` with direction, speed and maximum travel fixed in the board table (F-R34); position becomes valid at the endstop. Past the maximum travel the axis stops with a `fault` event (`home_timeout`) and position stays invalid (F-R20).
- `home` fails with `no_endstop` unless both the firmware board table attaches an endstop to the axis and the device's INI section says one is installed (D-R44).

**Test:** start `home`, then jumper the endstop: the axis stops, `pos` = 0, `pos_valid` = true. Without the jumper, the axis stops at the maximum travel with `home_timeout`. With `endstop = false` in the INI, `home` fails with `no_endstop` and nothing moves.

### 17. HX711

- PIO reader, `set_hx711` (gain), `read` (F-R43).
- Daemon: `weigh` returns `mg` and `raw`; `offset`, `tare`, `status`; `scale`, `offset`, `gain` from the INI section.

**Test without a load cell:** on the analyzer, 25 / 26 / 27 SCK pulses per read for gains 128 / 32 / 64. DOUT tied to GND reads `raw` 0; DOUT tied to 3V3 means "not ready", so `read` times out with an error. Unit test for the mg conversion. Full test once the hardware arrives.

### 18. Cleanup

- Delete the pre-migration code (`src/*.c` superseded by `src/daemon`). Done early, on 2026-11-01: `lib/lg/`, `lg-master/`, the lg-only `src/motor.c`, `src/motordriver.c` and `src/ramp.c`, and the `motor_demo` target.
- Rewrite the top-level `README.md` (build, flash, run).
- Bring both `impl.md` files' "Current state" up to date and empty their "Known gaps".

**Test:** clean build of everything from scratch; full manual run through the CLI.

Later, not in this plan: PWM / servo (F-R7, F-R41), ADC (F-R40), I2C / SPI (F-R42).

---

## Questions

### P1. Build system

- (a) Keep the `makefile` for the host programs (`stepperd`, `stepper-cli`, tests); the firmware gets its own CMake project in `src/firmware/` (required by the Pico SDK).
- (b) CMake for everything.

Recommendation: (a). It's the smallest change.

**Answer:**
ok.

### P2. Test tooling

- Host unit tests: plain C with `assert`, run by `make test`. No framework dependency.
- Dummy MCU and protocol scripts: Python, stdlib only (`pty`, `json`). They double as a reference client for your colleagues.

OK?

**Answer:**
ok. I would call it 'dummy mcu' instead of 'fake'.

### P3. Firmware language and SDK version

Pico SDK 2.x, C11 (B7 accepted Pico SDK in C). Any constraint on SDK version, e.g. one already used elsewhere in your lab?

**Answer:**
No constraints on SDK version. Just note which one we are using when we get one.

### P4. Unbounded homing

If the endstop never triggers (broken wire, or here, no jumper), homing should not run forever.
Proposal: the board table also has a maximum homing travel; past it the axis stops with a `fault` event (`code: home_timeout`), and position stays invalid.
This adds a requirement to F-R20 / F-R34.

**Answer:**
ok. also make sure that the INI specifies if an endstop is installed AND an endstop is configured in firmware; either one of these are not set should result in a failed `home`.

### P5. Dev-board pin map and analyzer channels

How many channels does your analyzer have?
Proposed Pico pins for the board table (all free GPIOs on a bare Pico):

| Signal | GPIO | Analyzer channel (if 8) |
|---|---|---|
| STEP | 2 | 0 |
| DIR | 3 | 1 |
| ENABLE | 4 | 2 |
| MS1 / MS2 / MS3 | 5 / 6 / 7 | 3 (MS1 only) |
| SLEEP / RESET | 8 / 9 | — |
| Endstop (input, pull-up, active low) | 10 | 4 |
| HX711 SCK / DOUT | 11 / 12 | 5 / 6 |
| Debug toggle (timing measurements) | 15 | 7 |

The debug pin lets the firmware mark events on the analyzer, e.g. "stop received" for measuring F-R21 latency.

**Answer:**
We have 8 channels. Don't try to automate that; do it only manually because it relies on very specific hardware.
