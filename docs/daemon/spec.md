# daemon: spec

Design specification to satisfy [intent.md](intent.md). 

See [../firmware/spec.md#host-link](../firmware/spec.md#host-link) for link details which is not restated here.

## Units

To ensure precision and consistency, units are in:
- For motor device:
    - Position: full steps, signed 32-bit.
    - Speed: full steps/s.
    - Acceleration: full steps/s^2.
    - Microsteps: 1, 2, 4, 8, 16.
- For load sensor device:
    - Mass: milligrams, signed 32-bit (range ±2147 kg).
    - Raw reading: HX711 counts, signed 24-bit. Clients MAY request raw counts alongside mass.
    - The firmware reports raw counts only. The daemon converts: `mass_mg = round((raw - offset) * scale)`, with `scale` (mg/count) and `offset` (counts) from the INI's load cell section.

## Process

- `D-R1` The daemon MUST be the only process that opens the RP2040's USB device, and MUST refuse to start if the device is already held. (`D-G1`)
- `D-R2` The daemon MUST run without a terminal; on SIGTERM/SIGINT, stop motion, then close the link. (`D-G1`)
- `D-R3` The daemon MUST load an INI config at startup. After every handshake it MUST query the firmware's objects (F-R35), send each object that has an INI section its settings, and then signal that settings are complete (F-R33), before accepting motion commands. Objects without an INI section keep the firmware's safe defaults (F-R34). Settings changed by clients at runtime (e.g. `speed`, `offset`, `tare`) MUST be kept by the daemon and re-sent instead of the INI values; they revert to the INI only when the daemon restarts. The INI holds behavioural settings only; the hardware layout is defined by the firmware (F-R34). (`D-G3`)

## Clients

- `D-R10` Daemon MUST accept at least two simultaneous client connections. (`D-G2`)
- `D-R11` Client API MUST be independent of the client type. The CLI and the Python clients use the same commands. (`D-G2`)
- `D-R12` Clients MUST connect over a single Unix domain socket. The daemon MUST NOT listen on any network socket. Access is controlled by the socket file's permissions. (`D-G2`)
  - Default path: `/run/stepper-daemon/control.sock`, mode `0660`, group `stepper`. The directory is created by systemd `RuntimeDirectory=`.
  - Path and group are overridable in the INI.
- `D-R13` Every command MUST belong to exactly one class: 'read' (read-only), 'session' (lease handling), 'emergency', or 'write'. The daemon MUST be the only place where conflicts between clients are resolved. (`D-G1`, `D-G2`)
- `D-R15` Every client, including the interactive CLI, MUST be a separate process that connects through the client API. The daemon MUST NOT contain a user interface or serve HTTP. (`D-G2`)
- `D-R16` Read-only and session commands MUST be accepted from any client at any time, and need no lease. (`D-G2`)
- `D-R17` Emergency commands MUST be honored from any client at any time, with or without a lease, and MUST be sent to the firmware ahead of all other queued traffic. (`D-G1`)
- `D-R18` A write command MUST be rejected with a "lease held" error unless the sender holds the lease. Lease rules: (`D-G2`)
  - acquired explicitly and kept by renewal;
  - expires after 5 s (configurable) without renewal;
  - freed immediately on release or disconnect;
  - only one client holds it at a time;
  - running move when the lease expires: continue;
  - there is one lease for the whole daemon, covering all devices;
  - clients SHOULD hold the lease only while it is needed, and release it as early as possible.
- `D-R19` The client protocol MUST be fully specified in this file, and SHOULD be implementable using only the Python standard library (`socket`, `json`). Wire format: see [Client protocol](#client-protocol). (`D-G2`)

## Client protocol

- `D-R40` Framing: newline-delimited JSON over the Unix socket, UTF-8, one object per line terminated by `\n`. Lines are capped at 4096 bytes. An over-length or unparseable line is dropped and answered with `bad_request`; if its `seq` can't be read, the error is sent as a `bad_request` event instead of a reply. (`D-G2`)
- `D-R41` Every request MUST get exactly one reply, echoing the request's `seq`. Replies and events MAY interleave on the same connection. (`D-G2`)
- `D-R42` Receivers MUST ignore unknown fields. (`D-G2`)
- `D-R43` Devices are the objects the firmware reports (F-R35), named by their `oid` (e.g. `m0`, `lc0`). What is installed is known only from that query. The INI has one section per device, named by its `oid` (e.g. `[m0]`, `[lc0]`); there are no per-type defaults. A section for an `oid` the firmware doesn't report is ignored with a log line. The section names `daemon` and `link` are reserved for daemon settings and are never used as `oid`s. (`D-G1`)
- `D-R44` A stepper's INI section MUST state whether an endstop is installed (`endstop = true` / `false`; missing means `false`). The daemon MUST reject `home` with `no_endstop`, without sending it to the firmware, unless the INI says an endstop is installed; the firmware rejects it in turn if its board table has none (F-R20). (`D-G1`)

Envelope:

| Field | Direction | Meaning |
|---|---|---|
| `seq` | both | uint32 chosen by the client, required on every request; the reply echoes it |
| `cmd` | client → daemon | Command name |
| `dev` | client → daemon | Target device's `oid` (D-R43). Required for device commands, absent for daemon commands |
| `ack` | daemon → client | `true` on success, `false` on failure |
| `err` | daemon → client | Error code when `ack` is `false` |
| `msg` | daemon → client | Optional human-readable detail for an error |
| `evt` | daemon → client | Name of an unsolicited event; events carry no `seq` |
| *(other)* | both | Command-specific arguments and results as flat key/value pairs |

```
{"seq":7,"dev":"m0","cmd":"rel","steps":400}
{"seq":7,"ack":true}
{"seq":8,"dev":"m0","cmd":"speed","value":300}
{"seq":8,"ack":false,"err":"lease_held","msg":"lease held by another client"}
{"seq":9,"dev":"lc0","cmd":"weigh"}
{"seq":9,"ack":true,"mg":152300,"raw":-84211}
{"evt":"status","dev":"m0","moving":true,"pos":1200,"pos_valid":true,"locked":true}
```

Error codes:

| Code | Meaning |
|---|---|
| `bad_request` | Line over-length, not JSON, or missing `seq` / `cmd` |
| `unknown_cmd` | No such command (for this device) |
| `unknown_dev` | No such device |
| `bad_param` | Argument missing, wrong type or out of range |
| `lease_held` | Write command without holding the lease, or lease held by another client |
| `busy` | Device is moving and the command needs it idle |
| `not_moving` | `stop` with no move in progress |
| `pos_invalid` | Absolute move while position is invalid |
| `link_down` | MCU link down, or config not yet re-sent (D-R30) |
| `no_endstop` | `home` on an axis without an endstop, in the INI or in the firmware (D-R44) |
| `not_impl` | Command defined but not implemented yet |

## Commands

Arguments are listed in braces as JSON keys. Results are returned as keys in the reply.

- `D-R14` Every device command MUST name its target device in `dev`; there is no default device, for the CLI as well as other clients. More axes and I/O are expected; some may move concurrently without coordination. (`D-G1`)

### Motor device

| Command | Class | Effect | Errors |
|---|---|---|---|
| `rel` {`steps`} | write | Relative move | lease held, busy, link down |
| `abs` {`pos`} | write | Absolute move | lease held, busy, position invalid, link down |
| `stop` | write | Stop the current move | lease held, not moving |
| `speed` / `accel` / `micro` {`value`} | write | Set a parameter (rejected when moving) | lease held, busy, bad parameter |
| `pos` / `status` | read | Report state | n/a |
| `zero` | write | Set the current position to 0 | lease held |
| `home` | write | Run the homing sequence | lease held, no endstop, not implemented yet |
| `lock` / `unlock` | write | Enable or disable the driver output | lease held |

### Load sensor device

| Command | Class | Effect | Errors |
|---|---|---|---|
| `weigh` | write | Query the sensor (non-atomic bit-banged exchange, hence `write`). Returns `mg` and `raw` | lease held |
| `offset` {`value`} | write | Set sensor reading offset, in raw counts | lease held, bad parameter |
| `tare` | write | Read the sensor and set the offset to the current raw reading | lease held |
| `status` | read | Report state | n/a |

### Daemon

Commands without `dev`.

| Command | Class | Effect | Errors |
|---|---|---|---|
| `lease` / `renew` / `release` | session | Acquire, keep or give up the lease | lease held by another client |
| `watch` / `unwatch` | read | Subscribe to or unsubscribe from `status` events (D-R21) | n/a |
| `estop` | emergency | Stop everything now (F-R24) | n/a |

Client-local commands such as `help` and `quit` are not part of the protocol.

## State reporting

- `D-R20` Reported position, `moving` and fault state MUST come from firmware reports, never daemon predictions. (`D-G2`)
- `D-R21` Daemon MUST push a `status` event to clients subscribed with `watch` when any reported field changes, like current `status_watch_t`. (`D-G2`)
- `D-R22` Every state report MUST be internally consistent, from a single snapshot. (`D-G2`)

## Link failure

- `D-R30` On link drop or firmware reset (USB re-enumeration, or a report with `settings_applied` false, F-R33), daemon MUST mark position invalid, report link-down to all clients, and reject motion until reconnected and config re-sent. A request the MCU doesn't answer within a timeout (default 500 ms, configurable) counts as a link drop; the daemon does not retransmit. (`D-G3`, `D-G4`)
- `D-R31` Daemon MUST reconnect automatically. Backoff: exponential, starting at 250 ms and doubling up to a 2 s cap, retrying forever; it resets after a successful handshake (F-R33). A failure is logged once per change of state, not on every retry. (`D-G4`)
