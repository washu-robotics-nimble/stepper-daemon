# Description

This repository holds firmware for a hardware control board, and code for the daemon to interface with it from a Linux host.

The firmware runs on a RP2040-based board to handle all hardware modules. It communicates with the host over USB CDC.

A Linux daemon, `nimhubd` (`src/daemon/`) exclusively owns the link. It configures all hardware on init, and serves clients over a Unix socket. Current clients include:
    - `nimhub-cli` (`src/cli/`): the command-line client, with a live status bar.
    - *TBD*: Python client that can be integrated with other programs.

Design documents are in `docs/` (start with `docs/README.md`).

# Build

```shell
# The daemon and the CLI, build/nimhubd and build/nimhub-cli (gcc):
make

# Host unit tests:
make test

# Both against a dummy MCU on a pty, no board needed (Python 3):
make check-daemon

# Firmware (Pico SDK 2.x, arm-none-eabi-gcc, cmake):
cmake -S src/firmware -B src/firmware/build -DPICO_SDK_PATH=<pico-sdk>
cmake --build src/firmware/build
# Flash: hold BOOTSEL while plugging the board in, then copy
# src/firmware/build/stepper_fw.uf2 to the drive that appears.

# Talk to the firmware directly (Python 3, standard library only):
tests/hostlink.py --wait /dev/serial/by-id/<board> '{"cmd":"identify"}'
tests/check_commands.py /dev/serial/by-id/<board>   # checks every command
```

Details, including `picotool` and the SDK version, are in `docs/firmware/impl.md`.

# Run

```shell
build/nimhubd -d nimhub.ini    # -d: also log every frame
```

Copy `nimhub.ini` and set `device` in `[daemon]` to the board's `/dev/serial/by-id/<board>`. Without a CONFIG argument, nimhubd reads `/etc/nimhubd/nimhub.ini`, or, if that doesn't exist, `~/.config/nimhubd/nimhub.ini` (handy for testing without write access to `/etc`). It runs in the foreground and logs to stderr; stop it with Ctrl-C.

Clients connect to the Unix socket set by `socket` in `[daemon]` (default `/run/nimhubd/control.sock`, whose directory systemd creates). When running nimhubd by hand, point `socket` at a writable place, e.g. `<runtime-dir>/control.sock`.

```shell
build/nimhub-cli -s <runtime-dir>/control.sock              # interactive, with a status bar
build/nimhub-cli -s <runtime-dir>/control.sock m0 rel 400   # one command, then exit
```

Type `help` in the CLI for its commands. Requests are JSON lines (`docs/daemon/spec.md`, Client protocol), so any client works, e.g.:

```shell
socat - UNIX-CONNECT:<runtime-dir>/control.sock
{"seq":1,"dev":"m0","cmd":"rel","steps":400}
{"seq":2,"dev":"m0","cmd":"status"}
```

Without the board, `tests/dummy_mcu.py --link <path>` makes a dummy MCU on a pty; point `device` at `<path>`.

# Status

- [x] Firmware, one stepper: moves with trapezoid ramps, stop, emergency stop, settings and state, soft limits, link-loss handling
- [ ] Daemon: link, handshake, settings, the client socket, the lease, and every motor command but `home` done; `watch`, reconnecting and systemd to come
- [x] CLI client, with automatic lease handling
- [ ] Endstop and homing
- [ ] HX711 load cell

# License

MIT.

# Old code

This project *was* designed to interface hardware from a Raspberry Pi 4 through the `lg` library. This is no longer the intent, and legacy code is in `src/*.c` (does not build).
