#!/usr/bin/env python3
"""smoketest.py - end-to-end smoke test for the flashed T1S<->100BASE-T bridge firmware.

Proves the core functions are alive after a build/flash:
  1. bridge IP reachable                (the bridge's own TCP/IP stack on eth1)
  2. L2 forwarding                       (PC -> bridge -> T1S -> endpoint, ICMP)
  3. on-board console + stack (optional) (UART 'stats' over --com)

Each check prints PASS/FAIL; the script exits non-zero if any CRITICAL check fails.
Checks whose port is unavailable are SKIPPED (not failed). No external deps
except pyserial (only for the optional --com UART check).

Usage:
  python smoketest.py                                   # defaults (.181 / .54)
  python smoketest.py --bridge 192.168.0.181 --endpoint 192.168.0.54 --com COM8
"""
import argparse, subprocess, sys, time

results = []   # (name, status, detail)  status in {"PASS","FAIL","SKIP"}
def record(name, status, detail=""):
    results.append((name, status, detail))
    mark = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "skip"}[status]
    print(f"  [{mark}] {name}" + (f"  - {detail}" if detail else ""))


def ping(host, timeout_s=1):
    if sys.platform == "win32":
        cmd = ["ping", "-n", "2", "-w", str(int(timeout_s * 1000)), host]
    else:
        cmd = ["ping", "-c", "2", "-W", str(int(timeout_s)), host]
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


# --- 1) bridge reachable -----------------------------------------------------------
def check_bridge(args):
    record("bridge IP reachable (%s)" % args.bridge,
           "PASS" if ping(args.bridge) else "FAIL")


# --- 2) L2 forwarding to the endpoint ----------------------------------------------
def check_forwarding(args):
    record("L2 forwarding to endpoint (%s via bridge)" % args.endpoint,
           "PASS" if ping(args.endpoint) else "FAIL")


# --- 3) on-board console + stack (optional, UART) -----------------------------------
def check_uart(args):
    if not args.com:
        record("on-board console (UART)", "SKIP", "no --com given")
        return
    try:
        import serial
    except ImportError:
        record("on-board console (UART)", "SKIP", "pyserial not installed")
        return
    try:
        ser = serial.Serial(args.com, 115200, timeout=0.1)
    except Exception as e:
        record("on-board console (UART)", "SKIP", f"{args.com} unavailable ({e})")
        return
    try:
        time.sleep(0.4); ser.write(b"\r"); ser.flush(); time.sleep(0.3)
        ser.reset_input_buffer(); ser.write(b"stats\r"); ser.flush()
        end = time.time() + 2.0; out = b""
        while time.time() < end:
            n = ser.in_waiting
            out += ser.read(n) if n else b""
            if not n:
                time.sleep(0.02)
        txt = out.decode(errors="replace")
        if "eth0" in txt and "eth1" in txt:
            record("on-board console (UART)", "PASS", "stats responded")
        else:
            record("on-board console (UART)", "FAIL", "no valid 'stats' output")
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description="Smoke test for the T1S<->100BASE-T bridge firmware")
    ap.add_argument("--bridge", default="192.168.0.181", help="bridge eth1 IP")
    ap.add_argument("--endpoint", default="192.168.0.54", help="LAN866x endpoint IP (behind the bridge)")
    ap.add_argument("--com", default=None, help="serial port for the optional UART check (e.g. COM8)")
    args = ap.parse_args()

    print(f"== bridge firmware smoke test ==  bridge={args.bridge} endpoint={args.endpoint}"
          + (f" com={args.com}" if args.com else ""))
    check_bridge(args)
    check_forwarding(args)
    check_uart(args)

    npass = sum(1 for _, s, _ in results if s == "PASS")
    nfail = sum(1 for _, s, _ in results if s == "FAIL")
    nskip = sum(1 for _, s, _ in results if s == "SKIP")
    print(f"\n== result: {npass} pass, {nfail} fail, {nskip} skip ==")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
