#!/usr/bin/env python3
"""
test_follower_txd_impact.py - Does disabling a NON-coordinator follower's own
transmitter (T1SPMACTL.TXD) disturb OTHER nodes' traffic that has nothing to
do with it?

Complements test_txd_impact.py, which toggled the *coordinator's* transmitter
and found it stalls the whole segment (FALLSTRICKE.md, 2026-08-26/27). Here
the bridge (node 0, coordinator) runs the iperf SERVER, follower A runs the
iperf CLIENT, and follower B - not a party to that traffic at all, just a
silent bystander on the shared bus - has its own transmitter toggled off and
back on while iperf keeps running. Expected result under the PLCA model this
project has been using: no effect on the bridge<->follower A transfer, since
a non-coordinator with nothing to send never had to be given a turn in the
first place. This script does not assume that - it only captures everything
for offline analysis.

Needs three EDBG virtual COM ports open at once - the bridge and both
followers must all expose lan_read/lan_rmw (i.e. run lan865x_diag.c
firmware like the bridge does, not just a plain iperf CLI).

Sequence:
  1. bridge:     iperf -s                                    (server)
  2. follower A: iperf -c <bridge-ip> -t <iperf-seconds>      (TCP client)
  3. follower B: STATS6/STATS7 once/s for --window s          (before)
  4. follower B: TXD=1 (transmitter off)
  5. follower B: STATS6/STATS7 once/s for --window s          (during)
  6. follower B: TXD=0 (transmitter on)
  7. follower B: STATS6/STATS7 once/s for --window s          (after)
  8. wait for iperf's own "Ready for the next session" line on the bridge's
     console (its summary, including the aggregate rate, prints right before
     that) - or --max-wait seconds, whichever comes first - so the log
     captures iperf's own final summary, not just a snapshot mid-run.

TXD on follower B is always restored to 0 on exit, including on error or
Ctrl-C - a script crash must never leave a node's transmitter silenced.

Usage:
  python test_follower_txd_impact.py --bridge COM8 --follower-a COM9 --follower-b COM10
  python test_follower_txd_impact.py --bridge COM8 --follower-a COM9 --follower-b COM10 \
      --bridge-ip 192.168.0.200 --iperf-seconds 60 --out run2.txt
"""
import argparse
import os
import sys
import threading
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import serial  # pyserial
from test_lan8651 import reg_value  # reuse the "Read OK: Addr=... Value=..." parser

T1SPMACTL = 0x000308F9
TXD_MASK = 0x4000
STATS6_TFRX = 0x0001020E  # Total Frames Received, including errors - RC (clears on read)
STATS7_FRX = 0x0001020F  # Frames Received without Error - RC (clears on read)
IPERF_DONE_MARKER = "Ready for the next session"

_log_lock = threading.Lock()
_t0 = None


def log(fh, tag, line):
    """One timestamped, tagged line to both stdout and the log file. Locked so the
    main thread and the two background readers never interleave a line mid-write."""
    stamp = time.time() - _t0
    text = "[t+%6.2fs] %-10s %s" % (stamp, tag, line)
    with _log_lock:
        print(text)
        fh.write(text + "\n")
        fh.flush()


def open_console(port, baud):
    ser = serial.Serial(port, baud, timeout=0.1)
    ser.reset_input_buffer()
    ser.write(b"\r\n")
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def send(ser, cmd):
    ser.write((cmd + "\r\n").encode("ascii"))
    ser.flush()


def drain(ser, seconds):
    out = bytearray()
    floor = time.time() + seconds
    deadline = floor
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
            deadline = max(floor, time.time() + 0.5)
        else:
            time.sleep(0.02)
    return out.decode("utf-8", errors="replace")


def send_and_log(fh, ser, tag, cmd, read=1.0):
    """Synchronous send-then-drain, for follower B's timed lan_read/lan_rmw polling -
    unlike the bridge and follower A here, follower B never emits anything
    unprompted, so there is no race with a background reader to worry about."""
    send(ser, cmd)
    text = drain(ser, read)
    for line in text.splitlines():
        line = line.strip()
        if line and line != ">":
            log(fh, tag, line)
    return text


def poll_frame_counters(fh, ser, tag, seconds):
    end = time.time() + seconds
    while time.time() < end:
        tick = time.time()
        t6 = send_and_log(fh, ser, tag, "lan_read 0x%08X" % STATS6_TFRX, read=0.3)
        t7 = send_and_log(fh, ser, tag, "lan_read 0x%08X" % STATS7_FRX, read=0.3)
        v6 = reg_value(t6, STATS6_TFRX)
        v7 = reg_value(t7, STATS7_FRX)
        log(fh, tag, "frame counters: TFRX(total incl. errors)=%s FRX(error-free)=%s" % (v6, v7))
        time.sleep(max(0.0, tick + 1.0 - time.time()))


class PassiveReader:
    """Continuously drains one node's COM port in the background so no
    asynchronous output (iperf's own status lines, its final summary) is ever
    missed. Optionally sets .done when a line contains `done_marker` - used
    on the bridge (the iperf server here) to know when the whole session,
    summary included, has actually finished."""

    def __init__(self, fh, tag, port, baud, done_marker=None):
        self.fh = fh
        self.tag = tag
        self.ser = open_console(port, baud)
        self.done = threading.Event()
        self._done_marker = done_marker
        self._stop = threading.Event()
        self._buf = ""
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            n = self.ser.in_waiting
            if n:
                self._buf += self.ser.read(n).decode("utf-8", errors="replace")
                while "\n" in self._buf:
                    line, self._buf = self._buf.split("\n", 1)
                    line = line.strip("\r").strip()
                    if line and line != ">":
                        log(self.fh, self.tag, line)
                        if self._done_marker and self._done_marker in line:
                            self.done.set()
            else:
                time.sleep(0.02)

    def send(self, cmd):
        log(self.fh, self.tag, "$ %s" % cmd)
        send(self.ser, cmd)

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)
        if self._buf.strip():
            log(self.fh, self.tag, self._buf.strip())
        self.ser.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bridge", required=True, help="bridge's EDBG COM port, e.g. COM8 (runs iperf -s)")
    ap.add_argument("--follower-a", required=True, help="follower A's COM port (runs the iperf -c client)")
    ap.add_argument("--follower-b", required=True, help="follower B's COM port (TXD toggled, not part of the iperf transfer)")
    ap.add_argument("--bridge-ip", default="192.168.0.200",
                     help="bridge's T1S IP (eth0/ip0), used as the iperf -c target on follower A")
    ap.add_argument("--iperf-seconds", type=int, default=60, help="iperf -t on follower A")
    ap.add_argument("--window", type=int, default=10, help="seconds per frame-counter logging window on follower B")
    ap.add_argument("--max-wait", type=int, default=None,
                     help="give up waiting for iperf's completion marker after this many seconds "
                          "(default: iperf-seconds + 60)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default=None, help="log file path (default: follower_txd_impact_<timestamp>.txt)")
    args = ap.parse_args()
    max_wait = args.max_wait if args.max_wait is not None else args.iperf_seconds + 60

    global _t0
    _t0 = time.time()

    out_path = args.out or datetime.now().strftime("follower_txd_impact_%Y%m%d_%H%M%S.txt")
    fh = open(out_path, "w", encoding="utf-8")
    print("Logging to %s" % out_path)

    bridge = PassiveReader(fh, "BRIDGE", args.bridge, args.baud, done_marker=IPERF_DONE_MARKER)
    follower_a = PassiveReader(fh, "FOLLOWER_A", args.follower_a, args.baud)
    follower_b = open_console(args.follower_b, args.baud)

    try:
        log(fh, "TEST", "1. bridge: iperf -s")
        bridge.send("iperf -s")
        time.sleep(1.0)

        log(fh, "TEST", "2. follower A: iperf -c %s -t %d" % (args.bridge_ip, args.iperf_seconds))
        follower_a.send("iperf -c %s -t %d" % (args.bridge_ip, args.iperf_seconds))

        log(fh, "TEST", "3. follower B: frame counters, %ds (before)" % args.window)
        poll_frame_counters(fh, follower_b, "FOLLOWER_B", args.window)

        log(fh, "TEST", "4. follower B: TXD=1 (transmitter off)")
        send_and_log(fh, follower_b, "FOLLOWER_B",
                      "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, TXD_MASK))

        log(fh, "TEST", "5. follower B: frame counters, %ds (during)" % args.window)
        poll_frame_counters(fh, follower_b, "FOLLOWER_B", args.window)

        log(fh, "TEST", "6. follower B: TXD=0 (transmitter on)")
        send_and_log(fh, follower_b, "FOLLOWER_B",
                      "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, 0))

        log(fh, "TEST", "7. follower B: frame counters, %ds (after)" % args.window)
        poll_frame_counters(fh, follower_b, "FOLLOWER_B", args.window)

        log(fh, "TEST", "8. waiting for iperf to finish on the bridge (max %ds)..." % max_wait)
        deadline = time.time() + max_wait
        while not bridge.done.is_set() and time.time() < deadline:
            time.sleep(0.2)
        if bridge.done.is_set():
            time.sleep(1.0)  # grace period for a trailing line or two after the marker
            log(fh, "TEST", "iperf finished")
        else:
            log(fh, "TEST", "WARNING: gave up waiting for iperf's completion marker after %ds" % max_wait)

        log(fh, "TEST", "done")
    finally:
        # TXD on follower B must never stay disabled just because something above
        # raised - idempotent if step 6 already ran.
        try:
            send_and_log(fh, follower_b, "FOLLOWER_B",
                          "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, 0))
        except Exception as error:
            log(fh, "TEST", "WARNING: could not confirm follower B's TXD was restored: %s" % error)
        bridge.close()
        follower_a.close()
        follower_b.close()
        fh.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
