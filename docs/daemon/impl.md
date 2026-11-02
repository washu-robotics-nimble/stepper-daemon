# daemon: impl

How the code meets [spec.md](spec.md). Update this file in the same commit as any structural change to the daemon.

## Current state (2026-11-02)

The code in `src/` (outside `src/firmware/` and `src/daemon/`) is still **pre-migration**: the remains of `motor_demo`, which ran on a Raspberry Pi and drove one A4988 directly through lg. lg and the lg-only modules were removed on 2026-11-01, so this code no longer builds; it is kept as the starting point for the daemon. `motor.h` still includes the removed `lgpio.h`, and `command.c` still calls `lguErrorText()`.
We need to move all daemon code into `src/daemon/` later.

Build plan step 0 (2026-10-31): cJSON v1.7.19 is vendored in `lib/cJSON/`. `make test` builds and runs every `tests/test_*.c` as a host program (plain C with `assert`). Since 2026-11-01, the `motor_demo` target is gone; since 2026-11-02, plain `make` builds `build/nimhubd`.

Build plan step 8 (2026-11-02): the daemon is called `nimhubd`, in `src/daemon/`. It loads the INI, opens the MCU's serial device exclusively (D-R1), runs the handshake (D-R3), and then keeps the link alive and logs the MCU's events. There is no client socket yet (step 9; added since, below). A link drop or a failed handshake ends it with status 1 (reconnecting is step 13); SIGINT / SIGTERM close the link and end it with status 0.

| File | Role |
|---|---|
| `main.c` | Command line `nimhubd [-d] [CONFIG]` (`-d`: debug, which logs every frame), config, signals, and the single-threaded `ppoll()` main loop. SIGINT and SIGTERM are blocked except inside `ppoll()`, so none is missed between checking the flag and waiting |
| `config.c` / `.h` | INI loading with inih (D-R3, D-R43), below |
| `serial.c` / `.h` | Opening the device for exclusive use (D-R1), below; raw mode, non-blocking |
| `link.c` / `.h` | Host link (F-R31): adds `seq` (starting over at 1 on each connect), assembles received lines (up to 4096 bytes), matches replies to requests, buffers output the device hasn't taken yet, and hands events to a callback. A request without its reply after `reply_timeout_ms` (default 500) is a link drop (D-R30), as are a read or write error and a hangup. Pings when nothing was sent for a quarter of `timeout_ms` (F-R22) |
| `handshake.c` / `.h` | The handshake (D-R3, F-R33, F-R35), below. A state machine driven by replies, one request at a time, so it never blocks the main loop |

Dependencies are vendored in `lib/`: inih, cJSON v1.7.19, and rxi/log.c (`lib/log/`, commit `f9ea349`, unchanged) for logging. The log goes to stderr, which the journal picks up under systemd. inih is compiled with `INI_HANDLER_LINENO=1` (line numbers in messages), `INI_CALL_HANDLER_ON_NEW_SECTION=1` (an empty section still counts) and `INI_ALLOW_MULTILINE=0` (an indented line is a key of its own).

Config (`config.c`):
- Default path `/etc/nimhubd/nimhub.ini`. If it doesn't exist, `$XDG_CONFIG_HOME/nimhubd/nimhub.ini` (`$XDG_CONFIG_HOME` defaults to `~/.config`) is used instead, for testing without write access to `/etc`. The sample is `nimhub.ini` at the top of the repository (`motor.ini` until 2026-11-02).
- `[daemon]`: `device` (required), `socket` (default `/run/nimhubd/control.sock`, D-R12), `group` (default `stepper`); the last two are used from step 9.
- `[link]`: `report_period` (default 250, F-R32), `timeout_ms` (the firmware's F-R22 timeout, default 0 = off), `reply_timeout_ms` (D-R30, default 500). `report_period` and `timeout_ms` are always sent, so none is left over from an earlier session.
- Device sections: the key names are the `set_motion` fields: `microsteps`, `speed`, `accel`, `pulse_width_us`, `dir_polarity`, `soft_limit_min`, `soft_limit_max`, plus `endstop` (D-R44), which stays in the daemon. `default_speed` and `max_accel` were renamed to `speed` and `accel`, and `accel` must be at least 1 (there is no "0 = no ramp" any more).
- Every value is checked when the file is loaded, against the firmware's ranges; an unknown key, a key set twice, or `soft_limit_min` above `soft_limit_max` is an error too. Every error is logged with its line, and the daemon doesn't start.
- The device type is known only from `list_objects`, so the handshake checks that every key in a section belongs to its object's type.

Exclusive use (`serial.c`, D-R1), in this order:
1. `open()`: fails with `EBUSY` if another process holds the device with `TIOCEXCL` (e.g. another nimhubd).
2. `flock(LOCK_EX | LOCK_NB)`: fails if another process has it locked (e.g. picocom).
3. `TIOCEXCL`: from here on, other opens fail with `EBUSY`, except by root.
4. A scan of `/proc/*/fd` for any other process with the device open, e.g. `screen`, which takes no lock.

Nothing is flushed after the open: the open itself raises DTR and so brings the link up (F-R22), and what arrives from then on, such as `link_recovered`, belongs to this session.

Handshake (`handshake.c`):
1. `identify`: a `proto` other than 1 ends the handshake (F-R33).
2. `list_objects`: an INI section for an `oid` the firmware doesn't report is logged and ignored (D-R43); an object without a section keeps the firmware's defaults (F-R34).
3. `set_link` with the `[link]` settings; then the pings start, if `timeout_ms` is set.
4. `set_motion` for each stepper with a section, with the keys the section sets. Soft limits it leaves out are sent as `null` (no limit, F-R25); other keys left out aren't sent. A `busy` reply (an axis still finishing a move from an earlier session's link loss) is retried every 50 ms, for up to 2 s.
5. `settings_done`.
6. `get_state` for each stepper, to fill the machine model (since step 9). The handshake is done after the last one.

Any other error reply ends the handshake, and with it the daemon, since retrying won't change it. A `bad_param` from `set_motion` usually means settings no move could run with (F-R2), e.g. a `pulse_width_us` too long for `speed` × `microsteps`.

Tests:
- `tests/test_daemon_config.c` (part of `make test`) checks the INI parsing and the `set_motion` fields.
- `tests/dummy_mcu.py` is a stdlib-only dummy MCU on a pty. It implements the firmware's message catalogue with the same checks and error codes, moves that take their profile's time, and link loss (port closed, timeout). Options inject faults: `--no-reply CMD`, `--busy-ms`, `--proto`, `--objects`.
- `tests/check_daemon.py` (`make check-daemon`, Python 3) runs nimhubd against the dummy. It checks the handshake and the settings sent, the pings, the exclusive-use cases, a restart, and every case where nimhubd must give up.

Build plan step 9 (2026-11-02): the client socket, with `status`, `pos`, `rel`, `abs`, `stop` and `estop`. There is no lease yet (step 11): every client may write. The other commands in the spec reply `not_impl`.

| File | Role |
|---|---|
| `server.c` / `.h` | The Unix socket and its clients (D-R10, D-R12, D-R40), below |
| `commands.c` / `.h` | The client protocol and the one dispatch path for every command (D-R11, D-R40–D-R43), below |
| `model.c` / `.h` | Machine model (D-R20, D-R22), below |

Socket (`server.c`):
- Opened after the device, so a second nimhubd stops at the device (D-R1) before it touches the first one's socket. An old socket file at the path is removed; a path that exists but isn't a socket is an error.
- Created with mode `0660` (umask around `bind()`), then given to the INI's `group`. If that group doesn't exist or can't be set (not root), a warning is logged, and the socket keeps the user's group.
- nimhubd doesn't create the socket's directory: systemd's `RuntimeDirectory=` does (step 14). When run by hand, `socket =` in `[daemon]` points to a writable place.
- Removed when nimhubd stops.
- Up to 8 clients; a 9th connection is closed at once. Lines are up to 4096 bytes including the `\n`. Output a client hasn't read is buffered up to 64 KiB; a client over that is dropped.
- A client that shuts down its sending side (`socat` at the end of its input) is kept until the replies to its pending requests are sent (D-R41). `commands.c` marks those with `server_hold()` / `server_release()`.

Commands (`commands.c`):
- The envelope: a line that isn't a JSON object, or has no `seq` that is an integer in uint32 range, gets a `bad_request` event (also for an over-length line, or one with a NUL byte). With a `seq` but no string `cmd`, or a `dev` that isn't a string, the reply is `bad_request`.
- Without `dev`: daemon commands (`estop`; `lease`, `renew`, `release`, `watch`, `unwatch` are `not_impl`). A device command without `dev` is `unknown_cmd`. `estop` goes to the firmware whenever the link is up, even during the handshake.
- With `dev`: `link_down` until the handshake is done; then `unknown_dev` for an `oid` the firmware didn't list, and `unknown_cmd` for a command its type doesn't have. Motor: `rel` → `move`, `abs` → `move_to`, `stop` → `stop`; `pos` and `status` come from the model. `speed`, `accel`, `micro`, `zero`, `home`, `lock` and `unlock` are `not_impl`.
- `abs` with the position invalid is `pos_invalid`, without asking the firmware: the firmware checks that only when soft limits are set (F-R25).
- A command that goes to the firmware replies from the firmware's reply. Its errors keep their code (`busy`, `bad_param`, `pos_invalid`, `not_moving`), except `unknown_oid` → `unknown_dev` and `not_configured` → `link_down`. If the link goes down first, the reply is `link_down`: the link layer then calls every waiting request's callback with no reply.

Machine model (`model.c`): the devices from `list_objects`, and for each stepper `pos`, `pos_valid`, `moving` and `locked` (driver enabled). It is filled by the handshake's `get_state`, then kept current from firmware reports only (D-R20):
- `move_start`, `position` and `move_end` events set `pos`; `move_start` and `move_end` set `moving`;
- an accepted `move` / `move_to` sets `moving` at once, since its reply comes before `move_start`;
- an accepted `estop` voids every position and clears every `locked` (F-R24);
- a link drop voids every position (D-R30).

This is enough because nimhubd is the MCU's only host (D-R1). `set_position` and `set_enable` will update it the same way once `zero`, `lock` and `unlock` exist (step 11).

Tests: `tests/check_daemon.py` adds the client checks:
- every command, and every error code nimhubd or the firmware can give;
- the `bad_request` events and replies;
- several clients, the 9th refused, a client gone with a request pending, and a half-closed client;
- `estop`, a reply that never comes (`link_down`), and requests before the handshake is done;
- the socket's mode, an old socket file, and a missing directory.

`dummy_mcu.py --pos-valid` starts its steppers with a valid position, for `abs`.

| Module | Role today | Fate in the new architecture |
|---|---|---|
| `src/main.c` | Loads config, starts the motor thread, runs the CLI in-process | Becomes the daemon entry point: daemonise, open link, serve clients. Superseded by `src/daemon/main.c` (step 8), which runs in the foreground (systemd, step 14) |
| `src/config.c` | INI loading (inih) | Kept, but the INI schema is redone: one section per device, named by its `oid`, holding behavioural settings only (D-R3, D-R43). A stepper section keeps `microsteps`, `default_speed`, `max_accel`, `pulse_width_us`, `dir_polarity` (initial turning direction), `endstop` (installed or not, D-R44), and optional `soft_limit_min` / `soft_limit_max` (omitted = no limit, F-R25); a load cell section has `scale`, `offset` and `gain`. `[daemon]` holds the USB device and the socket path and group (D-R12); `[link]` holds `report_period` and `timeout_ms`. `chip` and `*_pin` move into the firmware's board table (F-R34); `feedback_pin` is dropped, as the firmware counts pulses exactly (F-R1). Superseded by `src/daemon/config.c` (step 8), where `default_speed` and `max_accel` became `speed` and `accel`, and `[link]` also has `reply_timeout_ms` |
| `src/motor.h` | The `motor_*` API (`src/motor.c`, the lg GPIO + `lgTxPulse` motion thread, is removed) | Implemented by a link client that translates `motor_*` calls into host-link messages. The public `motor_*` API is the seam to preserve |
| `src/ramp.c` | Removed | The firmware computes ramps itself (F-R5, `src/firmware/planner.c`) |
| `src/command.c` | Parse and run text commands | Kept as the shared command layer for all clients (D-R11). Parses JSON requests instead of text lines (D-R40, D-R41), and gains each command's class (D-R13). `help` and `quit` move to the CLI. Superseded by `src/daemon/commands.c` (step 9); the command classes come with the lease (step 11) |
| `src/status.c` | Snapshot and change tracking | Kept. Snapshots fill from firmware reports instead of local atomics (D-R20). The snapshot part is `src/daemon/model.c` since step 9; change tracking comes with `watch` (step 12) |
| `src/cli.c` | stdin CLI with a status bar | Becomes the interactive CLI client, a separate program on the Unix socket (D-R12, D-R15). Translates typed lines into JSON requests; the user always names the device (D-R14). Acquires the lease just before a write command and releases it right after the reply (D-R18) |
| `src/motordriver.c`, `lib/lg/` | Removed (lg PWM experiment; vendored lg) | - |


## Planned structure

Not designed in detail yet. Expected pieces:

- Link layer: JSON framing, `seq` matching, response timeout, and report decoding for the host link. Emergency stop skips the outgoing queue. Reconnects with exponential backoff (250 ms doubling to 2 s). Implements D-R1, D-R17, D-R30, D-R31. Since step 8: `src/daemon/link.c` and `serial.c`, without the estop queue jump (step 14) and reconnects (step 13).
- Machine model: the latest firmware-reported state for each device. This is the only source for `status_capture` (D-R20, D-R22). Converts HX711 raw counts to mg with the INI `scale` and `offset`. Since step 9: `src/daemon/model.c`, steppers only.
- Client server: a Unix socket listener feeding one dispatch path through `command.c`, plus `status` events to watching clients (D-R10–D-R12, D-R15, D-R19, D-R21, D-R40–D-R43). Since step 9: `src/daemon/server.c` and `commands.c`, without `watch` (step 12).
- Lease arbiter: checks each command's class against the current lease, and handles expiry and disconnects (D-R13, D-R16–D-R18).

Dependencies: libc, inih, and cJSON (client protocol D-R40 and host link F-R31). Since step 8, also rxi/log.c.

## Known gaps

- `motor_home` is not implemented (no endstop support yet).
- `soft_limit_min` and `soft_limit_max` are parsed but not enforced. Enforcement moves to the firmware (F-R25); the daemon only forwards them. Since step 8, nimhubd forwards them with `set_motion`.
- The top-level `README.md` has a brief rewrite (2026-11-01) covering the firmware and host tests; it needs the daemon's build and run steps once they exist (build plan step 18). Since step 8, it has how to build and run nimhubd.
- A link drop ends nimhubd instead of reconnecting (D-R30, D-R31; step 13).
- SIGINT / SIGTERM close the link without stopping motion first (D-R2; step 14). The firmware stops it anyway, as a link loss (F-R22).
- Exclusive use (D-R1): the `/proc` scan only sees the processes of nimhubd's own user unless it runs as root, and `TIOCEXCL` doesn't keep root out.
- A key a device section leaves out keeps whatever the firmware has. If the firmware wasn't reset, that can be a value set by an earlier daemon session, not the board table's default.
- No lease yet: every client may write (D-R13, D-R16–D-R18; step 11). Events don't reach clients yet (D-R21; step 12).
- An event with `settings_applied` false (a firmware reset, D-R30) isn't acted on yet (step 13).
- Without `zero` (step 11) there is no way to make a position valid through nimhubd, so `abs` only works after a valid position from an earlier session (e.g. one set with `tests/hostlink.py` before nimhubd started).
