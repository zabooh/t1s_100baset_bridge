#!/usr/bin/env python3
"""Flash a board by its role (bridge, follower_a, follower_b) instead of by probe serial.

    python flash_boards.py                 flash both followers (A then B) - the default
    python flash_boards.py followers       ... same, explicit
    python flash_boards.py follower_a      only A
    python flash_boards.py follower_b      only B
    python flash_boards.py bridge          the bridge board
    python flash_boards.py --list          show configured roles, their probe, connected or not
    python flash_boards.py <role> --dry-run

json/boards.json maps each role to a probe serial - that part is the single
source of truth and this script never duplicates it. Which *image* belongs to a role
is the one thing boards.json deliberately does not say (it stays "which physical
board is this", not "which firmware last shipped to it"), so ROLE_IMAGES below is that
mapping's one place, as follower/flash.bat's header comment promises.

Referenced by follower/flash.bat, which forwards its arguments here unchanged.
"""
import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))
import flash_same54  # noqa: E402 (needs REPO_ROOT on sys.path first)

BOARDS_JSON = REPO_ROOT / "json" / "boards.json"

# boards.json role -> firmware image, relative to REPO_ROOT.
ROLE_IMAGES = {
    "bridge": "release/T1S_100BaseT_Bridge.hex",
    "follower_a": "follower/release/T1S_Follower.hex",
    "follower": "follower/release/T1S_Follower.hex",  # boards.json's name for Follower B
}

# CLI-facing name -> boards.json role key. This script's own usage promises
# "follower_a"/"follower_b"; boards.json still calls B plain "follower" (its
# original, single-follower name from before there were two).
CLI_TO_ROLE = {
    "bridge": "bridge",
    "follower_a": "follower_a",
    "follower_b": "follower",
}

GROUPS = {
    "followers": ["follower_a", "follower_b"],
}


def load_boards():
    with open(BOARDS_JSON, encoding="utf-8") as f:
        return json.load(f)["projects"]


def resolve_targets(name):
    if name in GROUPS:
        return GROUPS[name]
    if name in CLI_TO_ROLE:
        return [name]
    return None


def do_list(boards):
    connected = {p.unique_id for p in flash_same54.list_probes()}
    print("\nConfigured roles:")
    for cli_name, role in CLI_TO_ROLE.items():
        info = boards.get(role, {})
        probe = info.get("probe", "?")
        state = "connected" if probe in connected else "NOT connected"
        print(f"  {cli_name:<12} probe {probe}  [{state}]  {info.get('description', '')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("target", nargs="?", default="followers",
                         help="bridge, follower_a, follower_b, or followers (both A and B; default)")
    parser.add_argument("--dry-run", action="store_true",
                         help="print the pyOCD commands without executing them")
    parser.add_argument("--list", action="store_true",
                         help="show configured roles, their probe, and whether it is currently connected")
    args = parser.parse_args()

    boards = load_boards()

    if args.list:
        do_list(boards)
        return 0

    cli_names = resolve_targets(args.target)
    if cli_names is None:
        choices = ", ".join(sorted(set(CLI_TO_ROLE) | set(GROUPS)))
        parser.error(f"unknown target {args.target!r}; choices: {choices}")

    pack = flash_same54.resolve_pack(None)
    if pack is None:
        print(
            "Warning: no Microchip.SAME54_DFP pack found locally; flashing may fail "
            "('no default RAM defined'). Install MPLAB X / MCC.",
            file=sys.stderr,
        )
    else:
        print(f"Using pack: {pack}")

    for cli_name in cli_names:
        role = CLI_TO_ROLE[cli_name]
        if role not in boards:
            print(f"ERROR: {cli_name!r} (role {role!r}) not found in boards.json", file=sys.stderr)
            return 1
        probe = boards[role]["probe"]
        image = REPO_ROOT / ROLE_IMAGES[role]
        if not image.is_file():
            print(f"ERROR: image not found for {cli_name}: {image}", file=sys.stderr)
            return 1
        print(f"\n=== {cli_name} (probe {probe}) : {image.relative_to(REPO_ROOT)} ===")
        rc = flash_same54.flash(image, flash_same54.DEFAULT_TARGET, pack, probe,
                                 2_000_000, True, args.dry_run)
        if rc != 0:
            print(f"ERROR: flashing {cli_name} failed (rc={rc})", file=sys.stderr)
            return rc

    return 0


if __name__ == "__main__":
    sys.exit(main())
