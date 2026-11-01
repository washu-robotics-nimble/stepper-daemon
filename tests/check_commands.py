#!/usr/bin/env python3
"""Check the firmware's commands and replies on the board, over USB (build
plan step 6). Standard library only.

Usage:
    check_commands.py DEVICE [OID]

Runs the handshake (identify, list_objects, settings, settings_done), then
every stepper command with its success and error cases, including moves of
a few seconds. Prints each check and a summary; exits non-zero if any check
failed. It ends with the board defaults restored, except that settings_done
stays sent, the driver is disabled and the position is invalid (after the
estop check).

The not_configured checks need a freshly reset board; on a board that has
seen settings_done already, they are skipped.
"""

import json
import sys
import time

from hostlink import Link, open_port

REPLY_TIMEOUT_S = 2.0


class Board:
    def __init__(self, path, oid):
        self.link = Link(open_port(path), echo=False)
        self.oid = oid
        self.events = []    # not yet taken by wait_event()
        self.history = []   # every event received

    def _receive(self, timeout):
        for msg in self.link.receive(timeout):
            if "evt" in msg:
                self.events.append(msg)
                self.history.append(msg)
            else:
                return msg
        return None

    def call(self, cmd, obj=True, **fields):
        """Send one command; return its reply. Events seen meanwhile are kept."""
        frame = {"cmd": cmd, **({"oid": self.oid} if obj else {}), **fields}
        seq = self.link.send(json.dumps(frame, separators=(",", ":")))
        deadline = time.monotonic() + REPLY_TIMEOUT_S
        while time.monotonic() < deadline:
            msg = self._receive(deadline - time.monotonic())
            if msg is not None and msg.get("seq") == seq:
                return msg
        raise SystemExit(f"no reply to {frame}")

    def wait_event(self, evt, timeout):
        """Return the first kept or new event named evt, dropping older ones."""
        deadline = time.monotonic() + timeout
        while True:
            for i, e in enumerate(self.events):
                if e["evt"] == evt:
                    del self.events[: i + 1]
                    return e
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self._receive(min(left, 0.1))

    def drain(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self._receive(end - time.monotonic())


failures = 0


def check(name, cond, detail=""):
    global failures
    print(f"{'ok  ' if cond else 'FAIL'} {name}" + (f"  ({detail})" if detail and not cond else ""),
          flush=True)
    if not cond:
        failures += 1


def acked(r):
    return r.get("ack") is True


def err(r, code):
    return r.get("ack") is False and r.get("err") == code


def run_move(b, name, expect_pos, timeout=15, **move):
    cmd = "move_to" if "pos" in move else "move"
    r = b.call(cmd, **move)
    check(f"{name}: acked", acked(r), r)
    start = b.wait_event("move_start", 2)
    check(f"{name}: move_start", start is not None and "settings_applied" in start, start)
    end = b.wait_event("move_end", timeout)
    check(f"{name}: move_end at {expect_pos}",
          end is not None and end["pos"] == expect_pos and end["reason"] == "done", end)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    b = Board(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else "m0")

    print("-- handshake")
    r = b.call("identify", obj=False)
    check("identify: fw, proto 1, settings_applied",
          acked(r) and isinstance(r.get("fw"), str) and r.get("proto") == 1
          and isinstance(r.get("settings_applied"), bool), r)
    fresh = r.get("settings_applied") is False

    r = b.call("list_objects", obj=False)
    commands = {"set_motion", "move", "move_to", "stop", "set_enable", "set_position", "get_state"}
    check("list_objects: the stepper and its commands",
          acked(r) and {"oid": b.oid, "type": "stepper"} in r.get("objects", [])
          and commands <= set(r.get("types", {}).get("stepper", [])), r)

    if fresh:
        check("move before settings_done: not_configured",
              err(b.call("move", steps=10), "not_configured"))
        check("move_to before settings_done: not_configured",
              err(b.call("move_to", pos=10), "not_configured"))
    else:
        print("skip not_configured checks (settings_done already sent since reset)")

    for fields in ({"microsteps": 3}, {"speed": 0}, {"accel": 0}, {"pulse_width_us": 0},
                   {"dir_polarity": "X"}, {"speed": 100000, "microsteps": 16},
                   {"microsteps": 1, "speed": -1}):
        check(f"set_motion {fields}: bad_param", err(b.call("set_motion", **fields), "bad_param"))
    check("set_motion soft limit: not_impl",
          err(b.call("set_motion", soft_limit_max=100), "not_impl"))
    check("set_motion, all fields", acked(b.call(
        "set_motion", microsteps=1, speed=200, accel=400, pulse_width_us=20, dir_polarity="H")))
    check("set_link report_period -1: bad_param",
          err(b.call("set_link", obj=False, report_period=-1), "bad_param"))
    check("set_link report_period 100", acked(b.call("set_link", obj=False, report_period=100)))
    check("settings_done", acked(b.call("settings_done", obj=False)))
    check("identify: settings_applied", b.call("identify", obj=False).get("settings_applied") is True)

    print("-- position and moves")
    check("set_position 0", acked(b.call("set_position", pos=0)))
    r = b.call("get_state")
    check("get_state: pos 0, valid, idle",
          acked(r) and r.get("pos") == 0 and r.get("pos_valid") is True
          and r.get("moving") is False and r.get("settings_applied") is True, r)

    b.history.clear()
    r = b.call("move", steps=400)
    check("move 400: acked", acked(r), r)
    check("move_start at 0", (b.wait_event("move_start", 2) or {}).get("pos") == 0)
    check("move while moving: busy", err(b.call("move", steps=1), "busy"))
    check("set_motion while moving: busy", err(b.call("set_motion", speed=100), "busy"))
    check("set_position while moving: busy", err(b.call("set_position", pos=5), "busy"))
    r = b.call("get_state")
    check("get_state while moving: moving, 0 < pos < 400",
          r.get("moving") is True and 0 < r.get("pos", -1) < 400, r)
    end = b.wait_event("move_end", 10)
    positions = [e["pos"] for e in b.history if e["evt"] == "position"]
    check("move_end at 400", end is not None and end["pos"] == 400 and end["reason"] == "done", end)
    # 2.45 s at a 100 ms report period: about 24; the poll runs every loop
    check("position events: about every 100 ms, increasing",
          15 <= len(positions) <= 30 and positions == sorted(positions),
          f"{len(positions)} events: {positions}")
    b.events.clear()

    run_move(b, "move_to 0", 0, pos=0)
    run_move(b, "move 0", 0, steps=0)

    print("-- stop")
    check("stop while idle: not_moving", err(b.call("stop"), "not_moving"))
    b.call("move", steps=2000)
    b.wait_event("move_start", 2)
    b.drain(1.0)
    r = b.call("stop")
    check("stop: acked with pos", acked(r) and isinstance(r.get("pos"), int), r)
    end = b.wait_event("move_end", 2)
    check("move_end: reason stop, pos as replied",
          end is not None and end["reason"] == "stop" and end["pos"] == r.get("pos"), end)
    check("get_state: the same pos", b.call("get_state").get("pos") == r.get("pos"))
    run_move(b, "move_to 0 after stop", 0, pos=0)

    print("-- microsteps")
    check("set_motion microsteps 16", acked(b.call("set_motion", microsteps=16)))
    run_move(b, "move 10 at 1/16", 10, steps=10)
    b.call("move", steps=100)
    b.wait_event("move_start", 2)
    b.drain(0.4)
    r = b.call("stop")
    end = b.wait_event("move_end", 2)
    check("stop at 1/16: ends on a full step, as replied",
          acked(r) and end is not None and end["pos"] == r.get("pos"), (r, end))
    run_move(b, "move_to 0 at 1/16", 0, pos=0)
    check("set_motion microsteps 1", acked(b.call("set_motion", microsteps=1)))

    print("-- enable and estop")
    check("set_enable 2: bad_param", err(b.call("set_enable", value=2), "bad_param"))
    check("set_enable 1", acked(b.call("set_enable", value=1)))
    r = b.call("get_state")
    check("get_state: enabled, position still valid",
          r.get("enabled") is True and r.get("pos_valid") is True, r)
    check("set_enable 0", acked(b.call("set_enable", value=0)))
    r = b.call("get_state")
    check("get_state: disabled, position invalid",
          r.get("enabled") is False and r.get("pos_valid") is False, r)
    check("set_position 0, set_enable 1",
          acked(b.call("set_position", pos=0)) and acked(b.call("set_enable", value=1)))
    b.call("move", steps=2000)
    b.wait_event("move_start", 2)
    b.drain(0.5)
    check("estop", acked(b.call("estop", obj=False)))
    end = b.wait_event("move_end", 2)
    check("move_end: reason estop", end is not None and end["reason"] == "estop", end)
    r = b.call("get_state")
    check("get_state: disabled, position invalid",
          r.get("enabled") is False and r.get("pos_valid") is False, r)
    run_move(b, "move after estop (not latched)", r.get("pos", 0) + 1, steps=1)

    print("-- errors")
    check("unknown oid", err(b.call("get_state", obj=False, oid="zz9"), "unknown_oid"))
    check("unknown command", err(b.call("fly"), "unknown_cmd"))
    check("move steps 1.5: bad_param", err(b.call("move", steps=1.5), "bad_param"))
    check("set_position pos \"0\": bad_param", err(b.call("set_position", pos="0"), "bad_param"))

    check("set_link report_period 250 (default)",
          acked(b.call("set_link", obj=False, report_period=250)))

    print(f"-- {'all checks passed' if failures == 0 else f'{failures} check(s) FAILED'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
