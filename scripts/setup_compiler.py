r"""
setup_compiler.py — Select the XC32 compiler version to use for build.bat.

Scans C:\Program Files\Microchip\xc32\ for installed versions, lets the user
pick one, and writes the choice to setup_compiler.config (JSON) so build.bat
and build_summary.py know which compiler bin directory to use.

Usage:
    python setup_compiler.py
"""

import json
import os
import sys

# setup_compiler.config is a repo-root file (both build.bat and
# follower/build.bat read it from there), not next to this script.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_FILE = os.path.join(REPO_ROOT, "setup_compiler.config")

XC32_BASE = r"C:\Program Files\Microchip\xc32"


def find_xc32_versions(base_dir: str) -> list[dict]:
    """Return list of dicts for every installed XC32 version found under base_dir."""
    versions = []
    if not os.path.isdir(base_dir):
        return versions
    for name in sorted(os.listdir(base_dir)):
        compiler = os.path.join(base_dir, name, "bin", "xc32-gcc.exe")
        if os.path.isfile(compiler):
            versions.append({
                "version": name,          # e.g. "v5.10"
                "bin_dir": os.path.join(base_dir, name, "bin"),
                "compiler": compiler,
            })
    return versions


def load_current_config() -> dict | None:
    """Return existing config dict, or None if not found."""
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, OSError):
            pass
    return None


def save_config(entry: dict) -> None:
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        json.dump(entry, f, indent=2)
    print(f"Saved: {CONFIG_FILE}")


def main() -> None:
    print("=" * 60)
    print("  XC32 Compiler Setup for build.bat")
    print("=" * 60)

    versions = find_xc32_versions(XC32_BASE)

    if not versions:
        print(f"\nERROR: No XC32 installations found under:\n  {XC32_BASE}")
        print("Please install MPLAB XC32 and run this script again.")
        sys.exit(1)

    # Show current selection
    current = load_current_config()
    if current:
        print(f"\nCurrent selection: {current.get('version', '?')}  "
              f"({current.get('compiler', '?')})")
    else:
        print("\nNo compiler configured yet.")

    # List available versions
    print(f"\nInstalled XC32 versions ({len(versions)} found):\n")
    for i, v in enumerate(versions, start=1):
        marker = " <-- current" if (current and current.get("version") == v["version"]) else ""
        print(f"  [{i}] {v['version']:10s}  {v['compiler']}{marker}")

    print(f"\n  [0] Abort / keep current selection")

    # User choice
    while True:
        try:
            raw = input("\nSelect version number: ").strip()
        except (KeyboardInterrupt, EOFError):
            print("\nAborted.")
            sys.exit(0)

        if raw == "0":
            print("No changes made.")
            sys.exit(0)

        if raw.isdigit():
            idx = int(raw) - 1
            if 0 <= idx < len(versions):
                chosen = versions[idx]
                break

        print(f"  Invalid input. Enter a number between 0 and {len(versions)}.")

    # Confirm
    print(f"\nSelected: {chosen['version']}")
    print(f"  Compiler : {chosen['compiler']}")
    print(f"  Bin dir  : {chosen['bin_dir']}")
    try:
        confirm = input("Save this selection? [Y/n]: ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        print("\nAborted.")
        sys.exit(0)

    if confirm not in ("", "y", "yes"):
        print("Aborted.")
        sys.exit(0)

    save_config(chosen)
    print(f"\nDone. build.bat will use XC32 {chosen['version']} (via MP_CC_DIR).")


if __name__ == "__main__":
    main()
