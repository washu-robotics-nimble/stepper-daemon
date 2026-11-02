# daemon: impl

How the code meets [spec.md](spec.md). Update this file in the same commit as any structural change to the daemon.

## Current state (2026-11-02)

The code in `src/` (outside `src/firmware/` and `src/daemon/`) is still **pre-migration**: the remains of `motor_demo`, which ran on a Raspberry Pi and drove one A4988 directly through lg. lg and the lg-only modules were removed on 2026-11-01, so this code no longer builds; it is kept as the starting point for the daemon. `motor.h` still includes the removed `lgpio.h`, and `command.c` still calls `lguErrorText()`.
We need to move all daemon code into `src/daemon/` later.

Build plan step 0 (2026-10-31): cJSON v1.7.19 is vendored in `lib/cJSON/`. `make test` builds and runs every `tests/test_*.c` as a host program (plain C with `assert`). Since 2026-11-01, the `motor_demo` target is gone; since 2026-11-02, plain `make` builds `build/nimhubd`.

Build plan step 8 (2026-11-02): the daemon is called `nimhubd`, in `src/daemon/`. It loads the INI, opens the MCU's serial device exclusively (D-R1), runs the handshake (D-R3), and then keeps the link alive and logs the MCU's events. There is no client socket yet (step 9; added since, below). A link drop or a failed handshake ends it with status 1 (reconnecting is step 13; since then, it retries instead, below); SIGINT / SIGTERM close the link and end it with status 0.

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
- `[daemon]`: `device` (required), `socket` (default `/run/nimhubd/control.sock`, D-R12), `group` (default `stepper`); the last two are used from step 9. Since step 11 also `lease_timeout_ms` (default 5000, D-R18).
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
- Without `dev`: daemon commands (`estop`; `devices` since step 10, `link_down` until the handshake is done; `lease`, `renew`, `release`, `watch`, `unwatch` are `not_impl`; the lease commands work since step 11). A device command without `dev` is `unknown_cmd`. `estop` goes to the firmware whenever the link is up, even during the handshake.
- With `dev`: `link_down` until the handshake is done; then `unknown_dev` for an `oid` the firmware didn't list, and `unknown_cmd` for a command its type doesn't have. Motor: `rel` → `move`, `abs` → `move_to`, `stop` → `stop`; `pos` and `status` come from the model. `speed`, `accel`, `micro`, `zero`, `home`, `lock` and `unlock` are `not_impl` (all but `home` work since step 11).
- `abs` with the position invalid is `pos_invalid`, without asking the firmware: the firmware checks that only when soft limits are set (F-R25).
- A command that goes to the firmware replies from the firmware's reply. Its errors keep their code (`busy`, `bad_param`, `pos_invalid`, `not_moving`), except `unknown_oid` → `unknown_dev` and `not_configured` → `link_down`. If the link goes down first, the reply is `link_down`: the link layer then calls every waiting request's callback with no reply.

Machine model (`model.c`): the devices from `list_objects`, and for each stepper `pos`, `pos_valid`, `moving` and `locked` (driver enabled). It is filled by the handshake's `get_state`, then kept current from firmware reports only (D-R20):
- `move_start`, `position` and `move_end` events set `pos`; `move_start` and `move_end` set `moving`;
- an accepted `move` / `move_to` sets `moving` at once, since its reply comes before `move_start` (withdrawn 2026-11-02, see "State after a command" below);
- an accepted `estop` voids every position and clears every `locked` (F-R24) (withdrawn 2026-11-02, likewise);
- a link drop voids every position (D-R30). (Withdrawn in step 13: that was the daemon's own guess, against D-R20; while the link is down, reads get `link_down` instead.)

This is enough because nimhubd is the MCU's only host (D-R1). `set_position` and `set_enable` will update it the same way once `zero`, `lock` and `unlock` exist (step 11). (Since 2026-11-02, after any of these the state comes from `get_state` instead.)

Tests: `tests/check_daemon.py` adds the client checks:
- every command, and every error code nimhubd or the firmware can give;
- the `bad_request` events and replies;
- several clients, the 9th refused, a client gone with a request pending, and a half-closed client;
- `estop`, a reply that never comes (`link_down`), and requests before the handshake is done;
- the socket's mode, an old socket file, and a missing directory.

`dummy_mcu.py --pos-valid` starts its steppers with a valid position, for `abs`.

Build plan step 10 (2026-11-02): the CLI client, `nimhub-cli` (planned as `stepper-cli`), in `src/cli/`, built to `build/nimhub-cli`. It is a separate process on the socket (D-R15) and uses only the client protocol (D-R11). nimhubd gained the `devices` command, so clients can find the devices (D-R43).

| File | Role |
|---|---|
| `main.c` | Command line `nimhub-cli [-s SOCKET] [COMMAND ...]` (default socket `/run/nimhubd/control.sock`, no fallback), the three modes, and the status bar |
| `parse.c` / `.h` | Typed lines to requests. Pure C |
| `conn.c` / `.h` | The socket connection and its line framing |

Dependencies: cJSON, and linenoise (`lib/linenoise/`, BSD-2-Clause, commit `a473823`, unchanged) for line editing, history and tab completion.

Modes:
- One-shot: `nimhub-cli m0 rel 400` sends that one command, prints the reply and exits. Options end at the first word, so `m0 rel -100` works.
- Stdin: with stdin not a terminal, one command per line.
- Interactive: on a terminal with no command.
- Replies and events are printed as nimhubd sends them, one JSON line each. Local errors go to stderr, except in interactive mode.
- The exit status is 1 if any command failed (an error reply, a `bad_request` event, a local error, no reply within 3 s, or no connection).

Typed lines (`parse.c`):
- `<dev> <cmd> [arg]` is a device command (D-R14). The argument goes into `steps` for `rel`, `pos` for `abs`, and `value` for any other command; it is sent as a number if it reads as one, else as a string.
- A single word is a daemon command (`estop`, `devices`, ...).
- A line starting with `{` is sent as raw JSON, with `seq` added if it has none; a `seq` it has is kept, and its reply is matched by it.
- `help` and `quit` are local.
- Anything else goes out as typed, and nimhubd decides (`unknown_cmd`, `not_impl`, `bad_param`), so the CLI stays thin.

Interactive mode (`main.c`):
- One `poll()` loop for the terminal and the socket. linenoise is used through its multiplexing API (`linenoiseEditStart` / `Feed` / `Stop`, `linenoiseHide` / `Show`), so the bar keeps updating while a line is being typed.
- The bar sits on the line below the prompt and is repainted with cursor save / restore, as in the old `src/cli.c`. Every 250 ms it sends `status` for each motor from `devices` (polling until `watch`, step 12; since step 12 it follows `status` events instead, below). It shows `m0 400 (invalid) idle free`, teal while a motor moves, red for `link down` or `nimhubd not running`.
- When the socket is gone, the CLI reconnects every second.
- Tab completes commands and device names. History is kept for the session only.
- Ctrl-C with something typed drops the line; Ctrl-D, or Ctrl-C on an empty line, quits.
- While a request waits for its reply (up to 3 s), there is no prompt.

Tests:
- `tests/test_cli_parse.c` (part of `make test`) checks the parser.
- `tests/check_cli.py` (also run by `make check-daemon`) runs nimhub-cli through nimhubd and the dummy: one-shot and stdin mode, and a short interactive session on a pty (bar, a move, tab completion, quitting). How the bar looks is checked by hand.

Build plan step 11 (2026-11-02): the lease, the command classes, and the motor commands `speed`, `accel`, `micro`, `zero`, `lock` and `unlock`. `home` stays `not_impl` (step 16), as do `watch` and `unwatch` (step 12).

| File | Role |
|---|---|
| `lease.c` / `.h` (daemon) | The lease arbiter (D-R13, D-R16–D-R18): one lease, its expiry, and the check of each command's class. Pure C, with the time passed in |

Lease (`lease.c`, `commands.c`):
- Every command in `commands.c`'s tables has its class from the spec. Before a command runs, `lease_check()` lets read, session and emergency commands through, and a write only from the holder. Only `lease` and `renew` renew the lease; writes don't, as the spec says (D-R18). Otherwise the reply is `lease_held`, with `msg` saying whether another client holds the lease or nobody does.
- The order of the checks: the envelope, `link_down`, `unknown_dev`, `unknown_cmd`, then the lease, then `not_impl` and the command's own checks.
- Clients are keyed by their server slot and its generation, so a new connection in the same slot is a different client. The server tells `commands.c` when a client goes (`server_close_fn`), and its lease is freed then.
- Expiry is checked when the lease is next used; nothing runs on a timer. A move started under an expired lease goes on.
- `stop` is a write command, as the spec says: a client without the lease can't stop another client's move; `estop` can.

Motor commands (`commands.c`):
- `speed`, `accel` and `micro` take an int32 `value` and send one `set_motion` field (`speed`, `accel`, `microsteps`). The firmware checks the range and the combination (F-R2), and refuses while moving (`busy`).
- An accepted value is kept in the model (`model_set_setting()`), for step 13 to re-send after a reconnect instead of the INI's (D-R3). (Since step 13 they are kept in the model's runtime table, which `model_reset()` leaves alone.) `handshake_start()` resets the model, so step 13 has to keep those values across it.
- `zero` sends `set_position 0`; the model's position becomes 0 and valid. (Since 2026-11-02: from `get_state`, see "State after a command".)
- `lock` / `unlock` send `set_enable 1` / `0`; the model follows, and `unlock` voids the position, as the firmware does. (Likewise.)

nimhub-cli (`main.c`, `parse.c`):
- A typed write command (`cli_is_write()`, a copy of the spec's write class) is wrapped in the lease: `lease`, then the write, then `release`. Only the write's reply is printed. If the lease is held by another client, the `lease_held` reply is printed instead, and nothing is sent.
- Raw JSON goes out as it is, with no lease around it.
- Typing `lease` keeps the lease: writes then go out without the wrapping, and in interactive mode the lease is renewed in the background at half its `timeout_ms`, and shown in the bar (`lease: yours`), until `release`. In stdin mode there is no background loop, so the CLI sends `renew` before the next command once half the timeout has passed; a long pause between lines can still let the lease expire.
- If a renewal fails, the CLI says the lease is lost.

Tests:
- `tests/test_daemon_lease.c` (part of `make test`) checks taking, renewing, expiry, release, disconnect and each class.
- `tests/check_daemon.py` adds checks for writes without the lease, two clients and one lease, release on disconnect, expiry with a short `lease_timeout_ms` (writes and reads don't renew it; `lease` and `renew` do), a move outliving its lease, `estop` without the lease, and the new motor commands.
- `tests/check_cli.py` adds the automatic lease, a write refused while another client holds the lease, the user's own lease in stdin mode (renewed before commands when lines come slower than the lease), and its background renewal in interactive mode.
- `tests/test_cli_parse.c` checks the write classification.

Build plan step 12 (2026-11-02): `watch` / `unwatch` and `status` events (D-R20–D-R22); the CLI's bar follows them instead of polling.

Daemon (`commands.c`, `model.c`, `main.c`):
- `commands.c` keeps the watching clients by server slot and generation; a disconnect ends the watch.
- `watch` replies, then sends that client a `status` event for every stepper whose state is known. `unwatch` stops the events.
- Each model device remembers the state last broadcast. `model_take_change()` compares it with the current state, and `commands_broadcast()` sends one event per changed device to every watcher. Several fields changing at once (`move_end`: `pos` and `moving`) give one event.
- `commands_broadcast()` runs after every firmware event (`on_event()` in `main.c`) and after every firmware reply to a client command (after that reply is sent, so the sender gets the reply first). It also runs once per main loop pass, which catches the handshake's `get_state`: a client that watched before the handshake was done gets every device's state then.
- An accepted `move` sets `moving` at once (step 9), so the event for the start of a move comes with the reply, and `move_start` changes nothing after it. (Since 2026-11-02, `moving` comes from the `get_state` after the move.) A `position` event with an unchanged `pos` sends nothing.
- `status` replies and events share `add_state()`, so they have the same fields.

nimhub-cli: after `devices` succeeds, it sends `watch`, and the bar follows the `status` events, which aren't printed in interactive mode. `devices` is retried every 250 ms while nimhubd has no MCU yet (`link_down`). In one-shot and stdin mode, events are printed like any other line.

Tests:
- `tests/check_daemon.py`: the snapshot after `watch` (the same as `status`), silence while nothing changes, one event per change through a move (moving first, growing positions, the end at the target, no two in a row the same), `zero`, `lock`, `unlock`, nothing for a command that changes nothing, `watch` again, `unwatch`, and watching before the handshake is done.
- `tests/check_cli.py`: the bar follows a move, and nimhubd's log shows the CLI's `watch` and no `status` requests from it.

Build plan step 13 (2026-11-02): link failure and reconnect (D-R30, D-R31), and runtime settings re-sent after it (D-R3).

Connection (`main.c`): the device is closed (`fd` -1, retried at `next_attempt`), or open with a handshake running, done or failed.
- **Drop:** a link drop (read or write error, hangup, missing reply, or `link_close()`) or a failed handshake closes the device. Requests still waiting get `link_down` replies. If clients had been told `link_up`, they all get `link_down`, with the reason.
- **Retry:** the next attempt waits for the backoff: 250 ms, doubling up to 2 s, forever.
- **Up:** a finished handshake resets the backoff, sends `link_up` to every client, and then sends the devices' state to watchers. `update()` runs right after the I/O of each loop pass and before the catch-all broadcast, so `link_up` always comes first.
- **Logging once (D-R31):** each failure reason is logged at error level when it differs from the last one, and at debug level when it repeats; success logs "link up again". To make that possible, `serial_open()`, the link and the handshake hand back their reasons (`err`, `down_reason`, `error`) instead of logging them. While retrying, the handshake's progress and the firmware's events (e.g. `link_recovered` on every open) go to debug level too.
- **Startup:** another process holding the device still makes nimhubd refuse to start (D-R1). A missing device, or any other open error, is retried like a reconnect, so nimhubd can start before the board is plugged in.
- **Second instance:** without the device, the device lock can't stop a second nimhubd. So before replacing an old socket file, `server_open()` tries to connect to it, and refuses if another nimhubd answers.
- **Firmware reset (D-R30):** an event with `settings_applied` false after the handshake (`on_event()`), or a `not_configured` reply to a client command (`commands.c`), calls `link_close()`. That reconnects with a full handshake.
- **While down:** device commands, `pos`, `status` and `devices` get `link_down`. Nothing reports a position, and no `status` event goes out: the daemon never makes up `pos_valid: false` (D-R20). Session and read commands without `dev` work, since D-R16 says any time. `estop` gets `link_down`, because it can't be sent. The lease and the watch subscriptions are kept.
- `commands_t.up` says clients were told `link_up` and not `link_down` since. Device commands, `devices`, the `watch` snapshot and broadcasts check it instead of the handshake state.

Runtime settings (D-R3): accepted `speed`, `accel` and `micro` values are kept by `oid` in the model's runtime table, which `model_reset()` leaves alone. The handshake sends `set_motion` to every stepper that has an INI section or runtime settings, and the runtime values replace the INI's fields.

State after a command (2026-11-02, D-R20): the model changes only on firmware reports, never on what an accepted command implies.
- After the firmware accepts a command that changes a device's state, `commands.c` sends `get_state` for that device, and replies to the client once the answer is in the model. That covers `move` / `move_to`, `set_position` (`zero`) and `set_enable` (`lock`, `unlock`); for `estop`, every stepper is asked.
- So `unlock` and `estop` void the position because the firmware reports it, and an accepted move shows `moving` because `get_state` says so.
- `refresh()` / `on_refresh()` count the `get_state` requests still waiting; `finish()` sends the held reply, then the status events, then releases the client.
- A `get_state` without a reply (link drop) leaves the model as it was; clients get `link_down` anyway.
- `stop` needs no refresh: its reply carries the end position, and `move_end` follows. `speed`, `accel` and `micro` change settings, not state.
- `model_move_accepted()`, `model_estop()`, `model_zeroed()` and `model_enabled()` are gone.

nimhub-cli: on `link_down` the bar shows "link down"; on `link_up` it asks for `devices` again and re-sends `watch`. Both events are printed.

Tests:
- `tests/dummy_mcu.py` gains SIGUSR1 (reset to the state after boot, without closing the port) and SIGUSR2 (stop or resume replying).
- `tests/check_daemon.py`:
  - unplugged during a move: `link_down` to every client, `link_down` replies, the lease kept, no `status` event while down;
  - back after 2 s: `link_up` at the backoff's 4th attempt, the state from the firmware, the runtime `speed` re-sent, each error logged once;
  - a second drop: back at the first attempt, since the backoff was reset; runtime settings for a stepper without an INI section re-sent too;
  - a firmware reset through `not_configured`; a silent MCU (missed replies), retried and logged once;
  - nimhubd starting before the device exists;
  - failed handshakes (protocol, rejected settings, wrong key for the type, missing reply, busy) now retry, logging once, instead of exiting.
- `tests/check_cli.py`: the bar goes red on `link_down` and back on `link_up`.
- On the Pico (USB only, 2026-11-02): unplugged during a move for about 10 s. The watcher got `link_down`, nothing else until `link_up`, then the firmware's state (it had rebooted). The runtime `speed` 300 was re-sent instead of the INI's 200, and each error was logged once.

| Module | Role today | Fate in the new architecture |
|---|---|---|
| `src/main.c` | Loads config, starts the motor thread, runs the CLI in-process | Becomes the daemon entry point: daemonise, open link, serve clients. Superseded by `src/daemon/main.c` (step 8), which runs in the foreground (systemd, step 14) |
| `src/config.c` | INI loading (inih) | Kept, but the INI schema is redone: one section per device, named by its `oid`, holding behavioural settings only (D-R3, D-R43). A stepper section keeps `microsteps`, `default_speed`, `max_accel`, `pulse_width_us`, `dir_polarity` (initial turning direction), `endstop` (installed or not, D-R44), and optional `soft_limit_min` / `soft_limit_max` (omitted = no limit, F-R25); a load cell section has `scale`, `offset` and `gain`. `[daemon]` holds the USB device and the socket path and group (D-R12); `[link]` holds `report_period` and `timeout_ms`. `chip` and `*_pin` move into the firmware's board table (F-R34); `feedback_pin` is dropped, as the firmware counts pulses exactly (F-R1). Superseded by `src/daemon/config.c` (step 8), where `default_speed` and `max_accel` became `speed` and `accel`, and `[link]` also has `reply_timeout_ms` |
| `src/motor.h` | The `motor_*` API (`src/motor.c`, the lg GPIO + `lgTxPulse` motion thread, is removed) | Implemented by a link client that translates `motor_*` calls into host-link messages. The public `motor_*` API is the seam to preserve |
| `src/ramp.c` | Removed | The firmware computes ramps itself (F-R5, `src/firmware/planner.c`) |
| `src/command.c` | Parse and run text commands | Kept as the shared command layer for all clients (D-R11). Parses JSON requests instead of text lines (D-R40, D-R41), and gains each command's class (D-R13). `help` and `quit` move to the CLI. Superseded by `src/daemon/commands.c` (step 9); the command classes come with the lease (step 11), and are there since |
| `src/status.c` | Snapshot and change tracking | Kept. Snapshots fill from firmware reports instead of local atomics (D-R20). The snapshot part is `src/daemon/model.c` since step 9; change tracking comes with `watch` (step 12), and is `model_take_change()` since |
| `src/cli.c` | stdin CLI with a status bar | Becomes the interactive CLI client, a separate program on the Unix socket (D-R12, D-R15). Translates typed lines into JSON requests; the user always names the device (D-R14). Acquires the lease just before a write command and releases it right after the reply (D-R18). Superseded by `src/cli/` (`nimhub-cli`, step 10), with the lease handling since step 11 |
| `src/motordriver.c`, `lib/lg/` | Removed (lg PWM experiment; vendored lg) | - |


## Planned structure

Not designed in detail yet. Expected pieces:

- Link layer: JSON framing, `seq` matching, response timeout, and report decoding for the host link. Emergency stop skips the outgoing queue. Reconnects with exponential backoff (250 ms doubling to 2 s). Implements D-R1, D-R17, D-R30, D-R31. Since step 8: `src/daemon/link.c` and `serial.c`, without the estop queue jump (step 14) and reconnects (step 13).
- Machine model: the latest firmware-reported state for each device. This is the only source for `status_capture` (D-R20, D-R22). Converts HX711 raw counts to mg with the INI `scale` and `offset`. Since step 9: `src/daemon/model.c`, steppers only.
- Client server: a Unix socket listener feeding one dispatch path through `command.c`, plus `status` events to watching clients (D-R10–D-R12, D-R15, D-R19, D-R21, D-R40–D-R43). Since step 9: `src/daemon/server.c` and `commands.c`, without `watch` (step 12; there since).
- Lease arbiter: checks each command's class against the current lease, and handles expiry and disconnects (D-R13, D-R16–D-R18). Since step 11: `src/daemon/lease.c`.

Dependencies: libc, inih, and cJSON (client protocol D-R40 and host link F-R31). Since step 8, also rxi/log.c.

## Known gaps

- `motor_home` is not implemented (no endstop support yet).
- `soft_limit_min` and `soft_limit_max` are parsed but not enforced. Enforcement moves to the firmware (F-R25); the daemon only forwards them. Since step 8, nimhubd forwards them with `set_motion`.
- The top-level `README.md` has a brief rewrite (2026-11-01) covering the firmware and host tests; it needs the daemon's build and run steps once they exist (build plan step 18). Since step 8, it has how to build and run nimhubd.
- ~~A link drop ends nimhubd instead of reconnecting (D-R30, D-R31; step 13).~~ Done in step 13.
- SIGINT / SIGTERM close the link without stopping motion first (D-R2; step 14). The firmware stops it anyway, as a link loss (F-R22).
- Exclusive use (D-R1): the `/proc` scan only sees the processes of nimhubd's own user unless it runs as root, and `TIOCEXCL` doesn't keep root out.
- A key a device section leaves out keeps whatever the firmware has. If the firmware wasn't reset, that can be a value set by an earlier daemon session, not the board table's default.
- ~~No lease yet: every client may write (D-R13, D-R16–D-R18; step 11).~~ Done in step 11. ~~Events don't reach clients yet (D-R21; step 12).~~ Done in step 12.
- Fault state (D-R20) isn't reported to clients; the firmware's `fault` events are only logged (step 16, with `home_timeout`).
- ~~An event with `settings_applied` false (a firmware reset, D-R30) isn't acted on yet (step 13).~~ Done in step 13. That path has no end-to-end test: the dummy MCU sends no state report on its own after a reset, so only the `not_configured` path is tested.
- The `/proc` check (D-R1) refuses to start only at startup. A process that grabs the device while nimhubd is reconnecting is logged and retried around.
- ~~nimhub-cli doesn't handle the lease yet.~~ Done in step 11.
- ~~Without `zero` (step 11) there is no way to make a position valid through nimhubd, so `abs` only works after a valid position from an earlier session (e.g. one set with `tests/hostlink.py` before nimhubd started).~~ `zero` exists since step 11.
