# PTP Diagnose & Konvergenztest

Datum: 2026-04-01  
Uhrzeit: (Erstellung) 14:00 MESZ  
Zuletzt aktualisiert: 2026-04-01 (Testlauf 4 — `ptp_diag_20260401_165641.log`)

---

## 1. Was ist bereits implementiert

### Grandmaster (`ptp_gm_task.c`)

| Funktion | Status |
|---|---|
| `PTP_GM_Init()` | TX-Match-Register (TXMLOC/PATH/PATL/MSKH/MSKL), MAC_TI=40, PPSCTL=0x7D |
| `PTP_GM_Service()` | 1 ms Tick-basierte State Machine |
| `PTP_GM_Deinit()` | TX-Match disarm, MAC_TI=0, PPSCTL=0 |
| PTP Sync-Frame senden | EtherType 0x88F7, `tsc=0x01` (TX Timestamp Capture aktiviert) |
| TX Match arm vor jedem Sync | `GM_TXMCTL = 0x0001` in `GM_STATE_SEND_SYNC` |
| TX-Timestamp-Capture | State Machine: READ_TXMCTL → WAIT_TXMCTL → WAIT_STATUS0 → READ_TTSCA_H/L → WRITE_CLEAR → WAIT_CLEAR |
| PTP FollowUp senden | `GM_STATE_SEND_FOLLOWUP`: statischen TX-Pfadoffset (7650 ns) addiert |
| DST-MAC konfigurierbar | `ptp_dst broadcast` / `multicast` (01:80:C2:00:00:0E) |
| Sync-Intervall konfigurierbar | `ptp_interval <ms>` (default 125 ms) |
| State-Logging | Nur noch Schlüsselereignisse: `Sync #N`, `TXPMDET ok`, `FU #N t1=...`, Fehler-Timeouts (2026-04-01) |
| Diagnostikkommandos | `ptp_status`, `ptp_offset`, `ptp_reset` |

**LAN865x-API-Nutzung GM (alle aktiv seit 2026-04-01):**
- `DRV_LAN865X_WriteRegister` — TX-Match, MAC_TI, PPSCTL, Deinit ✓
- `DRV_LAN865X_ReadRegister` — TXMCTL-Polling, TTSCA Timestamps ✓
- `DRV_LAN865X_SendRawEthFrame` — Sync + FollowUp senden ✓
- `DRV_LAN865X_GetAndClearTsCapture` — STATUS0-Bits aus Interrupt-Handler ✓

---

### Follower (`ptp_bridge_task.c`)

| Funktion | Status |
|---|---|
| `PTP_Bridge_Init()` | PPSCTL=0x02 (Puls-Preset), SEVINTEN (PPSDONE-Interrupt) |
| `PTP_Bridge_OnFrame()` | Einstiegspunkt für EtherType 0x88F7 aus `pktEth0Handler()` |
| Sync-Frame verarbeiten | `processSync()`: SequenzID-Tracking und Empfangsbestätigung |
| FollowUp verarbeiten | `processFollowUp()`: t1 aus PTP-Header extrahieren, t2 aus HW-RX-Timestamp |
| Rate-Ratio-Schätzung | FIR-Tiefpassfilter über `diffLocal`/`diffRemote` |
| Servo-State-Machine | UNINIT → MATCHFREQ → HARDSYNC → COARSE → FINE |
| Uhranpassung | `MAC_TISUBN` + `MAC_TI` (Rate), `MAC_TA` (Offset), `MAC_TSL`/`MAC_TN` (Hard-Sync) |
| 1PPS aktivieren | PPSCTL=0x7D nach erstem Sync (einmalig) |
| `PTP_Bridge_GetOffset()` | Liest aktuellen Offset + Absolutwert aus |
| **Buffer-Leak-Fix** | `TCPIP_PKT_PacketAcknowledge()` nach `PTP_Bridge_OnFrame()` ✓ (2026-04-01) |

**LAN865x-API-Nutzung FOL (immer direkt aktiv, kein Makro-Schutz):**
- `DRV_LAN865X_WriteRegister` — MAC_TSL, MAC_TN, MAC_TI, MAC_TISUBN, MAC_TA, PPSCTL ✓

---

### Packet-Handler (`app.c`)

| Funktion | Status |
|---|---|
| `pktEth0Handler()` | EtherType 0x88F7 → `PTP_Bridge_OnFrame()` + `PacketAcknowledge()` |
| RX-Timestamp IPC | `g_ptp_rx_ts` (aus TC6-Callback) → an `PTP_Bridge_OnFrame()` übergeben |
| `ptp_mode follower` | `PTP_Bridge_SetMode(PTP_SLAVE)` + `resetSlaveNode()` |
| `ptp_mode master` | `PTP_GM_Init()` + `PTP_Bridge_SetMode(PTP_MASTER)` |
| `ptp_mode off` | `PTP_GM_Deinit()` + `PTP_Bridge_SetMode(PTP_DISABLED)` |

---

## 2. Testplan

### Vorbedingungen
- GM: COM10, IP 192.168.0.20, PLCA nodeId=0
- FOL: COM8, IP 192.168.0.30, PLCA nodeId=1
- Beide Boards mit aktueller Firmware geflasht (GM + Follower HEX)

### Schritt 1 — PTP-Modus starten und FollowUp-Pfad prüfen

**Ziel:** Prüfen ob GM jetzt den vollständigen Pfad läuft:  
`SEND_SYNC → READ_TXMCTL → WAIT_TXMCTL → WAIT_STATUS0 → READ_TTSCA_H/L → SEND_FOLLOWUP`

**Erwartete GM-Konsolausgabe:**
```
[PTP-GM][STATE] GM_STATE_SEND_SYNC @L...
[PTP-GM][STATE] GM_STATE_READ_TXMCTL @L...
[PTP-GM][STATE] GM_STATE_WAIT_TXMCTL @L...
[PTP-GM][STATE] GM_STATE_WAIT_STATUS0 @L...
[PTP-GM][STATE] GM_STATE_READ_TTSCA_H @L...
[PTP-GM][STATE] GM_STATE_READ_TTSCA_L @L...
[PTP-GM][STATE] GM_STATE_WRITE_CLEAR @L...
[PTP-GM][STATE] GM_STATE_WAIT_CLEAR @L...
[PTP-GM][STATE] GM_STATE_SEND_FOLLOWUP @L...
[PTP-GM][STATE] GM_STATE_WAIT_PERIOD @L...
```

**Kriterium:** `SEND_FOLLOWUP` erscheint → FollowUp wird gesendet.

### Schritt 2 — Follower-Servo-Konvergenz messen

**Ziel:** Follower-Servo läuft durch alle Phasen und konvergiert auf FINE.

**Erwartete FOL-Konsolausgabe (zeitlicher Ablauf):**
```
PTP UNINIT->MATCHFREQ  MAC_TI=40 TISUBN=0x...
PTP COARSE  offset=... val=...
PTP FINE    offset=... val=...
```

**Konvergenzkritierium:** `ptp_offset` auf FOL zeigt Offset < 100 ns für ≥ 10 aufeinanderfolgende Abfragen.

### Schritt 3 — Langzeitmessung Offset-Stabilität (60 s)

**Ziel:** Offset-Verlauf über 60 Sekunden messen (Drift, Schwankung, Ausreißer).

**Messung:** `ptp_offset` alle 1 s abfragen → Min/Max/Mittelwert/Standardabweichung.

### Schritt 4 — PTP-Modus-Zyklus (ptp_mode off → follower → master → prüfen)

**Ziel:** Prüfen ob nach `ptp_mode off` + Neustart des PTP-Modus die Konvergenz erneut stattfindet (kein Leak, kein Hänger).

---

## 3. Test-Ergebnisse — Testlauf 2026-04-01

### Laufparameter
- Firmware-Build: `build_dual.bat` → `=== BEIDE BUILDS ERFOLGREICH ===`
- Flash: `flash_dual.py` → `FOLLOWER SUCCESS` + `GRANDMASTER SUCCESS`
- Testskript: `ptp_diag.py` (Schritt 0–7, ohne `--no-reset`)

### Schritt 1 (FollowUp-Pfad)  ✗ FAIL

**Beobachtete GM-Ausgabe (Dauerschleife):**
```
[PTP-GM][STATE] GM_STATE_SEND_SYNC @L374
[PTP-GM][STATE] GM_STATE_READ_TXMCTL @L413
[PTP-GM][STATE] GM_STATE_WAIT_PERIOD @L421
[PTP-GM][STATE] GM_STATE_SEND_SYNC @L374
...
```
`WAIT_TXMCTL`, `WAIT_STATUS0`, `SEND_FOLLOWUP` — **nie erreicht**.

### Schritt 2 (Follower-Konvergenz)  ✗ FAIL

- `ptp_offset` auf FOL zeigt dauerhaft `offset=0 ns  abs=0 ns`
- Kein `PTP UNINIT->MATCHFREQ` in FOL-Log
- Servo bleibt in UNINIT (kein FollowUp empfangen)
- `CONVERGENCE_TIMEOUT = 120 s` → abgelaufen

### Schritt 3 (Stabilität) — übersprungen (kein Konvergenz)

### Zusammenfassung Testlauf

| Schritt | Ergebnis |
|---|---|
| 0 IP-Konfiguration | PASS |
| 1 Ping beidseitig | PASS |
| 2 PTP aktivieren | PASS |
| 3 GM Sync-Zähler steigt | PASS (`gmSyncs` zählt hoch) |
| 4 TX-Match TXPMDET | FAIL (TXPMDET nie gesetzt) |
| 5 Follower PTP-RX | PASS (0x88F7-Frames empfangen — nur Sync, kein FollowUp) |
| 6 GM FollowUp-Pfad | FAIL (`SEND_FOLLOWUP` nie erreicht) |
| 7 FOL Servo Konvergenz | FAIL (FINE timeout) |

---

## 4. Ursachenanalyse

### Root Cause #2 — Falscher TXMLOC-Wert (Byte-Offset EtherType)

**Symptom:** TXPMDET bleibt dauerhaft 0 — Timeout nach `MAX_RETRIES` Versuchen, kein `WAIT_STATUS0`

**Ursache:**  
`GM_PTP_INIT()` schrieb `TXMLOC = 30`, also suchte der TX-Match-Detektor das Pattern
`88 F7 10` bei **Byte 30** des Ethernet-Frames. Tatsächlich liegt das EtherType-Feld
(0x88F7) bei **Byte 12** (nach 6 Byte DST-MAC + 6 Byte SRC-MAC). Byte 30 liegt im
PTP-Payload (reserved-Felder = 0x00) → Pattern passt nie → TXPMDET nie gesetzt.

**Ethernet-Frame-Layout:**
```
Byte 0-5:   Dst-MAC
Byte 6-11:  Src-MAC
Byte 12-13: EtherType = 0x88F7   ← TXMLOC muss 12 sein
Byte 14:    PTP tsmt = 0x10
...
Byte 30:    PTP reserved (0x00)  ← TXMLOC=30 suchte hier
```

**Fix in `ptp_gm_task.c` `PTP_GM_Init()`:**
```c
// ALT (kaputt):
gm_write(GM_TXMLOC, 30u);   /* falsch: liegt im PTP-Payload */

// NEU (korrekt):
gm_write(GM_TXMLOC, 12u);   /* korrekt: EtherType bei Byte 12 (6+6=12) */
```

**Status:** ✓ ANGEWENDET (2026-04-01)

---

### Root Cause #1 — Invertierter Rückgabewert in `gm_read_register()` und `gm_write_register()`

**Symptom:** GM-State-Machine: `SEND_SYNC → READ_TXMCTL → WAIT_PERIOD` (sofortiger Rückfall)

**Ursache:**
```c
// ptp_gm_task.c, Zeile ~154
static bool gm_read_register(uint32_t addr, bool useCallbackProtectedMode)
{
    return DRV_LAN865X_ReadRegister(0u, addr, useCallbackProtectedMode, gm_op_cb, NULL);
    //     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //     Rückgabetyp: TCPIP_MAC_RES (int-Enum)
    //     TCPIP_MAC_RES_OK = 0  →  als bool: false  →  "fehlgeschlagen"
    //     TCPIP_MAC_RES_PENDING, ERROR = nonzero → als bool: true → "erfolgreich"
    //     Semantik ist INVERTIERT!
}
```

**Effekt in der State-Machine:**
```c
case GM_STATE_READ_TXMCTL:
    if (!gm_read_register(GM_TXMCTL, true)) {  // !false == true  ← immer wahr
        GM_SET_STATE(GM_STATE_WAIT_PERIOD);     // ← immer genommen
        break;
    }
    GM_SET_STATE(GM_STATE_WAIT_TXMCTL);         // ← nie erreicht
```

**Betroffen:** Alle Aufrufe mit Return-Check:
- `GM_STATE_READ_TXMCTL` — `gm_read_register(GM_TXMCTL, ...)`
- `GM_STATE_READ_TTSCA_H` — `gm_read_register(secReg, ...)`
- `GM_STATE_READ_TTSCA_L` — `gm_read_register(nsecReg, ...)`
- `GM_STATE_WRITE_CLEAR` — `gm_write_register(GM_OA_STATUS0, ...)`

**Fix:** Wrapper-Funktionen returnieren `== TCPIP_MAC_RES_OK`:
```c
static bool gm_read_register(uint32_t addr, bool useCallbackProtectedMode)
{
    return DRV_LAN865X_ReadRegister(...) == TCPIP_MAC_RES_OK;
}

static bool gm_write_register(uint32_t addr, uint32_t value, bool useCallbackProtectedMode)
{
    return DRV_LAN865X_WriteRegister(...) == TCPIP_MAC_RES_OK;
}
```

**Status:** ✓ ANGEWENDET (2026-04-01)

---

### Mögliche weitere Ursachen (nach Fix #1 zu prüfen)

| # | Mögliche Ursache | Symptom | Prüfung |
|---|---|---|---|
| 2 | TXPMDET-Bit wird nie gesetzt — TXMCTL-Arm zu früh (vor SPI-TX-Commit) | `WAIT_TXMCTL` läuft, aber TXPMDET=0 immer | `lan_read 0x00040040` nach Sync |
| 3 | `DRV_LAN865X_GetAndClearTsCapture()` gibt immer 0 — TTSCA-Bits in STATUS0 werden vom Interrupt-Handler gelöscht bevor GM sie abfragen kann | `WAIT_STATUS0` timeout | SYS_CONSOLE_PRINT STATUS0-Wert direkt nach TTSCA-Prüfung |
| 4 | TX-Timestamp-Capture-Slot falsch — TXMLOC=30 passt nicht zu Sync-Frame-Layout | TTSCA gesetzt, aber Timestamp-Wert 0 | Prüfe Sync-Frame: Byte 30/31 = EtherType 0x88F7? |
| 5 | Callback `gm_op_cb` wird nicht aufgerufen — `gm_op_done` bleibt `false` | `WAIT_TXMCTL` cb timeout nach 200 ms | SYS_CONSOLE_PRINT in `gm_op_cb` |
| 6 | FOL-Servo berechnet falschen Offset — `processFollowUp()` extrahiert falsches t1 | FINE nie erreicht, Offset wächst unbegrenzt | `ptp_offset` + GM CONSOLE vergleichen |
| 7 | PTP-Multicast wird von Follower-Ethernet gefiltert | FollowUp wird gesendet, FOL empfängt but ignoriert | `ipdump 1` auf FOL während GM sendet |
| 8 | Timestamp-Auflösung MAC_TISUBN zu grob | Servo schwingt in COARSE, erreicht FINE nicht | Offset-Plot über 60 s auf COARSE-Verlauf prüfen |

---

## 6. Test-Ergebnisse — Testlauf 2 (2026-04-01, Fix #1 + Fix #2)

### Laufparameter
- Firmware-Build: `build_dual.bat`
- Flash: `flash_dual.py`
- Testskript: `ptp_diag.py`
- Fixes: Root Cause #1 (`TCPIP_MAC_RES_OK == ...`) + Root Cause #2 (`TXMLOC=12`)

### Ergebnisse — Testlauf 3 (2026-04-01, `ptp_diag_20260401_163547.log`)

| Schritt | Ergebnis | Notizen |
|---|---|---|
| 0 IP-Konfiguration | ✓ PASS | |
| 1 Ping beidseitig | ✓ PASS | 4/4 in beide Richtungen |
| 2 PTP aktivieren | ✓ PASS | |
| 2b GM kein WAIT_TXMCTL-Loop | ✓ PASS | |
| 3 GM Sync-Zähler steigt | ✓ PASS | gmSyncs 54→116 (delta=62) |
| 4 TX-Match TXPMDET | **✗ FAIL** | 1034 Timeouts bei 1338 Syncs — TXPMDET nie gesetzt |
| 5 Follower PTP-RX | ✓ PASS | 39 Sync-Frames (kein FollowUp empfangen) |
| 6 GM FollowUp-Pfad | **✗ FAIL** | SEND_FOLLOWUP nie erreicht |
| 7 FOL Servo Konvergenz | **✗ FAIL** | FINE-Timeout (kein FollowUp → kein t1) |

**Fix #1 und Fix #2 bereits angewendet, aber TXPMDET bleibt 0 → weiterer Bug.**

**Diagnose Testlauf 3:**
- Sync-Intervall auf dem Draht gemessen (ipdump): **~128 ms** ✓ korrekt
- UART-Überlastung durch `[PTP-GM][STATE]`-Spam bei ~80 Zeilen/s bestätigt
- Fix für Testlauf 4: State-Logging auf Schlüsselereignisse reduziert

---

## 6. Testlauf 4 (geplant)

### Änderungen gegenüber Testlauf 3

| Änderung | Datei | Beschreibung |
|---|---|---|
| Fix #3 | `ptp_gm_task.c` | `gm_set_state()` gibt kein `SYS_CONSOLE_PRINT` mehr aus — State-Spam entfernt |
| Fix #3 | `ptp_gm_task.c` | Neue Schlüsselereignis-Prints: `Sync #N`, `TXPMDET ok, Sync #N`, `FU #N t1=Xs.Yns` |

### Erwartetes Verhalten (Normalfall)
```
[PTP-GM] Init complete (MAC ...)
[PTP-GM] Sync #0
[PTP-GM] TXPMDET ok, Sync #0
[PTP-GM] FU #0 t1=1743600000s 007650000ns
[PTP-GM] Sync #1
...
```

### Erwartetes Verhalten (Fehlerfall TXPMDET)
```
[PTP-GM] Sync #0
[PTP-GM] TXPMDET timeout after Sync #0
[PTP-GM] Sync #1
...
```

### Ergebnisse — Testlauf 4 (2026-04-01, `ptp_diag_20260401_165641.log`)

| Schritt | Ergebnis | Notizen |
|---|---|---|
| 0 IP-Konfiguration | ✓ PASS | |
| 1 Ping beidseitig | ✓ PASS | 4/4 in beide Richtungen |
| 2 PTP aktivieren | ✓ PASS | |
| 2b GM kein WAIT_TXMCTL-Loop | ✓ PASS | |
| 3 GM Sync-Zähler steigt | ✓ PASS | gmSyncs 54→116 (delta=62) |
| 4 TX-Match TXPMDET | **✗ FAIL** | TXPMDET ok: 7×, TXPMDET timeout: 1188× — sporadisch |
| 5 Follower PTP-RX | ✓ PASS | 40 Sync-Frames (kein FollowUp) |
| 6 GM FollowUp-Pfad | **✗ FAIL** | FU=0 — TTSCA nicht gesetzt nach TXPMDET ok |
| 7 FOL Servo Konvergenz | **✗ FAIL** | FINE-Timeout |

**Fortschritt gegenüber Testlauf 3:** TXPMDET wird jetzt 7× erkannt (vorher 0×). Neues Fehlermuster: `TXPMDET ok → TTSCA not set` — STATUS0 TTSCA-Bits werden nie gesetzt.

**Log-Verbesserung:** Ausgabe auf 4871 Zeilen reduziert (vorher 35719) — UART-Überlastung beseitigt.

**Fix #3 (ptp_diag.py):** `NameError: log_path` behoben — `_main_run()` als separate Funktion, `tee.close()` in `finally`-Block.

**Neuer Root Cause (Priorität 1):** `TTSCA not set after Sync #N` trotz `TXPMDET ok`:
- TXPMDET-Bit wird gesetzt → State-Machine erreicht `GM_STATE_WAIT_STATUS0`
- `DRV_LAN865X_GetAndClearTsCapture()` gibt 0 zurück → Bits TTSCAA/B/C nie gesetzt
- Mögliche Ursachen: (a) Interrupt-Handler löscht STATUS0 bevor GetAndClear aufgerufen wird, (b) TX-Capture nicht aktiviert (`tsc`-Bit im Sync-Frame), (c) falscher Capture-Slot abgefragt

---

## 5. Modifizierter Testplan

### Modifikation 1 — Fix #1 anwenden (sofort)

**Aktion in `ptp_gm_task.c`:**
```c
// ALT (kaputt):
static bool gm_read_register(uint32_t addr, bool useCallbackProtectedMode)
{
    return DRV_LAN865X_ReadRegister(0u, addr, useCallbackProtectedMode, gm_op_cb, NULL);
}
static bool gm_write_register(uint32_t addr, uint32_t value, bool useCallbackProtectedMode)
{
    return DRV_LAN865X_WriteRegister(0u, addr, value, useCallbackProtectedMode, gm_op_cb, NULL);
}

// NEU (korrekt):
static bool gm_read_register(uint32_t addr, bool useCallbackProtectedMode)
{
    return TCPIP_MAC_RES_OK == DRV_LAN865X_ReadRegister(0u, addr, useCallbackProtectedMode, gm_op_cb, NULL);
}
static bool gm_write_register(uint32_t addr, uint32_t value, bool useCallbackProtectedMode)
{
    return TCPIP_MAC_RES_OK == DRV_LAN865X_WriteRegister(0u, addr, value, useCallbackProtectedMode, gm_op_cb, NULL);
}
```

**Danach:** Build → Flash → `ptp_diag.py --from-step 6`

---

### Modifizierter Schritt M1 — TXPMDET-Prüfung (nach Fix #1)

**Ziel:** Prüfen ob TXPMDET-Bit gesetzt wird nachdem State-Machine `WAIT_TXMCTL` erreicht.

**Erwartetes Ergebnis:**
```
[PTP-GM][STATE] GM_STATE_READ_TXMCTL @L...
[PTP-GM][STATE] GM_STATE_WAIT_TXMCTL @L...
[PTP-GM][STATE] GM_STATE_WAIT_STATUS0 @L...   ← TXPMDET war gesetzt
[PTP-GM][STATE] GM_STATE_READ_TTSCA_H @L...
```
**Fail-Muster:**
```
[PTP-GM] TXPMDET timeout after Sync #N   ← TXPMDET kam nie in MAX_RETRIES Versuchen
```
→ Dann Ursache #2 prüfen: TXMCTL-Arm-Zeitpunkt oder TXMLOC

---

### Modifizierter Schritt M2 — TTSCA-Capture prüfen (nach Fix #1 + M1 PASS)

**Ziel:** Prüfen ob `DRV_LAN865X_GetAndClearTsCapture()` einen Wert ≠ 0 zurückgibt.

**Erweiterung in `ptp_gm_task.c`** (temporär diagnostisch):
```c
case GM_STATE_WAIT_STATUS0:
    uint32_t tsCapture = gm_get_and_clear_ts_capture();
    SYS_CONSOLE_PRINT("[PTP-GM][DIAG] tsCapture=0x%08X\r\n", (unsigned)tsCapture);
    ...
```
**Erwartetes Ergebnis:** `tsCapture=0x00000100` (TTSCAA) oder TTSCAB/C  
**Fail-Muster:** `tsCapture=0x00000000` dauerhaft → Ursache #3 (Interrupt-Handler löscht W1C zu früh)

---

### Modifizierter Schritt M3 — Follower-Konvergenz-Volltest (nach M1 + M2 PASS)

Entspricht originalem **Schritt 2** — läuft durch `ptp_diag.py --from-step 7`

**Erweiterte Konvergenzdiagnose:**
- Wenn MATCHFREQ gesehen, aber kein COARSE → Ursache #6 (t1-Extraktion)
- Wenn COARSE, aber kein FINE → Ursache #8 (Servo-Parameter)
- Wenn FINE gesehen, aber `ptp_offset` > 1 μs → Ursache #7 (Filterung) oder #4 (Timestamp-Offset)

---

### Modifizierter Schritt M4 — 60 s Stabilitätsmessung

Identisch zu originalem **Schritt 3**. Kriterien:  
- PASS: ≥ 80 % der Messungen `|offset| < 100 ns`  
- Ziel Langzeit: `|offset| < 50 ns`, `stdev < 20 ns`

---

### Prioritäten-Übersicht (modifizierter Plan)

```
Jetzt:   Fix #1 → build_dual.bat → flash_dual.py → ptp_diag.py --from-step 6
         → Schritt M1: Erscheint WAIT_TXMCTL?
           JA  → Schritt M2: tsCapture ≠ 0?
             JA  → Schritt M3: FOL FINE?
               JA  → Schritt M4: 60 s Stabilität
             NEIN → Ursache #3 analysieren (W1C Race)
           NEIN → Ursache #2 analysieren (TXPMDET)
         → Schritt M1 FAIL auch nach Fix: TXMLOC/TXMPATH prüfen (--dump-txmatch)
```
