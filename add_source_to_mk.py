#!/usr/bin/env python3
"""add_source_to_mk.py - add a new firmware/src module to the generated makefile.

nbproject/configurations.xml is tracked and the source of truth, but
nbproject/Makefile-default.mk is generated and gitignored - and it is what
build.bat actually drives. A new .c file added without the IDE is therefore
silently not compiled. This script patches the makefile the way the IDE would:
the file-list variables at the top, plus one compile rule per configuration
(debug and production), modelled on an existing module.

It is idempotent, refuses to guess if the layout does not match, and becomes
unnecessary the moment the project is opened in MPLAB X, which regenerates the
makefile from configurations.xml.

    python add_source_to_mk.py <new-module> [<template-module>]

e.g.  python add_source_to_mk.py ptp_gm noip_test

Both names are bare module names without path or extension. Remember to add the
matching <itemPath> entries to configurations.xml as well - that is the part that
survives regeneration. See CLAUDE.md section 6.
"""
import re
import sys

MK = "firmware/T1S_100BaseT_Bridge.X/nbproject/Makefile-default.mk"
OBJDIR = "${OBJECTDIR}/_ext/1360937237/"      # object directory for everything in src/


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    new = sys.argv[1]
    old = sys.argv[2] if len(sys.argv) > 2 else "noip_test"

    with open(MK, "r", encoding="utf-8", newline="") as fh:
        lines = fh.readlines()

    if any(new in ln for ln in lines):
        print("%s is already in the makefile - nothing to do" % new)
        return 0
    if not any(old in ln for ln in lines):
        print("template module %s not found in the makefile" % old)
        return 1

    # 1) the file-list variables (SOURCEFILES, OBJECTFILES, dependency lists)
    hits = 0
    for i, ln in enumerate(lines[:100]):
        orig = ln
        ln = ln.replace("../src/%s.c" % old, "../src/%s.c ../src/%s.c" % (old, new))
        ln = ln.replace(OBJDIR + old + ".o.d", OBJDIR + old + ".o.d " + OBJDIR + new + ".o.d")
        ln = re.sub(re.escape(OBJDIR + old + ".o") + r"(?!\.d)",
                    OBJDIR + old + ".o " + OBJDIR + new + ".o", ln)
        if ln != orig:
            lines[i] = ln
            hits += 1

    # 2) the compile rules: target line, four body lines, blank separator
    target = OBJDIR + old + ".o: ../src/%s.c" % old
    out, rules, i = [], 0, 0
    while i < len(lines):
        out.append(lines[i])
        if lines[i].startswith(target):
            block = lines[i:i + 6]
            if not block[4].lstrip().startswith("${MP_CC}"):
                print("unexpected rule layout around line %d - patch by hand" % (i + 1))
                return 1
            out.extend(lines[i + 1:i + 6])
            out.extend(ln.replace(old, new) for ln in block)
            i += 6
            rules += 1
            continue
        i += 1

    if hits < 4 or rules < 1:
        print("only %d variable lines and %d rules matched - patch by hand" % (hits, rules))
        return 1

    with open(MK, "w", encoding="utf-8", newline="") as fh:
        fh.writelines(out)
    print("added %s: %d variable lines, %d compile rules" % (new, hits, rules))
    return 0


if __name__ == "__main__":
    sys.exit(main())
