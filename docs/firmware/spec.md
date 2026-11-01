# firmware: spec

Design specification to satisfy the goals in [intent.md](intent.md).

## Units

Same as the daemon: positions in full steps (signed 32-bit), speeds in full steps/s, acceleration in full steps/s^2. Timing in µs.

## Step generation

- `F-R1` For each axis, the number of STEP pulses emitted MUST equal the number commanded, unless the move is stopped. When a move is stopped, the number actually emitted MUST be known exactly and reported. (F-G1, F-G4)
- `F-R2` STEP high time MUST be at least the configured `pulse_width_us` and never below the A4988 minimum of 1 µs. Low time MUST be at least 1 µs (A4988 minimum). (`F-G1`)
- `F-R3` Step-period jitter MUST stay within 1 µs, and MUST NOT depend on host link traffic. (F-G1)
- `F-R4` DIR MUST be set at least 200 ns before the first STEP edge of a move (A4988 setup time), and MUST NOT change until at least 200 ns after the last STEP edge (A4988 hold time). (F-G1)
- `F-R5` The firmware MUST compute acceleration profiles itself from the target, speed and acceleration it is given. The host MUST NOT need to send per-step timing. Profiles MUST be trapezoidal, or triangular when the move is too short to reach full speed (`F-G1`, `F-G3`)
- `F-R6` Several outputs MUST be able to run at the same time, each independently. Synchronised movement is not required. (`F-G2`)
- `F-R7` The firmware MUST offer plain PWM outputs set by frequency and duty cycle, in addition to ramped step outputs. Frequency and resolution ranges: To the extent supported by RP2040 (max at clock speed). (F-G2)

## Driver control

- `F-R10` The firmware MUST drive each A4988's MS1–MS3, ENABLE, SLEEP and RESET lines on host command. Microstep changes MUST be rejected while that axis is moving. (F-G6)

## Inputs and safety

- `F-R20` A triggered endstop MUST stop the affected axis within 2 steps, without host involvement. One endstop is needed first. Homing sequence is triggered by the host and autonomously executed by the firmware. (`F-G3`)
  - If the endstop has not triggered within the axis's maximum homing travel (fixed in the board table, F-R34), the axis MUST stop, send a `fault` event with code `home_timeout`, and leave position invalid.
  - `home` on an axis with no endstop in the board table MUST be rejected with `no_endstop`.
- `F-R21` `stop` MUST cut step signal output within 2 steps of being received. (`F-G3`)
- `F-R22` The firmware MUST treat the host link as lost when the USB device is disconnected or suspended, when the host closes the port (DTR drop), or when no valid message arrives for a configurable period (default: infinity; the host keeps the link alive with `ping`). On link loss it MUST stop all motion, leave the drivers' ENABLE state unchanged, and report the loss once the link returns (`link_recovered` event). (`F-G5`)
- `F-R23` On boot or reset, all driver outputs MUST be in a safe state (STEP low; ENABLE high, i.e. driver disabled) until configured. (F-G5)
  - Hardware note: before the firmware starts, RP2040 GPIOs are inputs with pull-downs, which would enable an A4988. Boards MUST fit an external pull-up on ENABLE.
- `F-R24` Emergency stop MUST halt every output, and MUST be handled ahead of any host messages already queued. The drivers should also be set to disabled status. (`F-G3`, `D-R17`)
- `F-R25` A move whose target lies outside the axis's soft limits (`soft_limit_min`, `soft_limit_max`, set by the host) MUST be rejected with `bad_param` and not started. (`F-G3`)
  - A bound that the host has not set means no limit on that side. `0` is a real limit, because 0 is the natural lower bound after homing. (Previously `0` meant "unlimited", which made that bound impossible to express.)
  - While any limit is set and position is invalid, `move` and `move_to` MUST be rejected with `pos_invalid`, since the limits can't be checked. `home` is still allowed, because homing is how position becomes valid.

## Other I/O (planned)

- `F-R40` ADC inputs, read on host request or reported periodically. Channels and rate: deferred until the first ADC use case is defined; not in the message catalogue yet. (`F-G4`)
- `F-R41` Servo outputs (PWM), with configurable duty cycle. (`F-G2`)
- `F-R42` I2C and SPI devices, accessed on behalf of the host, with device address and payload. Supports read and write. (`F-G4`)
- `F-R43` Support reading HX711 load cell ADC using bit-banged interface, simultaneously with other active tasks. Readings are reported as raw counts; conversion to mass is done by the daemon. (`F-G7`)

## Host link

- `F-R30` The MCU uses its native USB port to directly connect to the Linux host. USB device class: CDC serial.
- `F-R31` Framing: newline-delimited JSON, UTF-8, one object per line terminated by `\n`. Lines are capped at 1024 bytes. Requirements:
  - integrity relies entirely on the USB layer's CRC; frames carry no checksum of their own;
  - a malformed frame (over-length or unparseable) is dropped and never acted on; the receiver discards any partial line on startup or reconnect;
  - every request carries a `seq` (uint32, incremented by the host, reset to 0 after each handshake) that its response echoes;
  - every request gets an explicit response (success, or failure with an error code), even when it carries no data;
  - reports the MCU sends on its own (events) carry no `seq`;
  - receivers ignore unknown fields, so new optional fields never break older peers;
  - the host does not retransmit; a missing response is handled as a link drop (D-R30).

  Envelope:

  | Field | Direction | Meaning |
  |---|---|---|
  | `seq` | both | Request sequence number; the response echoes it |
  | `cmd` | host -> MCU | Command name |
  | `oid` | both | Target object, for object commands and events |
  | `ack` | MCU -> host | `true` on success, `false` on failure |
  | `err` | MCU -> host | Error code when `ack` is `false` |
  | `evt` | MCU -> host | Event name |
  | *(other)* | both | Command-specific arguments and results as flat key/value pairs |

  ```
  {"seq":12,"cmd":"move","oid":"m0","steps":400}
  {"seq":12,"ack":true}
  {"seq":13,"ack":false,"err":"busy"}
  {"evt":"move_end","oid":"m0","pos":400,"reason":"done"}
  ```
- `F-R32` The firmware MUST send a state report when a move starts, finishes or is stopped, when a switch changes, and on fault. It SHOULD also send periodic position updates during a move, at a configurable rate. Default: 4 Hz. Reports carry no MCU timestamp; the host timestamps them on receipt. (`F-G4`)
- `F-R33` The first exchange MUST report the firmware and protocol versions, and a `settings_applied` flag. The flag is false after boot, and becomes true once the host signals that it has sent all settings; state reports also carry it. The daemon refuses a protocol it doesn't support.
- `F-R34` All behavioural settings MUST be settable over the link, per object (`F-G6`). The hardware layout (which objects exist, their pins and driver wiring) is fixed at build time and MUST NOT be settable over the link. Every object MUST have safe built-in default settings, which apply until (and unless) the host overrides them. Homing direction, homing speed and maximum homing travel are fixed in the firmware too, so a host can never send a motor into its hard end on the side without an endstop.
- `F-R35` On request, the firmware MUST report every object it has, each with its object ID (`oid`), its type, and the commands it supports. The firmware assigns the `oid`s. (`F-G6`)
  - An `oid` is a short string from the firmware's board table (e.g. `"m0"`, `"lc0"`).
  - `oid`s are part of the external interface (clients and scripts use them, D-R43). Once released, an `oid` MUST NOT be renamed, or reused for a different object.

## Message catalogue

Envelope and framing as in F-R31. Replies are `{"seq":n,"ack":true,...result fields}`, or `{"seq":n,"ack":false,"err":...}`.

Object types (from the firmware's board table): `stepper`, `endstop`, `hx711`; later `pwm` / `servo` (F-R7, F-R41).

### Global commands

| `cmd` | Args | Result | Notes |
|---|---|---|---|
| `identify` | - | `fw`, `proto`, `settings_applied` | F-R33; always the first exchange |
| `list_objects` | - | `objects` (list of `{oid, type}`), `types` (map: type → list of commands) | F-R35 |
| `settings_done` | - | - | Sets `settings_applied` (F-R33, D-R3) |
| `set_link` | `report_hz`, `timeout_ms` | - | F-R32 rate (default 4), F-R22 timeout (0 = infinity) |
| `ping` | - | - | Keeps the F-R22 timeout alive |
| `estop` | - | - | F-R24; handled before the queue |

### Stepper (`type: stepper`)

| `cmd` | Args | Result | Errors |
|---|---|---|---|
| `set_motion` | `oid`, and any of `microsteps`, `speed`, `accel`, `pulse_width_us`, `dir_polarity`, `soft_limit_min`, `soft_limit_max` | - | `busy` (moving), `bad_param` |
| `move` | `oid`, `steps` (relative) | - | `busy`, `bad_param` (limits, F-R25), `pos_invalid`, `not_configured` |
| `move_to` | `oid`, `pos` (absolute) | - | same as `move` |
| `stop` | `oid` | `pos` | `not_moving` |
| `set_enable` | `oid`, `value` (0/1) | - | - |
| `set_position` | `oid`, `pos` | - | `busy` |
| `home` | `oid` | - | `busy`, `no_endstop`, `not_impl`. Direction, speed, maximum travel and endstop are fixed in the board table (F-R20, F-R34) |
| `get_state` | `oid` | `pos`, `pos_valid`, `moving`, `enabled`, `settings_applied` | - |

`not_configured`: motion before `settings_done`.

### Endstop (`type: endstop`)

| `cmd` | Args | Result |
|---|---|---|
| `get_state` | `oid` | `state` (0/1) |

### HX711 (`type: hx711`)

| `cmd` | Args | Result | Errors |
|---|---|---|---|
| `set_hx711` | `oid`, `gain` (128 / 64 / 32) | - | `bad_param` |
| `read` | `oid` | `raw` | `busy` (read in progress) |

`scale`, `offset` and `tare` stay in the daemon; the firmware only reports raw counts (F-R43).

### Events (F-R32)

| `evt` | Fields |
|---|---|
| `move_start` | `oid`, `pos` |
| `move_end` | `oid`, `pos`, `reason` (`done` / `stop` / `endstop` / `estop` / `link_lost`) |
| `position` | `oid`, `pos` (at `report_hz` while moving) |
| `endstop` | `oid`, `state` |
| `fault` | `oid` (optional), `code` (e.g. `home_timeout`) |
| `link_recovered` | - Sent once when the link returns after a loss; motion was stopped (F-R22) |
| `rx_error` | `err` (`too_long` / `bad_json` / `no_seq`). Sent for a dropped frame, since its `seq` is unknown; the host's pending request then times out (D-R30). `no_seq`: valid JSON, but `seq` is missing or not an integer in uint32 range |

### Error codes

`unknown_cmd`, `unknown_oid`, `bad_param`, `busy`, `not_moving`, `not_configured`, `pos_invalid`, `no_endstop`, `not_impl`.