"""Scratch: read a serial port for a fixed number of seconds and stop.

cli.py drains until the console goes quiet, which never happens while the
follower's per-cycle log is on - that is what hung the earlier attempt.
"""
import io
import sys
import time

import serial

port = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
cmds = sys.argv[3:]

with serial.Serial(port, 115200, timeout=0.2) as ser:
    for c in cmds:
        ser.write((c + "\r\n").encode())
        time.sleep(0.3)
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += ser.read(4096)
    txt = buf.decode("ascii", "replace")
    io.open("_capture.out", "w", encoding="utf-8", newline="").write(txt)
    print("captured %d bytes to _capture.out" % len(buf))
