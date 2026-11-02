#!/usr/bin/env python3
"""End-to-end checks of nimhub-cli, through nimhubd and tests/dummy_mcu.py
(build plan step 10). Standard library only.

Usage:
    check_cli.py [NIMHUBD [NIMHUB_CLI]]   (default: build/nimhubd,
                                           build/nimhub-cli; run by
                                           `make check-daemon`)

Checks one-shot mode (a command on the command line), stdin mode, and a
short interactive session on a pty: the status bar, a move, and quitting.
The look of the bar is checked by hand. Prints PASS / FAIL per check; the
exit status is the number of failures.
"""

import fcntl
import json
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_daemon as cd   # noqa: E402  (Bench, Dummy, Client, check)
from check_daemon import acked, check, err   # noqa: E402

M0 = "microsteps = 4\nspeed = 300\naccel = 1000\npulse_width_us = 10\n"


def cli(b, *args, stdin=None, timeout=10):
    """Run nimhub-cli; return (exit status, stdout lines, stderr)"""
    p = subprocess.run([b.cli, "-s", b.socket, *args], input=stdin, capture_output=True,
                       text=True, timeout=timeout)
    return p.returncode, p.stdout.splitlines(), p.stderr


def replies(lines):
    """The JSON objects among the lines"""
    out = []
    for line in lines:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if isinstance(msg, dict):
            out.append(msg)
    return out


def one(lines):
    r = replies(lines)
    return r[0] if len(r) == 1 else None


def wait_idle(b, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rc, out, _ = cli(b, "m0", "status")
        st = one(out)
        if st is None or not st.get("moving"):
            return st
        time.sleep(0.1)
    return None


def batch_checks(b):
    rc, out, _ = cli(b, "devices")
    r = one(out)
    check("one-shot devices: the firmware's devices, exit 0",
          rc == 0 and r is not None and r.get("ack") is True
          and r.get("devices") == [{"dev": "m0", "type": "stepper"},
                                   {"dev": "m1", "type": "stepper"}], f"rc {rc} {out}")

    rc, out, _ = cli(b, "m0", "status")
    r = one(out)
    check("one-shot m0 status: the reply as nimhubd sent it",
          rc == 0 and r is not None and r.get("seq") == 1 and r.get("pos") == 0
          and r.get("moving") is False, f"rc {rc} {out}")

    # A write: lease, the write, release (D-R18); only the write's reply is shown
    rc, out, _ = cli(b, "m0", "rel", "400")
    check("one-shot m0 rel 400: the lease taken by itself, accepted, exit 0",
          rc == 0 and one(out) == {"seq": 2, "ack": True}, f"rc {rc} {out}")
    st = wait_idle(b)
    check("...and the move ends at 400", st is not None and st.get("pos") == 400, str(st))
    other = cd.Client(b.socket)
    check("...and the lease released after it", acked(other.call("lease")))

    rc, out, _ = cli(b, "m0", "rel", "10")
    r = one(out)
    check("a write while another client holds the lease: its lease_held reply, exit 1",
          rc == 1 and r is not None and r.get("seq") == 1 and r.get("err") == "lease_held",
          f"rc {rc} {out}")
    rc, out, _ = cli(b, "m0", "status")
    check("...and nothing moved, reads still work", rc == 0 and one(out).get("moving") is False
          and one(out).get("pos") == 400, f"rc {rc} {out}")
    other.call("release")
    other.close()

    rc, out, _ = cli(b, "m0", "rel", "-100")
    check("one-shot with a negative argument (not taken as an option)",
          rc == 0 and one(out) == {"seq": 2, "ack": True}, f"rc {rc} {out}")
    st = wait_idle(b)
    check("...and the move ends at 300", st is not None and st.get("pos") == 300, str(st))

    rc, out, _ = cli(b, "m7", "status")
    r = one(out)
    check("one-shot error: printed, exit 1",
          rc == 1 and r is not None and r.get("err") == "unknown_dev", f"rc {rc} {out}")

    rc, out, _ = cli(b, '{"seq":77,"cmd":"devices"}')
    r = one(out)
    check("one-shot raw JSON: its seq is kept", rc == 0 and r is not None and r.get("seq") == 77,
          f"rc {rc} {out}")

    rc, out, err = cli(b, "m0", "rel", "1", "2")
    check("one-shot with too many words: error, nothing sent",
          rc == 1 and out == [] and "too many words" in err, f"rc {rc} {out} {err}")

    stdin = ("m0 pos\n"
             "help\n"
             "\n"
             "a b c d\n"
             '{"cmd":"devices"}\n'
             '{"seq":"x","cmd":"estop"}\n'
             "m0 stop\n"
             "quit\n"
             "m0 pos\n")
    rc, out, err = cli(b, stdin=stdin)
    msgs = replies(out)
    check("stdin mode: one reply or event per request, nothing after quit",
          [m.get("seq", m.get("evt")) for m in msgs] == [1, 2, "bad_request", 4], str(msgs))
    check("stdin mode: pos, devices, then bad_request, then not_moving",
          len(msgs) == 4 and "pos" in msgs[0] and "devices" in msgs[1]
          and msgs[3].get("err") == "not_moving", str(msgs))
    check("stdin mode: help printed", any("<dev> rel <steps>" in line for line in out))
    check("stdin mode: local error on stderr, exit 1", rc == 1 and "too many words" in err,
          f"rc {rc} {err}")

    # The user's own lease: kept across writes, no automatic lease around them
    rc, out, _ = cli(b, stdin="lease\nm0 zero\nm0 lock\nm0 status\nrelease\n")
    msgs = replies(out)
    check("stdin mode with lease: lease, two writes, status, release; nothing else",
          rc == 0 and [m.get("seq") for m in msgs] == [1, 2, 3, 4, 5]
          and msgs[0].get("timeout_ms") == 600 and msgs[3].get("pos") == 0
          and msgs[3].get("pos_valid") is True and msgs[3].get("locked") is True, str(msgs))
    rc, out, _ = cli(b, "m0", "unlock")
    check("one-shot unlock", rc == 0, f"rc {rc} {out}")

    # Slower than the 600 ms lease: the CLI sends renew before a command once
    # half the timeout has passed, as only lease and renew keep it (D-R18)
    p = subprocess.Popen([b.cli, "-s", b.socket], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for line, pause in [("lease", 0.5), ("m0 zero", 0.5), ("m0 lock", 0.5),
                        ("m0 unlock", 0), ("release", 0)]:
        p.stdin.write(line + "\n")
        p.stdin.flush()
        time.sleep(pause)
    out, errs = p.communicate(timeout=10)
    msgs = replies(out.splitlines())
    check("stdin mode, slower than the lease: renewed before each command, all accepted",
          p.returncode == 0 and len(msgs) == 5 and all(m.get("ack") for m in msgs)
          and errs == "", f"rc {p.returncode} {msgs} {errs}")

    p = subprocess.run([b.cli, "-s", os.path.join(b.tmp.name, "none.sock"), "devices"],
                       capture_output=True, text=True)
    check("nimhubd not running: exit 1, with why",
          p.returncode == 1 and "cannot connect" in p.stderr, f"rc {p.returncode} {p.stderr}")


class Pty:
    """nimhub-cli on a pty, 100 x 30"""

    def __init__(self, b):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(b.cli, [b.cli, "-s", b.socket])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        self.out = b""

    def read(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                return
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                return
            if not data:
                return
            self.out += data

    def wait_for(self, text, seconds=3.0):
        deadline = time.monotonic() + seconds
        while text.encode() not in self.out and time.monotonic() < deadline:
            self.read(0.1)
        return text.encode() in self.out

    def type(self, keys):
        os.write(self.fd, keys.encode())

    def exit_code(self, seconds=3.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return os.waitstatus_to_exitcode(status)
            self.read(0.1)
        return None


def interactive_checks(b):
    t = Pty(b)
    check("interactive: prompt", t.wait_for("nimhub> "))
    check("interactive: status bar with m0 and m1",
          t.wait_for("m0 0 (invalid) idle free") and t.wait_for("m1 0 (invalid) idle free"),
          repr(t.out[-400:]))
    t.type("m0 rel 400\r")
    check("interactive: reply printed", t.wait_for('{"seq":'), repr(t.out[-400:]))
    check("interactive: the bar follows the move", t.wait_for("moving", 2.0)
          and t.wait_for("m0 400 (invalid) idle free", 5.0), repr(t.out[-400:]))

    # The user's own lease: shown in the bar, renewed in the background
    other = cd.Client(b.socket)
    t.type("lease\r")
    check("interactive: lease shown in the bar", t.wait_for("lease: yours", 2.0),
          repr(t.out[-300:]))
    time.sleep(1.5)     # 2.5 times the 600 ms timeout
    check("interactive: the lease is renewed while held", err(other.call("lease"), "lease_held"))
    t.type("release\r")
    time.sleep(0.5)
    check("interactive: release frees it", acked(other.call("lease")))
    other.call("release")
    other.close()
    t.type("m0 sta\t")
    check("interactive: tab completes a device command", t.wait_for("m0 status ", 1.0),
          repr(t.out[-200:]))
    t.type("\r")    # Enter takes the completion; Enter again sends it
    t.type("\r")
    check("interactive: the completed command is sent", t.wait_for('"locked":false}', 2.0),
          repr(t.out[-300:]))
    t.type("\x15")      # Ctrl-U: clear the line
    t.type("\x03")      # Ctrl-C on an empty line quits
    rc = t.exit_code()
    check("interactive: Ctrl-C on an empty line quits, exit 0", rc == 0, f"rc {rc}")


def main():
    nimhubd = sys.argv[1] if len(sys.argv) > 1 else "build/nimhubd"
    nimhub_cli = sys.argv[2] if len(sys.argv) > 2 else "build/nimhub-cli"
    for p in (nimhubd, nimhub_cli):
        if not os.access(p, os.X_OK):
            sys.exit(f"{p}: not found; run make first")
    b = cd.Bench(nimhubd)
    b.cli = os.path.abspath(nimhub_cli)
    dummy = cd.Dummy(b.tty, "--objects", "m0:stepper,m1:stepper")
    d = b.daemon(b.ini(m0=M0, daemon="lease_timeout_ms = 600"))
    try:
        if d.wait_for("handshake: done") is None:
            sys.exit("nimhubd didn't start:\n" + d.dump())
        batch_checks(b)
        interactive_checks(b)
    finally:
        d.stop()
        dummy.stop()
    print(f"{cd.failures} failure(s)" if cd.failures else "all checks passed")
    sys.exit(min(cd.failures, 125))


if __name__ == "__main__":
    main()
