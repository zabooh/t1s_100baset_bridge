#!/usr/bin/env python3
"""test_ptp.py - verify the PTP grandmaster in the bridge from a Wireshark PC.

The device under test is the grandmaster on eth0 (10BASE-T1S); the instrument is
this PC on eth1. Its own Sync frames do not reach eth1 by themselves - the raw
send path bypasses the mirror's TX hook and the MAC bridge only floods what it
RECEIVES on a port - so the capture path is the port mirror's third entry point,
MIRROR_RawTx(), gated by "mirror 1". test_rawtx_mirror.py certifies that path on
its own; this script takes it as a precondition and sets it.

Unlike test_mirror.py and test_rawtx_mirror.py, which count frames, this one
dissects them: pyshark gives field-level access, and "is this a valid Sync?" is a
question about fields.

Checks, one assertion each so a failure names what is wrong:

    1  default off      no PTP frames before "ptp start"
    2  frames arrive    EtherType 0x88F7, count > 0
    3  version          ptp.v2.versionptp == 2
    4  message types    0x00 (Sync) and 0x08 (Follow_Up) both present
    5  pairing          per sequenceId exactly one Sync and one Follow_Up, Sync first
    6  sequence         strictly ascending, no gaps
    7  two-step         twoStepFlag set on Sync
    8  timestamp there  preciseOriginTimestamp != 0
    9  timestamp sane   nanoseconds < 1e9, seconds ascending
    10 cadence          median inter-Sync gap ~ the configured interval
    11 stop             no frames after "ptp stop"

Not covered here: wire timing on the two-wire segment (these are timestamps of
mirror clones, taken on a second MAC) and timestamp accuracy against a reference
clock. Both need an instrument or the follower from phase 2.

Usage:  python test_ptp.py [--interval MS] [--iface NAME] [--port COMn]
"""
import argparse
import statistics
import sys
import threading
import time

sys.path.insert(0, ".")
import serial  # noqa: E402
from cli import drain  # noqa: E402

TSHARK = "C:/Program Files/Wireshark/tshark.exe"
IFACE = "\\Device\\NPF_{5A4D39DB-ECBD-49EA-AAFE-A6856DB0DD0E}"  # Ethernet 8
PORT = "COM8"
BPF = "ether proto 0x88F7"
INTERVAL_MS = 250
CYCLES = 6              # Sync/Follow_Up pairs the measurement window should hold
SETTLE_S = 3.0          # tshark needs seconds to open the adapter before we look
IDLE_WINDOW_S = 3.0     # length of the two "must be silent" windows

MSG_SYNC = 0x00
MSG_FOLLOW_UP = 0x08

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def console(cmds, read=1.5):
    with serial.Serial(PORT, 115200, timeout=0.2) as ser:
        out = []
        for c in cmds:
            ser.write((c + "\r\n").encode())
            time.sleep(0.2)
            out.append(drain(ser, read))
        return "\n".join(out)


class Capture(threading.Thread):
    """tshark in a thread, returning dissected PTP frames as dicts."""

    FIELDS = ["frame.time_relative", "eth.src", "eth.type", "ptp.v2.versionptp",
              "ptp.v2.messagetype", "ptp.v2.sequenceid", "ptp.v2.flags.twostep",
              "ptp.v2.logmessageinterval",
              "ptp.v2.fu.preciseorigintimestamp.seconds",
              "ptp.v2.fu.preciseorigintimestamp.nanoseconds"]

    def __init__(self, seconds):
        super().__init__()
        self.seconds = seconds
        self.frames = []
        self.error = None

    def run(self):
        import subprocess
        cmd = [TSHARK, "-i", IFACE, "-f", BPF, "-a", "duration:%d" % self.seconds,
               "-T", "fields", "-Q"]
        for f in self.FIELDS:
            cmd += ["-e", f]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            self.error = "tshark rc=%d %s" % (p.returncode, p.stderr.strip()[:200])
        for line in p.stdout.splitlines():
            if not line.strip():
                continue
            cols = (line.split("\t") + [""] * len(self.FIELDS))[:len(self.FIELDS)]
            self.frames.append(dict(zip(self.FIELDS, cols)))


def num(frame, field, base=10):
    v = frame.get(field, "")
    if v in ("", None):
        return None
    try:
        return int(v, base) if isinstance(v, str) and v.startswith("0x") else int(v, base)
    except ValueError:
        return None


def capture_window(seconds, before=None, after=None):
    """Capture for `seconds`; run `before` once tshark is up, `after` at the end."""
    cap = Capture(seconds)
    cap.start()
    time.sleep(SETTLE_S)
    if before:
        before()
    cap.join()
    if after:
        after()
    if cap.error:
        print("  note:", cap.error)
    return cap.frames


def main():
    global TSHARK, IFACE, PORT, INTERVAL_MS
    ap = argparse.ArgumentParser()
    ap.add_argument("--tshark", default=TSHARK)
    ap.add_argument("--iface", default=IFACE)
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--interval", type=int, default=INTERVAL_MS)
    args = ap.parse_args()
    TSHARK, IFACE, PORT, INTERVAL_MS = args.tshark, args.iface, args.port, args.interval

    window = int(SETTLE_S + (INTERVAL_MS * CYCLES) / 1000.0 + 2)

    # Precondition, not a subject of the test: without the mirror the capture is
    # empty and a healthy grandmaster looks dead.
    reply = console(["ptp stop", "mirror 1"], read=1.0)
    if "mirror" not in reply.lower():
        print("  note: unexpected reply to 'mirror 1':", reply.strip()[:120])

    # --- 1: default/stopped state is silent ---------------------------------
    print("--- stopped: no PTP frames must reach eth1 ---")
    idle = capture_window(int(SETTLE_S + IDLE_WINDOW_S))
    print("    frames while stopped: %d" % len(idle))
    if idle:
        fail("%d PTP frames captured while the grandmaster was stopped" % len(idle))

    # --- 2..10: running ----------------------------------------------------
    print("--- running at %d ms: frames must arrive and dissect ---" % INTERVAL_MS)
    console(["ptp interval %d" % INTERVAL_MS], read=1.0)
    frames = capture_window(window,
                            before=lambda: console(["ptp start"], read=1.0),
                            after=lambda: console(["ptp stop"], read=1.0))
    print("    frames while running: %d" % len(frames))
    if not frames:
        fail("no PTP frames on eth1 while running - grandmaster or mirror path dead")
        report()
        return

    for f in frames:
        if num(f, "eth.type", 16) != 0x88F7:
            fail("frame with EtherType %s, expected 0x88f7" % f.get("eth.type"))
            break

    bad_ver = [f for f in frames if num(f, "ptp.v2.versionptp") != 2]
    if bad_ver:
        fail("%d frames with versionPTP != 2 (first: %r)"
             % (len(bad_ver), bad_ver[0].get("ptp.v2.versionptp")))

    syncs = [f for f in frames if num(f, "ptp.v2.messagetype", 16) == MSG_SYNC]
    fups = [f for f in frames if num(f, "ptp.v2.messagetype", 16) == MSG_FOLLOW_UP]
    print("    sync: %d   follow_up: %d" % (len(syncs), len(fups)))
    if not syncs:
        fail("no Sync messages (messageType 0x00)")
    if not fups:
        fail("no Follow_Up messages (messageType 0x08)")

    # pairing: one of each per sequenceId, Sync first. Ignore the first and last
    # sequence ids, whose partner can legitimately fall outside the window.
    order = [(num(f, "ptp.v2.sequenceid"), num(f, "ptp.v2.messagetype", 16)) for f in frames]
    seqs = [s for s, _ in order if s is not None]
    inner = set(seqs[1:-1])
    for s in sorted(inner):
        ms = [t for q, t in order if q == s]
        if ms.count(MSG_SYNC) != 1 or ms.count(MSG_FOLLOW_UP) != 1:
            fail("sequenceId %d has %d Sync and %d Follow_Up, expected one of each"
                 % (s, ms.count(MSG_SYNC), ms.count(MSG_FOLLOW_UP)))
        elif ms[0] != MSG_SYNC:
            fail("sequenceId %d: Follow_Up arrived before its Sync" % s)

    sync_seqs = [num(f, "ptp.v2.sequenceid") for f in syncs]
    if sync_seqs != sorted(sync_seqs):
        fail("Sync sequenceIds not ascending: %s" % sync_seqs)
    gaps = [b - a for a, b in zip(sync_seqs, sync_seqs[1:])]
    if any(g != 1 for g in gaps):
        fail("gap in the Sync sequence: %s" % sync_seqs)

    not_two_step = [f for f in syncs if str(f.get("ptp.v2.flags.twostep")).lower() != "true"]
    if not_two_step:
        fail("%d Sync messages without twoStepFlag" % len(not_two_step))

    # timestamps
    ts = []
    for f in fups:
        sec = num(f, "ptp.v2.fu.preciseorigintimestamp.seconds")
        ns = num(f, "ptp.v2.fu.preciseorigintimestamp.nanoseconds")
        if sec is None or ns is None:
            fail("Follow_Up without a preciseOriginTimestamp field")
            continue
        if sec == 0 and ns == 0:
            fail("preciseOriginTimestamp is zero - the TX capture was not ready")
            continue
        if ns >= 1000000000:
            fail("preciseOriginTimestamp nanoseconds %d >= 1e9" % ns)
        ts.append(sec * 1000000000 + ns)
    if ts:
        print("    first/last timestamp: %d.%09d / %d.%09d s"
              % (ts[0] // 10**9, ts[0] % 10**9, ts[-1] // 10**9, ts[-1] % 10**9))
        if ts != sorted(ts):
            fail("preciseOriginTimestamp not monotonic: %s" % ts)
        deltas = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
        if deltas:
            med = statistics.median(deltas)
            print("    median timestamp delta: %.3f ms (interval %d ms)" % (med, INTERVAL_MS))
            if abs(med - INTERVAL_MS) > max(20.0, 0.25 * INTERVAL_MS):
                fail("median timestamp delta %.1f ms, expected ~%d ms" % (med, INTERVAL_MS))

    # cadence on the wire to the PC. Median, not maximum: a mirror clone is
    # best-effort and is dropped when the packet pool is busy.
    at = [float(f["frame.time_relative"]) for f in syncs if f.get("frame.time_relative")]
    arr = [(b - a) * 1000.0 for a, b in zip(at, at[1:])]
    if arr:
        med = statistics.median(arr)
        print("    median arrival gap: %.2f ms (min %.2f, max %.2f)" % (med, min(arr), max(arr)))
        if abs(med - INTERVAL_MS) > max(25.0, 0.3 * INTERVAL_MS):
            fail("median Sync arrival gap %.1f ms, expected ~%d ms" % (med, INTERVAL_MS))
        if max(arr) > 2.5 * INTERVAL_MS:
            notes.append("largest arrival gap %.1f ms - a mirror clone was probably dropped"
                         % max(arr))

    # --- 11: stopped again --------------------------------------------------
    print("--- stopped again: must be silent ---")
    tail = capture_window(int(SETTLE_S + IDLE_WINDOW_S))
    print("    frames after stop: %d" % len(tail))
    if tail:
        fail("%d PTP frames after 'ptp stop'" % len(tail))

    report()


def report():
    console(["ptp stop", "mirror 0"], read=1.0)   # back to the starting state
    for n in notes:
        print("note:", n)
    if failures:
        print("\nFAIL")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("\nPASS: Sync/Follow_Up pairs valid, sequence intact, timestamps live, cadence right")


if __name__ == "__main__":
    main()
