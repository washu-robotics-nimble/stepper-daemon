# daemon: intent

## Purpose

A long-running service on a Linux host that is the single point of control for the hardware. It talks to the RP2040 over USB, and provides a Unix socket interface for clients. Accepts commands from client processes, and reports hardware state back to them.

## Context

A Linux system cannot generate reliable software PWM to handle growing hardware control needs due to timing jitter. Hence timing-critical tasks are offloaded to a MCU, while the host focus on serving clients.

## Goals

- `D-G1` Provide a single, unique control entry point for the RP2040 MCU.
- `D-G2` Support multiple client connections.
- `D-G3` Load configurable parameters onto the RP2040 MCU to avoid reflashing firmware after MCU reboot.
- `D-G4` Maintain a stable and reliable link to the MCU.

## Non-goals

1. Do not handle any timing-critical tasks.
2. No motion planning. They should be done by the MCU.
3. No remote network connections. Focus on Unix socket instead.
4. Do not implement any UI. They are left as dedicated clients.