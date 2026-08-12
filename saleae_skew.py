#!/usr/bin/env python3
"""
saleae_skew.py - measure how close two followers actually fire, with a logic
analyser instead of a firmware counter.

Everything before this measured the boards with the boards.  This is the first
number in the project that comes from an independent observer, which matters:
PTP_TIMEBASE_PLAN.md 0.5 argues that software cannot establish simultaneity, so
software also cannot be the final judge of it.

Both followers are toggling PD10 on the same ABSOLUTE grandmaster grid (see plan
G.1: a relative "every 20 ms from now" would never align them).  Each rising
edge on one channel should therefore have a partner on the other, and the spread
of those differences is the simultaneity the whole plan is about.

Channel mapping is not hardcoded - saleae_wiring_check.py --signature measures it
and this script takes it as arguments, because a probe gets moved eventually.
"""
import argparse
import glob
import os
import statistics
import struct
import sys

TMP = os.environ.get("TEMP", ".")


def parse_binary(path):
    b = open(path, "rb").read()
    if b[:8] != b"<SALEAE>":
        raise ValueError(f"{path}: not a Saleae binary export")
    initial = struct.unpack_from("<I", b, 16)[0]
    n = struct.unpack_from("<Q", b, 36)[0]
    times = list(struct.unpack_from(f"<{n}d", b, 44)) if n else []
    return initial, times


def rising_edges(initial, times):
    """The export gives every transition plus the state it started from, so the
    rising ones are every other entry - offset by whether it started low."""
    start = 0 if initial == 0 else 1
    return times[start::2]


def pair_and_stats(a, b, label_a, label_b, max_pair_s):
    """Match each edge on A with its nearest on B.  Nearest rather than index-wise
    on purpose: if one board misses a period the index alignment would silently
    shift and every later difference would be a full period out."""
    if not a or not b:
        print(f"  no edges: {label_a}={len(a)}, {label_b}={len(b)}")
        return None

    d = []
    j = 0
    for t in a:
        while j + 1 < len(b) and abs(b[j + 1] - t) <= abs(b[j] - t):
            j += 1
        if abs(b[j] - t) <= max_pair_s:
            d.append((b[j] - t) * 1e9)          # ns

    if len(d) < 4:
        print(f"  only {len(d)} pairs within {max_pair_s * 1e3:.1f} ms - "
              f"are both boards on the same grid?")
        return None

    med = statistics.median(d)
    dev = sorted(abs(v - med) for v in d)
    mad = dev[len(dev) // 2]
    centred = sorted(v - med for v in d)
    return dict(n=len(d), median=med, mad=mad,
                lo=centred[0], hi=centred[-1],
                p90=dev[int(0.90 * (len(dev) - 1))],
                p99=dev[int(0.99 * (len(dev) - 1))],
                stdev=statistics.pstdev(centred))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ch-a", type=int, default=1, help="follower A channel")
    ap.add_argument("--ch-b", type=int, default=0, help="follower B channel")
    ap.add_argument("--ch-master", type=int, default=3, help="bridge channel")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--sample-rate", type=int, default=50_000_000)
    ap.add_argument("--period-ms", type=float, default=20.0,
                    help="trigger period, only used to bound edge pairing")
    ap.add_argument("--tag", default="skew")
    ap.add_argument("--edges", choices=("all", "rising"), default="all",
                    help="which edges to pair. 'all' is the right default for a "
                         "TOGGLE output: every fire makes one transition, but its "
                         "polarity carries the parity of the fire count, so two "
                         "boards can be fire-synchronous and yet anti-phase in "
                         "rising edges - pairing rising only reports a whole "
                         "period of skew where there is none.")
    args = ap.parse_args()

    from saleae import automation

    chans = sorted({args.ch_a, args.ch_b, args.ch_master})
    outdir = os.path.join(TMP, "sal_" + args.tag)
    os.makedirs(outdir, exist_ok=True)
    for f in glob.glob(os.path.join(outdir, "*")):
        os.remove(f)

    print(f"capturing {args.seconds:.1f} s at {args.sample_rate / 1e6:.0f} MS/s "
          f"({1e9 / args.sample_rate:.0f} ns per sample), channels {chans}")

    with automation.Manager.connect(port=10430) as man:
        dc = automation.LogicDeviceConfiguration(
            enabled_digital_channels=chans, digital_sample_rate=args.sample_rate)
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=args.seconds))
        with man.start_capture(device_configuration=dc, capture_configuration=cc) as cap:
            cap.wait()
            cap.export_raw_data_binary(directory=outdir, digital_channels=chans)
            cap.save_capture(filepath=os.path.join(outdir, args.tag + ".sal"))

    edges = {}
    for ch in chans:
        init, times = parse_binary(os.path.join(outdir, f"digital_{ch}.bin"))
        r = times if args.edges == "all" else rising_edges(init, times)
        edges[ch] = r
        if len(r) >= 3:
            gaps = [(r[i + 1] - r[i]) * 1e3 for i in range(len(r) - 1)]
            print(f"  Ch{ch}: {len(times):5d} transitions, {len(r):5d} used, "
                  f"median gap {statistics.median(gaps):.3f} ms")
        else:
            print(f"  Ch{ch}: {len(times):5d} transitions, {len(r):5d} rising")

    # Pairing window: half a period, so an edge can only pair with its own slot.
    win = args.period_ms / 1000.0 * 0.5

    print(f"\nFOLLOWER A (Ch{args.ch_a}) vs FOLLOWER B (Ch{args.ch_b}):")
    s = pair_and_stats(edges[args.ch_a], edges[args.ch_b], "A", "B", win)
    if s:
        print(f"  pairs                : {s['n']}")
        print(f"  median offset        : {s['median']:+.0f} ns   <- fixed skew")
        print(f"  MAD                  : {s['mad']:.0f} ns")
        print(f"  stdev                : {s['stdev']:.0f} ns")
        print(f"  spread around median : {s['lo']:+.0f} .. {s['hi']:+.0f} ns")
        print(f"  peak-to-peak         : {s['hi'] - s['lo']:.0f} ns")
        print(f"  p90 / p99 |deviation|: {s['p90']:.0f} / {s['p99']:.0f} ns")

    if edges.get(args.ch_master):
        print(f"\nMASTER (Ch{args.ch_master}) has {len(edges[args.ch_master])} rising edges"
              " - absolute reference available")
    else:
        print(f"\nMASTER (Ch{args.ch_master}): idle. The bridge has no trigger yet, so"
              " this run measures follower-to-follower only.")
    print(f"\ncapture saved: {os.path.join(outdir, args.tag + '.sal')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
