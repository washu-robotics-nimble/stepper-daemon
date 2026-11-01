#!/usr/bin/env python3
"""Minimal host-link client for manual firmware tests. Standard library only.

Usage:
    hostlink.py [--wait] DEVICE [FRAME ...]

Each FRAME is sent as one line. A FRAME that is a JSON object without "seq"
gets one added (1, 2, 3, ...); anything else is sent as is, which is useful
for testing malformed frames. Every line received is printed. With no FRAME
arguments, frames are read from stdin, one per line, until end of input.

Without --wait, each frame is followed by a fixed listening window, which ends
0.3 s after the last line received; events of a longer move are missed.

With --wait, each frame is followed by:
  1. its reply: the line with the same "seq", or an rx_error event, since a
     dropped frame gets no reply. "! no reply" is printed after 2 s.
  2. every move started meanwhile (a move_start event), until its move_end.
  3. a 0.3 s quiet period, for anything sent after the last event.
The next frame is sent only after that.

Example:
    hostlink.py --wait /dev/serial/by-id/<board> '{"cmd":"move","oid":"m0","steps":2000}'
"""

import argparse
import json
import os
import select
import sys
import time
import tty

REPLY_WAIT_S = 0.3
REPLY_TIMEOUT_S = 2.0


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)  # no echo, no CR/LF translation
    return fd


class Link:
    def __init__(self, fd, echo=True):
        self.fd = fd
        self.echo = echo   # print every line sent and received
        self.seq = 0
        self.rx = b""
        self.moving = set()  # oids between move_start and move_end

    def send(self, frame):
        """Send one frame; return its seq, or None if it has none."""
        try:
            obj = json.loads(frame)
        except ValueError:
            obj = None
        if isinstance(obj, dict) and "seq" not in obj:
            self.seq += 1
            obj = {"seq": self.seq, **obj}
            frame = json.dumps(obj, separators=(",", ":"))
        if self.echo:
            print(f"> {frame}", flush=True)
        os.write(self.fd, frame.encode() + b"\n")
        return obj.get("seq") if isinstance(obj, dict) else None

    def receive(self, timeout):
        """Print the complete lines received within timeout seconds (None:
        no limit), and return them parsed; [] if nothing arrived in time.
        Lines that aren't JSON objects are printed but not returned."""
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return []
        self.rx += os.read(self.fd, 4096)
        *lines, self.rx = self.rx.split(b"\n")
        msgs = []
        for line in lines:
            if self.echo:
                print(f"< {line.decode(errors='replace')}", flush=True)
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if isinstance(msg, dict):
                msgs.append(msg)
        return msgs

    def poll(self, timeout):
        """Print every line received until timeout seconds pass with none."""
        while self.receive(timeout):
            timeout = REPLY_WAIT_S

    def wait(self, seq):
        """Wait for the reply to seq, then for every move started meanwhile
        to end, then for a quiet period (see the module docstring)."""
        replied = False
        reply_deadline = time.monotonic() + REPLY_TIMEOUT_S
        quiet_until = None
        while True:
            now = time.monotonic()
            if not replied:
                timeout = reply_deadline - now
                if timeout <= 0:
                    print("! no reply", flush=True)
                    replied = True
                    quiet_until = now + REPLY_WAIT_S
                    continue
            elif self.moving:
                timeout = None
            else:
                timeout = quiet_until - now
                if timeout <= 0:
                    return

            for msg in self.receive(timeout):
                evt = msg.get("evt")
                if evt == "rx_error" or (seq is not None and evt is None
                                         and msg.get("seq") == seq):
                    replied = True
                elif evt == "move_start":
                    self.moving.add(msg.get("oid"))
                elif evt == "move_end":
                    self.moving.discard(msg.get("oid"))
                quiet_until = time.monotonic() + REPLY_WAIT_S


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wait", action="store_true",
                        help="after each frame, wait for its reply and the end of any move")
    parser.add_argument("device")
    parser.add_argument("frames", nargs="*", metavar="FRAME")
    args = parser.parse_args()

    link = Link(open_port(args.device))
    frames = args.frames or (line.rstrip("\n") for line in sys.stdin)
    for frame in frames:
        seq = link.send(frame)
        if args.wait:
            link.wait(seq)
        else:
            link.poll(REPLY_WAIT_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
