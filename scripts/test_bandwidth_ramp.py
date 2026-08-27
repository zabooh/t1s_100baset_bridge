#!/usr/bin/env python3
"""
test_bandwidth_ramp.py - Find the practical throughput ceiling of the bridge's
normal L2 forwarding path (PC eth1 -> bridge -> eth0/T1S -> a follower node),
independent of the mirror/sniffer feature.

Talks to firmware/src/testserver.c's TCP echo server (see follower/firmware/src
for the copy running on the follower node): the client paces its own sends at
a target rate, the server echoes back everything unmodified, and the client
measures what actually came back. Repeated at increasing target rates until
the achieved rate falls meaningfully short of the target - that is the
ceiling.

The bridge itself does nothing special during this test - no mirror, no
sniffer, just its normal forwarding. Start the server first:

    (over the follower's console) testserver start

Usage:
  python test_bandwidth_ramp.py --host 192.168.0.202 --port 5566
  python test_bandwidth_ramp.py --host 192.168.0.202 --start-bps 10000 --max-bps 20000000
"""
import argparse
import socket
import sys
import threading
import time
from datetime import datetime

CHUNK_SIZE = 1024
STEP_DURATION_S = 3.0
DRAIN_GRACE_S = 1.0
SUCCESS_RATIO = 0.90       # achieved/target below this counts as "did not keep up"
CONSECUTIVE_FAILS_TO_STOP = 2

_log_lock = threading.Lock()
_t0 = None


def log(fh, line):
    stamp = time.time() - _t0
    text = "[t+%7.2fs] %s" % (stamp, line)
    with _log_lock:
        print(text)
        fh.write(text + "\n")
        fh.flush()


def human_bps(bps):
    if bps >= 1_000_000:
        return "%.2f Mbps" % (bps / 1_000_000)
    if bps >= 1_000:
        return "%.1f Kbps" % (bps / 1_000)
    return "%d bps" % bps


def run_step(fh, sock, target_bps, duration_s):
    """Client paces sends at target_bps; a background thread drains whatever
    the server echoes back as fast as it arrives. Returns
    (sent_bytes, received_bytes, achieved_bps, elapsed_s)."""
    pattern = bytes((i % 256) for i in range(CHUNK_SIZE))
    interval = (CHUNK_SIZE * 8.0) / target_bps  # seconds per chunk to hit target_bps

    received = [0]
    last_recv_time = [None]
    stop_event = threading.Event()

    def receiver():
        sock.settimeout(0.2)
        while not stop_event.is_set():
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    return
                received[0] += len(chunk)
                last_recv_time[0] = time.time()
            except socket.timeout:
                continue
            except OSError:
                return

    reader = threading.Thread(target=receiver, daemon=True)
    reader.start()

    sent = 0
    start = time.time()
    stop_at = start + duration_s
    next_send = start
    while time.time() < stop_at:
        now = time.time()
        if now >= next_send:
            try:
                sock.sendall(pattern)
            except OSError as error:
                log(fh, "  send error: %s" % error)
                break
            sent += CHUNK_SIZE
            next_send += interval
        else:
            time.sleep(min(0.002, next_send - now))
    send_done = time.time()

    # Adaptive drain: wait for the echoes still in flight rather than a fixed
    # pause - at low target rates a single chunk's round trip can take longer
    # than a fixed grace window, which was undercounting "achieved" down to
    # near-zero even though the data was genuinely still arriving (found
    # empirically comparing this against the iperf-measured ~5.8 Mbps ceiling
    # this same segment sustains - see FALLSTRICKE.md, 2026-08-27). Stop
    # early once nothing new has arrived for a full second - that is "done",
    # not "still coming".
    grace_deadline = send_done + max(DRAIN_GRACE_S, interval * 3)
    while time.time() < grace_deadline and received[0] < sent:
        time.sleep(0.05)
        if last_recv_time[0] is not None and (time.time() - last_recv_time[0]) > 1.0:
            break

    stop_event.set()
    reader.join(timeout=2)

    end_time = last_recv_time[0] if last_recv_time[0] is not None else send_done
    elapsed = max(end_time - start, 0.001)
    achieved_bps = (received[0] * 8.0) / elapsed
    return sent, received[0], achieved_bps, elapsed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="testserver's IP, e.g. 192.168.0.202")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--start-bps", type=float, default=10_000, help="first step's target rate")
    ap.add_argument("--max-bps", type=float, default=20_000_000, help="give up if the ramp reaches this without hitting a ceiling")
    ap.add_argument("--factor", type=float, default=1.6, help="target rate multiplier per step")
    ap.add_argument("--duration", type=float, default=STEP_DURATION_S, help="seconds of sending per step")
    ap.add_argument("--out", default=None, help="log file path (default: bandwidth_ramp_<timestamp>.txt)")
    args = ap.parse_args()

    global _t0
    _t0 = time.time()

    out_path = args.out or datetime.now().strftime("bandwidth_ramp_%Y%m%d_%H%M%S.txt")
    fh = open(out_path, "w", encoding="utf-8")
    print("Logging to %s" % out_path)

    log(fh, "connecting to %s:%d ..." % (args.host, args.port))
    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log(fh, "connected")

    target = args.start_bps
    consecutive_fails = 0
    best_good_bps = 0.0
    ceiling_bps = None

    try:
        while target <= args.max_bps:
            log(fh, "step: target=%s duration=%.1fs" % (human_bps(target), args.duration))
            sent, received, achieved, elapsed = run_step(fh, sock, target, args.duration)
            ratio = (achieved / target) if target > 0 else 0.0
            ok = ratio >= SUCCESS_RATIO
            log(fh, "  sent=%d bytes  received=%d bytes  achieved=%s (%.0f%% of target)  %s" %
                (sent, received, human_bps(achieved), ratio * 100.0, "OK" if ok else "SHORTFALL"))

            if ok:
                best_good_bps = max(best_good_bps, achieved)
                consecutive_fails = 0
            else:
                consecutive_fails += 1
                if ceiling_bps is None:
                    ceiling_bps = best_good_bps
                if consecutive_fails >= CONSECUTIVE_FAILS_TO_STOP:
                    log(fh, "stopping: %d consecutive shortfalls - ceiling reached" % consecutive_fails)
                    break

            target *= args.factor
        else:
            log(fh, "reached --max-bps (%s) without a clear ceiling" % human_bps(args.max_bps))

        log(fh, "=== RESULT === best sustained rate: %s" % human_bps(best_good_bps))
        if ceiling_bps is not None:
            log(fh, "=== RESULT === ceiling first seen around: %s" % human_bps(ceiling_bps))
    finally:
        sock.close()
        fh.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
