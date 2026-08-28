#!/usr/bin/env python3
"""
test_txd_impact.py - How does disabling the PLCA coordinator's transmitter
(T1SPMACTL.TXD) affect frame reception - on the bridge itself, and on traffic
between two OTHER T1S nodes ("follower A"/"follower B") that never talk to
the bridge at all?

Reproduces the manual run recorded in docs/FALLSTRICKE.md (2026-08-26): toggling
TXD on the coordinator collapsed iperf throughput between two non-coordinator
nodes to ~0.6% of normal and PLCA_STS.PST tracked the coordinator's TX state
exactly, but it stayed open whether the residual traffic during the outage is
genuine CSMA/CD fallback or something else. This script does not decide that
itself - it only captures everything (the bridge's frame counters + both
followers' raw CLI output, including iperf's own asynchronous status lines)
into ONE merged, timestamped log for offline analysis.

Needs three EDBG virtual COM ports open at once - the bridge (must be PLCA
coordinator, node id 0) and two follower nodes running firmware with the
iperf CLI command (see CLAUDE.md section 5).

Fixed sequence:
  1. bridge:     TXD=1 (transmitter off)
  2. bridge:     STATS6/STATS7 once/s for --window s
  3. follower A: iperf -s                                     (server, backgrounded on the device)
  4. follower B: iperf -c <follower-a-ip> -t <iperf-seconds>  (TCP client, backgrounded)
  5. bridge:     STATS6/STATS7 once/s for --window s
  6. bridge:     STATS6/STATS7 once/s for --window s
  7. bridge:     TXD=0 (transmitter on)
  8. bridge:     STATS6/STATS7 once/s for --window s

STATS6 (TFRX, total frames received including errors) and STATS7 (FRX,
error-free frames only) are both RC - reading clears them - so each printed
value is "frames since the previous read of that register", not a running
total; that is exactly what a once-per-second poll wants.

TXD is always restored to 0 on exit, including on error or Ctrl-C - a script
crash must never leave the bridge's coordinator silenced.

Usage:
  python test_txd_impact.py --bridge COM8 --follower-a COM9 --follower-b COM10
  python test_txd_impact.py --bridge COM8 --follower-a COM9 --follower-b COM10 --out run1.txt
"""
import argparse
import sys
import threading
import time
from datetime import datetime

import serial  # pyserial
from test_lan8651 import reg_value  # reuse the "Read OK: Addr=... Value=..." parser

T1SPMACTL = 0x000308F9
TXD_MASK = 0x4000
STATS6_TFRX = 0x0001020E  # Total Frames Received, including errors - RC (clears on read)
STATS7_FRX = 0x0001020F  # Frames Received without Error - RC (clears on read)

_log_lock = threading.Lock()
_t0 = None


def log(fh, tag, line):
    """One timestamped, tagged line to both stdout and the log file. Locked so the
    bridge (main thread) and the two follower reader threads never interleave a
    line mid-write - three sources writing the same file at once otherwise garbles
    lines into each other."""
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


def bridge_cmd(fh, ser, cmd, read=1.0):
    send(ser, cmd)
    text = drain(ser, read)
    for line in text.splitlines():
        line = line.strip()
        if line and line != ">":
            log(fh, "BRIDGE", line)
    return text


def poll_frame_counters(fh, ser, seconds):
    end = time.time() + seconds
    while time.time() < end:
        tick = time.time()
        t6 = bridge_cmd(fh, ser, "lan_read 0x%08X" % STATS6_TFRX, read=0.3)
        t7 = bridge_cmd(fh, ser, "lan_read 0x%08X" % STATS7_FRX, read=0.3)
        v6 = reg_value(t6, STATS6_TFRX)
        v7 = reg_value(t7, STATS7_FRX)
        log(fh, "BRIDGE", "frame counters: TFRX(total incl. errors)=%s FRX(error-free)=%s" % (v6, v7))
        time.sleep(max(0.0, tick + 1.0 - time.time()))


class FollowerReader:
    """Continuously drains one follower's COM port in the background so no
    asynchronous iperf output is ever missed, however long it takes to arrive -
    unlike the bridge, a follower's iperf session keeps talking on its own for
    up to --iperf-seconds, independent of anything this script sends it."""

    def __init__(self, fh, tag, port, baud):
        self.fh = fh
        self.tag = tag
        self.ser = open_console(port, baud)
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
    ap.add_argument("--bridge", required=True, help="bridge's EDBG COM port, e.g. COM8")
    ap.add_argument("--follower-a", required=True, help="follower A's COM port (runs iperf -s)")
    ap.add_argument("--follower-b", required=True, help="follower B's COM port (runs the iperf -c client)")
    ap.add_argument("--follower-a-ip", default="192.168.0.202",
                     help="follower A's T1S IP, used as the iperf -c target on follower B")
    ap.add_argument("--iperf-seconds", type=int, default=40, help="iperf -t on follower B")
    ap.add_argument("--window", type=int, default=10, help="seconds per frame-counter logging window")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default=None, help="log file path (default: txd_impact_<timestamp>.txt)")
    args = ap.parse_args()

    global _t0
    _t0 = time.time()

    out_path = args.out or datetime.now().strftime("txd_impact_%Y%m%d_%H%M%S.txt")
    fh = open(out_path, "w", encoding="utf-8")
    print("Logging to %s" % out_path)

    bridge = open_console(args.bridge, args.baud)
    follower_a = FollowerReader(fh, "FOLLOWER_A", args.follower_a, args.baud)
    follower_b = FollowerReader(fh, "FOLLOWER_B", args.follower_b, args.baud)

    try:
        log(fh, "TEST", "1. bridge: TXD=1 (transmitter off)")
        bridge_cmd(fh, bridge, "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, TXD_MASK))

        log(fh, "TEST", "2. bridge: frame counters, %ds" % args.window)
        poll_frame_counters(fh, bridge, args.window)

        log(fh, "TEST", "3. follower A: iperf -s")
        follower_a.send("iperf -s")
        time.sleep(1.0)

        log(fh, "TEST", "4. follower B: iperf -c %s -t %d" % (args.follower_a_ip, args.iperf_seconds))
        follower_b.send("iperf -c %s -t %d" % (args.follower_a_ip, args.iperf_seconds))

        log(fh, "TEST", "5. bridge: frame counters, %ds" % args.window)
        poll_frame_counters(fh, bridge, args.window)

        log(fh, "TEST", "6. bridge: frame counters, %ds" % args.window)
        poll_frame_counters(fh, bridge, args.window)

        log(fh, "TEST", "7. bridge: TXD=0 (transmitter on)")
        bridge_cmd(fh, bridge, "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, 0))

        log(fh, "TEST", "8. bridge: frame counters, %ds" % args.window)
        poll_frame_counters(fh, bridge, args.window)

        log(fh, "TEST", "done")
    finally:
        # TXD must never stay disabled just because something above raised -
        # idempotent if step 7 already ran.
        try:
            bridge_cmd(fh, bridge, "lan_rmw 0x%08X 0x%04X 0x%04X" % (T1SPMACTL, TXD_MASK, 0))
        except Exception as error:
            log(fh, "TEST", "WARNING: could not confirm TXD was restored: %s" % error)
        follower_a.close()
        follower_b.close()
        bridge.close()
        fh.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
