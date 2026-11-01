# daemon: impl

How the code meets [spec.md](spec.md). Update this file in the same commit as any structural change to the daemon.

## Current state (2026-10-11)

The code in `src/` (outside `src/firmware/`) is still **pre-migration**: the remains of `motor_demo`, which ran on a Raspberry Pi and drove one A4988 directly through lg. lg and the lg-only modules were removed on 2026-11-01, so this code no longer builds; it is kept as the starting point for the daemon. `motor.h` still includes the removed `lgpio.h`, and `command.c` still calls `lguErrorText()`.
We need to move all daemon code into `src/daemon/` later.

Build plan step 0 (2026-10-31): cJSON v1.7.19 is vendored in `lib/cJSON/`. `make test` builds and runs every `tests/test_*.c` as a host program (plain C with `assert`). Since 2026-11-01, plain `make` does the same, until `stepperd` exists (build plan step 8); the `motor_demo` target is gone.

| Module | Role today | Fate in the new architecture |
|---|---|---|
| `src/main.c` | Loads config, starts the motor thread, runs the CLI in-process | Becomes the daemon entry point: daemonise, open link, serve clients |
| `src/config.c` | INI loading (inih) | Kept, but the INI schema is redone: one section per device, named by its `oid`, holding behavioural settings only (D-R3, D-R43). A stepper section keeps `microsteps`, `default_speed`, `max_accel`, `pulse_width_us`, `dir_polarity` (initial turning direction), `endstop` (installed or not, D-R44), and optional `soft_limit_min` / `soft_limit_max` (omitted = no limit, F-R25); a load cell section has `scale`, `offset` and `gain`. `[daemon]` holds the USB device and the socket path and group (D-R12); `[link]` holds `report_period` and `timeout_ms`. `chip` and `*_pin` move into the firmware's board table (F-R34); `feedback_pin` is dropped, as the firmware counts pulses exactly (F-R1) |
| `src/motor.h` | The `motor_*` API (`src/motor.c`, the lg GPIO + `lgTxPulse` motion thread, is removed) | Implemented by a link client that translates `motor_*` calls into host-link messages. The public `motor_*` API is the seam to preserve |
| `src/ramp.c` | Removed | The firmware computes ramps itself (F-R5, `src/firmware/planner.c`) |
| `src/command.c` | Parse and run text commands | Kept as the shared command layer for all clients (D-R11). Parses JSON requests instead of text lines (D-R40, D-R41), and gains each command's class (D-R13). `help` and `quit` move to the CLI |
| `src/status.c` | Snapshot and change tracking | Kept. Snapshots fill from firmware reports instead of local atomics (D-R20) |
| `src/cli.c` | stdin CLI with a status bar | Becomes the interactive CLI client, a separate program on the Unix socket (D-R12, D-R15). Translates typed lines into JSON requests; the user always names the device (D-R14). Acquires the lease just before a write command and releases it right after the reply (D-R18) |
| `src/motordriver.c`, `lib/lg/` | Removed (lg PWM experiment; vendored lg) | - |


## Planned structure

Not designed in detail yet. Expected pieces:

- Link layer: JSON framing, `seq` matching, response timeout, and report decoding for the host link. Emergency stop skips the outgoing queue. Reconnects with exponential backoff (250 ms doubling to 2 s). Implements D-R1, D-R17, D-R30, D-R31.
- Machine model: the latest firmware-reported state for each device. This is the only source for `status_capture` (D-R20, D-R22). Converts HX711 raw counts to mg with the INI `scale` and `offset`.
- Client server: a Unix socket listener feeding one dispatch path through `command.c`, plus `status` events to watching clients (D-R10–D-R12, D-R15, D-R19, D-R21, D-R40–D-R43).
- Lease arbiter: checks each command's class against the current lease, and handles expiry and disconnects (D-R13, D-R16–D-R18).

Dependencies: libc, inih, and cJSON (client protocol D-R40 and host link F-R31).

## Known gaps

- `motor_home` is not implemented (no endstop support yet).
- `soft_limit_min` and `soft_limit_max` are parsed but not enforced. Enforcement moves to the firmware (F-R25); the daemon only forwards them.
- The top-level `README.md` has a brief rewrite (2026-11-01) covering the firmware and host tests; it needs the daemon's build and run steps once they exist (build plan step 18).
