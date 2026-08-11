#!/usr/bin/env python3
"""Generic pyOCD-based flash tool for Microchip ATSAME54 boards (EDBG/nEDBG CMSIS-DAP probes).

Requires: pip install pyocd

Verified 2026-07-30 against a real ATSAME54_ETH1 board over its on-board EDBG probe
(see C:\\work\\parser\\test\\usart_ring_buffer_interrupt\\tools\\flash_same54.py, the
project this tool was ported from). Key finding this tool works around: pyOCD's
public pack index only offers an outdated third-party "Keil.SAME54_DFP" 1.0.4
mirror, which is missing RAM/flash-region metadata and fails with "CMSIS-Pack
device ATSAME54P20A has no default RAM defined". The real, current
Microchip.SAME54_DFP pack (as installed locally by MPLAB X / MCC) works fine,
so this tool finds it automatically and feeds it to pyOCD via --pack.
"""

import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

DEFAULT_TARGET = "atsame54p20a"
PACK_CACHE = Path.home() / ".mchp_packs" / "Microchip" / "SAME54_DFP"
MPLABX_ROOT = Path("C:/Program Files/Microchip/MPLABX")


def _version_key(name):
    try:
        return tuple(int(p) for p in name.split("."))
    except ValueError:
        return (0,)


def find_pack_dir():
    """Return the newest locally installed Microchip.SAME54_DFP pack directory, or None."""
    candidates = []
    if PACK_CACHE.is_dir():
        candidates += [d for d in PACK_CACHE.iterdir() if d.is_dir()]
    if MPLABX_ROOT.is_dir():
        for mplabx in MPLABX_ROOT.glob("v*"):
            cand = mplabx / "packs" / "Microchip" / "SAME54_DFP"
            if cand.is_dir():
                candidates += [d for d in cand.iterdir() if d.is_dir()]
    if not candidates:
        return None
    return max(candidates, key=lambda d: _version_key(d.name))


def ensure_pack_file(pack_path: Path) -> Path:
    """pyOCD needs a .pack (zip) file. The local Microchip cache stores packs unpacked,
    so zip one into a cached temp file on demand, matching the manual procedure that
    was verified to work on real hardware."""
    if pack_path.is_file():
        return pack_path
    pdsc = next(pack_path.glob("*.pdsc"), None)
    if pdsc is None:
        raise FileNotFoundError(f"{pack_path} does not look like a CMSIS pack (no .pdsc found)")
    out = Path(tempfile.gettempdir()) / f"{pdsc.stem}.{pack_path.name}.pack"
    if not out.exists() or out.stat().st_mtime < pack_path.stat().st_mtime:
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in pack_path.rglob("*"):
                if f.is_file():
                    zf.write(f, f.relative_to(pack_path))
    return out


def resolve_pack(explicit):
    if explicit:
        return ensure_pack_file(Path(explicit))
    pack_dir = find_pack_dir()
    if pack_dir is None:
        return None
    return ensure_pack_file(pack_dir)


def list_probes():
    from pyocd.core.helpers import ConnectHelper

    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    for p in probes:
        print(f"{p.unique_id}  {p.vendor_name} {p.product_name}")
    return probes


def run_pyocd(args, dry_run=False):
    cmd = [sys.executable, "-m", "pyocd"] + [str(a) for a in args]
    print("+", " ".join(cmd))
    if dry_run:
        return 0
    return subprocess.call(cmd)


def build_common_args(target, pack, probe, frequency):
    args = ["-t", target, "-f", str(frequency)]
    if pack:
        args += ["--pack", str(pack)]
    if probe:
        args += ["-u", probe]
    return args


def flash(image, target, pack, probe, frequency, reset, dry_run):
    common = build_common_args(target, pack, probe, frequency)
    rc = run_pyocd(["flash"] + common + [str(image)], dry_run)
    if rc != 0:
        return rc
    if reset:
        rc = run_pyocd(["reset"] + common, dry_run)
    return rc


def read_memory(address, length, target, pack, probe, frequency, out, dry_run):
    """Read target memory via 'pyocd commander'. Without --out, prints a hex dump
    (commander's read8); with --out, saves the raw bytes to a binary file (savemem)."""
    common = build_common_args(target, pack, probe, frequency)
    if out:
        command = f"savemem {address} {length} {out}"
    else:
        command = f"read8 {address} {length}"
    return run_pyocd(["commander"] + common + ["-c", command], dry_run)


def main():
    parser = argparse.ArgumentParser(description="Flash or read a SAME54 board via pyOCD/EDBG.")
    parser.add_argument("image", nargs="?", help=".hex or .elf firmware image to flash")
    parser.add_argument("--target", default=DEFAULT_TARGET, help=f"pyOCD target name (default: {DEFAULT_TARGET})")
    parser.add_argument("--pack", help="Explicit path to a SAME54_DFP .pack file or unpacked pack directory. Auto-detected if omitted.")
    parser.add_argument("--probe", "-u", help="Unique ID (serial) of the EDBG/nEDBG probe to use. Required if more than one is connected.")
    parser.add_argument("--project", help="Take the probe from boards.json for this project (bridge, follower). "
                                         "Run setup_flasher.py once to create that mapping. Ignored if --probe is given.")
    parser.add_argument("--reset-only", action="store_true", help="Reset the target and exit (no flashing)")
    parser.add_argument("--frequency", "-f", type=int, default=2_000_000, help="SWD clock in Hz (default: 2000000)")
    parser.add_argument("--no-reset", action="store_true", help="Skip the reset-and-run after flashing")
    parser.add_argument("--list", action="store_true", help="List connected probes and exit")
    parser.add_argument("--read", nargs=2, metavar=("ADDRESS", "LENGTH"),
                         help="Read LENGTH bytes starting at ADDRESS (e.g. 0x0) instead of flashing. "
                              "Prints a hex dump unless --out is given.")
    parser.add_argument("--out", help="With --read: save the read memory to this binary file instead of printing a hex dump")
    parser.add_argument("--dry-run", action="store_true", help="Print the pyOCD commands without executing them")
    args = parser.parse_args()

    if args.list:
        probes = list_probes()
        sys.exit(0 if probes else 1)

    if not args.read and not args.reset_only and not args.image:
        parser.error("image is required unless --list, --read or --reset-only is given")

    # Two boards on one desk: resolve which one this project owns before pyOCD has
    # to guess. --probe always wins, so a one-off override stays possible.
    if not args.probe and args.project:
        try:
            from setup_flasher import probe_for
        except ImportError:
            probe_for = None
        args.probe = probe_for(args.project) if probe_for else None
        if args.probe:
            print(f"Project '{args.project}' -> probe {args.probe} (boards.json)")
        else:
            print(f"Note: no board assigned to project '{args.project}' in boards.json. "
                  f"Run 'python setup_flasher.py' to assign one.", file=sys.stderr)

    image = None
    if args.image:
        image = Path(args.image)
        if not image.is_file():
            parser.error(f"image not found: {image}")

    if not args.probe:
        probes = list_probes()
        if len(probes) > 1:
            parser.error("multiple probes connected; either run 'python setup_flasher.py' once and "
                         "use --project, or pass --probe/-u with one of the unique IDs listed above")

    pack = resolve_pack(args.pack)
    if pack is None:
        print(
            "Warning: no Microchip.SAME54_DFP pack found locally and none given via --pack.\n"
            "         Falling back to pyOCD's built-in pack index, which as of pyOCD 0.43.0\n"
            "         only has an outdated Keil.SAME54_DFP (1.0.4) mirror that is known to\n"
            "         fail flashing ('no default RAM defined'). Install MPLAB X / MCC, or\n"
            "         pass --pack explicitly.",
            file=sys.stderr,
        )
    else:
        print(f"Using pack: {pack}")

    if args.reset_only:
        common = build_common_args(args.target, pack, args.probe, args.frequency)
        sys.exit(run_pyocd(["reset"] + common, args.dry_run))

    if args.read:
        address, length = args.read
        rc = read_memory(address, length, args.target, pack, args.probe, args.frequency, args.out, args.dry_run)
        sys.exit(rc)

    rc = flash(image, args.target, pack, args.probe, args.frequency, not args.no_reset, args.dry_run)
    sys.exit(rc)


if __name__ == "__main__":
    main()
