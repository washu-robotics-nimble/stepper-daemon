#!/usr/bin/env python3
"""Minimal host-link client for manual firmware tests. Standard library only.

Usage:
    hostlink.py DEVICE [FRAME ...]

Each FRAME is sent as one line. A FRAME that is a JSON object without "seq"
gets one added (1, 2, 3, ...); anything else is sent as is, which is useful
for testing malformed frames. Every line received is printed. With no FRAME
arguments, frames are read from stdin, one per line, until end of input.

Example:
    hostlink.py /dev/serial/by-id/<board> '{"cmd":"identify"}' '{"cmd":"list_objects"}'
"""

import json
import os
import select
import sys
import tty

REPLY_WAIT_S = 0.3


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)  # no echo, no CR/LF translation
    return fd


class Link:
    def __init__(self, fd):
        self.fd = fd
        self.seq = 0
        self.rx = b""

    def send(self, frame):
        try:
            obj = json.loads(frame)
        except ValueError:
            obj = None
        if isinstance(obj, dict) and "seq" not in obj:
            self.seq += 1
            obj = {"seq": self.seq, **obj}
            frame = json.dumps(obj, separators=(",", ":"))
        print(f"> {frame}")
        os.write(self.fd, frame.encode() + b"\n")

    def poll(self, timeout):
        """Print every complete line received within timeout seconds."""
        ready, _, _ = select.select([self.fd], [], [], timeout)
        while ready:
            self.rx += os.read(self.fd, 4096)
            *lines, self.rx = self.rx.split(b"\n")
            for line in lines:
                print(f"< {line.decode(errors='replace')}")
            ready, _, _ = select.select([self.fd], [], [], REPLY_WAIT_S)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    link = Link(open_port(sys.argv[1]))

    frames = sys.argv[2:]
    if frames:
        for frame in frames:
            link.send(frame)
            link.poll(REPLY_WAIT_S)
        return

    for line in sys.stdin:
        link.send(line.rstrip("\n"))
        link.poll(REPLY_WAIT_S)


if __name__ == "__main__":
    main()
