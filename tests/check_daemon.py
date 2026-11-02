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

    def ini(self, m0=M0, tty=None, socket=None):
        self.n += 1
        path = os.path.join(self.tmp.name, f"test{self.n}.ini")
        with open(path, "w") as f:
            f.write(INI.format(tty=tty or self.tty, socket=socket or self.socket, m0=m0))
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


def failure_checks(b):
    def run(name, dummy_args, expect, m0=M0, timeout=3.0):
        dummy = Dummy(b.tty, *dummy_args)
        d = b.daemon(b.ini(m0))
        rc = d.exit_code(timeout)
        check(name, rc == 1 and d.has(expect), f"rc {rc}\n" + d.dump())
        d.stop()
        dummy.stop()
        return dummy

    dummy = run("bad INI value: exits before opening the device", [],
                "bad value '3' for 'microsteps'", m0="microsteps = 3")
    check("...and sends nothing", dummy.received() == [])
    run("unknown INI key: exits", [], "unknown key 'max_accel'", m0="max_accel = 400")
    run("unsupported protocol: exits", ["--proto", "2"], "protocol 2")
    dummy = run("settings rejected by the MCU: exits", [],
                "rejected the settings: bad_param",
                m0="microsteps = 16\nspeed = 5000\npulse_width_us = 20")
    check("...without settings_done",
          not any(f.get("cmd") == "settings_done" for f in dummy.received()))
    run("INI key that doesn't fit the object's type: exits",
        ["--objects", "m0:hx711"], "'speed' is not a setting of a hx711", m0="speed = 100")
    run("missing reply (D-R30): link down, exits", ["--no-reply", "list_objects"],
        "no reply to list_objects")
    dummy = run("axis busy for too long: exits", ["--busy-ms", "5000"], "still moving",
                timeout=4.0)

    # Busy for a moment: retried until it goes through
    dummy = Dummy(b.tty, "--busy-ms", "300")
    d = b.daemon(b.ini())
    ok = d.wait_for("handshake: done") is not None
    tries = sum(f.get("cmd") == "set_motion" for f in dummy.received())
    check("axis busy for a moment: set_motion retried, handshake completes",
          ok and tries >= 3, f"{tries} set_motion\n" + d.dump())
    d.stop()
    dummy.stop()

    # The MCU goes away (unplugged)
    dummy = Dummy(b.tty)
    d = b.daemon(b.ini())
    d.wait_for("handshake: done")
    dummy.stop()
    rc = d.exit_code()
    check("MCU gone: link down, exits", rc == 1 and d.has("link down"), f"rc {rc}\n" + d.dump())
    d.stop()


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


# No soft limits: with the position still invalid, a move is only allowed
# without them (F-R25)
M0_FREE = "microsteps = 4\nspeed = 300\naccel = 1000\npulse_width_us = 10\n"


def client_checks(b):
    dummy = Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
    d = b.daemon(b.ini(m0=M0_FREE))
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
        p = c.call("pos", "m0")
        check("pos: pos and pos_valid", acked(p) and p.get("pos") == 0
              and p.get("pos_valid") is False and "moving" not in p, str(p))
        n_get_state = sum(f.get("cmd") == "get_state" for f in dummy.received())
        check("status and pos don't ask the MCU", n_get_state == 2)

        # A move, end to end
        r = c.call("rel", "m0", steps=400)
        check("rel 400: accepted", acked(r), str(r))
        frames = dummy.received()
        check("rel 400 is sent as move", frames[-1].get("cmd") == "move"
              and frames[-1].get("oid") == "m0" and frames[-1].get("steps") == 400,
              str(frames[-1]))
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
            ("speed: not_impl", c.call("speed", "m0", value=100), "not_impl"),
            ("lease: not_impl", c.call("lease"), "not_impl"),
            ("watch: not_impl", c.call("watch"), "not_impl"),
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

        # Several clients (D-R10), at most 8
        c2 = Client(b.socket)
        clients.append(c2)
        c.call("rel", "m0", steps=400)
        st = c2.call("status", "m0")
        check("a second client sees the first one's move", acked(st) and st.get("moving"), str(st))
        r = c2.call("stop", "m0")
        check("...and can stop it (no lease yet)", acked(r), str(r))
        c2.wait_idle()
        more = [Client(b.socket) for _ in range(6)]
        clients.extend(more)
        check("8 clients at once", all(acked(m.call("pos", "m0")) for m in more))
        ninth = Client(b.socket)
        clients.append(ninth)
        check("a 9th client is closed at once", ninth.receive(1.0) == "eof")

        # A client that goes away with a request pending
        gone = more[1]
        gone.seq += 1
        gone.send_raw(json.dumps({"seq": gone.seq, "cmd": "rel", "dev": "m0",
                                  "steps": 100}).encode() + b"\n")
        gone.close()
        clients.remove(gone)
        time.sleep(0.2)
        st = c.wait_idle()
        check("a client gone with a request pending: nimhubd carries on",
              d.p.poll() is None and st is not None, str(st))

        # A client that shuts down its sending side, as socat does at the end
        # of its input, still gets every reply (D-R41), then EOF
        half = more[2]
        half.send_raw(b'{"seq":1,"cmd":"rel","dev":"m0","steps":100}\n'
                      b'{"seq":2,"cmd":"status","dev":"m0"}\n'
                      b'{"seq":3,"cmd":"stop","dev":"m0"}\n')
        half.s.shutdown(socket.SHUT_WR)
        got = [half.receive() for _ in range(4)]
        seqs = [m.get("seq") for m in got[:3] if isinstance(m, dict)]
        check("half-closed client: all replies, then EOF",
              sorted(seqs) == [1, 2, 3] and got[3] == "eof", str(got))
        c.wait_idle()

        # estop: a daemon command (F-R24)
        c.call("rel", "m0", steps=5000)
        time.sleep(0.2)
        r = c.call("estop")
        check("estop: accepted", acked(r), str(r))
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
    r = c.call("abs", "m0", pos=-100)
    check("abs -100 with the position valid: accepted, sent as move_to", acked(r)
          and dummy.received()[-1].get("cmd") == "move_to", str(r))
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
    r = c.call("rel", "m0", steps=10)
    check("no reply from the MCU: link_down reply", err(r, "link_down"), str(r))
    c.close()
    d.stop()
    dummy.stop()

    # Before the handshake is done: link_down
    dummy = Dummy(b.tty, "--no-reply", "get_state")
    d = b.daemon(b.ini(m0=M0_FREE))
    d.wait_for("listening on")
    c = Client(b.socket)
    r = c.call("status", "m0")
    check("before the handshake is done: link_down", err(r, "link_down"), str(r))
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
    print(f"{failures} failure(s)" if failures else "all checks passed")
    sys.exit(min(failures, 125))


if __name__ == "__main__":
    main()
