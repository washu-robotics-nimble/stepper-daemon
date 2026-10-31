# firmware: impl

How the firmware meets [spec.md](spec.md). Update this file in the same commit as any structural change to the firmware.

## Current state (2026-10-11)

No firmware exists yet. Source location in the repo: `src/firmware`.

## Intended approach (not yet committed)

- Toolchain: Pico SDK, C, CMake, in `src/firmware/`. Same language as the daemon.
- USB: TinyUSB CDC, bundled with the Pico SDK (F-R30). USB disconnect, suspend and DTR drop are fed into link-loss handling (F-R22).
- Host link codec (F-R31): cJSON, as in the daemon.
- Board layout (F-R34, F-R35): a compiled-in table of the board's objects (`oid`, type, pins, wiring, safe default settings; for steppers also the attached endstop, homing direction and homing speed). One table per board build.
  - **Never rename or reuse an existing `oid`** in this table: clients and scripts address objects by it. Retire an object by deleting its entry; a new object gets a new `oid`.
- Step generation (F-R1–F-R4, F-R6): one PIO state machine per axis. The CPU pushes `(delay, count)` words into the PIO FIFO; the PIO emits exact pulse counts with cycle-exact timing, so jitter is independent of USB traffic (F-R3). A stopped move reads back the exact emitted count from the PIO (F-R1).
- Ramps (F-R5): the trapezoid planner, ported from the daemon's `src/ramp.c`, runs on core 1 and refills the PIO FIFOs. Core 0 handles USB, framing and command dispatch.
- Endstop (F-R20): a GPIO IRQ on core 1 halts the axis's PIO state machine directly.
- Emergency stop (F-R24): handled in the USB receive path, before the command queue.
- PWM and servo (F-R7, F-R41): RP2040 hardware PWM slices.
- HX711 (F-R43): a PIO program, so reading never competes with step timing.

## Known gaps

All of it.