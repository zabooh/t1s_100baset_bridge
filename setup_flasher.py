#!/usr/bin/env python3
"""setup_flasher.py - decide which attached board belongs to which project.

With one board connected, pyOCD picks it and nothing here is needed. With two -
the bridge and the follower - "pick the first probe" is a coin toss that flashes
the wrong device, and both boards report themselves as the same kind of EDBG
CMSIS-DAP probe. This tool asks once and writes the answer to boards.json, which
every flash path then reads.

Telling the two apart is the actual problem, so this does not just print serial
numbers: the EDBG serial also appears in the virtual COM port's hardware ID, so
each probe is shown together with its console port. That is usually enough
("the bridge is the one I have a terminal open on"), and if it is not, --reset
restarts one board so its boot banner appears on that port.

    python setup_flasher.py                 interactive assignment
    python setup_flasher.py --show          what is stored and what is attached
    python setup_flasher.py --set bridge=ATML3264031800001049
    python setup_flasher.py --clear follower
    python setup_flasher.py --reset bridge  reset the assigned board, to identify it

boards.json is machine-specific (serial numbers of the probes on this desk), so
it is deliberately not tracked in git. A fresh clone runs this once.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOARDS_JSON = HERE / "boards.json"
SCHEMA_VERSION = 1

# The projects that can own a board. Keep the keys stable: the flash scripts pass
# them on the command line.
PROJECTS = {
    "bridge": "10BASE-T1S <-> 100BASE-T bridge (firmware/T1S_100BaseT_Bridge.X)",
    "follower": "PTP follower on T1S (follower/firmware/T1S_Follower.X)",
}


def load(path=BOARDS_JSON):
    """Read boards.json, or return an empty mapping. A damaged file is reported
    rather than silently replaced - it holds a decision someone made once."""
    if not path.is_file():
        return {"version": SCHEMA_VERSION, "projects": {}}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (json.JSONDecodeError, OSError) as exc:
        print("cannot read %s: %s" % (path, exc), file=sys.stderr)
        sys.exit(1)
    data.setdefault("version", SCHEMA_VERSION)
    data.setdefault("projects", {})
    return data


def save(data, path=BOARDS_JSON):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("written: %s" % path)


def probe_for(project, path=BOARDS_JSON, quiet=False):
    """Resolve a project name to a probe serial. Returns None when unassigned.
    Used by flash_same54.py, so it must not print noise on the happy path."""
    entry = load(path)["projects"].get(project)
    if not entry:
        return None
    return entry.get("probe")


def com_ports_by_serial():
    """Map EDBG probe serial -> COM port, taken from the port's hardware ID
    (the EDBG virtual COM port carries SER=<probe serial>)."""
    out = {}
    try:
        from serial.tools import list_ports
    except ImportError:
        return out
    for p in list_ports.comports():
        hwid = (p.hwid or "")
        for token in hwid.replace(";", " ").split():
            if token.startswith("SER="):
                out.setdefault(token[4:], p.device)
    return out


def attached_probes():
    try:
        from pyocd.core.helpers import ConnectHelper
    except ImportError:
        print("pyocd is not installed - run setup.bat / pip install pyocd", file=sys.stderr)
        sys.exit(1)
    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    ports = com_ports_by_serial()
    return [{"uid": p.unique_id,
             "name": ("%s %s" % (p.vendor_name, p.product_name)).strip(),
             "port": ports.get(p.unique_id)} for p in probes]


def owner_of(data, uid):
    for name, entry in sorted(data["projects"].items()):
        if entry.get("probe") == uid:
            return name
    return None


def print_state(data, probes):
    print("attached boards:")
    if not probes:
        print("  (none found)")
    for i, pr in enumerate(probes, 1):
        own = owner_of(data, pr["uid"])
        print("  %d) %-22s %-30s %s%s"
              % (i, pr["uid"], pr["name"],
                 "console %s" % pr["port"] if pr["port"] else "console unknown",
                 "   -> %s" % own if own else ""))
    print("assignment in %s:" % BOARDS_JSON.name)
    for name in sorted(PROJECTS):
        entry = data["projects"].get(name)
        uid = entry.get("probe") if entry else None
        if not uid:
            print("  %-9s (unassigned)" % name)
            continue
        live = any(pr["uid"] == uid for pr in probes)
        print("  %-9s %s  %s" % (name, uid, "attached" if live else "NOT attached right now"))


def do_reset(project, data):
    uid = (data["projects"].get(project) or {}).get("probe")
    if not uid:
        print("%s has no board assigned" % project, file=sys.stderr)
        return 1
    tool = HERE / "flash_same54.py"
    print("resetting the board assigned to %s (%s) - watch its console for the boot banner"
          % (project, uid))
    return subprocess.call([sys.executable, str(tool), "--probe", uid, "--reset-only"])


def interactive(data, probes):
    if not probes:
        print("no boards found. Connect them and try again.")
        return 1
    print_state(data, probes)
    print()
    print("Enter the number of the board for each project, 'k' to keep the current")
    print("assignment, or 'n' for none. Ctrl-C aborts without writing.")
    try:
        for name in sorted(PROJECTS):
            current = (data["projects"].get(name) or {}).get("probe")
            prompt = "  %s [%s]: " % (name, current if current else "unassigned")
            while True:
                answer = input(prompt).strip().lower()
                if answer in ("", "k"):
                    break
                if answer == "n":
                    data["projects"].pop(name, None)
                    break
                if answer.isdigit() and 1 <= int(answer) <= len(probes):
                    pr = probes[int(answer) - 1]
                    clash = owner_of(data, pr["uid"])
                    if clash and clash != name:
                        # Two projects on one board is a mistake worth catching here
                        # rather than after flashing the wrong image.
                        print("    that board is already assigned to '%s' - freeing it" % clash)
                        data["projects"].pop(clash, None)
                    data["projects"][name] = {
                        "probe": pr["uid"],
                        "name": pr["name"],
                        "console": pr["port"] or "",
                        "description": PROJECTS[name],
                    }
                    break
                print("    please answer with 1..%d, 'k' or 'n'" % len(probes))
    except (KeyboardInterrupt, EOFError):
        print("\naborted - nothing written")
        return 1
    print()
    save(data)
    print()
    print_state(load(), probes)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--show", action="store_true", help="print stored assignment and attached boards")
    ap.add_argument("--set", metavar="PROJECT=UID", action="append", default=[],
                    help="assign non-interactively, e.g. --set bridge=ATML326...")
    ap.add_argument("--clear", metavar="PROJECT", action="append", default=[],
                    help="remove a project's assignment")
    ap.add_argument("--reset", metavar="PROJECT", help="reset that project's board to identify it")
    ap.add_argument("--probe-of", metavar="PROJECT",
                    help="print the probe serial for PROJECT and exit (for scripts)")
    args = ap.parse_args()

    data = load()

    if args.probe_of:
        uid = probe_for(args.probe_of)
        if not uid:
            return 1
        print(uid)
        return 0

    if args.set or args.clear:
        for spec in args.set:
            if "=" not in spec:
                ap.error("--set needs PROJECT=UID, got %r" % spec)
            name, uid = spec.split("=", 1)
            if name not in PROJECTS:
                ap.error("unknown project %r, expected one of %s" % (name, ", ".join(sorted(PROJECTS))))
            data["projects"][name] = {"probe": uid, "description": PROJECTS[name]}
        for name in args.clear:
            data["projects"].pop(name, None)
        save(data)
        print_state(load(), attached_probes())
        return 0

    if args.reset:
        return do_reset(args.reset, data)

    probes = attached_probes()
    if args.show:
        print_state(data, probes)
        return 0
    return interactive(data, probes)


if __name__ == "__main__":
    sys.exit(main())
