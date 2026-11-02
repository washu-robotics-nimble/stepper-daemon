#!/usr/bin/env python3
"""End-to-end checks of nimhubd against tests/dummy_mcu.py, without the Pico
(build plan steps 8 and 9). Standard library only.

Usage:
    check_daemon.py [NIMHUBD]       (default: build/nimhubd; run by
                                     `make check-daemon`)

Checks the handshake and the settings sent, keepalive pings, exclusive use of
the device (D-R1), and the cases where nimhubd must give up: a bad INI, an
unsupported protocol, settings the MCU rejects, a missing reply (D-R30), and
the MCU going away. Then the client socket (D-R10, D-R12, D-R40, D-R41):
every command and error, bad requests, several clients. Prints PASS / FAIL
per check; the exit status is the number of failures.
"""

import fcntl
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DUMMY = os.path.join(HERE, "dummy_mcu.py")

failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail and not ok else ""),
          flush=True)
    if not ok:
        failures += 1


class Proc:
    """A child process; a thread collects its output lines (stdout and stderr
    together)"""

    def __init__(self, args):
        self.args = args
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1)
        self.lines = []
        self.cond = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()

    def wait_for(self, text, timeout=3.0):
        """The first line containing text, waiting up to timeout s; or None"""
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for line in self.lines:
                    if text in line:
                        return line
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)

    def has(self, text):
        with self.cond:
            return any(text in line for line in self.lines)

    def exit_code(self, timeout=3.0):
        """Its exit status, or None if it is still running after timeout s"""
        try:
            return self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            try:
                self.p.wait(3)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        return self.p.returncode

    def dump(self):
        with self.cond:
            return "\n".join("    | " + line for line in self.lines)


class Dummy(Proc):
    def __init__(self, tty, *args):
        super().__init__([sys.executable, DUMMY, "--link", tty, *args])
        if self.wait_for("pty ") is None:
            raise RuntimeError("dummy_mcu.py didn't start:\n" + self.dump())

    def received(self):
        """The frames received, parsed, in order"""
        frames = []
        with self.cond:
            for line in self.lines:
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[1] == "<":
                    try:
                        frames.append(json.loads(parts[2]))
                    except ValueError:
                        pass
        return frames


INI = """\
[daemon]
device = {tty}
socket = {socket}
{daemon}

[link]
report_period = 100
timeout_ms = 400
reply_timeout_ms = 500

[m0]
{m0}

[m9]
speed = 100
"""

M0 = """\
microsteps = 4
speed = 300
accel = 1000
pulse_width_us = 10
dir_polarity = L
endstop = true
soft_limit_max = 5000
"""


class Bench:
    def __init__(self, nimhubd):
        self.nimhubd = os.path.abspath(nimhubd)
        self.tmp = tempfile.TemporaryDirectory(prefix="check_daemon.")
        self.tty = os.path.join(self.tmp.name, "mcu")
        self.socket = os.path.join(self.tmp.name, "control.sock")
        self.n = 0

    def ini(self, m0=M0, tty=None, socket=None, daemon=""):
        self.n += 1
        path = os.path.join(self.tmp.name, f"test{self.n}.ini")
        with open(path, "w") as f:
            f.write(INI.format(tty=tty or self.tty, socket=socket or self.socket, m0=m0,
                               daemon=daemon))
        return path

    def daemon(self, ini, debug=True):
        return Proc([self.nimhubd] + (["-d"] if debug else []) + [ini])


def handshake_checks(b):
    dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
    d = b.daemon(b.ini())
    try:
        ok = d.wait_for("handshake: done") is not None
        check("handshake completes", ok, "\n" + d.dump())
        if not ok:
            return

        frames = dummy.received()
        cmds = [f.get("cmd") for f in frames[:5]]
        check("handshake order: identify, list_objects, set_link, set_motion, settings_done",
              cmds == ["identify", "list_objects", "set_link", "set_motion", "settings_done"],
              str(cmds))
        check("seq starts at 1 and counts up", [f.get("seq") for f in frames[:5]] == [1, 2, 3, 4, 5])
        check("set_link carries the [link] settings",
              len(frames) > 2 and frames[2] == {"seq": 3, "cmd": "set_link",
                                                "report_period": 100, "timeout_ms": 400},
              str(frames[2:3]))
        expect = {"seq": 4, "cmd": "set_motion", "oid": "m0", "microsteps": 4, "speed": 300,
                  "accel": 1000, "pulse_width_us": 10, "dir_polarity": "L",
                  "soft_limit_max": 5000, "soft_limit_min": None}
        check("set_motion carries [m0]: no endstop, missing soft limit as null",
              len(frames) > 3 and frames[3] == expect, str(frames[3:4]))
        check("no set_motion for m1 (no INI section)",
              not any(f.get("cmd") == "set_motion" and f.get("oid") == "m1" for f in frames))
        check("log: m1 keeps the firmware defaults", d.has("m1 (stepper): no INI section"))
        check("log: [m9] ignored, no such object", d.has("[m9]") and d.has("no such object"))
        check("log (-d): frames sent and received",
              d.has('> {"seq":1,"cmd":"identify"}') and d.has('< {"seq":1,"ack":true'))

        time.sleep(1.0)
        pings = [f for f in dummy.received() if f.get("cmd") == "ping"]
        check("keepalive: pings every timeout_ms / 4 (100 ms) when idle",
              7 <= len(pings) <= 13, f"{len(pings)} pings in 1 s")
        check("keepalive: the dummy's timeout never expires", not dummy.has("link lost"))

        second = b.daemon(b.ini())
        rc = second.exit_code()
        check("a second nimhubd refuses to start (D-R1)",
              rc not in (None, 0) and second.has("exclusive use"), f"rc {rc}\n" + second.dump())
        second.stop()

        if os.geteuid() != 0:
            try:
                fd = os.open(b.tty, os.O_RDWR | os.O_NOCTTY)
                os.close(fd)
                busy = False
            except OSError as e:
                busy = e.errno == 16    # EBUSY
            check("other opens of the device fail with EBUSY (TIOCEXCL)", busy)
        check("the first nimhubd keeps running", d.p.poll() is None)
        check("...and its link was never disturbed", not dummy.has("link lost"))

        rc = d.stop()
        check("SIGTERM: exits with status 0", rc == 0, f"rc {rc}")
        check("SIGTERM: the port is closed", dummy.wait_for("port closed", 1.0) is not None)

        # Again on the same dummy: it is configured already, and reports the
        # link loss from the previous session's close
        d = b.daemon(b.ini())
        check("restart: handshake completes", d.wait_for("handshake: done") is not None,
              "\n" + d.dump())
        check("restart: identify reports settings_applied true",
              d.has("settings_applied true"))
        check("restart: link_recovered is logged", d.has('event: {"evt":"link_recovered"}'))
        check("restart: settings sent again (D-R3)",
              sum(f.get("cmd") == "set_motion" for f in dummy.received()) == 2)
        d.stop()
    finally:
        d.stop()
        dummy.stop()


def holder_checks(b):
    dummy = Dummy(b.tty)
    try:
        # Held without a lock, e.g. by screen: found in /proc
        fd = os.open(b.tty, os.O_RDWR | os.O_NOCTTY)
        d = b.daemon(b.ini())
        rc = d.exit_code()
        check("device already open by another process: refuses to start",
              rc not in (None, 0) and d.has(f"already open by process {os.getpid()}"),
              f"rc {rc}\n" + d.dump())
        d.stop()

        # Held with a lock, e.g. by picocom
        fcntl.flock(fd, fcntl.LOCK_EX)
        d = b.daemon(b.ini())
        rc = d.exit_code()
        check("device locked by another process: refuses to start",
              rc not in (None, 0) and d.has("locked by another process"),
              f"rc {rc}\n" + d.dump())
        d.stop()
        os.close(fd)

        frames = dummy.received()
        check("...and sends nothing", frames == [], str(frames))
    finally:
        dummy.stop()


def errors(d, text):
    """How many error lines in d's log contain text"""
    with d.cond:
        return sum(" ERROR " in line and text in line for line in d.lines)


def failure_checks(b):
    # Errors in the INI: nimhubd doesn't start
    def run_exit(name, expect, m0):
        dummy = Dummy(b.tty)
        d = b.daemon(b.ini(m0))
        rc = d.exit_code()
        check(name, rc == 1 and d.has(expect), f"rc {rc}\n" + d.dump())
        d.stop()
        dummy.stop()
        return dummy

    dummy = run_exit("bad INI value: exits before opening the device",
                     "bad value '3' for 'microsteps'", "microsteps = 3")
    check("...and sends nothing", dummy.received() == [])
    run_exit("unknown INI key: exits", "unknown key 'max_accel'", "max_accel = 400")

    # Failed handshakes: nimhubd stays up and retries with backoff, logging
    # the error once (D-R31)
    def run_retry(name, dummy_args, expect, m0=M0, seconds=2.5):
        dummy = Dummy(b.tty, *dummy_args)
        d = b.daemon(b.ini(m0))
        rc = d.exit_code(seconds)
        tries = sum(f.get("cmd") == "identify" for f in dummy.received())
        check(name, rc is None and errors(d, expect) == 1 and tries >= 2,
              f"rc {rc}, {tries} handshakes, {errors(d, expect)} errors\n" + d.dump())
        d.stop()
        dummy.stop()
        return dummy

    run_retry("unsupported protocol: retried, logged once", ["--proto", "2"], "protocol 2")
    dummy = run_retry("settings rejected by the MCU: retried, logged once", [],
                      "rejected the settings: bad_param",
                      m0="microsteps = 16\nspeed = 5000\npulse_width_us = 20")
    check("...never settings_done",
          not any(f.get("cmd") == "settings_done" for f in dummy.received()))
    run_retry("INI key that doesn't fit the object's type: retried, logged once",
              ["--objects", "m0:hx711"], "'speed' is not a setting of a hx711", m0="speed = 100")
    run_retry("missing reply (D-R30): link down, retried, logged once",
              ["--no-reply", "list_objects"], "no reply to list_objects")
    run_retry("axis busy for too long: retried, logged once", ["--busy-ms", "5000"],
              "still moving", seconds=5.0)

    # Busy for a moment: retried until it goes through
    dummy = Dummy(b.tty, "--busy-ms", "300")
    d = b.daemon(b.ini())
    ok = d.wait_for("handshake: done") is not None
    tries = sum(f.get("cmd") == "set_motion" for f in dummy.received())
    check("axis busy for a moment: set_motion retried, handshake completes",
          ok and tries >= 3, f"{tries} set_motion\n" + d.dump())
    d.stop()
    dummy.stop()

    # The MCU goes away (unplugged): nimhubd stays, and retries
    dummy = Dummy(b.tty)
    d = b.daemon(b.ini())
    d.wait_for("handshake: done")
    dummy.stop()
    rc = d.exit_code(3.0)
    check("MCU gone: link down logged once, nimhubd stays",
          rc is None and errors(d, "link down") == 1, f"rc {rc}\n" + d.dump())
    check("...and the missing device is logged once, not on every retry",
          errors(d, "cannot open") + errors(d, "cannot resolve") == 1, d.dump())
    d.stop()

    # The device not there at startup: nimhubd starts, and connects once it is
    d = b.daemon(b.ini())
    started = d.wait_for("listening on") is not None
    time.sleep(0.6)
    dummy = Dummy(b.tty)
    check("no device at startup: nimhubd starts anyway, and connects once it appears",
          started and d.wait_for("handshake: done", 4.0) is not None, d.dump())
    d.stop()
    dummy.stop()


class Client:
    """A client on nimhubd's socket. Events received are kept in events."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.rx = b""
        self.seq = 0
        self.events = []

    def close(self):
        self.s.close()

    def send_raw(self, data):
        self.s.sendall(data)

    def receive(self, timeout=2.0):
        """The next message, or None after timeout s, or "eof\""""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.rx:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                data = self.s.recv(65536)
            except socket.timeout:
                return None
            except ConnectionResetError:
                return "eof"
            if not data:
                return "eof"
            self.rx += data
        line, _, self.rx = self.rx.partition(b"\n")
        return json.loads(line)

    def event(self, timeout=2.0):
        """The next event, ignoring replies"""
        deadline = time.monotonic() + timeout
        while True:
            if self.events:
                return self.events.pop(0)
            msg = self.receive(max(0.0, deadline - time.monotonic()))
            if not isinstance(msg, dict):
                return None
            if "evt" in msg:
                return msg

    def call(self, cmd, dev=None, timeout=2.0, **args):
        """Send a request; return its reply (None if none came)"""
        self.seq += 1
        req = {"seq": self.seq, "cmd": cmd}
        if dev is not None:
            req["dev"] = dev
        req.update(args)
        self.send_raw(json.dumps(req).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            msg = self.receive(max(0.0, deadline - time.monotonic()))
            if not isinstance(msg, dict):
                return None
            if "evt" in msg:
                self.events.append(msg)
            elif msg.get("seq") == self.seq:
                return msg

    def wait_idle(self, dev="m0", timeout=10.0):
        """Poll status until dev isn't moving; return the last status"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            st = self.call("status", dev)
            if st is None or not st.get("moving"):
                return st
            time.sleep(0.05)
        return None


def acked(reply):
    return isinstance(reply, dict) and reply.get("ack") is True


def err(reply, code):
    return isinstance(reply, dict) and reply.get("ack") is False and reply.get("err") == code


STATE_KEYS = {"evt", "dev", "pos", "pos_valid", "moving", "locked"}


def wait_event(cl, name, timeout=5.0):
    """cl's events until one named name (returned), or None after timeout;
    all of them are in cl.seen"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        e = cl.event(max(0.0, deadline - time.monotonic()))
        if e is None:
            return None
        cl.seen.append(e)
        if e.get("evt") == name:
            return e
    return None


def shutdown_checks(b):
    """SIGTERM / SIGINT stop motion, then close the link (D-R2)"""
    def start(*dummy_args):
        dummy = Dummy(b.tty, *dummy_args)
        d = b.daemon(b.ini(m0=M0_FREE))
        d.wait_for("handshake: done")
        c = Client(b.socket)
        c.call("lease")
        return dummy, d, c

    # Idle: out at once, nothing sent
    dummy, d, c = start()
    rc = d.stop()
    check("SIGTERM while idle: exits 0 at once, no stop sent",
          rc == 0 and not any(f.get("cmd") == "stop" for f in dummy.received()), d.dump())
    c.close()
    dummy.stop()

    # During a move: stop, wait for move_end, then close
    dummy, d, c = start()
    c.call("rel", "m0", steps=5000)
    time.sleep(0.3)
    t = time.monotonic()
    rc = d.stop(signal.SIGINT)
    took = time.monotonic() - t
    frames = dummy.received()
    stop_at = next((i for i, f in enumerate(frames) if f.get("cmd") == "stop"), None)
    check("SIGINT during a move: stop sent, move_end awaited, exit 0",
          rc == 0 and stop_at is not None and frames[stop_at].get("oid") == "m0"
          and d.has("stopping 1 axis(es)") and d.has("motion stopped") and took < 1.5,
          f"rc {rc}, {took:.2f} s\n" + d.dump())
    check("...the port closed only after the stop",
          dummy.wait_for("port closed", 1.0) is not None
          and dummy.has('"evt":"move_end","oid":"m0"') and dummy.has('"reason":"stop"'))
    c.close()
    dummy.stop()

    # The stop goes unanswered: the reply timeout ends the wait
    dummy, d, c = start("--no-reply", "stop")
    c.call("rel", "m0", steps=5000)
    time.sleep(0.2)
    t = time.monotonic()
    rc = d.stop()
    took = time.monotonic() - t
    check("SIGTERM, stop unanswered: gives up after the reply timeout, exit 0",
          rc == 0 and d.has("not confirmed stopped") and took < 1.5,
          f"rc {rc}, {took:.2f} s\n" + d.dump())
    c.close()
    dummy.stop()

    # A second signal while waiting: out at once
    dummy, d, c = start("--no-reply", "stop")
    c.call("rel", "m0", steps=5000)
    time.sleep(0.2)
    t = time.monotonic()
    d.p.send_signal(signal.SIGTERM)
    d.wait_for("stopping 1 axis(es)", 1.0)
    d.p.send_signal(signal.SIGTERM)
    rc = d.exit_code(1.0)
    took = time.monotonic() - t
    check("a second SIGTERM while stopping: exits at once",
          rc == 0 and d.has("again: closing the link now") and took < 0.4,
          f"rc {rc}, {took:.2f} s\n" + d.dump())
    c.close()
    dummy.stop()


def dummy_estop_checks(b):
    """The dummy MCU runs an estop ahead of lines received before it (F-R24),
    as the firmware does; nimhubd's side (D-R17) is in test_daemon_link.c"""
    dummy = Dummy(b.tty)
    try:
        fd = os.open(b.tty, os.O_RDWR | os.O_NOCTTY)
        os.write(fd, b'{"seq":1,"cmd":"settings_done"}\n')
        time.sleep(0.2)
        # One write: a move, then an estop behind it
        os.write(fd, b'{"seq":2,"cmd":"move","oid":"m0","steps":10}\n'
                     b'{"seq":3,"cmd":"ping"}\n'
                     b'{"seq":4,"cmd":"estop"}\n')
        time.sleep(0.3)
        os.close(fd)
        order = [f.get("seq") for f in dummy.received()]
        check("dummy MCU: estop handled ahead of the lines queued before it (F-R24)",
              order == [1, 4, 2, 3], str(order))
    finally:
        dummy.stop()


def reconnect_checks(b):
    """Link failure and reconnect (D-R30, D-R31), and the runtime settings
    re-sent after it (D-R3)"""
    dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
    d = b.daemon(b.ini(m0=M0_FREE, daemon="lease_timeout_ms = 60000"))
    d.wait_for("handshake: done")
    c, c2 = Client(b.socket), Client(b.socket)
    c.seen, c2.seen = [], []
    try:
        c.call("lease")
        c.call("watch")
        events_until(c, lambda e: e.get("dev") == "m1", 2.0)
        r = c.call("speed", "m0", value=400)
        check("runtime speed 400 (the INI says 300)", acked(r), str(r))
        c.call("rel", "m0", steps=5000)
        time.sleep(0.3)

        # Unplugged during a move
        dummy.stop()
        t_drop = time.monotonic()
        e = wait_event(c, "link_down", 2.0)
        e2 = wait_event(c2, "link_down", 2.0)
        check("unplugged: link_down to every client, watching or not, with a reason",
              e is not None and e2 is not None and "link down" in e.get("reason", ""),
              f"{e} {e2}")
        c.seen = []
        for name, r in [("status", c.call("status", "m0")), ("pos", c.call("pos", "m0")),
                        ("devices", c.call("devices")), ("rel", c.call("rel", "m0", steps=1)),
                        ("estop", c.call("estop"))]:
            check(f"while down: {name} gets link_down", err(r, "link_down"), str(r))
        r = c.call("renew")
        check("while down: the lease is kept", acked(r), str(r))

        # Back after 2 s: attempts at 0.25, 0.75, 1.75 and 3.75 s (D-R31)
        time.sleep(max(0.0, 2.0 - (time.monotonic() - t_drop)))
        dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
        e = wait_event(c, "link_up", 4.0)
        t_up = time.monotonic() - t_drop
        check("no state made up by nimhubd while down (D-R20): no status event before link_up",
              not any(x.get("evt") == "status" for x in c.seen), str(c.seen))
        check("plugged in again: link_up, at the backoff's 4th attempt (3.75 s)",
              e is not None and 3.4 <= t_up <= 4.4, f"{e} after {t_up:.2f} s")
        check("...to every client", wait_event(c2, "link_up", 1.0) is not None)
        evs = events_until(c, lambda e: e.get("dev") == "m1", 2.0)
        check("...then the state of every device, from the firmware",
              [x.get("dev") for x in evs] == ["m0", "m1"]
              and evs[0].get("pos") == 0 and evs[0].get("moving") is False, str(evs))
        motion = [f for f in dummy.received() if f.get("cmd") == "set_motion"]
        check("...and the runtime speed re-sent instead of the INI's (D-R3)",
              len(motion) == 1 and motion[0].get("oid") == "m0" and motion[0].get("speed") == 400
              and motion[0].get("microsteps") == 4, str(motion))
        check("...and errors logged once: the drop and the missing device",
              errors(d, "link down") == 1 and errors(d, "cannot") == 1, d.dump())
        r = c.call("rel", "m0", steps=10)
        check("...and service is back, with the same lease", acked(r), str(r))
        c.wait_idle()

        # A second drop, with the device back at once: the backoff was reset
        r = c.call("accel", "m1", value=900)
        dummy.stop()
        t_drop = time.monotonic()
        wait_event(c, "link_down", 2.0)
        dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
        e = wait_event(c, "link_up", 3.0)
        t_up = time.monotonic() - t_drop
        check("backoff reset after a good handshake: back at the 1st attempt (0.25 s)",
              e is not None and t_up < 1.0, f"{e} after {t_up:.2f} s")
        motion = [f for f in dummy.received() if f.get("cmd") == "set_motion"]
        check("...runtime settings for both: m0 speed, m1 accel (no INI section)",
              acked(r) and [(f.get("oid"), f.get("speed"), f.get("accel")) for f in motion]
              == [("m0", 400, 1000), ("m1", None, 900)], str(motion))
        events_until(c, lambda e: e.get("dev") == "m1", 2.0)

        # A firmware reset: not_configured (D-R30)
        dummy.p.send_signal(signal.SIGUSR1)
        time.sleep(0.2)
        r = c.call("rel", "m0", steps=10)
        check("firmware reset: the move gets link_down (not_configured)", err(r, "link_down"),
              str(r))
        e = wait_event(c, "link_down", 2.0)
        check("...then link_down, saying why", e is not None and "firmware reset" in e["reason"],
              str(e))
        check("...then link_up after a new handshake", wait_event(c, "link_up", 3.0) is not None)
        motion = [f for f in dummy.received() if f.get("cmd") == "set_motion"]
        check("...with the settings sent again", len(motion) == 4, str(motion))
        events_until(c, lambda e: e.get("dev") == "m1", 2.0)

        # The MCU stops answering (D-R30): a missing reply is a drop
        dummy.p.send_signal(signal.SIGUSR2)
        time.sleep(0.2)
        r = c.call("rel", "m0", steps=10)
        check("no replies: the request gets link_down after the reply timeout",
              err(r, "link_down"), str(r))
        e = wait_event(c, "link_down", 2.0)
        check("...and link_down: no reply", e is not None and "no reply" in e["reason"], str(e))
        time.sleep(2.0)
        # The first missed reply may be a keepalive ping's or the move's
        check("...retried while it stays silent, each reason logged once",
              errors(d, "no reply to") == 2 and errors(d, "no reply to identify") == 1
              and d.p.poll() is None, d.dump())
        dummy.p.send_signal(signal.SIGUSR2)
        check("answering again: link_up", wait_event(c, "link_up", 4.0) is not None)
    finally:
        c.close()
        c2.close()
        d.stop()
        dummy.stop()


def events_until(w, done, timeout=10.0):
    """w's events until done(event) is true for one (included), or timeout"""
    got = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        e = w.event(max(0.0, deadline - time.monotonic()))
        if e is None:
            break
        got.append(e)
        if done(e):
            break
    return got


def quiet(cl, seconds=0.5):
    """True if nothing arrives for cl within seconds"""
    return not cl.events and cl.receive(seconds) is None


def watch_checks(b, c):
    """watch and status events (D-R21, D-R22). c holds the lease; m0 is idle,
    with an invalid position, unlocked."""
    w = Client(b.socket)
    try:
        r = w.call("watch")
        snap = events_until(w, lambda e: e.get("dev") == "m1", 2.0)
        check("watch: the reply, then the state of every device",
              acked(r) and [e.get("dev") for e in snap] == ["m0", "m1"]
              and all(e.get("evt") == "status" and set(e) == STATE_KEYS for e in snap), str(snap))
        st = c.call("status", "m0")
        check("...the same state status gives",
              snap and {k: snap[0][k] for k in ("pos", "pos_valid", "moving", "locked")}
              == {k: st[k] for k in ("pos", "pos_valid", "moving", "locked")}, f"{snap} {st}")
        check("no events while nothing changes", quiet(w))
        start = snap[0]["pos"] if snap else 0

        c.call("rel", "m0", steps=400)
        evs = events_until(w, lambda e: e.get("moving") is False)
        check("a move: moving first, then positions, then the end at the target",
              len(evs) >= 3 and evs[0].get("moving") is True and evs[0].get("pos") == start
              and evs[-1].get("moving") is False and evs[-1].get("pos") == start + 400, str(evs))
        check("...one event per change: no two in a row the same",
              all(a != b2 for a, b2 in zip(evs, evs[1:])), str(evs))
        check("...positions only grow", all(a["pos"] <= b2["pos"] for a, b2 in zip(evs, evs[1:])),
              str(evs))
        check("...all for m0, with every field (D-R22)",
              all(e.get("dev") == "m0" and set(e) == STATE_KEYS for e in evs), str(evs))
        check("the client that moved it, not watching, gets no events", quiet(c, 0.2))

        c.call("zero", "m0")
        e = w.event()
        check("zero: one event, pos 0 and valid",
              e is not None and e.get("pos") == 0 and e.get("pos_valid") is True, str(e))
        c.call("lock", "m0")
        e = w.event()
        check("lock: one event, locked", e is not None and e.get("locked") is True, str(e))
        c.call("unlock", "m0")
        e = w.event()
        check("unlock: one event, free and position void", e is not None
              and e.get("locked") is False and e.get("pos_valid") is False, str(e))
        check("...and nothing more", quiet(w, 0.3))
        r = c.call("stop", "m0")
        check("a command that changes nothing: no event", err(r, "not_moving") and quiet(w, 0.3))

        r = w.call("watch")
        snap = events_until(w, lambda e: e.get("dev") == "m1", 2.0)
        check("watch again: the state again", acked(r) and len(snap) == 2, str(snap))
        r = w.call("unwatch")
        c.call("rel", "m0", steps=10)
        check("unwatch: no more events", acked(r) and quiet(w, 1.0))
        check("unwatch again: fine", acked(w.call("unwatch")))
        c.wait_idle()
    finally:
        w.close()


# No soft limits: with the position still invalid, a move is only allowed
# without them (F-R25)
M0_FREE = "microsteps = 4\nspeed = 300\naccel = 1000\npulse_width_us = 10\n"


def client_checks(b):
    dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
    # A long lease: this session holds it for longer than the default 5 s,
    # and only lease and renew renew it (D-R18)
    d = b.daemon(b.ini(m0=M0_FREE, daemon="lease_timeout_ms = 60000"))
    clients = []
    try:
        ok = d.wait_for("handshake: done") is not None
        check("client checks: handshake completes", ok, "\n" + d.dump())
        if not ok:
            return
        mode = stat.S_IMODE(os.stat(b.socket).st_mode)
        check("socket mode 0660 (D-R12)", mode == 0o660, oct(mode))
        check("handshake: get_state for each stepper",
              [f.get("oid") for f in dummy.received() if f.get("cmd") == "get_state"]
              == ["m0", "m1"])

        c = Client(b.socket)
        clients.append(c)
        st = c.call("status", "m0")
        check("status: from the handshake's get_state",
              acked(st) and st.get("seq") == 1 and st.get("pos") == 0
              and st.get("pos_valid") is False and st.get("moving") is False
              and st.get("locked") is False, str(st))
        r = c.call("devices")
        check("devices: the firmware's objects, in order",
              acked(r) and r.get("devices") == [{"dev": "m0", "type": "stepper"},
                                                {"dev": "m1", "type": "stepper"}], str(r))
        p = c.call("pos", "m0")
        check("pos: pos and pos_valid", acked(p) and p.get("pos") == 0
              and p.get("pos_valid") is False and "moving" not in p, str(p))
        n_get_state = sum(f.get("cmd") == "get_state" for f in dummy.received())
        check("status and pos don't ask the MCU", n_get_state == 2)

        # The lease (D-R18): a write needs it
        r = c.call("rel", "m0", steps=400)
        check("write without the lease: lease_held, nothing sent",
              err(r, "lease_held") and dummy.received()[-1].get("cmd") != "move", str(r))
        r = c.call("lease")
        check("lease: taken, with timeout_ms", acked(r) and r.get("timeout_ms") == 60000, str(r))
        r = c.call("lease")
        check("lease again by the holder: renewed", acked(r), str(r))
        r = c.call("renew")
        check("renew by the holder", acked(r) and r.get("timeout_ms") == 60000, str(r))

        # A move, end to end
        r = c.call("rel", "m0", steps=400)
        check("rel 400: accepted", acked(r), str(r))
        frames = dummy.received()
        check("rel 400 is sent as move, then get_state: the new state comes from the firmware",
              [f.get("cmd") for f in frames[-2:]] == ["move", "get_state"]
              and frames[-2].get("oid") == "m0" and frames[-2].get("steps") == 400,
              str(frames[-2:]))
        st = c.call("status", "m0")
        check("status right after rel: moving", acked(st) and st.get("moving") is True, str(st))
        time.sleep(0.6)
        st = c.call("status", "m0")
        check("status during the move: pos from position events",
              acked(st) and st.get("moving") and 0 < st.get("pos", 0) < 400, str(st))
        st = c.wait_idle()
        check("rel 400: ends at pos 400", st is not None and st.get("pos") == 400, str(st))

        r = c.call("abs", "m0", pos=-100)
        check("abs with the position invalid: pos_invalid, nothing sent",
              err(r, "pos_invalid") and dummy.received()[-1].get("cmd") != "move_to", str(r))

        # stop, busy, not_moving: the MCU's errors come through
        c.call("rel", "m0", steps=5000)
        r = c.call("rel", "m0", steps=10)
        check("rel while moving: busy", err(r, "busy"), str(r))
        time.sleep(0.3)
        r = c.call("stop", "m0")
        check("stop: accepted, with pos", acked(r) and isinstance(r.get("pos"), int), str(r))
        st = c.wait_idle()
        check("stop: ends at the pos stop returned",
              st is not None and acked(r) and st.get("pos") == r.get("pos"), str(st))
        r = c.call("stop", "m0")
        check("stop while idle: not_moving", err(r, "not_moving"), str(r))

        # Errors found by nimhubd itself
        cases = [
            ("unknown dev: unknown_dev", c.call("status", "m7"), "unknown_dev"),
            ("unknown command: unknown_cmd", c.call("fly", "m0"), "unknown_cmd"),
            ("device command without dev: unknown_cmd", c.call("rel", steps=1), "unknown_cmd"),
            ("daemon command with dev: unknown_cmd", c.call("estop", "m0"), "unknown_cmd"),
            ("home: not_impl", c.call("home", "m0"), "not_impl"),
            ("speed 'x': bad_param", c.call("speed", "m0", value="x"), "bad_param"),
            ("speed without value: bad_param", c.call("speed", "m0"), "bad_param"),
            ("rel steps 'x': bad_param", c.call("rel", "m0", steps="x"), "bad_param"),
            ("rel steps 1.5: bad_param", c.call("rel", "m0", steps=1.5), "bad_param"),
            ("rel steps 2^31: bad_param", c.call("rel", "m0", steps=2**31), "bad_param"),
            ("abs without pos: bad_param", c.call("abs", "m0"), "bad_param"),
            ("dev not a string: bad_request", c.call("status", 5), "bad_request"),
        ]
        for name, reply, code in cases:
            check(name, err(reply, code), str(reply))
        c.seq += 1
        c.send_raw(json.dumps({"seq": c.seq, "dev": "m0"}).encode() + b"\n")
        r = c.receive()
        check("missing cmd: bad_request reply with seq", err(r, "bad_request")
              and r.get("seq") == c.seq, str(r))

        # Lines whose seq can't be read: a bad_request event (D-R40)
        for name, line in [("not JSON", b"garbage"),
                           ("not an object", b"[1,2]"),
                           ("no seq", b'{"cmd":"status","dev":"m0"}'),
                           ("seq -1", b'{"seq":-1,"cmd":"status","dev":"m0"}'),
                           ("seq 2^32", b'{"seq":4294967296,"cmd":"status","dev":"m0"}'),
                           ("over-length line", b'{"seq":1,"x":"' + b"a" * 5000 + b'"}')]:
            c.send_raw(line + b"\n")
            e = c.event()
            check(f"{name}: bad_request event", isinstance(e, dict)
                  and e.get("evt") == "bad_request" and "seq" not in e, str(e))
        r = c.call("status", "m0")
        check("the connection still works after bad requests", acked(r), str(r))
        c.send_raw(b'{"seq":4294967295,"cmd":"pos","dev":"m0"}\n')
        r = c.receive()
        check("seq 2^32 - 1 is echoed", acked(r) and r.get("seq") == 4294967295, str(r))

        # The runtime settings (D-R3): one set_motion field each
        for cmd, field, value in [("speed", "speed", 400), ("accel", "accel", 2000),
                                  ("micro", "microsteps", 8)]:
            r = c.call(cmd, "m0", value=value)
            f = dummy.received()[-1]
            check(f"{cmd} {value}: accepted, sent as set_motion {field}", acked(r)
                  and f == {"seq": f.get("seq"), "cmd": "set_motion", "oid": "m0", field: value},
                  f"{r} {f}")
        r = c.call("micro", "m0", value=3)
        check("micro 3: the MCU's bad_param", err(r, "bad_param"), str(r))
        c.call("rel", "m0", steps=2000)
        r = c.call("speed", "m0", value=300)
        check("speed while moving: the MCU's busy", err(r, "busy"), str(r))
        c.call("stop", "m0")
        c.wait_idle()

        # zero, lock, unlock
        r = c.call("zero", "m0")
        st = c.call("status", "m0")
        check("zero: pos 0, valid", acked(r) and st.get("pos") == 0 and st.get("pos_valid") is True,
              f"{r} {st}")
        r = c.call("abs", "m0", pos=50)
        st = c.wait_idle()
        check("abs 50 after zero: ends at 50", acked(r) and st is not None and st.get("pos") == 50,
              f"{r} {st}")
        r = c.call("lock", "m0")
        st = c.call("status", "m0")
        check("lock: locked, still valid", acked(r) and st.get("locked") is True
              and st.get("pos_valid") is True, f"{r} {st}")
        r = c.call("unlock", "m0")
        st = c.call("status", "m0")
        check("unlock: free, position void", acked(r) and st.get("locked") is False
              and st.get("pos_valid") is False, f"{r} {st}")
        frames = [f for f in dummy.received() if f.get("cmd") in ("set_position", "set_enable")]
        check("zero, lock, unlock are sent as set_position 0, set_enable 1 and 0",
              [(f["cmd"], f.get("pos", f.get("value"))) for f in frames[-3:]]
              == [("set_position", 0), ("set_enable", 1), ("set_enable", 0)], str(frames[-3:]))
        cmds = [f.get("cmd") for f in dummy.received() if f.get("cmd") != "ping"]
        after = [cmds[i + 1] if i + 1 < len(cmds) else None
                 for i, x in enumerate(cmds) if x in ("set_position", "set_enable")]
        check("...each followed by get_state (D-R20)",
              len(after) >= 3 and all(x == "get_state" for x in after), str(cmds[-10:]))

        watch_checks(b, c)

        # Several clients (D-R10), at most 8; one lease (D-R18)
        c2 = Client(b.socket)
        clients.append(c2)
        check("another client can't take the lease", err(c2.call("lease"), "lease_held"))
        check("...or renew it", err(c2.call("renew"), "lease_held"))
        r = c2.call("release")
        check("...and its release does nothing", acked(r) and acked(c.call("renew")), str(r))
        c.call("rel", "m0", steps=400)
        st = c2.call("status", "m0")
        check("a second client sees the first one's move", acked(st) and st.get("moving"), str(st))
        r = c2.call("stop", "m0")
        check("...but can't stop it without the lease (stop is a write)",
              err(r, "lease_held") and r.get("msg") == "lease held by another client", str(r))
        c.call("stop", "m0")
        c.wait_idle()
        more = [Client(b.socket) for _ in range(6)]
        clients.extend(more)
        check("8 clients at once", all(acked(m.call("pos", "m0")) for m in more))
        ninth = Client(b.socket)
        clients.append(ninth)
        check("a 9th client is closed at once", ninth.receive(1.0) == "eof")
        r = c.call("release")
        check("release: the lease is free for others", acked(r) and acked(c2.call("lease"))
              and acked(c2.call("release")), str(r))

        # A client that goes away with a request pending: the lease goes with it
        gone = more[1]
        check("a client takes the lease", acked(gone.call("lease")))
        gone.seq += 1
        gone.send_raw(json.dumps({"seq": gone.seq, "cmd": "rel", "dev": "m0",
                                  "steps": 100}).encode() + b"\n")
        gone.close()
        clients.remove(gone)
        time.sleep(0.2)
        st = c.wait_idle()
        check("...goes with a request pending: nimhubd carries on",
              d.p.poll() is None and st is not None, str(st))
        check("...and its lease is freed at once (D-R18)", acked(c.call("lease"))
              and acked(c.call("release")))

        # A client that shuts down its sending side, as socat does at the end
        # of its input, still gets every reply (D-R41), then EOF
        half = more[2]
        half.send_raw(b'{"seq":1,"cmd":"lease"}\n'
                      b'{"seq":2,"cmd":"rel","dev":"m0","steps":100}\n'
                      b'{"seq":3,"cmd":"status","dev":"m0"}\n'
                      b'{"seq":4,"cmd":"stop","dev":"m0"}\n')
        half.s.shutdown(socket.SHUT_WR)
        got = [half.receive() for _ in range(5)]
        replies = {m.get("seq"): m for m in got[:4] if isinstance(m, dict)}
        check("half-closed client: all replies, then EOF",
              sorted(replies) == [1, 2, 3, 4] and acked(replies.get(2)) and got[4] == "eof",
              str(got))
        c.wait_idle()

        # estop: emergency class, from any client (D-R17, F-R24)
        check("the lease again", acked(c.call("lease")))
        c.call("rel", "m0", steps=5000)
        time.sleep(0.2)
        r = c2.call("estop")
        check("estop from a client without the lease: accepted", acked(r), str(r))
        tail = [(f.get("cmd"), f.get("oid")) for f in dummy.received()[-3:]]
        check("...then get_state for every stepper, before the reply (D-R20)",
              tail == [("estop", None), ("get_state", "m0"), ("get_state", "m1")], str(tail))
        st = c.call("status", "m0")
        check("after estop: position void, driver disabled",
              acked(st) and st.get("pos_valid") is False and st.get("locked") is False, str(st))
        st = c.wait_idle()
        check("after estop: the move ends", st is not None and not st.get("moving"), str(st))

        for cl in clients:
            cl.close()
        clients = []
        rc = d.stop()
        check("SIGTERM: socket file removed", rc == 0 and not os.path.exists(b.socket))
    finally:
        for cl in clients:
            cl.close()
        d.stop()
        dummy.stop()

    # abs with a valid position
    dummy = Dummy(b.tty, "--pos-valid")
    d = b.daemon(b.ini(m0=M0_FREE))
    d.wait_for("handshake: done")
    c = Client(b.socket)
    c.call("lease")
    r = c.call("abs", "m0", pos=-100)
    check("abs -100 with the position valid: accepted, sent as move_to", acked(r)
          and [f.get("cmd") for f in dummy.received()[-2:]] == ["move_to", "get_state"], str(r))
    st = c.wait_idle()
    check("abs -100: ends at pos -100", st is not None and st.get("pos") == -100
          and st.get("pos_valid") is True, str(st))
    c.close()
    d.stop()
    dummy.stop()

    # Soft limits set and the position invalid: the MCU's pos_invalid
    dummy = Dummy(b.tty)
    d = b.daemon(b.ini(m0="soft_limit_max = 5000"))
    d.wait_for("handshake: done")
    c = Client(b.socket)
    c.call("lease")
    r = c.call("rel", "m0", steps=10)
    check("rel with soft limits and an invalid position: pos_invalid", err(r, "pos_invalid"), str(r))
    c.close()
    d.stop()
    dummy.stop()

    # A reply that never comes: the request still gets one (D-R41)
    dummy = Dummy(b.tty, "--no-reply", "move")
    d = b.daemon(b.ini(m0=M0_FREE))
    d.wait_for("handshake: done")
    c = Client(b.socket)
    c.call("lease")
    r = c.call("rel", "m0", steps=10)
    check("no reply from the MCU: link_down reply", err(r, "link_down"), str(r))
    c.close()
    d.stop()
    dummy.stop()

    # Lease expiry (D-R18), with a short timeout
    dummy = Dummy(b.tty)
    d = b.daemon(b.ini(m0=M0_FREE, daemon="lease_timeout_ms = 400"))
    d.wait_for("handshake: done")
    c, c2 = Client(b.socket), Client(b.socket)
    r = c.call("lease")
    check("lease_timeout_ms from the INI", acked(r) and r.get("timeout_ms") == 400, str(r))
    time.sleep(0.25)
    check("a write while held", acked(c.call("rel", "m0", steps=1)))
    c.call("status", "m0")
    time.sleep(0.25)
    check("writes and reads don't renew it: expired at 400 ms, lease_held",
          err(c.call("rel", "m0", steps=1), "lease_held"))
    check("...and renew too", err(c.call("renew"), "lease_held"))
    check("...and another client can take it", acked(c2.call("lease")))
    time.sleep(0.25)
    check("renew keeps it", acked(c2.call("renew")))
    time.sleep(0.25)
    check("...past the first timeout", acked(c2.call("rel", "m0", steps=1)))
    c2.wait_idle()
    check("...and lease by the holder renews it too", acked(c2.call("lease")))
    c2.call("rel", "m0", steps=3000)
    time.sleep(0.6)
    st = c.call("status", "m0")
    check("a move goes on after its lease expires", acked(st) and st.get("moving") is True, str(st))
    check("...and the lease is free", acked(c.call("lease")))
    c.call("stop", "m0")
    c.close()
    c2.close()
    d.stop()
    dummy.stop()

    # Before the handshake is done: link_down
    dummy = Dummy(b.tty, "--no-reply", "get_state")
    d = b.daemon(b.ini(m0=M0_FREE))
    d.wait_for("listening on")
    c = Client(b.socket)
    r = c.call("status", "m0")
    check("before the handshake is done: link_down", err(r, "link_down"), str(r))
    r = c.call("devices")
    check("devices before the handshake is done: link_down", err(r, "link_down"), str(r))
    r = c.call("watch")
    check("watch before the handshake is done: accepted, no devices to report yet",
          acked(r) and quiet(c, 0.2), str(r))
    c.close()
    d.stop()
    dummy.stop()

    # ...then, once it is done, the state of every device
    dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper", "--busy-ms", "500")
    d = b.daemon(b.ini(m0=M0_FREE))
    d.wait_for("listening on")
    c = Client(b.socket)
    r = c.call("watch")
    evs = events_until(c, lambda e: e.get("dev") == "m1", 3.0)
    check("watching before the handshake: link_up, then every device's state once it is done",
          acked(r) and d.has("handshake: done")
          and [e.get("evt") for e in evs] == ["link_up", "status", "status"]
          and [e.get("dev") for e in evs[1:]] == ["m0", "m1"], f"{r} {evs}")
    c.close()
    d.stop()
    dummy.stop()

    # An old socket file is replaced; a missing directory or a file that
    # isn't a socket is an error
    old = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old.bind(b.socket)
    old.close()
    dummy = Dummy(b.tty)
    d = b.daemon(b.ini(m0=M0_FREE))
    check("an old socket file is replaced", d.wait_for("handshake: done") is not None,
          "\n" + d.dump())
    d.stop()
    dummy.stop()
    for name, path, expect in [
            ("socket directory missing: exits", os.path.join(b.tmp.name, "nodir", "s.sock"),
             "set socket ="),
            ("socket path is a regular file: exits", b.ini(), "is not a socket")]:
        dummy = Dummy(b.tty)
        d = b.daemon(b.ini(m0=M0_FREE, socket=path))
        rc = d.exit_code()
        check(name, rc == 1 and d.has(expect), f"rc {rc}\n" + d.dump())
        d.stop()
        dummy.stop()


def main():
    nimhubd = sys.argv[1] if len(sys.argv) > 1 else "build/nimhubd"
    if not os.access(nimhubd, os.X_OK):
        sys.exit(f"{nimhubd}: not found; run make first")
    b = Bench(nimhubd)
    handshake_checks(b)
    holder_checks(b)
    failure_checks(b)
    client_checks(b)
    reconnect_checks(b)
    shutdown_checks(b)
    dummy_estop_checks(b)
    print(f"{failures} failure(s)" if failures else "all checks passed")
    sys.exit(min(failures, 125))


if __name__ == "__main__":
    main()
