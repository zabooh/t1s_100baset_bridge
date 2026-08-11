#!/usr/bin/env python3
"""test_servo_convergence.py - how long the follower's servo needs, as a diagram.

Restarts the servo from scratch, records one line per PTP cycle while it locks,
and turns that into numbers plus a plot: how long to each state, when the offset
first drops below one millisecond / one microsecond / 300 ns / 150 ns, and how
tightly it sits once settled.

    python test_servo_convergence.py
    python test_servo_convergence.py --interval 125 --seconds 60
    python test_servo_convergence.py --no-plot          numbers and CSV only

Console ports come from boards.json (written by setup_flasher.py), so the two
boards do not have to be named on the command line - override with
--bridge-port / --follower-port if needed.

Exit code 0 when the servo reached FINE inside the time budget and stayed within
the settling tolerance, non-zero otherwise, with the reason printed.

Output files (next to this script):
    servo_convergence.png    the diagram
    servo_convergence.csv    the raw samples, for a different plot or a report

The time axis comes from the PTP sequence number times the send interval, not
from host arrival times: the sequence is the master's own cadence and therefore a
cleaner clock than a Windows timestamp on a serial read.
"""
import argparse
import csv
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import serial  # noqa: E402

LOG_RE = re.compile(r"\[PTPF\] seq=(\d+)\s+offset=(-?\d+) ns\s+delta=(-?\d+) ns\s+"
                    r"(\w+)\s+rate=(-?\d+) ppb")
STATE_RE = re.compile(r"\[PTPF\] servo (\w+) -> (\w+)")
STATES = ["UNINIT", "MATCHFREQ", "HARDSYNC", "COARSE", "FINE"]
THRESHOLDS_NS = [1000000, 1000, 300, 150]     # 1 ms, 1 us, COARSE entry, FINE entry


def resolve_ports(args):
    """bridge and follower console ports, from boards.json where possible."""
    bridge, follower = args.bridge_port, args.follower_port
    if bridge and follower:
        return bridge, follower
    try:
        from setup_flasher import com_ports_by_serial, probe_for
        ports = com_ports_by_serial()
        bridge = bridge or ports.get(probe_for("bridge") or "")
        follower = follower or ports.get(probe_for("follower") or "")
    except Exception as exc:                      # noqa: BLE001 - best effort only
        print("note: could not read boards.json (%s)" % exc)
    return bridge or "COM8", follower or "COM10"


def send(port, cmds, settle=0.35):
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        for c in cmds:
            ser.write((c + "\r\n").encode())
            time.sleep(settle)
        return ser.read(4096).decode("ascii", "replace")


def ensure_master(port, interval_ms):
    """The grandmaster has to be sending, or there is nothing to converge onto."""
    reply = send(port, ["ptp interval %d" % interval_ms, "ptp start", "ptp status"], settle=0.5)
    if "sending: on" not in reply:
        print("could not start the grandmaster on %s. Reply was:" % port)
        print(reply.strip()[:400])
        return False
    return True


def cold_start(follower_port):
    """Reset the follower board, so the run starts from a defined state.

    "ptpf off" plus "ptpf on" is NOT a cold start: it leaves the corrected clock
    increment and the already-aligned time in the hardware, so the first sample is
    already within a few nanoseconds and every convergence time comes out as zero -
    which is exactly what the first version of this test reported. A board reset
    puts the wall clock back to zero and the increment back to nominal, which is
    the state a follower really wakes up in."""
    import subprocess
    tool = HERE / "flash_same54.py"
    rc = subprocess.call([sys.executable, str(tool), "--project", "follower", "--reset-only"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if rc != 0:
        print("note: could not reset the follower board (rc=%d) - measuring from the "
              "current state instead, so the convergence times mean nothing" % rc)
        return False
    time.sleep(4.0)                     # boot, stack up, app in its idle state
    return True


def record(port, seconds, interval_ms):
    """Start listening and read the per-cycle log while the servo locks."""
    samples, transitions = [], []
    t0 = None
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        # The board was just reset, so there is nothing to stop - and the capture
        # has to be open before the first sample, because the first seconds are the
        # interesting part.
        ser.reset_input_buffer()
        for c in ("ptpf log on", "ptpf servo on", "ptpf on"):
            ser.write((c + "\r\n").encode())
            time.sleep(0.35)

        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").strip()
                m = LOG_RE.search(text)
                if m:
                    seq, off, delta, state, rate = m.groups()
                    if t0 is None:
                        t0 = int(seq)
                    samples.append({
                        "t_s": (int(seq) - t0) * interval_ms / 1000.0,
                        "seq": int(seq),
                        "offset_ns": int(off),
                        "delta_ns": int(delta),
                        "state": state,
                        "rate_ppb": int(rate),
                    })
                    continue
                m = STATE_RE.search(text)
                if m and samples:
                    transitions.append((samples[-1]["t_s"], m.group(1), m.group(2)))
        ser.write(b"ptpf log off\r\n")
        time.sleep(0.3)
    return samples, transitions


def transitions_from(samples):
    """State changes derived from the samples themselves - the log's own transition
    lines say the same thing, but deriving them keeps a CSV re-plot complete."""
    out = []
    for a, b in zip(samples, samples[1:]):
        if a["state"] != b["state"]:
            out.append((b["t_s"], a["state"], b["state"]))
    return out


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    for r in rows:
        r["t_s"] = float(r["t_s"])
        for k in ("seq", "offset_ns", "delta_ns", "rate_ppb"):
            r[k] = int(r[k])
    return rows


def first_time_below(samples, limit_ns):
    """First moment |offset| goes below limit and stays below for 3 samples."""
    for i, s in enumerate(samples):
        window = samples[i:i + 3]
        if len(window) == 3 and all(abs(w["offset_ns"]) < limit_ns for w in window):
            return s["t_s"]
    return None


def settled_stats(samples, tail_fraction=0.25):
    tail = samples[max(0, int(len(samples) * (1.0 - tail_fraction))):]
    offs = [s["offset_ns"] for s in tail]
    if not offs:
        return None
    mean = sum(offs) / len(offs)
    var = sum((o - mean) ** 2 for o in offs) / len(offs)
    return {
        "n": len(offs),
        "from_s": tail[0]["t_s"],
        "mean": mean,
        "sigma": var ** 0.5,
        "min": min(offs),
        "max": max(offs),
        "p2p": max(offs) - min(offs),
        "state": tail[-1]["state"],
    }


def write_csv(samples, path):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["t_s", "seq", "offset_ns", "delta_ns", "state", "rate_ppb"])
        w.writeheader()
        w.writerows(samples)


def plot(samples, transitions, settled, interval_ms, path):
    import matplotlib
    matplotlib.use("Agg")                          # no display on a build machine
    import matplotlib.pyplot as plt

    t = [s["t_s"] for s in samples]
    off = [s["offset_ns"] for s in samples]
    absoff = [max(abs(o), 1) for o in off]         # 1 ns floor so a log axis works
    rate = [s["rate_ppb"] for s in samples]

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(11, 10), sharex=True,
                                        gridspec_kw={"height_ratios": [3, 2, 2]})

    ax1.semilogy(t, absoff, linewidth=0.9, color="#1f4e79")
    ax1.axhline(300, color="#c00000", linestyle="--", linewidth=0.8, label="COARSE entry, 300 ns")
    ax1.axhline(150, color="#548235", linestyle="--", linewidth=0.8, label="FINE entry, 150 ns")
    ax1.set_ylabel("|offset|  [ns]  (log)")
    ax1.set_title("PTP follower servo convergence   -   master interval %d ms, %d samples"
                  % (interval_ms, len(samples)))
    ax1.grid(True, which="both", alpha=0.25)

    # State changes as vertical markers: the shape of the curve and the state
    # machine's opinion of it should agree, and seeing both at once is the point.
    # Labels are staggered, because the first two transitions are a tenth of a
    # second apart and printed on top of each other otherwise.
    for x, _old, _new in transitions:
        ax1.axvline(x, color="#7f7f7f", linewidth=0.7, alpha=0.7)
    if transitions:
        timeline = ["state changes"]
        timeline += ["%6.2f s  %s" % (x, new_state) for x, _old, new_state in transitions]
        ax1.text(0.015, 0.05, chr(10).join(timeline), transform=ax1.transAxes,
                 ha="left", va="bottom", fontsize=7.5, family="monospace",
                 bbox=dict(boxstyle="round,pad=0.4", facecolor="white", edgecolor="#bbbbbb"))
    ax1.legend(fontsize=8, loc="upper left")

    # Key numbers into the figure, so the PNG stands on its own in a report.
    lines = []
    for limit in THRESHOLDS_NS:
        tt = first_time_below(samples, limit)
        label = "%d ns" % limit if limit < 1000 else ("%d us" % (limit // 1000))
        lines.append("|offset| < %-7s %s" % (label, ("%5.1f s" % tt) if tt is not None else "n/a"))
    if settled:
        lines.append("settled  %+.0f ns  sigma %.0f ns" % (settled["mean"], settled["sigma"]))
        lines.append("         peak-to-peak %d ns" % settled["p2p"])
    ax1.text(0.985, 0.03, chr(10).join(lines), transform=ax1.transAxes, ha="right", va="bottom",
             fontsize=7.5, family="monospace",
             bbox=dict(boxstyle="round,pad=0.4", facecolor="white", edgecolor="#bbbbbb"))

    ax2.plot(t, off, linewidth=0.9, color="#1f4e79")
    if settled:
        ax2.axhspan(settled["min"], settled["max"], color="#548235", alpha=0.15,
                    label="settled band %+d .. %+d ns" % (settled["min"], settled["max"]))
        ax2.axvline(settled["from_s"], color="#548235", linewidth=0.8, linestyle=":")
        # Scale to the settled band, not to the start-up excursion - otherwise the
        # "zoomed" panel shows a flat line through the middle of an empty plot.
        lim = max(200, settled["p2p"] * 4)
        ax2.set_ylim(-lim, lim)
        ax2.legend(fontsize=8, loc="upper right")
    ax2.axhline(0, color="black", linewidth=0.6)
    ax2.set_ylabel("offset  [ns]  (linear, zoomed)")
    ax2.grid(True, alpha=0.25)

    ax3.plot(t, rate, linewidth=0.9, color="#833c00")
    ax3.axhline(0, color="black", linewidth=0.6)
    ax3.set_ylabel("residual rate  [ppb]")
    ax3.set_xlabel("time since the first sample  [s]")
    ax3.grid(True, alpha=0.25)

    fig.tight_layout()
    fig.savefig(path, dpi=130)
    print("wrote %s" % path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bridge-port")
    ap.add_argument("--follower-port")
    ap.add_argument("--interval", type=int, default=125, help="master send interval in ms")
    ap.add_argument("--seconds", type=float, default=75.0, help="how long to record")
    ap.add_argument("--max-lock-s", type=float, default=40.0, help="FINE must be reached by then")
    ap.add_argument("--max-spread-ns", type=int, default=2000, help="settled peak-to-peak limit")
    ap.add_argument("--no-plot", action="store_true")
    ap.add_argument("--from-csv", metavar="PATH",
                    help="re-plot an earlier run instead of measuring, for iterating on "
                         "the diagram without touching the boards")
    ap.add_argument("--no-reset", action="store_true",
                    help="skip the board reset; the run then starts from whatever state the "
                         "servo is in, which is not a convergence measurement")
    args = ap.parse_args()

    if args.from_csv:
        samples = read_csv(args.from_csv)
        print("re-plotting %d samples from %s" % (len(samples), args.from_csv))
        transitions = transitions_from(samples)
    else:
        bridge, follower = resolve_ports(args)
        print("bridge console %s   follower console %s" % (bridge, follower))
        if not ensure_master(bridge, args.interval):
            return 1

        if not args.no_reset:
            print("resetting the follower board for a cold start ...")
            cold_start(follower)
        print("recording %.0f s while the servo locks ..." % args.seconds)
        samples, _log_transitions = record(follower, args.seconds, args.interval)
        transitions = transitions_from(samples)
    print("samples: %d   state changes: %d" % (len(samples), len(transitions)))
    if len(samples) < 20:
        print("too few samples - is the follower listening and the master sending?")
        return 1

    failures = []

    print("\nstate changes")
    for x, old, new in transitions:
        print("   %6.2f s   %-9s -> %s" % (x, old, new))
    reached = {new: x for x, _, new in transitions}
    if "FINE" not in reached and samples[-1]["state"] != "FINE":
        failures.append("FINE was never reached within %.0f s" % args.seconds)
    elif "FINE" in reached and reached["FINE"] > args.max_lock_s:
        failures.append("FINE reached only after %.1f s (limit %.1f s)"
                        % (reached["FINE"], args.max_lock_s))

    print("\nfirst time |offset| stays below")
    for limit in THRESHOLDS_NS:
        t = first_time_below(samples, limit)
        label = "%d ns" % limit if limit < 1000 else ("%d us" % (limit // 1000))
        print("   %-8s %s" % (label, ("%6.2f s" % t) if t is not None else "not reached"))

    settled = settled_stats(samples)
    if settled:
        print("\nsettled over the last %d samples (from %.1f s), state %s"
              % (settled["n"], settled["from_s"], settled["state"]))
        print("   mean %+.1f ns   sigma %.1f ns   min %+d ns   max %+d ns   peak-to-peak %d ns"
              % (settled["mean"], settled["sigma"], settled["min"], settled["max"], settled["p2p"]))
        if settled["state"] != "FINE":
            failures.append("still in %s at the end of the run" % settled["state"])
        if settled["p2p"] > args.max_spread_ns:
            failures.append("settled peak-to-peak %d ns exceeds %d ns"
                            % (settled["p2p"], args.max_spread_ns))

    gaps = [(a["seq"], b["seq"]) for a, b in zip(samples, samples[1:]) if b["seq"] - a["seq"] != 1]
    if gaps:
        print("\nnote: %d gap(s) in the sequence, first %s - a lost pair, or the console dropped a line"
              % (len(gaps), gaps[0]))

    if not args.from_csv:
        write_csv(samples, HERE / "servo_convergence.csv")
        print("\nwrote %s" % (HERE / "servo_convergence.csv"))
    if not args.no_plot:
        plot(samples, transitions, settled, args.interval, HERE / "servo_convergence.png")

    if failures:
        print("\nFAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("\nPASS: servo reached FINE and stayed inside the tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
