# Description

This repository holds firmware for a hardware control board, and code for the daemon to interface with it from a Linux host.

- An RP2040 board (e.g. a Raspberry Pi Pico) runs the firmware in `src/firmware/`: it generates the step pulses on its PIO, with trapezoid ramps, and talks to the host over USB (CDC serial, newline-delimited JSON).
- A Linux daemon, `nimhubd` (`src/daemon/`), owns that link. It will serve clients over a Unix socket, with a CLI client on top; for now it connects, configures the board and keeps the link alive.

Design documents are in `docs/` (start with `docs/README.md`); the build plan is `docs/build-plan.md`.

# Build

```shell
# The daemon, build/nimhubd (gcc):
make

# Host unit tests:
make test

# The daemon against a dummy MCU on a pty, no board needed (Python 3):
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

Clients connect to the Unix socket set by `socket` in `[daemon]` (default `/run/nimhubd/control.sock`, whose directory systemd creates). When running nimhubd by hand, point `socket` at a writable place, e.g. `<runtime-dir>/control.sock`. Requests are JSON lines (`docs/daemon/spec.md`, Client protocol):

```shell
socat - UNIX-CONNECT:<runtime-dir>/control.sock
{"seq":1,"dev":"m0","cmd":"rel","steps":400}
{"seq":2,"dev":"m0","cmd":"status"}
```

Without the board, `tests/dummy_mcu.py --link <path>` makes a dummy MCU on a pty; point `device` at `<path>`.

# Status

- [x] Firmware, one stepper: moves with trapezoid ramps, stop, emergency stop, settings and state, soft limits, link-loss handling
- [ ] Daemon: link, handshake, settings, and the client socket with `status`, `pos`, `rel`, `abs`, `stop`, `estop` done; lease and `watch` to come
- [ ] CLI client
- [ ] Endstop and homing
- [ ] HX711 load cell

`src/*.c` (directly in `src/`) is the pre-migration code (it drove the A4988 from a Raspberry Pi's GPIO through the `lg` library). It no longer builds, and is the starting point for the daemon.
