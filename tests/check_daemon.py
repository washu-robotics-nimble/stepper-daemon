#!/usr/bin/env python3
"""End-to-end checks of nimhubd against tests/dummy_mcu.py, without the Pico
(build plan step 8). Standard library only.

Usage:
    check_daemon.py [NIMHUBD]       (default: build/nimhubd; run by
                                     `make check-daemon`)

Checks the handshake and the settings sent, keepalive pings, exclusive use of
the device (D-R1), and the cases where nimhubd must give up: a bad INI, an
unsupported protocol, settings the MCU rejects, a missing reply (D-R30), and
the MCU going away. Prints PASS / FAIL per check; the exit status is the
number of failures.
"""

import fcntl
import json
import os
import signal
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
socket = {tmp}/control.sock

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
        self.n = 0

    def ini(self, m0=M0, tty=None):
        self.n += 1
        path = os.path.join(self.tmp.name, f"test{self.n}.ini")
        with open(path, "w") as f:
            f.write(INI.format(tty=tty or self.tty, tmp=self.tmp.name, m0=m0))
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


def main():
    nimhubd = sys.argv[1] if len(sys.argv) > 1 else "build/nimhubd"
    if not os.access(nimhubd, os.X_OK):
        sys.exit(f"{nimhubd}: not found; run make first")
    b = Bench(nimhubd)
    handshake_checks(b)
    holder_checks(b)
    failure_checks(b)
    print(f"{failures} failure(s)" if failures else "all checks passed")
    sys.exit(min(failures, 125))


if __name__ == "__main__":
    main()
