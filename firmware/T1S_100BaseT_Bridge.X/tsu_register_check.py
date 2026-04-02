#!/usr/bin/env python3
"""
tsu_register_check.py
---------------------
LAN8651 TSU / PTP Register Validation.

Implementiert Testplan aus README_TSU_Check.md + Block-A-Hypothese aus README_TSU_RootCause.md.
Höchste Priorität: TIER 4 + CHK-12 (NETWORK_CONTROL Bit 15).

Verwendung:
    python tsu_register_check.py --gm COM10 [--fol COM8]
    python tsu_register_check.py --gm COM10 --chk12-only
    python tsu_register_check.py --gm COM10 --skip-tier1 --skip-tier2
"""

import serial
import time
import re
import sys
import argparse
import threading
from datetime import datetime

# ---------------------------------------------------------------------------
# Konfiguration
# ---------------------------------------------------------------------------
BAUDRATE       = 115200
SERIAL_TIMEOUT = 3
RESP_TIMEOUT   = 5.0
PROMPT_MARKERS = ["\r\n> ", "\n> ", "> "]

TTSC_WAIT_S    = 30.0    # Wartezeit auf TTSCAA/TTSCMA pro Testlauf
CHK12_WAIT_S   = 12.0    # Wartezeit pro A/B-Testlauf (CHK-12)

# ---------------------------------------------------------------------------
# Register-Adressen (aus ptp_gm_task.h + LAN8651 Datenblatt)
# ---------------------------------------------------------------------------
REG = {
    "CONFIG0"     : 0x00000004,
    "STATUS0"     : 0x00000008,
    "STATUS1"     : 0x00000009,
    "IMASK0"      : 0x0000000C,
    "IMASK1"      : 0x0000000D,
    "TTSCAH"      : 0x00000010,
    "TTSCAL"      : 0x00000011,
    "TTSCBH"      : 0x00000012,
    "TTSCBL"      : 0x00000013,
    "TTSCCH"      : 0x00000014,
    "TTSCCL"      : 0x00000015,
    "NETWORK_CTRL": 0x00010000,
    "NETWORK_CFG" : 0x00010001,
    "MAC_TSH"     : 0x00010070,
    "MAC_TSL"     : 0x00010074,
    "MAC_TN"      : 0x00010075,
    "MAC_TI"      : 0x00010077,
    "TXMCTL"      : 0x00040040,
    "TXMPATH"     : 0x00040041,
    "TXMPATL"     : 0x00040042,
    "TXMMSKH"     : 0x00040043,
    "TXMMSKL"     : 0x00040044,
    "TXMLOC"      : 0x00040045,
    "RXMCTL"      : 0x00040050,
    "RXMMSKH"     : 0x00040053,
    "RXMMSKL"     : 0x00040054,
    "RXMLOC"      : 0x00040055,
}

# ---------------------------------------------------------------------------
# Logging: Tee (Konsole + Datei)
# ---------------------------------------------------------------------------
class _Tee:
    def __init__(self, stream, filepath):
        self._stream = stream
        self._file   = open(filepath, "w", encoding="utf-8")

    def write(self, data):
        self._stream.write(data)
        self._stream.flush()
        self._file.write(data)
        self._file.flush()

    def flush(self):
        self._stream.flush()
        self._file.flush()

    def fileno(self):
        return self._stream.fileno()

    def close(self):
        self._file.close()

# ---------------------------------------------------------------------------
# Serielle Hilfsfunktionen (gleiche Muster wie ptp_diag.py)
# ---------------------------------------------------------------------------
def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUDRATE, timeout=SERIAL_TIMEOUT)
    time.sleep(0.2)
    ser.reset_input_buffer()
    print(f"[{port}] Verbunden.")
    return ser

def wake_port(ser: serial.Serial, name: str) -> None:
    ser.write(b"\r\n")
    time.sleep(0.5)
    ser.reset_input_buffer()
    print(f"[{name}] Prompt bereit.")

def send_cmd(ser: serial.Serial, name: str, cmd: str,
             timeout: float = RESP_TIMEOUT) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    print(f"[{name}] >>> {cmd}")
    response = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            response += chunk
            if any(response.endswith(p) for p in PROMPT_MARKERS):
                break
        else:
            time.sleep(0.05)
    time.sleep(0.1)
    if ser.in_waiting:
        response += ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
    lines = [l for l in response.splitlines() if cmd not in l]
    out = "\n".join(lines).strip()
    if out:
        print(f"[{name}] <<< {out}")
    return response

def capture_async(ser: serial.Serial, name: str, duration_s: float) -> str:
    deadline = time.time() + duration_s
    captured = ""
    while time.time() < deadline:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            captured += chunk
            for line in chunk.splitlines():
                if line.strip():
                    print(f"  [{name}] {line}")
        else:
            time.sleep(0.05)
    return captured

def capture_until(ser: serial.Serial, name: str,
                  stop_strings: list, timeout_s: float) -> str:
    """Capture until any stop_string appears, or timeout."""
    deadline = time.time() + timeout_s
    captured = ""
    while time.time() < deadline:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            captured += chunk
            for line in chunk.splitlines():
                if line.strip():
                    print(f"  [{name}] {line}")
            if any(s in captured for s in stop_strings):
                break
        else:
            time.sleep(0.05)
    return captured

def drain(ser: serial.Serial) -> None:
    time.sleep(0.15)
    ser.reset_input_buffer()

# ---------------------------------------------------------------------------
# Register-Zugriff via CLI (lan_read / lan_write)
# ---------------------------------------------------------------------------
def lan_read(ser: serial.Serial, name: str, addr: int) -> "int | None":
    addr_str = f"0x{addr:08X}"
    resp = send_cmd(ser, name, f"lan_read {addr_str}", timeout=3.0)
    time.sleep(0.1)
    extra = capture_async(ser, name, 0.4)
    text = resp + extra
    # Format: "LAN865X Read: Addr=0x00010000 Value=0x0000000C"
    m = re.search(r'LAN865X Read: Addr=0x[0-9A-Fa-f]+\s+Value=(0x[0-9A-Fa-f]+)', text)
    if m:
        return int(m.group(1), 16)
    # Fallback: any Value= on the line
    m2 = re.search(r'Value=(0x[0-9A-Fa-f]+)', text)
    if m2:
        return int(m2.group(1), 16)
    return None

def lan_write(ser: serial.Serial, name: str, addr: int, value: int) -> str:
    resp = send_cmd(ser, name, f"lan_write 0x{addr:08X} 0x{value:08X}", timeout=3.0)
    time.sleep(0.1)
    return resp

# ---------------------------------------------------------------------------
# Ausgabe-Helfer
# ---------------------------------------------------------------------------
def ok(label: str, detail: str = "") -> bool:
    s = f"  [PASS] {label}"
    if detail: s += f"  ({detail})"
    print(s)
    return True

def fail(label: str, detail: str = "") -> bool:
    s = f"  [FAIL] {label}"
    if detail: s += f"  ({detail})"
    print(s)
    return False

def warn(label: str, detail: str = "") -> None:
    s = f"  [WARN] {label}"
    if detail: s += f"  ({detail})"
    print(s)

def hint(text: str) -> None:
    for line in text.strip().splitlines():
        print(f"         ↳ {line}")

def info(label: str, value) -> None:
    print(f"  [INFO] {label}: {value}")

def section(title: str) -> None:
    bar = "─" * 62
    print(f"\n{bar}\n{title}\n{bar}")

# ---------------------------------------------------------------------------
# TIER 1: TSU Wall-Clock Verification
# ---------------------------------------------------------------------------
def tier1_tsu(ser: serial.Serial, name: str) -> bool:
    section("TIER 1: TSU Wall-Clock Verification (TEST_TSU_01..04)")
    results = []

    # TEST_TSU_01 — MAC_TI Readback
    val = lan_read(ser, name, REG["MAC_TI"])
    if val is None:
        results.append(fail("TEST_TSU_01 MAC_TI lesbar"))
        hint("lan_read antwortet nicht → SPI-Problem oder Firmware nicht geladen")
    elif val == 0x28:
        results.append(ok("TEST_TSU_01 MAC_TI=0x28 (40 ns/Takt)"))
    else:
        results.append(fail("TEST_TSU_01 MAC_TI falsch", f"0x{val:08X} statt 0x28"))
        hint("PTP_GM_Init() nicht aufgerufen, oder LOFR-Reset hat MAC_TI überschrieben")

    # TEST_TSU_02 — MAC_TN zählt
    tn1 = lan_read(ser, name, REG["MAC_TN"])
    info("TEST_TSU_02 MAC_TN t=0", f"0x{tn1:08X}" if tn1 is not None else "N/A")
    print("  Warte 500 ms ...")
    time.sleep(0.5)
    tn2 = lan_read(ser, name, REG["MAC_TN"])
    info("TEST_TSU_02 MAC_TN t=500ms", f"0x{tn2:08X}" if tn2 is not None else "N/A")
    if tn1 is not None and tn2 is not None:
        delta = (tn2 - tn1) if tn2 >= tn1 else (1_000_000_000 - tn1 + tn2)
        expected = 500_000_000
        in_range = abs(delta - expected) < expected * 0.25
        if in_range:
            results.append(ok("TEST_TSU_02 MAC_TN zählt", f"Δ={delta:,} ns ({delta/1e6:.0f} ms)"))
        else:
            results.append(fail("TEST_TSU_02 MAC_TN Δ außerhalb 375–625 ms",
                                f"Δ={delta:,} ns"))
            hint("TSU zählt nicht korrekt → MAC_TI=0 oder GEM nicht initialisiert")
    else:
        results.append(fail("TEST_TSU_02 MAC_TN lesbar"))

    # TEST_TSU_03 — MAC_TSL steigt nach 1.2 s
    sl1 = lan_read(ser, name, REG["MAC_TSL"])
    info("TEST_TSU_03 MAC_TSL t=0", f"0x{sl1:08X}" if sl1 is not None else "N/A")
    print("  Warte 1.2 s auf Sekunden-Rollover ...")
    time.sleep(1.2)
    sl2 = lan_read(ser, name, REG["MAC_TSL"])
    info("TEST_TSU_03 MAC_TSL t=1.2s", f"0x{sl2:08X}" if sl2 is not None else "N/A")
    if sl1 is not None and sl2 is not None:
        if sl2 - sl1 >= 1:
            results.append(ok("TEST_TSU_03 MAC_TSL steigt", f"Δ={sl2 - sl1} s"))
        else:
            results.append(fail("TEST_TSU_03 MAC_TSL steigt nicht",
                                f"sl1={sl1} sl2={sl2}"))
            hint("MAC_TN läuft aber TSL nicht → Sekunden-Überlauf-Logik defekt?")
    else:
        results.append(fail("TEST_TSU_03 MAC_TSL lesbar"))

    # TEST_TSU_04 — MAC_TSH stabil
    h1 = lan_read(ser, name, REG["MAC_TSH"])
    time.sleep(2.0)
    h2 = lan_read(ser, name, REG["MAC_TSH"])
    if h1 is not None and h2 is not None:
        if h1 == h2:
            results.append(ok("TEST_TSU_04 MAC_TSH stabil", f"0x{h1:08X}"))
        else:
            results.append(fail("TEST_TSU_04 MAC_TSH instabil",
                                f"0x{h1:08X} → 0x{h2:08X}"))
    else:
        results.append(fail("TEST_TSU_04 MAC_TSH lesbar"))

    return all(results)

# ---------------------------------------------------------------------------
# TIER 2: OA Register Coherence
# ---------------------------------------------------------------------------
def tier2_oa(ser: serial.Serial, name: str) -> bool:
    section("TIER 2: OA Register Kohärenz (TEST_OA_01..04)")
    results = []

    # TEST_OA_01 — CONFIG0
    val = lan_read(ser, name, REG["CONFIG0"])
    if val is None:
        results.append(fail("TEST_OA_01 CONFIG0 lesbar"))
    elif val == 0x90E6:
        results.append(ok("TEST_OA_01 CONFIG0=0x90E6 (FTSE+FTSS korrekt)"))
    elif val == 0x9026:
        results.append(fail("TEST_OA_01 CONFIG0=0x9026 (FTSE/FTSS nicht gesetzt!)",
                             "case 8 lief vor MemMap → überschrieben"))
        hint("_InitUserSettings case 8 schreibt 0x90E6, aber MEMMAP schreibt 0x9026\n"
             "Wenn hier 0x9026 gelesen wird: Reihenfolge Init→MemMap→case8 ist kaputt")
    else:
        results.append(False)
        warn("TEST_OA_01 CONFIG0 unerwarteter Wert", f"0x{val:08X} (erwartet 0x90E6)")

    # TEST_OA_02 — STATUS0 W1C
    lan_write(ser, name, REG["STATUS0"], 0xFFFFFFFF)
    time.sleep(0.05)
    val = lan_read(ser, name, REG["STATUS0"])
    if val is None:
        results.append(fail("TEST_OA_02 STATUS0 nach W1C lesbar"))
    elif val == 0:
        results.append(ok("TEST_OA_02 STATUS0 W1C korrekt (0x00000000 nach Löschen)"))
    else:
        warn("TEST_OA_02 STATUS0 nach W1C ≠ 0", f"0x{val:08X} — neue Events zwischen Write und Read")
        hint("Wenn permanent ≠ 0: LOFR oder andere Dauerfehler aktiv")

    # TEST_OA_03 — IMASK0 / IMASK1
    imask0 = lan_read(ser, name, REG["IMASK0"])
    imask1 = lan_read(ser, name, REG["IMASK1"])
    if imask0 is not None:
        if imask0 == 0:
            results.append(ok("TEST_OA_03 IMASK0=0x00000000 (TTSCAA unmaskiert ✓)"))
        else:
            results.append(fail("TEST_OA_03 IMASK0≠0", f"0x{imask0:08X}"))
            if imask0 & 0x0100:
                hint("❌ KRITISCH: Bit 8 (TTSCAA) ist maskiert → STATUS0-EXST-Interrupt blockiert!")
    if imask1 is not None:
        info("TEST_OA_03 IMASK1", f"0x{imask1:08X}")

    # TEST_OA_04 — TTSC Register Baseline
    print("\n  TEST_OA_04: Capture-Register vor Aktivierung (sollen 0 sein):")
    all_zero = True
    for reg_name in ["TTSCAH", "TTSCAL", "TTSCBH", "TTSCBL", "TTSCCH", "TTSCCL"]:
        v = lan_read(ser, name, REG[reg_name])
        sym = "✓" if v == 0 else "⚠"
        print(f"    {sym}  {reg_name} = {'0x{:08X}'.format(v) if v is not None else 'N/A'}")
        if v is not None and v != 0:
            all_zero = False
    if all_zero:
        results.append(ok("TEST_OA_04 Alle TTSC-Register = 0 (Baseline)"))
    else:
        warn("TEST_OA_04 Mindestens ein TTSC-Register ≠ 0",
             "Capture offen aus vorherigem Lauf?")

    return all(results)

# ---------------------------------------------------------------------------
# TIER 3: TX-Match + NETWORK_CONTROL Analyse
# ---------------------------------------------------------------------------
def tier3_txm_and_nc(ser: serial.Serial, name: str) -> bool:
    section("TIER 3: TX-Match-Register + NETWORK_CONTROL (CHK-10)")
    results = []

    # TEST_TXM_01 — Readback aller TX-Match-Register
    expected = {
        "TXMLOC"  : 12,
        "TXMPATH" : 0x88,
        "TXMPATL" : 0xF710,
        "TXMMSKH" : 0x00,
        "TXMMSKL" : 0x00,
    }
    print("\n  TEST_TXM_01: TX-Match-Register Readback vs. Expected:")
    all_match = True
    for reg_name, exp_val in expected.items():
        v = lan_read(ser, name, REG[reg_name])
        if v is None:
            print(f"    ?  {reg_name:<10} = N/A  (Lesefehler)")
            all_match = False
        elif (v & 0xFFFF) == (exp_val & 0xFFFF):
            print(f"    ✓  {reg_name:<10} = 0x{v:08X}  (erwartet 0x{exp_val:04X})")
        else:
            print(f"    ✗  {reg_name:<10} = 0x{v:08X}  (erwartet 0x{exp_val:04X})  ← MISMATCH")
            all_match = False
    results.append(ok("TEST_TXM_01 alle TX-Match-Werte korrekt") if all_match
                   else fail("TEST_TXM_01 TX-Match-Mismatch"))

    # TXMPATL unteres Byte analysieren
    txmpatl = lan_read(ser, name, REG["TXMPATL"])
    if txmpatl is not None:
        low = txmpatl & 0xFF
        eth_low = (txmpatl >> 8) & 0xFF
        info("TEST_TXM_02 TXMPATL", f"0x{txmpatl:08X}  EType-Low=0x{eth_low:02X}  CtrlByte=0x{low:02X}")
        if low == 0x10:
            hint("Bit 4 (0x10) in TXMPATL CtrlByte — möglicherweise TSMT-Trigger-Bit\n"
                 "Für TSC-Pfad irrelevant, für Pattern-Match-Trigger evtl. nötig")
        if eth_low == 0xF7:
            results.append(ok("TEST_TXM_02 EtherType-Low = 0xF7 korrekt"))
        else:
            results.append(fail("TEST_TXM_02 EtherType-Low falsch", f"0x{eth_low:02X}"))

    # TXMCTL aktueller Zustand
    txmctl = lan_read(ser, name, REG["TXMCTL"])
    if txmctl is not None:
        info("TXMCTL", (f"0x{txmctl:08X}  "
                        f"TXME={int(bool(txmctl & 0x0002))}  "
                        f"MACTXTSE={int(bool(txmctl & 0x0004))}  "
                        f"TXPMDET={int(bool(txmctl & 0x0080))}"))

    # --- CHK-10: NETWORK_CONTROL Basis-Analyse ---
    print()
    nc = lan_read(ser, name, REG["NETWORK_CTRL"])
    if nc is not None:
        txen  = bool(nc & 0x0008)
        rxen  = bool(nc & 0x0004)
        bit15 = bool(nc & 0x8000)
        bit18 = bool(nc & 0x40000)
        info("CHK-10 NETWORK_CTRL", (f"0x{nc:08X}  "
                                     f"TXEN={int(txen)}  RXEN={int(rxen)}  "
                                     f"Bit15={int(bit15)}  Bit18={int(bit18)}"))
        if not txen or not rxen:
            results.append(fail("CHK-10 TXEN oder RXEN nicht gesetzt!",
                                f"0x{nc:08X}"))
            hint("MAC nicht aktiv — kein TX möglich")
        else:
            ok("CHK-10 TXEN+RXEN gesetzt")
        if not bit15:
            warn("CHK-10 NETWORK_CTRL Bit 15 NICHT gesetzt",
                 "→ Block-A-Hypothese: GEM-TSU TX-Capture möglicherweise deaktiviert")
            hint("Test: lan_write 0x00010000 0x0000800C\n"
                 "Dann ptp_mode master → prüfe ob [DBG] _OnStatus0: 0x00000100 erscheint")
        else:
            ok("CHK-10 Bit 15 gesetzt (GEM-TSU TX-Capture-Enable aktiv)")

    # RX Match Baseline
    print("\n  TIER 5 (kurz): RX-Match-Register:")
    for r in ["RXMCTL", "RXMMSKH", "RXMMSKL", "RXMLOC"]:
        v = lan_read(ser, name, REG[r])
        print(f"    {r:<10} = {'0x{:08X}'.format(v) if v is not None else 'N/A'}")

    return all(results)

# ---------------------------------------------------------------------------
# Hilfsfunktion: TTSC-Auswertung aus Capture-Text
# ---------------------------------------------------------------------------
def _analyse_capture(captured: str) -> dict:
    dbg0 = re.findall(r'\[DBG\] _OnStatus0: (0x[0-9A-Fa-f]+)', captured)
    dbg1 = re.findall(r'\[DBG\] _OnStatus1: (0x[0-9A-Fa-f]+)', captured)
    fu    = len(re.findall(r'\[PTP-GM\] FU #', captured))
    syncs = len(re.findall(r'\[PTP-GM\] Sync #', captured))
    txpmd = len(re.findall(r'TXPMDET ok', captured))
    ttsca_not = len(re.findall(r'TTSCA not set', captured))
    ttscma = len(re.findall(r'TX_Timestamp_Capture_Missed_A', captured))

    # Check TTSCAA = bit 8 in STATUS0 events
    ttscaa_events = [v for v in dbg0 if int(v, 16) & 0x0700]

    # Decode STATUS1 raw values
    ttscma_raw = sum(1 for v in dbg1 if int(v, 16) & 0x01000000)
    ttscofa_raw = sum(1 for v in dbg1 if int(v, 16) & 0x00200000)
    fsm_err     = sum(1 for v in dbg1 if int(v, 16) & 0x00020000)

    return {
        "dbg0"       : dbg0,
        "dbg1"       : dbg1,
        "fu"         : fu,
        "syncs"      : syncs,
        "txpmd"      : txpmd,
        "ttsca_not"  : ttsca_not,
        "ttscma_print": ttscma,
        "ttscaa_events": ttscaa_events,
        "ttscma_raw" : ttscma_raw,
        "ttscofa_raw": ttscofa_raw,
        "fsm_err"    : fsm_err,
    }

def _print_analysis(a: dict) -> None:
    info("Syncs gesendet",   a["syncs"])
    info("TXPMDET ok",       a["txpmd"])
    info("STATUS0 events",   f"{len(a['dbg0'])}  (mit TTSCAA-Bit: {len(a['ttscaa_events'])})")
    info("STATUS1 events",   f"{len(a['dbg1'])}  (TTSCMA={a['ttscma_raw']}  TTSCOFA={a['ttscofa_raw']}  FSM_ERR={a['fsm_err']})")
    info("FU gesendet",      a["fu"])
    info("TTSCA not set",    a["ttsca_not"])
    if a["ttscaa_events"]:
        for v_str in a["ttscaa_events"][:5]:
            v = int(v_str, 16)
            print(f"    STATUS0={v_str}  TTSCAA={int(bool(v & 0x100))}  "
                  f"TTSCAB={int(bool(v & 0x200))}  TTSCAC={int(bool(v & 0x400))}")
    if a["dbg1"]:
        for v_str in a["dbg1"][:5]:
            v = int(v_str, 16)
            print(f"    STATUS1={v_str}  TTSCMA={int(bool(v & 0x01000000))}  "
                  f"TTSCOFA={int(bool(v & 0x00200000))}  FSM_ERR={int(bool(v & 0x00020000))}")

# ---------------------------------------------------------------------------
# TIER 4: TX-Timestamp Capture Test
# ---------------------------------------------------------------------------
def tier4_ttscaa(ser: serial.Serial, name: str) -> bool:
    section("TIER 4: TX Timestamp Capture — TEST_TTSC_01 (BASELINE)")

    print(f"\n  Setup: ptp_mode off → STATUS0 clear → ptp_mode master")
    send_cmd(ser, name, "ptp_mode off", timeout=3.0)
    time.sleep(0.4)
    lan_write(ser, name, REG["STATUS0"], 0xFFFFFFFF)
    time.sleep(0.1)
    send_cmd(ser, name, "ptp_mode master", timeout=5.0)
    time.sleep(0.5)

    print(f"\n  Warte {TTSC_WAIT_S:.0f} s auf TTSCAA oder TTSCMA ...")
    stop_tokens = ["[PTP-GM] FU #", "TX_Timestamp_Capture_Missed_A",
                   "[DBG] _OnStatus0:", "[DBG] _OnStatus1:"]
    captured = capture_until(ser, name, stop_tokens, TTSC_WAIT_S)
    captured += capture_async(ser, name, 3.0)

    a = _analyse_capture(captured)
    print("\n  ── TIER 4 Auswertung ──")
    _print_analysis(a)

    ttscaa_ok = bool(a["ttscaa_events"]) or a["fu"] > 0

    send_cmd(ser, name, "ptp_mode off", timeout=3.0)

    if ttscaa_ok:
        return ok("TEST_TTSC_01 TTSCAA gesetzt! *** CAPTURE ERFOLGREICH ***",
                  f"FU={a['fu']}, TTSCAA-Events={len(a['ttscaa_events'])}")
    elif a["ttscma_raw"] > 0 or a["ttscma_print"] > 0:
        fail("TEST_TTSC_01 TTSCMA — GEM-TSU liefert keinen Timestamp",
             f"TTSCMA-Events={max(a['ttscma_raw'], a['ttscma_print'])}, Syncs={a['syncs']}")
        hint("TC6 sieht TSC=1 → will Capture → GEM-TSU verweigert es\n"
             "→ Hypothese Block A: NETWORK_CONTROL fehlt GEM-TSU-Enable-Bit\n"
             "→ CHK-12 wird automatisch gestartet")
        return False
    elif a["ttsca_not"] > 0:
        fail("TEST_TTSC_01 TTSCA not set — EXST ohne TTSCAA",
             f"TTSCA-not-set={a['ttsca_not']}, Syncs={a['syncs']}")
        hint("EXST feuert, STATUS0 zeigt nirgends TTSCAA")
        return False
    else:
        fail("TEST_TTSC_01 Kein Ereignis in 30 s",
             f"Syncs={a['syncs']}")
        hint("Keine Syncs → ptp_mode master funktioniert nicht\nPLCA-Link down?")
        return False

# ---------------------------------------------------------------------------
# CHK-12: NETWORK_CONTROL Bit-15 A/B Vergleichstest
# ---------------------------------------------------------------------------
def chk12_nc_bit15(ser: serial.Serial, name: str) -> bool:
    section("CHK-12: NETWORK_CONTROL Bit 15 — A/B Vergleichstest")
    print("\n  Hypothese (Block A, README_TSU_RootCause.md):")
    print("  GEM-TSU TX-Capture erfordert Bit 15 in NETWORK_CONTROL (STORE_TX_TS).")
    print(f"\n  Test A: NC = 0x0000000C (Baseline, kein Bit15)  — {CHK12_WAIT_S:.0f} s")
    print(f"  Test B: NC = 0x0000800C (+Bit15)                — {CHK12_WAIT_S:.0f} s")

    def run_one(nc_val: int, label: str) -> dict:
        print(f"\n  ── {label} — NC=0x{nc_val:08X} ──")
        send_cmd(ser, name, "ptp_mode off", timeout=3.0)
        time.sleep(0.4)
        lan_write(ser, name, REG["STATUS0"], 0xFFFFFFFF)
        lan_write(ser, name, REG["NETWORK_CTRL"], nc_val)
        time.sleep(0.1)
        rb = lan_read(ser, name, REG["NETWORK_CTRL"])
        if rb is not None:
            info(f"  NC readback", f"0x{rb:08X}{' ✓' if rb == nc_val else ' ← MISMATCH!'}")
        send_cmd(ser, name, "ptp_mode master", timeout=5.0)
        time.sleep(0.5)
        stop_tokens = ["[PTP-GM] FU #", "TX_Timestamp_Capture_Missed_A",
                       "[DBG] _OnStatus0:", "[DBG] _OnStatus1:"]
        cap = capture_until(ser, name, stop_tokens, CHK12_WAIT_S)
        cap += capture_async(ser, name, 2.5)
        send_cmd(ser, name, "ptp_mode off", timeout=3.0)
        return _analyse_capture(cap)

    a = run_one(0x0000000C, "Test A — Baseline")
    time.sleep(1.0)
    b = run_one(0x0000800C, "Test B — +Bit15")

    # Restore
    lan_write(ser, name, REG["NETWORK_CTRL"], 0x0000000C)

    # Report
    ttscaa_a = bool(a["ttscaa_events"]) or a["fu"] > 0
    ttscaa_b = bool(b["ttscaa_events"]) or b["fu"] > 0
    ttscma_a = max(a["ttscma_raw"], a["ttscma_print"])
    ttscma_b = max(b["ttscma_raw"], b["ttscma_print"])

    print("\n  ── CHK-12 Vergleich ──")
    print(f"  Test A (NC=000C):  TTSCAA={'JA ✓' if ttscaa_a else 'nein'}  "
          f"TTSCMA={ttscma_a}  Syncs={a['syncs']}  FU={a['fu']}")
    print(f"  Test B (NC=800C):  TTSCAA={'JA ✓' if ttscaa_b else 'nein'}  "
          f"TTSCMA={ttscma_b}  Syncs={b['syncs']}  FU={b['fu']}")

    print()
    if not ttscaa_a and ttscaa_b:
        ok("CHK-12 BESTÄTIGT: Bit 15 ist entscheidend!",
           "Test A: TTSCMA / Test B: TTSCAA")
        hint("FIX: _InitUserSettings case 9 → Wert von 0x0000000C auf 0x0000800C ändern\n"
             "Datei: src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c")
        return True
    elif ttscaa_a and ttscaa_b:
        ok("CHK-12: TTSCAA in beiden Tests — Bit 15 nicht das Problem",
           "Möglicherweise bereits durch anderen Fix behoben")
        return True
    elif not ttscaa_a and not ttscaa_b:
        fail("CHK-12: TTSCAA in KEINEM Test — Bit 15 allein nicht ausreichend")
        hint("Weitere Bits nötig? GEM Bit18, NETWORK_CFG, oder GEM braucht Soft-Reset?\n"
             "Nächster Schritt: STATUS1 FSM_Error prüfen, Microchip Support kontaktieren")
        return False
    else:
        warn("CHK-12: Test A hatte TTSCAA, Test B nicht — Regression durch Bit 15")
        return False

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="LAN8651 TSU Register Validation (ptp_gm_task + drv_lan865x)")
    parser.add_argument("--gm",         default="COM10",
                        help="GM COM-Port (default: COM10)")
    parser.add_argument("--fol",        default=None,
                        help="Follower COM-Port (optional, nur für RX-Tests)")
    parser.add_argument("--skip-tier1", action="store_true",
                        help="TIER 1 TSU Wall-Clock überspringen")
    parser.add_argument("--skip-tier2", action="store_true",
                        help="TIER 2 OA-Register überspringen")
    parser.add_argument("--skip-tier3", action="store_true",
                        help="TIER 3 TX-Match überspringen")
    parser.add_argument("--skip-tier4", action="store_true",
                        help="TIER 4 Capture-Test überspringen")
    parser.add_argument("--chk12-only", action="store_true",
                        help="Nur CHK-12 A/B Bit-15-Test ausführen")
    args = parser.parse_args()

    ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = f"tsu_check_{ts}.log"
    tee      = _Tee(sys.stdout, log_path)
    sys.stdout = tee

    print(f"\n{'═'*62}")
    print(f"  LAN8651 TSU Register Validation — {ts}")
    print(f"  GM: {args.gm}" + (f"   FOL: {args.fol}" if args.fol else ""))
    print(f"  Log: {log_path}")
    print(f"{'═'*62}")

    try:
        ser_gm = open_port(args.gm)
        wake_port(ser_gm, args.gm)
    except Exception as e:
        print(f"\n[FEHLER] Kann {args.gm} nicht öffnen: {e}")
        sys.exit(1)

    results = {}
    try:
        if args.chk12_only:
            results["CHK-12"] = chk12_nc_bit15(ser_gm, args.gm)
        else:
            if not args.skip_tier1:
                results["TIER-1 TSU-Uhr"] = tier1_tsu(ser_gm, args.gm)
            if not args.skip_tier2:
                results["TIER-2 OA-Reg"] = tier2_oa(ser_gm, args.gm)
            if not args.skip_tier3:
                results["TIER-3 TX-Match+NC"] = tier3_txm_and_nc(ser_gm, args.gm)
            if not args.skip_tier4:
                t4_ok = tier4_ttscaa(ser_gm, args.gm)
                results["TIER-4 TTSCAA-Capture"] = t4_ok
                if not t4_ok:
                    print("\n  TIER 4 fehlgeschlagen → starte CHK-12 automatisch ...")
                    time.sleep(1.5)
                    results["CHK-12 Bit15-Test"] = chk12_nc_bit15(ser_gm, args.gm)

    except KeyboardInterrupt:
        print("\n\n[ABBRUCH] Strg+C erkannt.")
    finally:
        # Sicherheitszustand wiederherstellen
        try:
            send_cmd(ser_gm, args.gm, "ptp_mode off", timeout=3.0)
            lan_write(ser_gm, args.gm, REG["NETWORK_CTRL"], 0x0000000C)
        except Exception:
            pass

        # Zusammenfassung
        print(f"\n{'═'*62}")
        print("  ZUSAMMENFASSUNG")
        print(f"{'═'*62}")
        passed_count = 0
        for test_name, passed in results.items():
            sym = "✓" if passed else "✗"
            print(f"  {sym}  {test_name}")
            if passed:
                passed_count += 1
        print(f"\n  {passed_count}/{len(results)} Tests bestanden.")
        print(f"  Log gespeichert: {log_path}")

        if ser_gm.is_open:
            ser_gm.close()

        sys.stdout = tee._stream
        tee.close()

if __name__ == "__main__":
    main()
