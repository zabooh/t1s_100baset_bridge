#!/usr/bin/env python3
"""
flash_all.py
------------
Programmiert beide Boards (Follower + Grandmaster) nacheinander via MDB
und gibt den Controller nach dem Flash automatisch frei (reset + run).

Aufruf:
  python flash_all.py
"""
import sys
import os

# --- Konfiguration -----------------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))

HEX_FILE         = os.path.join(_HERE, r"dist\default\production\T1S_100BaseT_Bridge.X.production.hex")
FOLLOWER_SERIAL  = "ATML3264031800001049"
GRANDMASTER_SERIAL = "ATML3264031800001290"
# -----------------------------------------------------------------------------

# mdb_flash.py liegt im selben Verzeichnis
sys.path.insert(0, _HERE)
from mdb_flash import flash

def main():
    errors = 0

    print("\n### Flash FOLLOWER ###")
    rc = flash(HEX_FILE, FOLLOWER_SERIAL, label="FOLLOWER")
    if rc != 0:
        print("[FOLLOWER] FEHLER beim Programmieren!")
        errors += 1

    print("\n### Flash GRANDMASTER ###")
    rc = flash(HEX_FILE, GRANDMASTER_SERIAL, label="GRANDMASTER")
    if rc != 0:
        print("[GRANDMASTER] FEHLER beim Programmieren!")
        errors += 1

    print()
    if errors == 0:
        print("=== Beide Boards erfolgreich programmiert und gestartet. ===")
    else:
        print(f"=== {errors} Board(s) konnten nicht programmiert werden. ===")

    return errors

if __name__ == "__main__":
    sys.exit(main())
