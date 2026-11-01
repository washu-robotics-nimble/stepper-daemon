#!/usr/bin/env python3
"""Flood the host link with ping while a move runs (F-R3). Standard library only.

Usage:
    flood.py DEVICE STEPS [OID]

Sends one move, then pings as fast as the link takes them until the move
ends. Prints the move's events and every fault or rx_error as they arrive,
then how many pings were sent and answered. Measure the step timing on the
analyzer meanwhile: it must not change with the traffic.

Example:
    flood.py /dev/serial/by-id/<board> 2000
"""

import json
import os
import select
import sys
import time

from hostlink import open_port

DRAIN_S = 0.5


def frame(seq, **fields):
    return json.dumps({"seq": seq, **fields}, separators=(",", ":")).encode() + b"\n"


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    fd = open_port(sys.argv[1])
    steps = int(sys.argv[2])
    oid = sys.argv[3] if len(sys.argv) == 4 else "m0"

    move = frame(1, cmd="move", oid=oid, steps=steps)
    print(f"> {move.decode().strip()}", flush=True)
    os.write(fd, move)

    seq = 1
    pings = pongs = 0
    rx = b""
    moving = True
    end = None   # drain deadline, once the move has ended
    start = time.monotonic()

    while end is None or time.monotonic() < end:
        wlist = [fd] if moving else []
        r, w, _ = select.select([fd], wlist, [], 0.1)
        if w:
            seq += 1
            os.write(fd, frame(seq, cmd="ping"))
            pings += 1
        if r:
            rx += os.read(fd, 4096)
            *lines, rx = rx.split(b"\n")
            for line in lines:
                try:
                    msg = json.loads(line)
                except ValueError:
                    print(f"< {line.decode(errors='replace')}", flush=True)
                    continue
                if not isinstance(msg, dict):
                    continue
                if msg.get("seq", 1) != 1 and "evt" not in msg:
                    pongs += 1   # a ping reply
                    continue
                print(f"< {line.decode(errors='replace')}", flush=True)
                if msg.get("evt") == "move_end" or msg.get("ack") is False:
                    moving = False
                    end = time.monotonic() + DRAIN_S

    elapsed = time.monotonic() - start - DRAIN_S
    print(f"{pings} pings sent, {pongs} answered, in {elapsed:.2f} s", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
