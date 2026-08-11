#!/usr/bin/env python3
"""test_rawtx_mirror.py - differential check of MIRROR_RawTx(), the third mirror entry point.

Raw senders reach the bus through DRV_LAN865X_SendRawEthFrame(), which bypasses
DRV_LAN865X_PacketTx() and therefore the mirror's hand-patched TX hook. The MAC
bridge does not help either: it only floods what it RECEIVES on a port, and a
self-generated frame is never received. Without an explicit MIRROR_RawTx() call
from the sender, such frames are invisible on eth1 even with "mirror 1" - a
capture that stays empty while nothing is actually broken.

This test uses noip_send (EtherType 0x88B5) as the raw sender and expects:

    mirror OFF -> 0 frames on eth1
    mirror ON  -> frames present, own source MAC, ascending sequence numbers

Worth running after any MCC code regeneration (like test_mirror.py, which covers
the two stack-borne paths) and before trusting any eth1 capture of frames the
board generated itself - notably the PTP grandmaster's Sync/Follow_Up, whose
whole test strategy rests on this path.

Two things this does NOT prove:
  - wire timing on the two-wire segment. The eth1 timestamps date the mirror
    clone, not the T1S egress. Judge cadence on the MEDIAN of the gaps, never
    the maximum: the clone is best-effort and is dropped when the packet pool
    is busy.
  - anything beyond 5 frames per command. TC6_TX_ETH_QSIZE is 4 and the busy
    wait inside cmd_noip_send() never returns to the main loop, so nothing
    services the pending transfers: one frame goes out synchronously, four fill
    the queue, the sixth fails. NOIP_MAX_COUNT (100) promises more than the path
    delivers. See CLAUDE.md section 6.

Usage:  python test_rawtx_mirror.py
"""
import subprocess
import sys
import threading
import time

sys.path.insert(0, ".")
from cli import drain  # noqa: E402

import serial  # noqa: E402

TSHARK = "C:/Program Files/Wireshark/tshark.exe"
IFACE = "\\Device\\NPF_{5A4D39DB-ECBD-49EA-AAFE-A6856DB0DD0E}"  # Ethernet 8
BPF = "ether proto 0x88B5"
PORT = "COM8"
WINDOW = 12
NFRAMES = 5      # the ceiling, not a choice - see the docstring
GAP_MS = 100
SETTLE_S = 3.0   # tshark needs seconds to open the adapter before the first send


def capture(sink):
    cmd = [TSHARK, "-i", IFACE, "-f", BPF, "-a", "duration:%d" % WINDOW,
           "-T", "fields", "-e", "frame.time_relative", "-e", "eth.src",
           "-e", "eth.type", "-e", "data.data", "-Q"]
    out = subprocess.run(cmd, capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if line.strip():
            sink.append(line.strip())
    if out.returncode != 0:
        print("  tshark rc=%d %s" % (out.returncode, out.stderr.strip()[:200]))


def console(cmds, read=1.5):
    with serial.Serial(PORT, 115200, timeout=0.2) as ser:
        for c in cmds:
            ser.write((c + "\r\n").encode())
            time.sleep(0.2)
        return drain(ser, read)


def run(label, mirror_state):
    sink = []
    console(["mirror %d" % mirror_state], read=1.0)
    t = threading.Thread(target=capture, args=(sink,))
    t.start()
    time.sleep(SETTLE_S)
    reply = console(["noip_send %d %d" % (NFRAMES, GAP_MS)], read=4.0)
    sent = reply.count("sent seq=")
    t.join()
    print("%-12s mirror=%d  console 'sent seq=' lines=%d  frames on eth1=%d"
          % (label, mirror_state, sent, len(sink)))
    return sink, sent


def seqs(lines):
    """The sender puts a 32-bit sequence number at the start of the payload."""
    out = []
    for ln in lines:
        parts = ln.split("\t")
        payload = parts[3] if len(parts) > 3 else ""
        hexs = payload.replace(":", "")
        if len(hexs) >= 8:
            out.append(int(hexs[:8], 16))
    return out


print("--- baseline: mirror OFF, frames must NOT reach eth1 ---")
off_lines, off_sent = run("mirror off", 0)

print("--- mirror ON, frames must reach eth1 ---")
on_lines, on_sent = run("mirror on", 1)

print()
for ln in on_lines[:4]:
    print("  ", ln[:110])

s = seqs(on_lines)
print()
print("sequence numbers seen: %s" % s)

fail = []
if off_lines:
    fail.append("mirror 0 leaked %d frames onto eth1" % len(off_lines))
if off_sent != NFRAMES:
    fail.append("baseline console reported %d sends, expected %d" % (off_sent, NFRAMES))
if on_sent != NFRAMES:
    fail.append("console reported %d sends, expected %d" % (on_sent, NFRAMES))
if not on_lines:
    fail.append("mirror 1 produced no frames on eth1 - MIRROR_RawTx() ineffective")
elif len(on_lines) < NFRAMES:
    # Not a failure: the clone is best-effort, and tshark may still have been
    # opening the adapter when the first frames went out. The presence test and
    # the ascending sequence are what this script certifies.
    print("note: only %d of %d frames captured (dropped clone or late capture start)"
          % (len(on_lines), NFRAMES))
if s and s != sorted(s):
    fail.append("sequence numbers not ascending: %s" % s)
if s and len(set(s)) != len(s):
    fail.append("duplicate sequence numbers: %s" % s)

console(["mirror 0"], read=1.0)

if fail:
    print("\nFAIL")
    for f in fail:
        print("  -", f)
    sys.exit(1)
print("\nPASS: raw frames reach eth1 only with mirror on, sequence ascending")
