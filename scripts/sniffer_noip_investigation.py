#!/usr/bin/env python3
"""
sniffer_noip_investigation.py - Implements T1-T4 from docs/SNIFFER_2_TESTPLAN.md.

Uses noip_send (Follower B -> Bridge, EtherType 0x88B5, raw T1S frames) and
bigframe (Bridge -> eth1 directly) instead of iperf: no TCP/UDP overhead,
exact control over size/count/gap, and every noip frame carries a 4-byte
sequence number that's read straight out of the capture - so pass/fail is
judged by which sequence numbers actually arrived at the PC, not by the
firmware's own (previously shown to be misleading) counters.

Usage:
  python sniffer_noip_investigation.py t1 --size 1600 --count 200 --gap-ms 0
  python sniffer_noip_investigation.py t2 --size 1600 --count 200 --gap-ms 0
  python sniffer_noip_investigation.py t3 --size 1600 --count 200
  python sniffer_noip_investigation.py t4 --size 1600 --count 200 --gap-ms 0
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial  # pyserial

SCRIPT_DIR = Path(__file__).resolve().parent
BRIDGE_PORT = "COM8"
BRIDGE_PROBE = "ATML3264031800001049"
FOLLOWER_B_PORT = "COM23"
FOLLOWER_B_PROBE = "ATML3264031800001103"
BAUD = 115200
TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
IFACE = "2"  # "Ethernet 8", per `tshark -D`
NOIP_ETHERTYPE_FILTER = "eth.type==0x88b5"


def send(port, cmd, wait=1.0):
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
    subprocess.run(["python", "-m", "pyocd", "reset", "-t", "atsame54p20a",
                     "-u", probe], check=False,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def reset_bridge_and_follower_b():
    reset_board(BRIDGE_PROBE, "bridge")
    reset_board(FOLLOWER_B_PROBE, "follower B")
    time.sleep(8)


def start_capture(out_path, duration, bpf_filter=None):
    args = [TSHARK, "-i", IFACE, "-w", str(out_path), "-a", f"duration:{duration}"]
    if bpf_filter:
        args += ["-f", bpf_filter]
    return subprocess.Popen(args)


def read_noip_seqs(pcap_path):
    """Return the sorted list of sequence numbers of noip frames (eth.type ==
    0x88b5) actually present in the capture, read straight from the raw
    payload (first 4 bytes = big-endian seq, per NOIP_SeqFromFrame())."""
    out = subprocess.run(
        [TSHARK, "-r", str(pcap_path), "-Y", NOIP_ETHERTYPE_FILTER,
         "-T", "fields", "-e", "frame.time_relative", "-e", "data.data"],
        capture_output=True, text=True, check=False)
    seqs = []
    for line in out.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        hexdata = parts[1].replace(":", "")
        if len(hexdata) < 8:
            continue
        seq = int(hexdata[0:8], 16)
        seqs.append(seq)
    return sorted(seqs)


def parse_sent_count(follower_output):
    """Pull the last 'sent seq=N' out of noip_send's own console output."""
    matches = re.findall(r"sent seq=(\d+)", follower_output)
    return int(matches[-1]) if matches else None


def run_noip_burst(size, count, gap_ms, out_path, capture_duration, poll=False):
    """T1/T2/T3 core: reset bridge+follower B, sniffer on, send a noip burst,
    optionally poll bridge counters during the run, capture on the PC filtered
    to the noip EtherType, return a result dict.

    Drives the repetition from Python, ONE `noip_send 1 0 <size>` call per
    frame - NOT the firmware's own `noip_send <count> ...` loop. That loop
    reuses a single static buffer and TC6_SendRawEthernetPacket() only
    queues a pointer into it (see the NOIP_SendOne() comment in
    noip_test.c), so a multi-frame call corrupts/loses frames on its own,
    independent of anything this investigation is trying to isolate - worse
    the bigger the frame (longer SPI transfer = wider race window). One
    call per frame sidesteps that entirely, same as NOIP_SendOne() does for
    the PTP-trigger test."""
    reset_bridge_and_follower_b()
    send(BRIDGE_PORT, "uptime")
    send(BRIDGE_PORT, "noip_stat")

    tshark = start_capture(out_path, capture_duration, bpf_filter="ether proto 0x88b5")
    time.sleep(2)

    print("=== bridge: sniffer ON ===")
    send(BRIDGE_PORT, "sniffer 1", wait=2.0)

    print(f"=== follower B: {count}x 'noip_send 1 0 {size}' (one call per frame) ===")
    follower_lines = []
    poll_samples = []
    poll_every = max(1, count // 10)
    for i in range(count):
        out = send(FOLLOWER_B_PORT, f"noip_send 1 0 {size}", wait=0.15)
        follower_lines.append(out)
        if gap_ms:
            time.sleep(gap_ms / 1000.0)
        if poll and (i % poll_every == 0):
            poll_samples.append((i,
                                  send(BRIDGE_PORT, "sniffer", wait=0.3),
                                  send(BRIDGE_PORT, "stats", wait=0.3)))
    follower_out = "".join(follower_lines)
    if poll:
        print("=== T1 poll samples ===")
        for i, sniff, stats in poll_samples:
            print(f"--- frame #{i} ---\n{sniff}{stats}")

    print("=== bridge: noip_stat + sniffer counters after ===")
    send(BRIDGE_PORT, "noip_stat")
    send(BRIDGE_PORT, "sniffer")
    send(BRIDGE_PORT, "uptime")

    send(BRIDGE_PORT, "sniffer 0")

    print("=== waiting for the tshark capture window to close ===")
    tshark.wait()

    sent_last = parse_sent_count(follower_out)
    arrived = read_noip_seqs(out_path)
    result = {
        "size": size, "count": count, "gap_ms": gap_ms,
        "sent_last_seq": sent_last,
        "arrived_count": len(arrived),
        "arrived_last_seq": arrived[-1] if arrived else None,
        "arrived_seqs": arrived,
    }
    print(f"=== result: sent_last={sent_last} arrived={len(arrived)} "
          f"arrived_last={result['arrived_last_seq']} ===")
    return result


def t1(args):
    out = SCRIPT_DIR / "sniffer_t1.pcapng"
    run_noip_burst(args.size, args.count, args.gap_ms, out, args.duration, poll=True)


def t2(args):
    out = SCRIPT_DIR / "sniffer_t2.pcapng"
    r = run_noip_burst(args.size, args.count, args.gap_ms, out, args.duration)
    expected = set(range(1, (r["sent_last_seq"] or 0) + 1))
    missing = sorted(expected - set(r["arrived_seqs"]))
    print(f"missing sequence numbers: {len(missing)} "
          f"(first 20: {missing[:20]})")
    if missing and r["arrived_seqs"]:
        contiguous_tail = missing[0] <= (r["arrived_seqs"][-1] if r["arrived_seqs"] else 0)
        print("pattern: " + ("scattered loss" if len(missing) > 1 and
              missing[-1] - missing[0] > len(missing) else "hard stop at "
              f"seq={missing[0]}"))


def t3(args):
    for gap_ms in (0, 2, 5, 10, 20):
        print(f"\n########## T3: gap_ms={gap_ms} ##########")
        out = SCRIPT_DIR / f"sniffer_t3_gap{gap_ms}.pcapng"
        run_noip_burst(args.size, args.count, gap_ms, out, args.duration)


def t4(args):
    """bigframe in a tight loop, no T1S/follower involved at all."""
    reset_board(BRIDGE_PROBE, "bridge")
    time.sleep(8)
    out = SCRIPT_DIR / "sniffer_t4.pcapng"
    tshark = start_capture(out, args.duration, bpf_filter="ether proto 0xfeed")
    time.sleep(2)
    print(f"=== bridge: bigframe x{args.count} @ size={args.size} ===")
    for i in range(args.count):
        send(BRIDGE_PORT, f"bigframe {args.size}", wait=0.05)
        if args.gap_ms:
            time.sleep(args.gap_ms / 1000.0)
    send(BRIDGE_PORT, "uptime")
    remaining = args.duration - 2 - 3
    if remaining > 0:
        time.sleep(remaining)
    tshark.wait()
    out_bf = subprocess.run(
        [TSHARK, "-r", str(out), "-Y", "eth.type==0xfeed", "-T", "fields",
         "-e", "frame.number"], capture_output=True, text=True, check=False)
    n = len([l for l in out_bf.stdout.splitlines() if l.strip()])
    print(f"=== result: bigframe frames arrived at PC: {n}/{args.count} ===")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("test", choices=["t1", "t2", "t3", "t4"])
    # 1515 = the exact established failing boundary for a raw (no IP/UDP)
    # Ethernet frame - 1514 total (14 hdr + 1500 payload, no FCS) is the
    # standard max and known to pass; NOIP_MAX_FRAME_LEN caps at 1518.
    ap.add_argument("--size", type=int, default=1515)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--gap-ms", type=int, default=0)
    ap.add_argument("--duration", type=int, default=60)
    args = ap.parse_args()

    {"t1": t1, "t2": t2, "t3": t3, "t4": t4}[args.test](args)


if __name__ == "__main__":
    sys.exit(main())
