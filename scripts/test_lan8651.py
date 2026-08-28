#!/usr/bin/env python3
"""
test_lan8651.py - Verify the LAN8651 transmitter test modes end to end, no scope needed.

Each mode is checked on three independent levels:

  1. Register readback  - the firmware's own [VERIFY] verdict on T1STSTCTL (0x000308FB).
  2. Traffic stops      - the T1S endpoint's periodic frames stop arriving on eth1.
  3. Traffic resumes    - ... and come back once normal operation is restored.

Level 1 on its own only proves that the register latched the value. Levels 2 and 3 are
what show the PHY actually changed state - the part that would otherwise need an
oscilloscope. What none of this proves is whether the transmitted waveform conforms to
IEEE 802.3-2022 section 147.5.2; that remains a measurement task.

The traffic oracle is whatever the endpoint sends periodically on its own - by default
the SOME/IP-SD OFFER multicast at 1 Hz. Nothing is generated on the host, so counting it
does not perturb the bus, unlike polling registers during a throughput test.

Prerequisite: the bridge must be the PLCA coordinator (node id 0). Otherwise the endpoint
could keep transmitting on its own and a dead bridge transmitter would not silence the
bus, which would make level 2 meaningless. The script checks this and refuses to run
otherwise.

Usage:
  python test_lan8651.py --port COM8
  python test_lan8651.py --port COM8 --modes 1,2 --window 6
  python test_lan8651.py --list-interfaces
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import serial  # pyserial
from cli import drain  # reuse the console read window from cli.py

DEFAULT_TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
DEFAULT_IFACE = "Ethernet 8"
DEFAULT_ENDPOINT = "192.168.0.54"
DEFAULT_BPF = "src host {ip} and udp port 30490"  # SOME/IP-SD OFFER

T1STSTCTL = 0x000308FB
T1STSTCTL_MASK = 0xE000
PLCA_CTRL1 = 0x0004CA02

MODE_NAMES = {
    0: "normal operation",
    1: "output voltage / timing jitter",
    2: "output droop",
    3: "PSD mask / transmitter distortion",
    4: "transmitter high impedance",
}


# --------------------------------------------------------------------------- console


class Console:
    """The bridge's CLI over the EDBG virtual COM port."""

    def __init__(self, port, baud, read, verbose):
        self.read = read
        self.verbose = verbose
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.ser.reset_input_buffer()
        self.ser.write(b"\r\n")
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def send(self, cmd, read=None):
        self.ser.write((cmd + "\r\n").encode("ascii"))
        self.ser.flush()
        text = drain(self.ser, self.read if read is None else read)
        if self.verbose:
            print(f"    | $ {cmd}")
            for line in text.splitlines():
                line = line.strip()
                if line and line != ">":
                    print(f"    | {line}")
        return text

    def close(self):
        self.ser.close()


def reg_value(text, addr):
    """Pull the value out of 'LAN865X Read OK: Addr=0x... Value=0x...' for `addr`."""
    m = re.search(r"Read OK:\s*Addr=0x0*%X\s+Value=0x([0-9A-Fa-f]+)" % addr, text)
    return int(m.group(1), 16) if m else None


def read_reg(console, addr):
    return reg_value(console.send("lan_read 0x%08X" % addr), addr)


def verify_verdict(text):
    """The firmware prints [VERIFY] PASS / [VERIFY] FAIL after a verified write."""
    if "[VERIFY] PASS" in text:
        return "PASS"
    if "[VERIFY] FAIL" in text:
        return "FAIL"
    return None


# --------------------------------------------------------------------------- capture


def resolve_iface(tshark, name):
    """Map a friendly name such as 'Ethernet 8' to its \\Device\\NPF_{...} path.
    Passed through unchanged if it already looks like a device path or an index."""
    if name.startswith("\\Device") or name.isdigit():
        return name
    out = subprocess.run([tshark, "-D"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\d+\.\s+(\S+)\s+\((.+)\)\s*$", line)
        if m and m.group(2).strip().lower() == name.strip().lower():
            return m.group(1)
    raise SystemExit(
        f"ERROR: no capture interface named {name!r}.\n"
        f"       Run with --list-interfaces to see what is available."
    )


def count_frames(tshark, iface, bpf, seconds):
    """Capture for `seconds` and return how many frames matched the BPF filter."""
    proc = subprocess.run(
        [tshark, "-i", iface, "-f", bpf, "-a", f"duration:{seconds}",
         "-T", "fields", "-e", "frame.number", "-Q"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise SystemExit(f"ERROR: tshark failed: {proc.stderr.strip()}")
    return len([ln for ln in proc.stdout.splitlines() if ln.strip()])


# --------------------------------------------------------------------------- the test


class Report:
    def __init__(self):
        self.rows = []
        self.failures = 0

    def add(self, mode, level, detail, ok):
        self.rows.append((mode, level, detail, ok))
        if not ok:
            self.failures += 1

    def dump(self):
        print("\n" + "=" * 78)
        print("  LAN8651 transmitter test modes - verification report")
        print("=" * 78)
        width = max(len(r[1]) for r in self.rows) if self.rows else 10
        for mode, level, detail, ok in self.rows:
            tag = "PASS" if ok else "FAIL"
            label = "mode -" if mode is None else f"mode {mode}"
            print(f"  [{tag}] {label:8s} {level:<{width}s}  {detail}")
        print("-" * 78)
        if self.failures:
            print(f"  {self.failures} check(s) FAILED")
        else:
            print("  all checks passed")
        print("  Not covered: waveform, level, jitter and spectrum conformance -")
        print("  those need an oscilloscope or spectrum analyser at the MDI.")
        print("=" * 78)


def run(args, console, tshark, iface, bpf, report):
    # --- preconditions -----------------------------------------------------
    plca = read_reg(console, PLCA_CTRL1)
    if plca is None:
        raise SystemExit("ERROR: could not read PLCA_CTRL1 - is the firmware in APP_STATE_IDLE?")
    node_id, node_cnt = plca & 0xFF, (plca >> 8) & 0xFF
    print(f"  PLCA_CTRL1 = 0x{plca:08X}  (node id {node_id}, node count {node_cnt})")
    if node_id != 0:
        raise SystemExit(
            f"ERROR: this board is PLCA node {node_id}, not the coordinator.\n"
            f"       The traffic test needs node id 0, otherwise the endpoint can keep\n"
            f"       transmitting and 'traffic stopped' proves nothing.\n"
            f"       Fix with:  setenv plca_id 0  +  saveenv"
        )

    start = read_reg(console, T1STSTCTL)
    print(f"  T1STSTCTL  = 0x{start:08X}  (mode {(start & T1STSTCTL_MASK) >> 13})")
    report.add(None, "start state", f"T1STSTCTL=0x{start:08X}", start is not None and
               (start & T1STSTCTL_MASK) == 0)
    if start is None or (start & T1STSTCTL_MASK) != 0:
        raise SystemExit("ERROR: not in normal operation at start - run 'testmode 0' first.")

    baseline = count_frames(tshark, iface, bpf, args.window)
    print(f"  baseline traffic: {baseline} frame(s) in {args.window} s")
    ok = baseline >= args.min_frames
    report.add(None, "baseline traffic", f"{baseline} frames in {args.window}s "
                                        f"(need >= {args.min_frames})", ok)
    if not ok:
        raise SystemExit(
            f"ERROR: only {baseline} frame(s) from {args.endpoint} in {args.window} s.\n"
            f"       The oracle has to work before the modes can be judged against it.\n"
            f"       Check the endpoint, the cabling and the capture interface."
        )

    # --- one pass per mode -------------------------------------------------
    for mode in args.modes:
        print(f"\n--- test mode {mode}: {MODE_NAMES.get(mode, '?')} " + "-" * 24)

        text = console.send(f"testmode {mode}")
        verdict = verify_verdict(text)
        got = reg_value(text, T1STSTCTL)
        want = (mode & 0x7) << 13
        ok = verdict == "PASS" and got is not None and (got & T1STSTCTL_MASK) == want
        report.add(mode, "register readback",
                   f"[VERIFY] {verdict}, T1STSTCTL=0x{(got or 0):08X}, expected 0x{want:08X}", ok)
        print(f"  readback: {verdict}, T1STSTCTL=0x{(got or 0):08X}")

        during = count_frames(tshark, iface, bpf, args.window)
        ok = during <= args.tolerance
        report.add(mode, "traffic stopped",
                   f"{during} frame(s) during mode (allowed <= {args.tolerance})", ok)
        print(f"  traffic during mode: {during} frame(s)")

        text = console.send("testmode 0")
        verdict = verify_verdict(text)
        got = reg_value(text, T1STSTCTL)
        ok = verdict == "PASS" and got is not None and (got & T1STSTCTL_MASK) == 0
        report.add(mode, "revert readback",
                   f"[VERIFY] {verdict}, T1STSTCTL=0x{(got or 0):08X}", ok)

        if args.settle:
            time.sleep(args.settle)
        after = count_frames(tshark, iface, bpf, args.window)
        ok = after >= args.min_frames
        report.add(mode, "traffic resumed",
                   f"{after} frame(s) after revert (need >= {args.min_frames})", ok)
        print(f"  traffic after revert: {after} frame(s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM8", help="EDBG virtual COM port (default: COM8)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--read", type=float, default=1.5,
                    help="console read window per command, seconds (default: 1.5)")
    ap.add_argument("--tshark", default=DEFAULT_TSHARK)
    ap.add_argument("--iface", default=DEFAULT_IFACE,
                    help=f"capture interface, friendly name or device path (default: {DEFAULT_IFACE!r})")
    ap.add_argument("--endpoint", default=DEFAULT_ENDPOINT,
                    help=f"T1S endpoint whose periodic frames are the oracle (default: {DEFAULT_ENDPOINT})")
    ap.add_argument("--bpf", default=None,
                    help="override the capture filter (default: SOME/IP-SD from --endpoint)")
    ap.add_argument("--modes", default="1,2,3,4",
                    help="comma-separated modes to exercise (default: 1,2,3,4)")
    ap.add_argument("--window", type=int, default=4,
                    help="capture window per measurement, seconds (default: 4)")
    ap.add_argument("--min-frames", type=int, default=2,
                    help="frames that must arrive in a healthy window (default: 2)")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="frames tolerated while a test mode is active (default: 0)")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to wait after reverting before counting again (default: 2.0)")
    ap.add_argument("--verbose", action="store_true", help="echo all console traffic")
    ap.add_argument("--list-interfaces", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.tshark):
        raise SystemExit(f"ERROR: tshark not found at {args.tshark} (override with --tshark)")

    if args.list_interfaces:
        subprocess.run([args.tshark, "-D"])
        return 0

    if args.list_interfaces is False and args.bpf is None:
        args.bpf = DEFAULT_BPF.format(ip=args.endpoint)

    try:
        args.modes = [int(m) for m in args.modes.split(",") if m.strip() != ""]
    except ValueError:
        raise SystemExit("ERROR: --modes takes a comma-separated list of integers, e.g. 1,2,3,4")
    if any(m < 0 or m > 4 for m in args.modes):
        raise SystemExit("ERROR: modes must be in 0..4")

    iface = resolve_iface(args.tshark, args.iface)

    print("=" * 78)
    print("  LAN8651 test-mode verification")
    print("=" * 78)
    print(f"  console : {args.port} @ {args.baud}")
    print(f"  capture : {iface}")
    print(f"  filter  : {args.bpf}")
    print(f"  modes   : {args.modes}")
    print()

    report = Report()
    console = Console(args.port, args.baud, args.read, args.verbose)
    try:
        run(args, console, args.tshark, iface, args.bpf, report)
    finally:
        # Never leave the PHY in a test mode - a forgotten one looks like dead hardware.
        text = console.send("testmode 0", read=2.0)
        final = reg_value(text, T1STSTCTL)
        restored = final is not None and (final & T1STSTCTL_MASK) == 0
        report.add(None, "final cleanup",
                   f"T1STSTCTL=0x{(final if final is not None else 0xFFFFFFFF):08X}", restored)
        console.close()
        report.dump()
        if not restored:
            print("\n  WARNING: could not confirm normal operation - check 'testmode' by hand!")

    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
