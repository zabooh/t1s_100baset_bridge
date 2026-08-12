#!/usr/bin/env python3
"""
tb_capture.py - collect raw (L, t1) pairs from the follower for phase A of
PTP_TIMEBASE_PLAN.md, and report the numbers that decide whether the approach
carries.

Why this exists instead of cli.py: cli.py's drain() extends its read window by
0.5 s on every arriving byte, so against a log that emits a line every 100 ms it
never returns - it hangs holding the COM port, which then makes the next
invocation block on open.  This script reads for a fixed wall-clock window and
always turns the log back off, even on Ctrl-C.

Usage:
  python tb_capture.py --port COM10 --seconds 60
  python tb_capture.py --port COM10 --seconds 60 --raw pairs.txt
  python tb_capture.py --analyse pairs.txt        # re-run the maths offline
"""
import argparse
import re
import statistics
import sys
import time

LINE = re.compile(r"\[TB\]\s+seq=(\d+)\s+L=(\d+)\s+t1=(\d+)\s+t2=(\d+)")

# SYS_TIME runs on TC0 at 60 MHz (plib_tc0.c TC0_TimerFrequencyGet).  The
# firmware prints the value it actually uses when 'ptpf tb' is called without an
# argument; this is only the default for offline re-analysis.
DEFAULT_TICK_HZ = 60_000_000


def collect(port, baud, seconds, raw_path):
    import serial

    ser = serial.Serial(port, baud, timeout=0.2, write_timeout=5)
    text = []
    try:
        ser.reset_input_buffer()
        ser.write(b"\r\nptpf tb on\r\n")
        ser.flush()
        deadline = time.time() + seconds
        while time.time() < deadline:
            n = ser.in_waiting
            if n:
                text.append(ser.read(n).decode("utf-8", errors="replace"))
            else:
                time.sleep(0.02)
    finally:
        # Always stop the stream.  A follower left logging is not broken, but the
        # next reader inherits a port that never goes quiet.
        try:
            ser.write(b"\r\nptpf tb off\r\n")
            ser.flush()
            time.sleep(0.3)
            ser.reset_input_buffer()
        except Exception as exc:                      # noqa: BLE001
            print(f"warning: could not turn the log off: {exc}", file=sys.stderr)
        ser.close()

    blob = "".join(text)
    if raw_path:
        with open(raw_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(blob)
    return blob


def parse(blob):
    out = []
    for m in LINE.finditer(blob):
        seq, L, t1, t2 = (int(g) for g in m.groups())
        out.append((seq, L, t1, t2))
    return out


def analyse(pairs, tick_hz, block=32):
    if len(pairs) < 8:
        print(f"only {len(pairs)} pairs - too few to say anything")
        return 1

    ns_per_tick = 1e9 / tick_hz
    seqs = [p[0] for p in pairs]

    # Sequence gaps: a lost Sync/Follow_Up pair shows up here, and pairing across
    # a gap is the silent error the plan warns about.
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)

    # d = L*ns_per_tick - t1 is "how long after the master's egress did the
    # application see this frame", in ns.  Its constant part is Delta_min +
    # D_const and cancels in every difference; its LINEAR part is the rate
    # difference between the local tick source and the master's wall clock, and
    # it dominates d completely - over a minute it is milliseconds.
    #
    # So the delay has to be DETRENDED before its spread means anything.  This is
    # the trap PTP_TIMEBASE_PLAN.md B.2 names: filtering the raw difference picks
    # the first or last sample of every block depending on the drift sign, and
    # reports the drift as if it were jitter.
    t0 = pairs[0][2]
    x = [(p[2] - t0) for p in pairs]                  # master time since start, ns
    d = [p[1] * ns_per_tick - p[2] for p in pairs]

    def fit(xs, ys):
        n = len(xs)
        mx = sum(xs) / n
        my = sum(ys) / n
        sxx = sum((v - mx) ** 2 for v in xs)
        if sxx == 0.0:
            return my, 0.0
        sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
        slope = sxy / sxx
        return my - slope * mx, slope

    # Two passes: a plain least-squares fit is pulled up by the long tail, so
    # refit on the lower half of the residuals to sit closer to the true floor.
    icept, slope = fit(x, d)
    res = [dv - (icept + slope * xv) for xv, dv in zip(x, d)]
    keep = sorted(range(len(res)), key=lambda i: res[i])[: max(4, len(res) // 2)]
    icept, slope = fit([x[i] for i in keep], [d[i] for i in keep])
    res = [dv - (icept + slope * xv) for xv, dv in zip(x, d)]

    floor = min(res)
    r = [v - floor for v in res]                      # ns above the fastest sample

    print(f"pairs                  : {len(pairs)}")
    print(f"sequence gaps          : {gaps}")
    print(f"span                   : seq {seqs[0]} .. {seqs[-1]}")
    print(f"t1 interval (median)   : {statistics.median(b[2] - a[2] for a, b in zip(pairs, pairs[1:])) / 1e6:.3f} ms")
    print(f"capture length         : {x[-1] / 1e9:.1f} s")
    print()
    print(f"local tick source vs master wall clock: {slope * 1e6:+.1f} ppm")
    print("  (a measurement, not an error - the fit exists to determine it)")
    print()
    print("HANDOVER DELAY after detrending, ns above the fastest sample:")
    print(f"  median               : {statistics.median(r):.0f}")
    print(f"  stdev                : {statistics.pstdev(r):.0f}")
    print(f"  max                  : {max(r):.0f}")
    for q in (50, 90, 99):
        k = max(0, min(len(r) - 1, int(round(q / 100 * (len(r) - 1)))))
        print(f"  p{q:<2}                  : {sorted(r)[k]:.0f}")

    # Min-filter on the RESIDUAL, one winner per block - what phase B does.
    winners = []
    for i in range(0, len(pairs) - block + 1, block):
        j = min(range(i, i + block), key=lambda k: res[k])
        winners.append((pairs[j], res[j]))

    print()
    print(f"min-filter on the residual, blocks of {block}: {len(winners)} winners")
    if len(winners) >= 2:
        wz = [w[1] - floor for w in winners]
        spread = max(wz) - min(wz)
        print(f"  winner residual max  : {max(wz):.0f} ns")
        print(f"  SPREAD OF WINNERS    : {spread:.0f} ns   <- what phase B has to live with")
        a, b = winners[0][0], winners[-1][0]
        dt1 = b[2] - a[2]
        if dt1 > 0:
            print(f"  baseline             : {dt1 / 1e9:.1f} s")
            err = spread / dt1 * 1e6
            print(f"  rate uncertainty     : +/-{err:.5f} ppm  (winner spread / baseline)")
            print()
            print("  => two boards each with this uncertainty drift apart in holdover by")
            print(f"     about {err * 2 * 1e3:.1f} ns/s, so {err * 2 * 1e3 * 60 / 1000:.2f} us per minute")
    else:
        print("  not enough blocks for a baseline - capture longer")

    # A missed SYS_TIME overflow is a sharp spike at exactly 65536 ticks.
    period_ns = 65536 * ns_per_tick
    near = [v for v in r if abs(v - period_ns) < 0.1 * period_ns]
    print()
    print(f"samples near one SYS_TIME period ({period_ns / 1e6:.3f} ms): {len(near)}"
          f"   <- missed 16-bit overflows, if any")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--block", type=int, default=32)
    ap.add_argument("--tick-hz", type=int, default=DEFAULT_TICK_HZ)
    ap.add_argument("--raw", help="write the captured console text here")
    ap.add_argument("--analyse", help="skip capturing, analyse this file instead")
    args = ap.parse_args()

    if args.analyse:
        with open(args.analyse, encoding="utf-8", errors="replace") as fh:
            blob = fh.read()
    else:
        print(f"capturing {args.seconds:.0f} s on {args.port} ...", file=sys.stderr)
        blob = collect(args.port, args.baud, args.seconds, args.raw)

    pairs = parse(blob)
    return analyse(pairs, args.tick_hz, args.block)


if __name__ == "__main__":
    sys.exit(main())
