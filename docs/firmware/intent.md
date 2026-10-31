# firmware: intent

## Purpose

RP2040 firmware. It performs all timing-critical actions: step pulses, PWM outputs, inputs, etc. It receives commands from a daemon over USB, executes them, and reports results.

## Goals

- `F-G1` Every commanded step MUST be emitted exactly once, at the
  commanded timing, regardless of host load. No dropped or bunched pulses.
- `F-G2` Several outputs MUST be able to run at the same time independently.
- `F-G3` Endstops, stops and faults are handled in the firmware within a bounded short time, without waiting for the host.
- `F-G4` The firmware is the source of truth for position and I/O state, and reports changes as they happen, including mid-move.
- `F-G5` If the host link is lost, hardware goes to a
  defined safe state.
- `F-G6` The hardware layout (which objects exist, their pins and wiring) is fixed in the firmware.
  Behavioural settings (speed, acceleration, microsteps, ...) come from the host at runtime; reflashing is only for firmware or hardware changes.
- `F-G7` Read HX711 load cell ADC using bit-banged interface.

## Non-goals

- Client handling, UI, persistent config. Those live in the daemon.
- Running without the daemon, beyond the F-G5 safe state.
- Receiving from multiple command sources.
- Coordinated multi-axis motion.
- Closed-loop feedback. Endstops are the only feedback.

## Scope

Now: one stepper axis and one endstop. Several outputs may run at once, but never coordinated.

Later: ADC, servo, I2C/SPI. Much later, possibly more axes.