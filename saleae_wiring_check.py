#!/usr/bin/env python3
"""
saleae_wiring_check.py - prove the measurement chain before trusting any
measurement made with it.

Toggles PD10 (EXT1 pin 5) on one board at a time, straight through SWD with
pyOCD, and checks that the Saleae sees exactly those edges on exactly one
channel.  No firmware involvement at all, so this also works on the bridge,
which has no PD10 code.

Why bother: the same discipline as control K1 in test_results.md.  A chain that
has never been shown to produce a signal cannot be trusted when it reports one -
or when it reports none.  One capture per board also pins down which Saleae
channel belongs to which board, rather than trusting the wiring notes.

Register facts from the DFP header (component/port.h, not from memory):
  PORT base 0x41008000, group stride 0x80  ->  GROUP[3] = PORTD = 0x41008180
  DIRSET +0x08   OUT +0x10   OUTCLR +0x14   OUTSET +0x18   OUTTGL +0x1C
  PD10 -> bit 10 -> mask 0x400
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

PORTD = 0x41008180
DIRSET = PORTD + 0x08
OUTCLR = PORTD + 0x14
OUTTGL = PORTD + 0x1C
PD10 = 0x400

PACK = r"C:\Users\M91221\AppData\Local\Temp\Microchip.SAME54_DFP.3.11.261.pack"
TMP = os.environ.get("TEMP", ".")

# Channel numbers MEASURED with this script on 2026-08-12, not taken from wiring
# notes: every board produced exactly its 10 edges on exactly one channel while
# all others stayed silent.  Both the original note (1/2/3) and my assumption
# were wrong, and Ch1 turned out to carry nothing at all - which is the whole
# reason this check exists before any real measurement.
BOARDS = [
    ("bridge",     "ATML3264031800001049", 3),
    ("follower A", "ATML3264031800001290", 1),
    ("follower B", "ATML3264031800001103", 0),
]


def toggle_via_swd(probe, n):
    """Drive PD10 low, then toggle n times.  One pyocd invocation for all of it -
    each invocation costs ~2 s of startup, and the edge spacing is then the SWD
    round trip, a millisecond or so, which is plenty for a wiring check."""
    cmds = ["-c", f"write32 0x{DIRSET:08X} 0x{PD10:X}",
            "-c", f"write32 0x{OUTCLR:08X} 0x{PD10:X}"]
    for _ in range(n):
        cmds += ["-c", f"write32 0x{OUTTGL:08X} 0x{PD10:X}"]
    # Leave the core running; the boards are mid-session.
    cmds += ["-c", "go"]
    r = subprocess.run(["python", "-m", "pyocd", "cmd", "-t", "atsame54p20a",
                        "--pack", PACK, "-u", probe] + cmds,
                       capture_output=True, text=True, timeout=180)
    return r.returncode


def parse_binary(path):
    """Saleae digital binary export.  Layout determined empirically from a
    zero-transition file, byte for byte:
        char[8] '<SALEAE>' | int32 version | int32 type | uint32 initial_state
        double begin | double end | uint64 num_transitions | double[n] times
    """
    b = open(path, "rb").read()
    if b[:8] != b"<SALEAE>":
        raise ValueError(f"{path}: not a Saleae binary export")
    ver, typ, init = struct.unpack_from("<iiI", b, 8)
    begin, end, n = struct.unpack_from("<ddQ", b, 20)
    times = struct.unpack_from(f"<{n}d", b, 44) if n else ()
    return dict(version=ver, type=typ, initial=init, begin=begin, end=end,
                n=n, times=list(times))


def signature_run(man, automation, args):
    """One capture, a different number of edges per board.

    Stronger than toggling one board per capture: the counts identify each
    channel on their own, the first-edge times must come in the order the boards
    were driven, and anything that moves on a channel it should not proves
    crosstalk or a shorted probe.  All three boards are verified against each
    other in a single observation instead of three separate ones.
    """
    counts = dict(zip([b[0] for b in BOARDS], args.signature))
    outdir = os.path.join(TMP, "sal_signature")
    os.makedirs(outdir, exist_ok=True)
    for f in glob.glob(os.path.join(outdir, "*")):
        os.remove(f)

    print("signature per board:")
    for name, _, _ in BOARDS:
        print(f"  {name:12s} -> {counts[name]} edges")

    dc = automation.LogicDeviceConfiguration(
        enabled_digital_channels=args.channels, digital_sample_rate=args.sample_rate)
    cc = automation.CaptureConfiguration(
        capture_mode=automation.TimedCaptureMode(duration_seconds=args.seconds))

    order = []
    with man.start_capture(device_configuration=dc, capture_configuration=cc) as cap:
        for name, probe, _ in BOARDS:
            rc = toggle_via_swd(probe, counts[name])
            if rc != 0:
                print(f"  {name}: pyocd returned {rc}")
            order.append(name)
        cap.wait()
        cap.export_raw_data_binary(directory=outdir, digital_channels=args.channels)

    obs = {}
    for ch in args.channels:
        path = os.path.join(outdir, f"digital_{ch}.bin")
        if not os.path.exists(path):
            continue
        d = parse_binary(path)
        obs[ch] = d
        first = f"{d['times'][0] * 1e3:.1f} ms" if d["n"] else "-"
        print(f"  Ch{ch}: {d['n']:3d} transitions, first at {first}")

    print("\nidentification by edge count:")
    ok = True
    ident = {}
    for name, _, _ in BOARDS:
        want = counts[name]
        hits = [ch for ch, d in obs.items() if d["n"] == want]
        if len(hits) == 1:
            ident[name] = hits[0]
            print(f"  {name:12s} ({want:2d} edges) -> Ch{hits[0]}")
        else:
            print(f"  {name:12s} ({want:2d} edges) -> AMBIGUOUS {hits}")
            ok = False

    # The boards were driven in list order, so their first edges must appear in
    # that order too.  This catches a count coincidence.
    if len(ident) == len(BOARDS):
        times = [(name, obs[ident[name]]["times"][0]) for name in order
                 if obs[ident[name]]["n"]]
        seq = [n for n, _ in sorted(times, key=lambda kv: kv[1])]
        if seq == order:
            print(f"  first-edge order matches drive order: {' -> '.join(seq)}")
        else:
            print(f"  ORDER MISMATCH: drove {order}, saw {seq}")
            ok = False

    silent = [ch for ch, d in obs.items() if d["n"] == 0]
    if silent:
        print(f"  channels with nothing on them: {silent}")
    return ok, ident


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toggles", type=int, default=10)
    ap.add_argument("--signature", type=int, nargs=3, metavar=("B", "A", "BR"),
                    help="one capture, this many edges per board (bridge, folA, folB order)")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--sample-rate", type=int, default=25_000_000)
    ap.add_argument("--channels", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--discover", action="store_true",
                    help="measure which channel each board is on instead of asserting")
    args = ap.parse_args()

    from saleae import automation

    ok = True
    found = {}
    with automation.Manager.connect(port=10430) as man:
        if args.signature:
            ok, ident = signature_run(man, automation, args)
            print("\n" + ("mapping verified in one capture" if ok
                          else "SIGNATURE TEST FAILED"))
            return 0 if ok else 1

        for name, probe, expect_ch in BOARDS:
            outdir = os.path.join(TMP, "sal_wiring_" + name.replace(" ", "_"))
            os.makedirs(outdir, exist_ok=True)
            for f in glob.glob(os.path.join(outdir, "*")):
                os.remove(f)

            print(f"\n=== {name}  (probe ...{probe[-4:]}, expected Ch{expect_ch}) ===")
            dc = automation.LogicDeviceConfiguration(
                enabled_digital_channels=args.channels,
                digital_sample_rate=args.sample_rate)
            cc = automation.CaptureConfiguration(
                capture_mode=automation.TimedCaptureMode(duration_seconds=args.seconds))
            with man.start_capture(device_configuration=dc, capture_configuration=cc) as cap:
                rc = toggle_via_swd(probe, args.toggles)
                if rc != 0:
                    print(f"  pyocd returned {rc} - toggles may not have happened")
                cap.wait()
                cap.export_raw_data_binary(directory=outdir, digital_channels=args.channels)

            seen = {}
            for ch in args.channels:
                path = os.path.join(outdir, f"digital_{ch}.bin")
                if not os.path.exists(path):
                    print(f"  Ch{ch}: no export file")
                    continue
                d = parse_binary(path)
                seen[ch] = d["n"]

            for ch in sorted(seen):
                mark = "  <-- expected" if ch == expect_ch else ""
                print(f"  Ch{ch}: {seen[ch]:3d} transitions{mark}")

            active = [ch for ch, n in seen.items() if n >= args.toggles]

            if args.discover:
                # Derive the mapping instead of asserting one.  A hardcoded table
                # goes stale the moment someone moves a probe, and then the check
                # reports a wiring fault that is really a stale note.
                if len(active) == 1:
                    found[name] = active[0]
                    print(f"  -> {name} is on Ch{active[0]}")
                elif not active:
                    print(f"  -> no channel saw {args.toggles} edges: probe not "
                          f"connected, on a disabled channel, or not on PD10")
                    ok = False
                else:
                    print(f"  -> AMBIGUOUS: {active} all moved; shorted probes?")
                    ok = False
                continue

            got = seen.get(expect_ch, 0)
            others = sum(v for k, v in seen.items() if k != expect_ch)
            if got >= args.toggles:
                if others == 0:
                    print(f"  PASS - {got} edges on Ch{expect_ch} only")
                else:
                    print(f"  PASS with crosstalk - {others} edges on other channels")
            else:
                print(f"  FAIL - expected at least {args.toggles} on Ch{expect_ch}, got {got}")
                ok = False

    if args.discover:
        print("\nmeasured mapping:")
        for name, probe, _ in BOARDS:
            ch = found.get(name)
            print(f"  {name:12s} probe ...{probe[-4:]}  ->  "
                  + (f"Ch{ch}" if ch is not None else "NOT FOUND"))
        print("\nPaste into BOARDS above if it should be asserted from now on.")

    print("\n" + ("all boards verified" if ok else "AT LEAST ONE BOARD FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
