#!/usr/bin/env python3
"""test_mirror.py - differential check of the eth0->eth1 port mirror (SPAN).

The bridge's own ICMP to the T1S node never reaches eth1 on its own: the echo
request is unicast to the node's MAC (so the MAC bridge sends it out eth0 only)
and the reply is addressed to eth0's own MAC (so it terminates at the bridge).
Anything of that conversation seen on eth1 therefore arrived through the mirror.

Expectation:  mirror OFF -> 0 frames,  mirror ON -> > 0 frames.

Worth running after any MCC code regeneration. The TX half of the mirror depends
on a hand-patched call to mirror_eth0_tx_hook() inside the generated
drv_lan865x_api.c; regeneration removes that call silently, and the only symptom
is that the bridge's own outgoing frames stop appearing in the capture. This test
fails loudly instead.

Usage:  python test_mirror.py          (needs the T1S node reachable at .54)
"""
import subprocess
import sys
import threading
import time

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
IFACE = r"\Device\NPF_{5A4D39DB-ECBD-49EA-AAFE-A6856DB0DD0E}"   # Ethernet 8
BPF = "icmp and host 192.168.0.200"
PORT = "COM8"
WINDOW = 9

sys.path.insert(0, ".")
import serial
from cli import drain


def console(cmds, read=1.5):
    ser = serial.Serial(PORT, 115200, timeout=0.1)
    try:
        ser.reset_input_buffer()
        ser.write(b"\r\n")
        time.sleep(0.2)
        ser.reset_input_buffer()
        out = []
        for c in cmds:
            ser.write((c + "\r\n").encode())
            ser.flush()
            out.append(drain(ser, read))
        return "\n".join(out)
    finally:
        ser.close()


def measure(label):
    """Capture for WINDOW seconds while pinging the T1S node from the board."""
    result = {}

    def cap():
        p = subprocess.run([TSHARK, "-i", IFACE, "-f", BPF, "-a", f"duration:{WINDOW}",
                            "-T", "fields", "-e", "ip.src", "-e", "ip.dst", "-Q"],
                           capture_output=True, text=True)
        result["lines"] = [l for l in p.stdout.splitlines() if l.strip()]
        result["err"] = p.stderr.strip()

    t = threading.Thread(target=cap)
    t.start()
    time.sleep(1.5)                      # let the capture come up
    console(["ping 192.168.0.54"], read=5.0)
    t.join()

    lines = result.get("lines", [])
    print(f"  {label}: {len(lines)} mirrored ICMP frame(s)")
    for l in lines[:4]:
        print(f"      {l}")
    if result.get("err"):
        print(f"      tshark: {result['err']}")
    return len(lines)


print("=" * 66)
print("  Port mirror differential check")
print("=" * 66)

print(console(["mirror 0"]).strip().splitlines()[-1])
off = measure("mirror OFF")

print(console(["mirror 1"]).strip().splitlines()[-1])
on = measure("mirror ON ")

print(console(["mirror 0"]).strip().splitlines()[-1])

print("-" * 66)
ok = (off == 0) and (on > 0)
print(f"  OFF={off}  ON={on}  ->  {'PASS' if ok else 'FAIL'}")
if not ok:
    if off:
        print("  FAIL: frames arrived on eth1 with the mirror off - the filter is")
        print("        catching traffic that reaches eth1 natively.")
    if not on:
        print("  FAIL: nothing mirrored. Check that the driver patch calling")
        print("        mirror_eth0_tx_hook() is still in drv_lan865x_api.c, and that")
        print("        the ping actually reached the node.")
print("=" * 66)
sys.exit(0 if ok else 1)
