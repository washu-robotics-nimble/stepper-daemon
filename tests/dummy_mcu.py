#!/usr/bin/env python3
"""Dummy MCU: speaks the host link on a pty, for testing nimhubd without the
Pico. Standard library only.

Usage:
    dummy_mcu.py [--link PATH] [--objects LIST] [options]

It implements the firmware's message catalogue (docs/firmware/spec.md) with
the same checks and error codes: the global commands, and for steppers
set_motion, move, move_to, stop, set_enable, set_position and get_state.
Moves take the time of their trapezoid (or triangle) profile, with
move_start, position and move_end events; stop and link loss end on the next
full step, estop at once. Link loss (F-R22) is the port closed (the pty's
other end closed, standing in for DTR) or the timeout_ms expiring;
link_recovered follows when the link returns.

Output, one line each, after the time in seconds since start:
    pty PATH        first line: the pty to open (or the --link path)
    < FRAME         a line received
    > FRAME         a line sent
    * NOTE          port opened / closed, link lost / recovered, and the like

Options for tests:
    --no-reply CMD  drop every frame with this cmd: no reply, no rx_error, as
                    if it were lost (D-R30). Repeatable.
    --busy-ms N     set_motion fails with busy for N ms after each port open,
                    as for an axis still finishing a move
    --pos-valid     steppers start with a valid position, as after homing
    --proto N, --fw VERSION   what identify reports

Example:
    dummy_mcu.py --link /tmp/mcu --objects m0:stepper,m1:stepper
"""

import argparse
import json
import math
import os
import select
import signal
import sys
import time
import tty

CLOCK_HZ = 125_000_000      # the RP2040's system clock (firmware impl)
LINE_MAX = 1024             # including the '\n' (F-R31)
SINGLE_OVERHEAD = 6         # stepgen.h: low time of a single word
MAX_PULSES = 1 << 31        # STEPGEN_MAX_REPEAT
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1

# The board table's defaults (F-R34)
DEFAULT_MOTION = {
    "microsteps": 1,
    "speed": 200,
    "accel": 400,
    "pulse_width_us": 20,
    "dir_polarity": "H",
    "soft_limit_min": None,
    "soft_limit_max": None,
}


class Error(Exception):
    """A command's error reply"""


def get_int32(req, key, lo=INT32_MIN, hi=INT32_MAX):
    """An integer argument, as the firmware reads it (any integral number)"""
    v = req.get(key)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise Error("bad_param")
    if v != math.floor(v) or not lo <= v <= hi:
        raise Error("bad_param")
    return int(v)


def timing_ok(m):
    """The firmware's get_timing(): could any move run with motion m (F-R2)"""
    rate = m["speed"] * m["microsteps"]
    accel = m["accel"] * m["microsteps"]
    if accel > 0xFFFFFFFF:
        return False
    period = (CLOCK_HZ + rate // 2) // rate
    high = m["pulse_width_us"] * CLOCK_HZ // 1_000_000
    one_us = CLOCK_HZ // 1_000_000
    if high < one_us or period < high + one_us:
        return False
    return high + SINGLE_OVERHEAD <= period < (1 << 30)


class Profile:
    """Pulses emitted over time by a run of n pulses from rest: trapezoid, or
    triangle when too short to reach the rate"""

    def __init__(self, n, rate, accel):
        self.n, self.a = n, accel
        ramp = rate * rate / (2 * accel)
        if 2 * ramp >= n:
            self.t_ramp = math.sqrt(n / accel)
            self.n_ramp = n / 2
            self.t_cruise = 0.0
            self.v = accel * self.t_ramp
        else:
            self.t_ramp = rate / accel
            self.n_ramp = ramp
            self.t_cruise = (n - 2 * ramp) / rate
            self.v = rate
        self.duration = 2 * self.t_ramp + self.t_cruise

    def emitted(self, t):
        if t >= self.duration:
            return self.n
        if t <= 0:
            return 0
        if t < self.t_ramp:
            x = 0.5 * self.a * t * t
        elif t < self.t_ramp + self.t_cruise:
            x = self.n_ramp + self.v * (t - self.t_ramp)
        else:
            left = self.duration - t
            x = self.n - 0.5 * self.a * left * left
        return min(self.n, int(x))


class Stepper:
    COMMANDS = ("set_motion", "move", "move_to", "stop", "set_enable",
                "set_position", "get_state")

    def __init__(self, mcu, oid):
        self.mcu, self.oid = mcu, oid
        self.m = dict(DEFAULT_MOTION)
        self.pos = 0                # during a move: where it started
        self.pos_valid = False
        self.enabled = False
        self.moving = False
        self.start_pending = False
        self.end_reason = None
        self.target = 0
        self.forward = True
        self.run = None             # (start time, Profile) of the current run
        self.pulses = 0             # pulses of this move's earlier runs
        self.last_report = 0.0

    # ---- motion ----

    def _start_run(self, n, now):
        micro = self.m["microsteps"]
        self.run = (now, Profile(n, self.m["speed"] * micro, self.m["accel"] * micro))

    def _emitted(self, now):
        if self.run is None:
            return 0
        start, prof = self.run
        return prof.emitted(now - start)

    def _pos_after(self, pulses):
        steps = pulses // self.m["microsteps"]
        return self.pos + steps if self.forward else self.pos - steps

    def live_pos(self, now):
        if not self.moving:
            return self.pos
        return self._pos_after(self.pulses + self._emitted(now))

    def halt(self, reason, finish_step, now):
        """Cut the move short; with finish_step, end on the next full step"""
        self.pulses += self._emitted(now)
        self.run = None
        micro = self.m["microsteps"]
        rest = (micro - self.pulses % micro) % micro if finish_step else 0
        if rest > 0:
            self._start_run(rest, now)
        self.target = self._pos_after(self.pulses + rest)
        if self.end_reason is None:
            self.end_reason = reason

    def start_move(self, steps, now):
        if not self.mcu.settings_applied:
            raise Error("not_configured")
        if self.moving:
            raise Error("busy")
        lo, hi = self.m["soft_limit_min"], self.m["soft_limit_max"]
        if (lo is not None or hi is not None) and not self.pos_valid:
            raise Error("pos_invalid")
        target = self.pos + steps
        if not INT32_MIN <= target <= INT32_MAX:
            raise Error("bad_param")
        if (lo is not None and target < lo) or (hi is not None and target > hi):
            raise Error("bad_param")
        self.run = None
        if steps != 0:
            pulses = abs(steps) * self.m["microsteps"]
            if pulses > MAX_PULSES or not timing_ok(self.m):
                raise Error("bad_param")
            self.forward = steps > 0
            self._start_run(pulses, now)
        self.target = target
        self.pulses = 0
        self.end_reason = None
        self.moving = True
        self.start_pending = True

    def poll(self, now):
        if self.start_pending:
            self.start_pending = False
            self.last_report = now
            self.mcu.pos_event(self.oid, "move_start", self.pos)
        if not self.moving:
            return
        if self.run is None or now - self.run[0] >= self.run[1].duration:
            self.pos = self.target
            self.moving = False
            self.run = None
            self.mcu.pos_event(self.oid, "move_end", self.pos, self.end_reason or "done")
            self.end_reason = None
            return
        period = self.mcu.report_period
        if period and (now - self.last_report) * 1000 >= period:
            self.last_report = now
            self.mcu.pos_event(self.oid, "position", self.live_pos(now))

    # ---- commands ----

    def cmd_set_motion(self, req, now):
        if self.moving:
            raise Error("busy")
        if now < self.mcu.busy_until:
            raise Error("busy")
        m = dict(self.m)
        if "microsteps" in req:
            v = get_int32(req, "microsteps", 1, 16)
            if v & (v - 1):
                raise Error("bad_param")
            m["microsteps"] = v
        for key in ("speed", "accel", "pulse_width_us"):
            if key in req:
                m[key] = get_int32(req, key, 1)
        if "dir_polarity" in req:
            if req["dir_polarity"] not in ("H", "L"):
                raise Error("bad_param")
            m["dir_polarity"] = req["dir_polarity"]
        for key in ("soft_limit_min", "soft_limit_max"):
            if key in req:
                m[key] = None if req[key] is None else get_int32(req, key)
        lo, hi = m["soft_limit_min"], m["soft_limit_max"]
        if lo is not None and hi is not None and lo > hi:
            raise Error("bad_param")
        if not timing_ok(m):
            raise Error("bad_param")
        self.m = m
        return {}

    def cmd_move(self, req, now):
        self.start_move(get_int32(req, "steps"), now)
        return {}

    def cmd_move_to(self, req, now):
        self.start_move(get_int32(req, "pos") - self.pos, now)
        return {}

    def cmd_stop(self, req, now):
        if not self.moving or self.end_reason is not None:
            raise Error("not_moving")
        self.halt("stop", True, now)
        return {"pos": self.target}

    def cmd_set_enable(self, req, now):
        v = get_int32(req, "value")
        if v not in (0, 1):
            raise Error("bad_param")
        self.enabled = v == 1
        if v == 0:
            self.pos_valid = False
        return {}

    def cmd_set_position(self, req, now):
        if self.moving:
            raise Error("busy")
        self.pos = get_int32(req, "pos")
        self.pos_valid = True
        return {}

    def cmd_get_state(self, req, now):
        return {"pos": self.live_pos(now), "pos_valid": self.pos_valid,
                "moving": self.moving, "enabled": self.enabled,
                "settings_applied": self.mcu.settings_applied}


class Object:
    """An object of a type the dummy doesn't implement: listed, no commands"""
    COMMANDS = ()

    def __init__(self, mcu, oid):
        self.oid = oid

    def poll(self, now):
        pass


class DummyMCU:
    GLOBAL = ("identify", "list_objects", "settings_done", "set_link", "ping", "estop")

    def __init__(self, master, objects, args):
        self.master = master
        self.args = args
        self.t0 = time.monotonic()
        self.objects = {}           # oid -> (type, object), in board order
        for oid, typ in objects:
            obj = Stepper(self, oid) if typ == "stepper" else Object(self, oid)
            if typ == "stepper":
                obj.pos_valid = args.pos_valid
            self.objects[oid] = (typ, obj)
        self.settings_applied = False
        self.report_period = 250
        self.timeout_ms = 0
        self.connected = False
        self.lost = None            # None, or why: "closed" / "timeout"
        self.last_frame = 0.0
        self.busy_until = 0.0
        self.rx = b""
        self.rx_too_long = False

    # ---- output ----

    def log(self, mark, text):
        print(f"{time.monotonic() - self.t0:9.3f} {mark} {text}", flush=True)

    def send(self, msg):
        line = json.dumps(msg, separators=(",", ":"))
        if not self.connected:
            self.log("*", f"not sent, port closed: {line}")
            return
        self.log(">", line)
        try:
            os.write(self.master, line.encode() + b"\n")
        except OSError as e:
            self.log("*", f"write failed: {e}")

    def pos_event(self, oid, evt, pos, reason=None):
        msg = {"evt": evt, "oid": oid, "pos": pos}
        if reason is not None:
            msg["reason"] = reason
        msg["settings_applied"] = self.settings_applied
        self.send(msg)

    # ---- link loss (F-R22) ----

    def lose(self, why, now):
        self.lost = why
        self.log("*", f"link lost ({why})")
        for typ, obj in self.objects.values():
            if typ == "stepper" and obj.moving and obj.end_reason is None:
                obj.halt("link_lost", True, now)

    def recover(self):
        self.lost = None
        self.log("*", "link recovered")
        self.send({"evt": "link_recovered"})

    def set_connected(self, up, now):
        if up == self.connected:
            return
        self.connected = up
        self.rx = b""               # the partial line, on both edges
        self.rx_too_long = False
        if up:
            self.log("*", "port opened")
            self.last_frame = now
            self.busy_until = now + self.args.busy_ms / 1000
            if self.lost is not None:
                self.recover()
        else:
            self.log("*", "port closed")
            if self.lost is None:
                self.lose("closed", now)

    # ---- frames ----

    def rx_error(self, err):
        self.send({"evt": "rx_error", "err": err})

    def handle_line(self, raw, now):
        self.log("<", raw.decode(errors="replace"))
        if b"\0" in raw:
            self.rx_error("bad_json")
            return
        try:
            req = json.loads(raw)
        except ValueError:
            req = None
        if not isinstance(req, dict):
            self.rx_error("bad_json")
            return
        seq = req.get("seq")
        if (isinstance(seq, bool) or not isinstance(seq, (int, float))
                or seq != math.floor(seq) or not 0 <= seq <= 0xFFFFFFFF):
            self.rx_error("no_seq")
            return
        seq = int(seq)
        cmd = req.get("cmd")
        if cmd in self.args.no_reply:
            self.log("*", f"no reply to {cmd} (--no-reply)")
            return

        # A frame that gets a reply keeps the link alive (F-R22)
        self.last_frame = now
        if self.lost == "timeout":
            self.recover()

        try:
            result = self.dispatch(req, cmd, now)
            self.send({"seq": seq, "ack": True, **result})
        except Error as e:
            self.send({"seq": seq, "ack": False, "err": str(e)})

    def dispatch(self, req, cmd, now):
        if isinstance(cmd, str) and cmd in self.GLOBAL:
            return getattr(self, "cmd_" + cmd)(req, now)
        if "oid" not in req:
            raise Error("unknown_cmd")
        oid = req["oid"]
        if not isinstance(oid, str):
            raise Error("bad_param")
        if oid not in self.objects:
            raise Error("unknown_oid")
        typ, obj = self.objects[oid]
        if not isinstance(cmd, str) or cmd not in obj.COMMANDS:
            raise Error("unknown_cmd")
        return getattr(obj, "cmd_" + cmd)(req, now)

    # ---- global commands ----

    def cmd_identify(self, req, now):
        return {"fw": self.args.fw, "proto": self.args.proto,
                "settings_applied": self.settings_applied}

    def cmd_list_objects(self, req, now):
        types = {}
        for typ, obj in self.objects.values():
            types[typ] = list(obj.COMMANDS)
        return {"objects": [{"oid": oid, "type": typ}
                            for oid, (typ, obj) in self.objects.items()],
                "types": types}

    def cmd_settings_done(self, req, now):
        self.settings_applied = True
        return {}

    def cmd_set_link(self, req, now):
        period = get_int32(req, "report_period", 0) if "report_period" in req else self.report_period
        timeout = get_int32(req, "timeout_ms", 0) if "timeout_ms" in req else self.timeout_ms
        self.report_period, self.timeout_ms = period, timeout
        return {}

    def cmd_ping(self, req, now):
        return {}

    def cmd_estop(self, req, now):
        for typ, obj in self.objects.values():
            if typ != "stepper":
                continue
            if obj.moving:
                obj.halt("estop", False, now)
            obj.pos_valid = False
            obj.enabled = False
        return {}

    # ---- main loop ----

    def receive(self, data, now):
        self.rx += data
        while True:
            line, nl, rest = self.rx.partition(b"\n")
            if not nl:
                break
            self.rx = rest
            if self.rx_too_long or len(line) + 1 > LINE_MAX:
                self.log("<", f"({len(line)} bytes, over-length)")
                self.rx_too_long = False
                self.rx_error("too_long")
            else:
                self.handle_line(line, now)
            self.poll(now)
        if len(self.rx) + 1 > LINE_MAX:
            self.rx = b""
            self.rx_too_long = True

    def poll(self, now):
        if (self.connected and self.lost is None and self.timeout_ms
                and (now - self.last_frame) * 1000 > self.timeout_ms):
            self.lose("timeout", now)
        for typ, obj in self.objects.values():
            obj.poll(now)

    def run(self):
        poller = select.poll()
        poller.register(self.master, select.POLLIN)
        while True:
            events = poller.poll(10)
            now = time.monotonic()
            hup = any(ev & select.POLLHUP for _, ev in events)
            data = b""
            if any(ev & select.POLLIN for _, ev in events):
                try:
                    data = os.read(self.master, 4096)
                except OSError:     # EIO: no one has the pty open
                    hup = True
            if data:
                self.set_connected(True, now)
                self.receive(data, now)
            elif not hup:
                self.set_connected(True, now)
            if hup:
                self.set_connected(False, now)
                time.sleep(0.01)    # poll() returns at once while closed
            self.poll(now)


def parse_objects(text):
    objects = []
    for item in text.split(","):
        oid, _, typ = item.partition(":")
        objects.append((oid, typ or "stepper"))
    return objects


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--link", help="also make PATH a symlink to the pty")
    parser.add_argument("--objects", default="m0:stepper",
                        help="board table, as oid:type,... (default: m0:stepper)")
    parser.add_argument("--fw", default="0.1.0-dummy")
    parser.add_argument("--proto", type=int, default=1)
    parser.add_argument("--no-reply", action="append", default=[], metavar="CMD")
    parser.add_argument("--busy-ms", type=int, default=0, metavar="N")
    parser.add_argument("--pos-valid", action="store_true")
    args = parser.parse_args()

    master, slave = os.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)
    os.close(slave)     # the pty is "closed" until the daemon opens it
    if args.link:
        if os.path.islink(args.link):
            os.unlink(args.link)
        os.symlink(path, args.link)
    print(f"pty {args.link or path}", flush=True)

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        DummyMCU(master, parse_objects(args.objects), args).run()
    finally:
        if args.link and os.path.islink(args.link):
            os.unlink(args.link)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
