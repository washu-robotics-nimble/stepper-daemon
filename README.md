# Description

This repository holds firmware for a hardware control board, and code for the daemon to interface with it from a Linux host.

- An RP2040 board (e.g. a Raspberry Pi Pico) runs the firmware in `src/firmware/`: it generates the step pulses on its PIO, with trapezoid ramps, and talks to the host over USB (CDC serial, newline-delimited JSON).
- A Linux daemon will own that link and serve clients over a Unix socket, with a CLI client on top. Not written yet.

Design documents are in `docs/` (start with `docs/README.md`); the build plan is `docs/build-plan.md`.

# Build

```shell
# Host unit tests (gcc):
make

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

# Status

- [x] Firmware, one stepper: moves with trapezoid ramps, stop, emergency stop, settings and state, soft limits, link-loss handling
- [ ] Daemon and CLI client
- [ ] Endstop and homing
- [ ] HX711 load cell

`src/*.c` is the pre-migration code (it drove the A4988 from a Raspberry Pi's GPIO through the `lg` library). It no longer builds, and is the starting point for the daemon.
