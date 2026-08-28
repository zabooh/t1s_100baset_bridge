#!/usr/bin/env python3
"""
test_sniffer_repro.py - Reproduce the sniffer investigation setup, and
(--search) bisect the largest UDP datagram size that still makes it through:

  Bridge   (COM8,  node 7) - sniffer ON, a pure passive tap (its own T1S
                             transmitter is disabled while sniffer is on,
                             see port_mirror.c SNIFFER_Set()).
  Follower A (COM10,  192.168.0.202, node ?) - iperf UDP server.
  Follower B (COM23,  192.168.0.200, node 1) - iperf UDP client -> Follower A,
                             rate-limited to UDP_BANDWIDTH_BPS.

tshark captures on the PC's "Ethernet 8" (index 2) for the whole run, so the
mirrored T1S traffic between the two followers - which never touches the
bridge's own IP stack - can be inspected afterwards in Wireshark.

All three boards are reset before every run - manual testing showed a run
right after a previous one (no reset) can give a misleading result, so this
is not optional.

Usage:
  python test_sniffer_repro.py [--out PATH] [--duration SECS] [--datagram-size BYTES]
  python test_sniffer_repro.py --search [--low N] [--high N]

  Single run: --datagram-size omitted uses iperf's own default (1470).
  --search bisects between --low (known to pass, default 1400) and --high
  (known to fail, default 1470) until they are adjacent integers, resetting
  all three boards and re-running the full capture for every candidate size.
  Pass/fail is judged by re-reading --out afterwards with tshark: did the
  UDP stream to port 5001 actually show up in enough volume, or did it stop
  early (or never start) the way it does on a failing run.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial  # pyserial

SCRIPT_DIR = Path(__file__).resolve().parent
BRIDGE_PORT = "COM8"
BRIDGE_PROBE = "ATML3264031800001049"
FOLLOWER_A_PORT = "COM10"
FOLLOWER_A_PROBE = "ATML3264031800001290"
FOLLOWER_A_IP = "192.168.0.202"
FOLLOWER_B_PORT = "COM23"
FOLLOWER_B_PROBE = "ATML3264031800001103"
UDP_BANDWIDTH_BPS = 1000000  # rate-limit the UDP test, to check whether raw
                              # throughput itself is what trips the PC-side
                              # capture (see docs/FALLSTRICKE.md/this investigation)
BAUD = 115200
TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
IFACE = "2"  # "Ethernet 8", per `tshark -D`
IPERF_UDP_PORT = 5001
# A full run at UDP_BANDWIDTH_BPS pushes several hundred datagrams over the
# ~10s iperf default duration regardless of datagram size in the range we
# care about (1400-1470 bytes) - a failing run stops after a handful of
# packets (sometimes just one), so this threshold sits comfortably between
# the two without needing to compute the exact expected count per size.
PASS_FRAME_THRESHOLD = 200


def send(port, cmd, wait=1.0):
    """Send one CLI command, print+return whatever comes back within `wait` s."""
    ser = serial.Serial(port, BAUD, timeout=0.1)
    try:
        ser.reset_input_buffer()
        ser.write(b"\r\n")
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write((cmd + "\r\n").encode("ascii"))
        ser.flush()
        out = bytearray()
        deadline = time.time() + wait
        while time.time() < deadline:
            n = ser.in_waiting
            if n:
                out += ser.read(n)
                deadline = max(deadline, time.time() + 0.3)
            else:
                time.sleep(0.02)
        text = out.decode("utf-8", errors="replace")
        print(f"[{port}] {cmd}\n{text}")
        return text
    finally:
        ser.close()


def reset_board(probe, label):
    print(f"=== reset {label} ({probe}) ===")
    subprocess.run([sys.executable, "-m", "pyocd", "reset", "-t", "atsame54p20a",
                     "-u", probe], check=False,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def reset_all_boards():
    reset_board(BRIDGE_PROBE, "bridge")
    reset_board(FOLLOWER_A_PROBE, "follower A")
    reset_board(FOLLOWER_B_PROBE, "follower B")
    time.sleep(8)  # let all three finish booting


def capture_has_udp_stream(pcap_path):
    """Re-read the capture and count frames of the iperf UDP stream. That
    count, not the firmware's own counters, is the ground truth here - the
    whole point of this investigation is that the firmware thinks everything
    is fine while the PC-side capture silently stops receiving."""
    try:
        out = subprocess.run(
            [TSHARK, "-r", str(pcap_path), "-Y", f"udp.port=={IPERF_UDP_PORT}",
             "-T", "fields", "-e", "frame.number"],
            capture_output=True, text=True, check=False)
        frames = [l for l in out.stdout.splitlines() if l.strip()]
        count = len(frames)
        print(f"    udp.port=={IPERF_UDP_PORT} frames in capture: {count}")
        return count >= PASS_FRAME_THRESHOLD
    except Exception as exc:
        print(f"    capture analysis failed: {exc}")
        return False


def run_one(datagram_size, out_path, duration=45, iperf_wait=13.0):
    """Full cycle: reset all boards, capture, sniffer on, iperf UDP client/
    server at the given datagram size, sniffer off. Returns True if the UDP
    stream actually showed up in the resulting capture."""
    reset_all_boards()
    send(BRIDGE_PORT, "uptime")

    print("=== clean up any stuck iperf instances on both followers ===")
    send(FOLLOWER_A_PORT, "iperfk -i 0")
    send(FOLLOWER_B_PORT, "iperfk -i 0")

    print("=== start tshark capture ===")
    tshark = subprocess.Popen([TSHARK, "-i", IFACE, "-w", str(out_path),
                                "-a", f"duration:{duration}"])
    time.sleep(2)  # let the capture actually start before anything interesting happens

    print("=== bridge: sniffer ON (passive tap, TX disabled) ===")
    # sniffer alone is enough for RX-side mirroring of everything on the bus,
    # including traffic between the two followers - see MIRROR_Eth0Rx() in
    # port_mirror.c. 'mirror' only adds the bridge's OWN outgoing traffic,
    # which never happens in this test (the bridge only listens).
    send(BRIDGE_PORT, "sniffer 1", wait=2.0)

    print("=== follower A: iperf server (UDP) ===")
    send(FOLLOWER_A_PORT, "iperf -s -u")

    client_cmd = f"iperf -c {FOLLOWER_A_IP} -u -b {UDP_BANDWIDTH_BPS} -l {datagram_size}"
    print(f"=== follower B: iperf client -> follower A ({client_cmd}) ===")
    send(FOLLOWER_B_PORT, client_cmd, wait=iperf_wait)

    print("=== bridge: uptime + sniffer counters after the run ===")
    send(BRIDGE_PORT, "uptime")
    send(BRIDGE_PORT, "sniffer")

    print("=== bridge: sniffer OFF again (cleanup) ===")
    send(BRIDGE_PORT, "sniffer 0")

    remaining = duration - 2 - iperf_wait - 3
    if remaining > 0:
        print(f"=== waiting {remaining:.0f}s for the tshark capture window to close ===")
        time.sleep(remaining)
    tshark.wait()
    print(f"capture written to: {out_path}")

    return capture_has_udp_stream(out_path)


def search(low, high, out_path, duration):
    """Bisect [low, high] - low is known to pass, high is known to fail -
    down to adjacent integers."""
    assert low < high
    print(f"=== searching for the pass/fail boundary in [{low}, {high}] ===")
    print(f"--- baseline: confirm low={low} still passes ---")
    if not run_one(low, out_path, duration):
        print(f"low={low} did NOT pass on this run - bounds are stale, stopping.")
        return
    print(f"--- baseline: confirm high={high} still fails ---")
    if run_one(high, out_path, duration):
        print(f"high={high} PASSED on this run - it's no longer a valid upper "
              f"bound, stopping. Try a larger --high.")
        return

    while high - low > 1:
        mid = (low + high) // 2
        print(f"\n--- trying {mid} (current bounds: pass={low}, fail={high}) ---")
        ok = run_one(mid, out_path, duration)
        if ok:
            low = mid
            print(f"    -> PASS. new bounds: pass={low}, fail={high}")
        else:
            high = mid
            print(f"    -> FAIL. new bounds: pass={low}, fail={high}")

    print(f"\n=== boundary found: {low} bytes passes, {high} bytes fails ===")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(SCRIPT_DIR / "sniffer_repro_ab.pcapng"))
    ap.add_argument("--duration", type=int, default=45,
                     help="tshark capture duration in seconds")
    ap.add_argument("--datagram-size", type=int, default=1470,
                     help="UDP datagram size in bytes (iperf -l). Default: "
                          "1470, iperf's own default.")
    ap.add_argument("--search", action="store_true",
                     help="bisect the largest passing datagram size between "
                          "--low (known pass) and --high (known fail)")
    ap.add_argument("--low", type=int, default=1400)
    ap.add_argument("--high", type=int, default=1470)
    args = ap.parse_args()

    if args.search:
        search(args.low, args.high, args.out, args.duration)
    else:
        ok = run_one(args.datagram_size, args.out, args.duration)
        print(f"\nresult: {'PASS' if ok else 'FAIL'}")


if __name__ == "__main__":
    sys.exit(main())
