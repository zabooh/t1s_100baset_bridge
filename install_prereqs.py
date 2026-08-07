#!/usr/bin/env python3
"""Installer/preflight check for everything flash_same54.py needs.

Automatically installs whatever can be safely automated (pyocd via pip).
Also checks USB probe visibility and the local Microchip.SAME54_DFP pack,
printing clear, concrete next steps for anything missing -- a pack download
is deliberately NOT attempted automatically, since there is no confirmed
official direct-download URL for it (the pack is normally provided locally
via MPLAB X/MCC).

Usage:
    python install_prereqs.py            # check only
    python install_prereqs.py --install  # also install pyocd if missing
"""

import argparse
import subprocess
import sys
from pathlib import Path

PYOCD_PIN = "0.43.0"  # verified on real hardware; 0.44.1 causes SWD connection issues

sys.path.insert(0, str(Path(__file__).parent))
from flash_same54 import find_pack_dir  # same pack search as flash_same54.py, no duplication


def check_python():
    ok = sys.version_info >= (3, 9)
    version = ".".join(str(p) for p in sys.version_info[:3])
    print(f"{'OK' if ok else 'MISSING'}: Python {version} ({'>= 3.9' if ok else 'Python 3.9+ required, see https://www.python.org/'})")
    return ok


def check_pyocd(install):
    try:
        import pyocd
        version = pyocd.__version__
        print(f"OK: pyocd {version} already installed")
        if version != PYOCD_PIN:
            print(
                f"     Note: verified with pyocd {PYOCD_PIN}. If you hit SWD problems right at\n"
                f"     connect (FAULT ACK immediately, not just on AP#2-4), consider switching back:\n"
                f"     pip install \"pyocd=={PYOCD_PIN}\""
            )
        return True
    except ImportError:
        pass

    if not install:
        print(f"MISSING: pyocd is not installed. Install it with: pip install \"pyocd=={PYOCD_PIN}\"")
        print("         Or run this tool again with --install.")
        return False

    print(f"Installing pyocd=={PYOCD_PIN} ...")
    rc = subprocess.call([sys.executable, "-m", "pip", "install", f"pyocd=={PYOCD_PIN}"])
    if rc != 0:
        print("ERROR: pip install pyocd failed (see output above)")
        return False
    print("OK: pyocd installed")
    return True


def check_probes():
    try:
        from pyocd.core.helpers import ConnectHelper
    except ImportError:
        print("SKIPPED: pyocd not installed, cannot detect probes")
        return False

    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    if not probes:
        print(
            "WARNING: no EDBG/nEDBG probe detected.\n"
            "         Connect the board via USB to the debug/programming port."
        )
        return False

    print(f"OK: {len(probes)} probe(s) detected:")
    for p in probes:
        print(f"     {p.unique_id}  {p.vendor_name} {p.product_name}")
    return True


def check_pack():
    pack_dir = find_pack_dir()
    if pack_dir is None:
        print(
            "MISSING: no Microchip.SAME54_DFP pack found locally.\n"
            "         This tool does NOT download it automatically (no confirmed official\n"
            "         direct-download URL). Two options:\n"
            "         1) Install MPLAB X (or just open it once) -- it downloads the pack\n"
            "            automatically into the local cache (~/.mchp_packs/Microchip/SAME54_DFP/...).\n"
            "         2) Download it manually from https://packs.download.microchip.com/ and\n"
            "            pass it explicitly with --pack <path> when flashing."
        )
        return False
    print(f"OK: Microchip.SAME54_DFP pack found: {pack_dir}")
    return True


def main():
    parser = argparse.ArgumentParser(description="Check/install prerequisites for flash_same54.py")
    parser.add_argument("--install", action="store_true", help="Automatically install missing pyocd via pip")
    args = parser.parse_args()

    print("== Prerequisites for flash_same54.py ==\n")

    results = [check_python(), check_pyocd(args.install)]
    print()
    results.append(check_probes())
    print()
    results.append(check_pack())
    print()

    if all(results):
        print("Everything is ready -- flash_same54.py can be used.")
        sys.exit(0)

    print("Something is still missing -- see the messages above.")
    sys.exit(1)


if __name__ == "__main__":
    main()
