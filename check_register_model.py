#!/usr/bin/env python3
"""Check lan8651_model.json - the register model the GUI shows.

The model is the one place where a wrong address or a mistyped bit range turns into a
tool that quietly lies to whoever is debugging with it. This checker cannot tell whether
a description matches the data sheet - only a read of the document can - but it catches
every error that has a structural signature:

  * an address whose MMS does not match the group it sits in
  * the same address or the same mnemonic twice
  * bit ranges that are reversed, exceed 31, or overlap inside one register
  * entries with no mnemonic, no name, or no bit fields at all
  * a bit field whose name repeats inside its register

It also reports how much of the model has actually been verified against a document, so
"unverified" stays visible instead of fading into the background.

    python check_register_model.py            # check, print a summary
    python check_register_model.py --quiet    # only complaints and the verdict

Exit code 0 = no errors. 1 = at least one error. Warnings alone do not fail the run.
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

MODEL_PATH = Path(__file__).parent / "lan8651_model.json"

# Group name -> the MMS its addresses must carry in the upper 16 bits.
GROUP_MMS = {"MMS0": 0, "MMS1": 1, "MMS2": 2, "MMS3": 3, "MMS4": 4, "MMS10": 10}

ADDR_RE = re.compile(r"0x[0-9A-Fa-f]{8}$")
BITS_RE = re.compile(r"(\d+)(?::(\d+))?$")
# A description this long is a pasted data sheet section rather than a description.
LONG_DESCRIPTION = 400


class Report:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, where, text):
        self.errors.append((where, text))

    def warn(self, where, text):
        self.warnings.append((where, text))


def check_header(model, rep):
    for key in ("model_version", "device", "sources", "verification", "groups"):
        if key not in model:
            rep.error("header", f"missing top-level key: {key}")
    sources = model.get("sources", {})
    ds = sources.get("datasheet", {})
    for key in ("doc", "revision", "date"):
        if not ds.get(key):
            rep.error("header", f"sources.datasheet.{key} is empty - the model must name the "
                                "document revision it was taken from")
    # Microchip numbers documents DS<8 digits><revision letter>, e.g. DS60001734F.
    if not re.fullmatch(r"DS\d{8}[A-Z]?", ds.get("doc", "")):
        rep.warn("header", f"sources.datasheet.doc looks unusual: {ds.get('doc')!r}")


def check_model(model, rep):
    groups = model.get("groups", {})
    seen_addr = {}
    seen_mnemonic = defaultdict(list)
    n_regs = n_bits = n_verified = 0

    for gname, group in groups.items():
        key = gname.split()[0]
        want = GROUP_MMS.get(key)
        if want is None:
            rep.error(gname, "unknown group - name must start with MMS0/1/2/3/4/10")
        if "mms" not in group:
            rep.error(gname, "group has no 'mms' field")
        elif want is not None and group["mms"] != want:
            rep.error(gname, f"group says mms={group['mms']}, name says {want}")

        for addr, reg in group.get("registers", {}).items():
            n_regs += 1
            where = f"{gname} {addr}"
            if not ADDR_RE.match(addr):
                rep.error(where, "address is not a 0x-prefixed 8-digit hex string")
                continue
            value = int(addr, 16)
            mms = value >> 16
            if want is not None and mms != want:
                rep.error(where, f"address carries MMS {mms}, group is MMS {want}")
            if addr in seen_addr:
                rep.error(where, f"address already used in {seen_addr[addr]}")
            seen_addr[addr] = gname

            mnemonic = (reg.get("mnemonic") or "").strip()
            if not mnemonic:
                rep.error(where, "no mnemonic")
            else:
                seen_mnemonic[mnemonic].append(addr)
            if not (reg.get("name") or "").strip():
                rep.error(where, f"{mnemonic}: no name")
            if len(reg.get("name") or "") > LONG_DESCRIPTION:
                rep.warn(where, f"{mnemonic}: name is {len(reg['name'])} characters - that reads "
                                "like a pasted section, not a register name")
            if reg.get("verified"):
                n_verified += 1

            bits = reg.get("bits", {})
            if not bits:
                rep.warn(where, f"{mnemonic}: no bit fields")
            used = {}
            names = defaultdict(list)
            for spec, field in bits.items():
                n_bits += 1
                m = BITS_RE.match(spec.strip())
                if not m:
                    rep.error(where, f"{mnemonic}: bit range {spec!r} is not 'n' or 'hi:lo'")
                    continue
                hi = int(m.group(1))
                lo = int(m.group(2)) if m.group(2) else hi
                if hi < lo:
                    rep.error(where, f"{mnemonic}: bit range {spec} is reversed")
                    hi, lo = lo, hi
                if hi > 31:
                    rep.error(where, f"{mnemonic}: bit range {spec} exceeds bit 31")
                for b in range(lo, min(hi, 31) + 1):
                    if b in used:
                        rep.error(where, f"{mnemonic}: bit {b} claimed by both "
                                         f"{used[b]!r} and {spec!r}")
                    used[b] = spec
                if not isinstance(field, dict):
                    rep.error(where, f"{mnemonic}: bit {spec} is not an object")
                    continue
                fname = (field.get("name") or "").strip()
                if not fname:
                    rep.error(where, f"{mnemonic}: bit {spec} has no name")
                else:
                    names[fname].append(spec)
                if not (field.get("description") or "").strip():
                    rep.warn(where, f"{mnemonic}: bit {spec} ({fname}) has no description")
            for fname, specs in names.items():
                if len(specs) > 1:
                    rep.error(where, f"{mnemonic}: bit field name {fname!r} used for "
                                     f"{', '.join(specs)}")

    for mnemonic, addrs in seen_mnemonic.items():
        if len(addrs) > 1:
            rep.error("model", f"mnemonic {mnemonic!r} used at {', '.join(sorted(addrs))}")

    return n_regs, n_bits, n_verified


def main():
    parser = argparse.ArgumentParser(description="Check the LAN8651 register model")
    parser.add_argument("model", nargs="?", default=str(MODEL_PATH), help="model file to check")
    parser.add_argument("--quiet", action="store_true", help="only complaints and the verdict")
    args = parser.parse_args()

    path = Path(args.model)
    try:
        model = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"ERROR: {path} not found")
        return 1
    except ValueError as exc:
        print(f"ERROR: {path} is not valid JSON: {exc}")
        return 1

    rep = Report()
    check_header(model, rep)
    n_regs, n_bits, n_verified = check_model(model, rep)

    if not args.quiet:
        ds = model.get("sources", {}).get("datasheet", {})
        er = model.get("sources", {}).get("errata", {})
        print(f"model      : {path.name} (version {model.get('model_version')})")
        print(f"device     : {model.get('device')}")
        print(f"data sheet : {ds.get('doc')} ({ds.get('date')}), chapter {ds.get('chapter')}")
        if er:
            print(f"errata     : {er.get('doc')} ({er.get('date')})")
        n_errata = sum(1 for g in model.get("groups", {}).values()
                       for r in g.get("registers", {}).values() if r.get("errata"))
        print(f"contents   : {n_regs} registers, {n_bits} bit fields, "
              f"{n_errata} carrying errata notes")
        pct = (100 * n_verified // n_regs) if n_regs else 0
        print(f"verified   : {n_verified}/{n_regs} registers ({pct}%) checked against a document")
        print()

    for where, text in rep.warnings:
        print(f"WARNING  {where}: {text}")
    for where, text in rep.errors:
        print(f"ERROR    {where}: {text}")

    if rep.errors:
        print(f"\n{len(rep.errors)} error(s), {len(rep.warnings)} warning(s)")
        return 1
    print(f"\nno errors ({len(rep.warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
