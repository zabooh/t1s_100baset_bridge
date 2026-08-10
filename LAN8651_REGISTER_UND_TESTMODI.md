# LAN8651 — Registerzugriff und IEEE-Test-Modi über die Bridge-CLI

> **Zweck.** Dieses Dokument beschreibt, wie mit der `lan_read`/`lan_write`-CLI dieser
> Bridge-Firmware die **IEEE-802.3cg-Transmitter-Test-Modi** des LAN8651 aktiviert werden, um das
> 10BASE-T1S-Segment **messtechnisch** zu überprüfen (Oszilloskop, Spektrumanalysator) — und was
> dabei zu beachten ist.
>
> **Status: am 2026-08-10 am Target verifiziert**, auf dem diese Firmware läuft. Das Messprotokoll
> steht in [§4](#4-verifikation-am-target-2026-08-10), der automatisierte Nachweis aller vier Modi
> in [§4.1](#41-nachtrag-alle-vier-modi-funktional-verifiziert-2026-08-10-später-am-tag).
>
> Für die Test-Modi sind **keine Firmware-Änderungen nötig** — `lan_read`/`lan_write` genügen, und
> so wurde es auch verifiziert. Seit 2026-08-10 gibt es zusätzlich die bequemeren Kommandos
> **`testmode`** und **`lan_rmw`** ([§8.1](#81-umgesetzt-am-2026-08-10)); der rohe Registerweg
> bleibt daneben unverändert gültig.
>
> Erstellt 2026-08-10 durch Quellcode-Analyse dieses Projekts plus Verifikation am Gerät.

---

## Inhalt

- [Kurzfassung](#kurzfassung)
- [1. Warum das für dieses Projekt relevant ist](#1-warum-das-für-dieses-projekt-relevant-ist)
- [2. Registerzugriff in dieser Firmware](#2-registerzugriff-in-dieser-firmware)
  - [2.1 Ort im Code](#21-ort-im-code)
  - [2.2 Kommandos](#22-kommandos)
  - [2.3 Adress-Kodierung: MMS in den obersten 16 Bit](#23-adress-kodierung-mms-in-den-obersten-16-bit)
  - [2.4 Ablauf und Grenzen der Zustandsmaschine](#24-ablauf-und-grenzen-der-zustandsmaschine)
  - [2.5 Host-seitig automatisieren mit `cli.py`](#25-host-seitig-automatisieren-mit-clipy)
- [3. Die Test-Mode-Register](#3-die-test-mode-register)
  - [3.1 T1STSTCTL — Test Mode Control](#31-t1ststctl--test-mode-control)
  - [3.2 T1SPMACTL — PMA Control](#32-t1spmactl--pma-control)
  - [3.3 T1SPMASTS — PMA Status](#33-t1spmasts--pma-status)
  - [3.4 Fertige Kommandos zum Kopieren](#34-fertige-kommandos-zum-kopieren)
- [4. Verifikation am Target (2026-08-10)](#4-verifikation-am-target-2026-08-10)
  - [4.1 Nachtrag: alle vier Modi funktional verifiziert](#41-nachtrag-alle-vier-modi-funktional-verifiziert-2026-08-10-später-am-tag)
- [5. Messrezepte](#5-messrezepte)
  - [5.1 Sender-Compliance: Mode 1, 2, 3](#51-sender-compliance-mode-1-2-3)
  - [5.2 Mode 4 und TXD: den Bus *ohne* diesen Sender messen](#52-mode-4-und-txd-den-bus-ohne-diesen-sender-messen)
  - [5.3 PMA-Loopback: den internen Pfad prüfen](#53-pma-loopback-den-internen-pfad-prüfen)
  - [5.4 Deterministischen Verkehr erzeugen](#54-deterministischen-verkehr-erzeugen)
- [6. Betrieb und Sicherheit](#6-betrieb-und-sicherheit)
- [7. Fallstricke und korrigierte Irrtümer](#7-fallstricke-und-korrigierte-irrtümer)
  - [7.6 `lan_write_callback()` verwirft den `value`-Parameter](#76-lan_write_callback-verwirft-den-value-parameter)
  - [7.7 RMW-Maske: `value` wird nicht maskiert](#77-rmw-maske-value-wird-nicht-maskiert)
- [8. Ausbaustand und was noch fehlt](#8-ausbaustand-und-was-noch-fehlt)
  - [8.1 Umgesetzt am 2026-08-10](#81-umgesetzt-am-2026-08-10)
  - [8.2 Weiterhin offen](#82-weiterhin-offen)
- [9. Referenzen](#9-referenzen)

---

## Kurzfassung

| Frage | Antwort |
|---|---|
| **Test-Modi über die CLI aktivierbar?** | **Ja, verifiziert.** `lan_write 0x000308FB 0x2000` → Readback `0x00002000` ([§4](#4-verifikation-am-target-2026-08-10)) |
| **Firmware-Änderung nötig?** | **Nein.** Die Test-Modi sind reine Registerschreibzugriffe; `lan_read`/`lan_write` genügen. Komfort-Kommando `testmode` seit 2026-08-10 vorhanden |
| **Indirekte Adressierung (Clause 22) nötig?** | **Nein.** Direkter MMS-3-Zugriff funktioniert |
| **Welche Register?** | `0x000308FB` T1STSTCTL (Modi 1–4), `0x000308F9` T1SPMACTL (Loopback, TX-Disable), `0x000308FA` T1SPMASTS |
| **Bricht das den Link?** | Ja — beabsichtigt. Der Steuerkanal bleibt, weil die CLI über UART läuft, nicht über T1S |
| **PLCA-Registeradresse** | `0x0004CA02` (MMS 4) — belegt aus dem Code dieses Projekts, [§7.2](#72-plca-liegt-auf-mms-4-nicht-mms-2) |
| **Womit gemessen wird** | Mode 1/2 Oszilloskop, Mode 3 Spektrumanalysator, Mode 4 Kabel-/Busmessung ohne diesen Sender |

---

## 1. Warum das für dieses Projekt relevant ist

Diese Bridge sitzt mit `eth0` direkt am 10BASE-T1S-Segment (LAN8651 auf der **Two-Wire ETH
Click**, MIKROE-5543, über SERCOM-SPI am ATSAME54P20A). Damit ist sie nicht nur Teilnehmer, sondern
das naheliegende **Messwerkzeug für das Segment selbst**:

- Sie hat als einziges Gerät am Bus eine bequeme Kommandoschnittstelle (UART über den EDBG-COM-Port),
  die **unabhängig vom T1S-Link** funktioniert. Man kann den Bus also gezielt stören oder stilllegen
  und behält trotzdem die Kontrolle.
- Der LAN8651 bringt die IEEE-802.3cg-Test-Modi in Hardware mit. Sie erzeugen definierte
  Sendemuster, **ohne dass Verkehr nötig ist** — genau das, was man für Pegel-, Jitter-, Droop- und
  Spektrumsmessungen braucht.
- Für die Gegenprobe („wie sieht der Bus aus, wenn *dieser* Knoten schweigt?") gibt es
  Transmit-Disable und den High-Impedance-Modus.

Ergänzend liefert die Firmware die Bordmittel, um dabei *Verkehr* kontrolliert zu erzeugen und
mitzulesen: `noip_send` (rohe Ethernet-Frames), `iperf` und der SPAN-Port `mirror` für Wireshark —
siehe [§5.4](#54-deterministischen-verkehr-erzeugen) und die
[CLI-Übersicht im README](README.md#cli-commands).

---

## 2. Registerzugriff in dieser Firmware

### 2.1 Ort im Code

Alles liegt in [firmware/src/app.c](firmware/src/app.c) — es gibt **kein** separates
Register-CLI-Modul.

| Element | Stelle | Bemerkung |
|---|---|---|
| `lan_read()` Kommando-Handler | [app.c:957](firmware/src/app.c#L957) | prüft Argumente, setzt nur den Zustand |
| `lan_write()` Kommando-Handler | [app.c:976](firmware/src/app.c#L976) | dito, zusätzlich Wert |
| `cmd_lan_rmw()` Kommando-Handler | [app.c:1000](firmware/src/app.c#L1000) | Read-Modify-Write, verifiziert maskiert |
| `cmd_testmode()` Kommando-Handler | [app.c:1044](firmware/src/app.c#L1044) | Testmodus setzen/abfragen, mit Auto-Revert |
| `lan_read_callback()` | [app.c:303](firmware/src/app.c#L303) | übernimmt `success` + gelesenen Wert |
| `lan_write_callback()` | [app.c:310](firmware/src/app.c#L310) | übernimmt **nur** `success`, verwirft `value` — siehe [§7.6](#76-lan_write_callback-verwirft-den-value-parameter) |
| `lan_rmw_callback()` | [app.c:319](firmware/src/app.c#L319) | übernimmt zusätzlich den zurückgeschriebenen Wert |
| Zustandsmaschine | [app.c:469 ff.](firmware/src/app.c#L469) | im Zweig `APP_STATE_IDLE` der App-Task |
| Treiberaufruf Lesen | [app.c:484](firmware/src/app.c#L484) | `DRV_LAN865X_ReadRegister(0, addr, true, …)` |
| Treiberaufruf Schreiben | [app.c:540](firmware/src/app.c#L540) | `DRV_LAN865X_WriteRegister(0, addr, value, true, …)` |
| Treiberaufruf RMW | [app.c:579](firmware/src/app.c#L579) | `DRV_LAN865X_ReadModifyWriteRegister(0, addr, value, mask, true, …)` |
| Timeout-Konstante | [app.c:247](firmware/src/app.c#L247) | `APP_LAN_TIMEOUT_MS  200u` |
| Zustands-Enum / -Variable | [app.c:249](firmware/src/app.c#L249) / [app.c:256](firmware/src/app.c#L256) | `APP_LAN_IDLE`, `APP_LAN_WAIT_READ`, `APP_LAN_WAIT_WRITE`, `APP_LAN_WAIT_RMW` |
| Eintrag in `msd_cmd_tbl` | [app.c:1245 ff.](firmware/src/app.c#L1245) | Kommandogruppe `Test`, registriert in `Command_Init()` |

> **Zeilennummern altern.** Sie stimmen für den Stand vom 2026-08-10 (Commit mit `testmode`/`lan_rmw`).
> Bei Abweichung nach Symbolnamen greppen, nicht der Zahl folgen.

### 2.2 Kommandos

```
lan_read  <addr_hex>
lan_write <addr_hex> <value_hex>
lan_rmw   <addr_hex> <mask_hex> <value_hex>     # neu 2026-08-10, verifiziert maskiert
testmode  [0..4] [sekunden]                     # neu 2026-08-10, verifiziert per Readback
```

Die Kommandos gehören zur Gruppe **`Test`**. Wie im [README](README.md#cli-commands) vermerkt, ist
das Gruppenpräfix **nicht erforderlich** — `lan_read 0x000308FB` funktioniert genauso wie
`Test lan_read 0x000308FB`. Der Hilfetext in [app.c:345](firmware/src/app.c#L345) zeigt die
Langform, das ist kein Widerspruch.

**Erfolgsausgaben:**

```
LAN865X Read OK: Addr=0x000308FB Value=0x00002000
LAN865X Write OK: Addr=0x000308FB Value=0x00002000
```

**Fehlerausgaben** — alle vier sind unterscheidbar und sollten beim Skripten getrennt behandelt
werden:

| Ausgabe | Bedeutung |
|---|---|
| `ERROR: Previous LAN operation still in progress` | Kommando wurde **nicht** angenommen; vorherige Operation läuft noch |
| `LAN865X Read failed to start: result=<n>` | Treiber hat die Operation abgelehnt (`TCPIP_MAC_RES`) |
| `LAN865X Read timeout for addr=0x…` | 200 ms ohne Callback |
| `LAN865X Read failed for addr=0x…` | Callback kam, meldet aber `success = false` |

Argumente werden mit `strtoul(argv[n], NULL, 0)` geparst: `0x`-Präfix wird erkannt, dezimale
Eingabe ist ebenfalls zulässig. Es gibt **keine Plausibilitätsprüfung** der Adresse — jede
32-Bit-Zahl wird durchgereicht.

### 2.3 Adress-Kodierung: MMS in den obersten 16 Bit

Die Adresse ist 32 Bit breit: **obere 16 Bit = MMS (Memory Map Selector), untere 16 Bit =
Registeroffset innerhalb dieser Bank.**

Belegt im Treiber:
[tc6.c:885](firmware/src/config/default/driver/lan865x/src/dynamic/tc6/tc6.c#L885) („*Upper 16 bit:
Memory map selector (MMS)*") und die Umsetzung in
[tc6.c:935](firmware/src/config/default/driver/lan865x/src/dynamic/tc6/tc6.c#L935) bzw.
[tc6.c:977](firmware/src/config/default/driver/lan865x/src/dynamic/tc6/tc6.c#L977):

```c
SET_VAL(HDR_C_MMS, (addr >> 16), tx_buf);
```

| MMS | Bank | Beispiel | im Projekt verwendet für |
|---|---|---|---|
| 0 | OA Standard (OA_CONFIG0, OA_STATUS0/1, OA_IMASK0/1) | `lan_read 0x00000000` | Chip-ID, Statusbits |
| 1 | MAC (inkl. Wall Clock MAC_TSH/L/N, MAC_TI, MAC_TA) | `lan_read 0x00010077` | in dieser Firmware nicht aktiv genutzt |
| 2 | PHY PCS | `lan_read 0x00020000` | — |
| **3** | **PHY PMA/PMD — hier liegen die Test-Modi** | `lan_read 0x000308FB` | **Gegenstand dieses Dokuments** |
| 4 | PHY Vendor-Specific (PLCA, ACMA, CBS, SQI, Cable Fault Diag) | `lan_read 0x0004CA02` | **PLCA_CTRL1** ([app.c:868](firmware/src/app.c#L868)), SQI-Konfiguration |
| 10 (0x0A) | Miscellaneous (Event Capture/Generator, 1PPS, PADCTRL, DEVID) | `lan_read 0x000A0094` | DEVID (Errata-relevant) |

Die Test-Mode-Register `0x08F9`/`0x08FB` sind dabei keine herstellerspezifische Erfindung: es sind
die Clause-45-Adressen **2297** und **2299** in MMD 1 (PMA/PMD) nach IEEE 802.3cg. Die
Adressbildung `0x0003_08FB` ist also standardkonform.

### 2.4 Ablauf und Grenzen der Zustandsmaschine

Der Kommando-Handler **führt den Zugriff nicht selbst aus**. Er legt Adresse (und Wert) ab und
schaltet `app_lan_state` auf `APP_LAN_WAIT_READ` bzw. `APP_LAN_WAIT_WRITE`. Die eigentliche
TC6-Transaktion startet die App-Task, das Ergebnis kommt asynchron per Callback und wird von der
Task ausgegeben.

Daraus folgen fünf Einschränkungen, die man beim Messen kennen muss:

1. **Bedient wird nur in `APP_STATE_IDLE`.** Die Register-Zustandsmaschine hängt im
   `APP_STATE_IDLE`-Zweig der App-Task ([app.c:432](firmware/src/app.c#L432)). Solange die
   Anwendung in einem anderen Zustand steckt, wird kein Registerzugriff bearbeitet — das Kommando
   ist dann angenommen, aber es passiert scheinbar nichts.
2. **Nur eine Operation gleichzeitig.** Ein zweites `lan_read`/`lan_write` vor Abschluss wird mit
   `ERROR: Previous LAN operation still in progress` **abgewiesen** — der Wert geht verloren, es
   wird nichts eingereiht. Beim Skripten also immer auf die Antwortzeile warten, nicht auf gut Glück
   nachschieben.
3. **Timeout 200 ms** ([app.c:247](firmware/src/app.c#L247)). Danach wird der Zustand freigegeben
   und der Fehler gemeldet.
4. **Alle Zugriffe laufen `protected`** — sowohl Lesen ([app.c:440](firmware/src/app.c#L440)) als
   auch Schreiben ([app.c:470](firmware/src/app.c#L470)) übergeben `true`, also OA-TC6-Transaktionen
   mit Protection-Bytes. Siehe [§7.3](#73-protected--true-gilt-für-lesen-und-schreiben).
5. **Kein Read-Modify-Write in der CLI.** Der Treiber hätte
   `DRV_LAN865X_ReadModifyWriteRegister()`, die CLI exponiert es nicht. Einzelne Bits ändern heißt
   also: lesen, host-seitig verknüpfen, ganzes Wort zurückschreiben. **Wichtig bei T1SPMACTL**, wo
   mehrere Steuerbits in einem Register liegen — ein blindes `lan_write` löscht die anderen.

### 2.5 Host-seitig automatisieren mit `cli.py`

Für reproduzierbare Messabläufe gibt es im Projekt [cli.py](cli.py) — es schickt CLI-Kommandos über
den COM-Port und sammelt die Antworten inklusive asynchroner Ausgaben ein:

```bash
python cli.py --port COM8 --read 1 "lan_read 0x000308FB"
python cli.py --port COM8 --read 1 "lan_write 0x000308FB 0x2000" "lan_read 0x000308FB"
python cli.py --port COM8 --listen 8          # nur mitlesen
```

`--read` ist wichtig: die Antwort erscheint erst, wenn die App-Task die Transaktion abgeschlossen
hat, nicht unmittelbar nach dem Kommando. Eine Sekunde ist reichlich, aber nicht null.

---

## 3. Die Test-Mode-Register

Grundlage: LAN8650/1-Datenblatt §11 (Register Descriptions, MMS 3) und IEEE Std 802.3-2022
§147.5.2. Alle drei Register sind 16 Bit breit und liegen in den unteren 16 Bit des über die CLI
gelesenen/geschriebenen 32-Bit-Worts.

### 3.1 T1STSTCTL — Test Mode Control

| Eigenschaft | Wert |
|---|---|
| MMS / Offset | 3 / `0x08FB` |
| CLI-Adresse | **`0x000308FB`** |
| Bitfeld | 15:13 (`TSTCTL[2:0]`) |
| Zugriff / Reset | R/W / `0x0000` |
| Formel | `wert = (modus & 0x7) << 13` |

| TSTCTL | Wert | Modus | Was geprüft wird | Messmittel |
|---|---|---|---|---|
| `000` | `0x0000` | Normal Operation | — | — |
| `001` | `0x2000` | **Test Mode 1** | Transmitter Output Voltage, Timing Jitter | Oszilloskop |
| `010` | `0x4000` | **Test Mode 2** | Transmitter Output Droop | Oszilloskop |
| `011` | `0x6000` | **Test Mode 3** | Transmitter PSD Mask (Störaussendung) | Spektrumanalysator |
| `100` | `0x8000` | **Test Mode 4** | Transmitter High Impedance | Kabel-/Busmessung |
| `101`–`111` | — | Reserved | — | — |

Die Modi 1–3 erzeugen ein **dauerhaftes, definiertes Sendemuster ohne Nutzverkehr**. Das ist der
Grund, warum sie fürs Messen taugen: der Bus zeigt ein stationäres Bild, das man in Ruhe
vermessen kann.

### 3.2 T1SPMACTL — PMA Control

| Eigenschaft | Wert |
|---|---|
| MMS / Offset | 3 / `0x08F9` |
| CLI-Adresse | **`0x000308F9`** |
| Zugriff / Reset | R/W / `0x0000` |

| Bit | Maske | Name | Funktion | Hinweis |
|---|---|---|---|---|
| 15 | `0x8000` | `RST` | PMA Reset, self-clearing | **nicht** mit anderen Bits zusammen setzen |
| 14 | `0x4000` | `TXD` | Transmit Disable | für Normalbetrieb 0; nimmt den Sender vom Bus |
| 13:12 | — | Reserved | — | RO |
| 11 | `0x0800` | `LPE` | Low Power Enable | entspricht Power-Down in BASIC_CONTROL |
| 10 | `0x0400` | `MDE` | Multidrop Enable | laut Datenblatt ohne Effekt auf die Device-Operation |
| 9:1 | — | Reserved | — | RO |
| 0 | `0x0001` | `LBE` | PMA Loopback Enable | Vorbedingungen siehe [§5.3](#53-pma-loopback-den-internen-pfad-prüfen) |

**Achtung:** mehrere Steuerbits in einem Register, und die CLI kann kein Read-Modify-Write. Wer
`TXD` setzen will, ohne `LBE` zu verlieren, muss vorher lesen und den kombinierten Wert schreiben.
Bei Reset-Default `0x0000` ist das unkritisch, in einem laufenden Testaufbau nicht.

### 3.3 T1SPMASTS — PMA Status

| Eigenschaft | Wert |
|---|---|
| MMS / Offset | 3 / `0x08FA` |
| CLI-Adresse | **`0x000308FA`** |
| Zugriff | read-only |

Nützlich als Gegenprobe zum Sendezustand. **Nicht** als Erreichbarkeitstest für MMS 3 verwenden —
warum, steht in [§7.1](#71-mms-erreichbarkeit-nie-an-einem-register-prüfen-das-legitim-0-liefert).

### 3.4 Fertige Kommandos zum Kopieren

```
# ---- Zustand aufnehmen (vor jedem Test) ----
lan_read  0x000308FB          # aktiver Test-Modus (erwartet 0x00000000)
lan_read  0x000308F9          # PMA-Control    (erwartet 0x00000000)
lan_read  0x000308FA          # PMA-Status

# ---- IEEE-Test-Modi ----
lan_write 0x000308FB 0x2000   # Mode 1 — Output Voltage & Timing Jitter
lan_write 0x000308FB 0x4000   # Mode 2 — Output Droop
lan_write 0x000308FB 0x6000   # Mode 3 — PSD Mask
lan_write 0x000308FB 0x8000   # Mode 4 — High Impedance
lan_read  0x000308FB          # IMMER kontrollieren: Readback == geschriebener Wert?

# ---- PMA-Steuerung ----
lan_write 0x000308F9 0x4000   # TXD = 1  -> Sender aus (Bus ohne diesen Knoten)
lan_write 0x000308F9 0x0001   # LBE = 1  -> PMA-Loopback
lan_write 0x000308F9 0x8000   # RST = 1  -> PMA-Reset (self-clearing)

# ---- Aufräumen (nach JEDEM Test) ----
lan_write 0x000308FB 0x0000
lan_write 0x000308F9 0x0000
lan_read  0x000308FB          # muss 0x00000000 zeigen
lan_read  0x000308F9          # muss 0x00000000 zeigen
```

---

## 4. Verifikation am Target (2026-08-10)

Vor dieser Messung war offen, ob MMS 3 über die CLI überhaupt direkt beschreibbar ist oder ob ein
indirekter Clause-22-Pfad nötig wäre (Vorgeschichte: [§7.1](#71-mms-erreichbarkeit-nie-an-einem-register-prüfen-das-legitim-0-liefert)).
Entschieden wurde es durch einen **Write-Readback am Target, auf dem diese Firmware läuft**:

```
> lan_read 0x000308F9
LAN865X Read OK: Addr=0x000308F9 Value=0x00000000     # T1SPMACTL, Reset-Default — plausibel

> lan_write 0x000308FB 0x2000
LAN865X Write OK: Addr=0x000308FB Value=0x00002000    # TSTCTL = 001 (Test Mode 1)

> lan_read 0x000308FB
LAN865X Read OK: Addr=0x000308FB Value=0x00002000     # Wert hält -> Write ist angekommen
```

**Der Readback ist der Beweis, nicht die Write-Bestätigung.** `LAN865X Write OK` sagt nur, dass die
TC6-Transaktion ohne Fehler durchlief; dass der Wert im Register *stehen bleibt*, zeigt erst das
anschließende Lesen. Damit ist belegt:

- Direkter Schreib- **und** Lesezugriff auf MMS 3 funktioniert über die normale CLI.
- Mit `protected = true`, wie die Firmware es fest vorgibt.
- Ohne Firmware-Änderung, ohne indirekte Adressierung.

### Reichweite des Nachweises

| Belegt | **Nicht** belegt |
|---|---|
| Adressbildung für MMS 3 ist richtig | dass der Sender daraufhin das IEEE-Muster tatsächlich ausgibt |
| Register ist beschreibbar und hält den Wert | Signalform, Pegel, Jitter, Spektrum |
| Kein indirekter Zugriffspfad erforderlich | Wirkung von `LBE`/`TXD` |
| Reset-Default von T1SPMACTL ist `0x0000` | |

Der Registerpfad war die eigentliche Unsicherheit — die ist beseitigt. Was bleibt, ist eine reine
Messaufgabe am MDI. Ein PHY, der den geschriebenen Modus zurückliest, hat ihn übernommen; ob das
Sendesignal der Norm entspricht, entscheidet danach das Messgerät, nicht mehr das Register.

### 4.1 Nachtrag: alle vier Modi funktional verifiziert (2026-08-10, später am Tag)

Der obige Stand ließ „Verhalten der Modi 2, 3 und 4" offen, weil nur Mode 1 gesetzt worden war. Das
ist inzwischen erledigt, und zwar **ohne Oszilloskop** — über eine Beobachtung, die den Readback um
eine funktionale Stufe ergänzt.

**Der Trick ist ein Oracle, das der Endpoint von selbst liefert.** Am T1S-Bus hängt ein Endpoint
(`192.168.0.54`), der zyklisch **SOME/IP-SD-OFFER-Multicasts mit 1 Hz** sendet. Diese Frames laufen
durch die Bridge und sind auf dem `eth1`-Adapter des PCs direkt sichtbar. Damit ergibt sich eine
dreistufige Beweiskette je Modus:

| Stufe | Beobachtung | Beweist |
|---|---|---|
| 1 | `[VERIFY] PASS` auf `T1STSTCTL`, maskiert mit `0xE000` | Adressbildung, Register hält den Wert |
| 2 | Frames des Endpoints hören auf einzutreffen | Der PHY hat den Zustand **übernommen**, nicht nur gelatcht |
| 3 | Frames kommen nach `testmode 0` wieder | Rückweg in den Normalbetrieb funktioniert vollständig |

Stufe 2 ist der eigentliche Gewinn: sie schließt die Lücke zwischen „Register beschreibbar" und
„Sender reagiert". Automatisiert in **`test_lan8651.py`** (Repo-Wurzel), das die Frames per `tshark`
zählt und bei jeder Abweichung mit Exitcode ≠ 0 endet.

**Ergebnis:** alle vier Modi bestehen alle drei Stufen — 19 Prüfungen, Exitcode 0.

| Modus | `T1STSTCTL` | Readback | Verkehr während | Verkehr nach Revert |
|---|---|---|---|---|
| 1 | `0x00002000` | PASS | 0 Frames | 4 Frames / 4 s |
| 2 | `0x00004000` | PASS | 0 Frames | 4 Frames / 4 s |
| 3 | `0x00006000` | PASS | 0 Frames | 4 Frames / 4 s |
| 4 | `0x00008000` | PASS | 0 Frames | 5 Frames / 4 s |

Baseline vor dem Lauf: `T1STSTCTL = 0x00000000`, `PLCA_CTRL1 = 0x00000800` (Node-ID 0, Node-Count 8),
4 Frames in 4 s.

**Voraussetzung, ohne die Stufe 2 nichts aussagt: die Bridge muss PLCA-Coordinator sein (Node-ID 0).**
Mit einem externen Coordinator dürfte der Endpoint weitersenden, und „kein Verkehr" wäre dann keine
Aussage über *diesen* Sender. `test_lan8651.py` prüft das über `PLCA_CTRL1` und bricht sonst ab.

Weiterhin **nicht** belegt: Signalform, Pegel, Jitter und Spektrum. Das bleibt Messaufgabe. Was jetzt
zusätzlich belegt ist: der PHY wechselt in allen vier Modi tatsächlich seinen Sendezustand und kommt
sauber zurück.

---

## 5. Messrezepte

### 5.1 Sender-Compliance: Mode 1, 2, 3

1. Bus in einen definierten Zustand bringen: möglichst nur dieses Gerät und das Messmittel am
   Segment, korrekt terminiert. Keinen Nutzverkehr laufen lassen (siehe
   [§7.4](#74-registerzugriffe-konkurrieren-mit-dem-datenpfad)).
2. Ausgangszustand lesen (`0x000308FB`, `0x000308F9`) und notieren.
3. Modus setzen, **Readback prüfen**.
4. Messen: Mode 1 → Pegel und Timing-Jitter; Mode 2 → Droop; Mode 3 → PSD-Maske am
   Spektrumanalysator.
5. Auf `0x0000` zurückstellen und Readback prüfen.

Beim Durchlaufen mehrerer Modi nach jedem Schritt zurücklesen. Weicht der Readback ab, hat der PHY
den Modus **nicht** übernommen — das sieht man sofort am Register, während man es am Oszilloskop
leicht für ein Messproblem hält.

### 5.2 Mode 4 und TXD: den Bus *ohne* diesen Sender messen

Auf einem Multidrop-Segment ist oft genau das die Frage: wie sieht der Bus aus, wenn dieser Knoten
schweigt? Zwei Wege, die nicht dasselbe sind:

| Mittel | Kommando | Wirkung |
|---|---|---|
| **Test Mode 4** | `lan_write 0x000308FB 0x8000` | Transmitter in High Impedance — der Sender ist hochohmig, das Gerät belastet den Bus praktisch nicht mehr |
| **TXD** | `lan_write 0x000308F9 0x4000` | Transmit-Pfad des PMA abgeschaltet |

High-Z ist das Mittel der Wahl, wenn man **Reflexionen, Terminierung oder Leitungsimpedanz** ohne
den Einfluss dieses Ports beurteilen will. Beides unterbricht die Kommunikation dieses Knotens —
was hier gewollt ist.

### 5.3 PMA-Loopback: den internen Pfad prüfen

Der Loopback (`LBE`, Bit 0 in `0x000308F9`) schleift intern zurück:

```
MAC → PCS Scrambler/Descrambler → 4B/5B Encoder/Decoder
    → PMA Differential Manchester Encoder/Decoder → zurück zum MAC
```

Damit lässt sich der komplette Digitalpfad ohne Leitung prüfen — sinnvoll, um bei einem
Busproblem PHY und Verkabelung auseinanderzuhalten.

**Vorbedingungen und Nachwehen:**

- **PLCA muss aus sein oder dieser Knoten muss Coordinator sein (Node-ID 0).** In dieser Firmware
  ist das bequem: `plca_node 0` setzt die Node-ID zur Laufzeit (volatil), persistent geht es über
  `setenv plca_id 0` + `saveenv` — siehe
  [README §5](README.md#5-changing-ip-and-plca-configuration). Die Firmware schreibt die Node-ID
  intern nach `PLCA_CTRL1` (`0x0004CA02`), Bits 15:8 = NODE_CNT, 7:0 = NODE_ID
  ([app.c:868](firmware/src/app.c#L868)).
- **Keine externe Kommunikation** während aktivem Loopback.
- **Nach dem Abschalten ist ein Reset erforderlich**, damit der reguläre Betrieb sauber wieder
  anläuft. `LBE = 0` allein genügt laut Datenblatt nicht.
- Weil die CLI kein Read-Modify-Write kann: vorher `0x000308F9` lesen, damit man `LBE` nicht
  zusammen mit anderen Bits versehentlich umschaltet.

### 5.4 Deterministischen Verkehr erzeugen

Für Messungen, die *Verkehr* brauchen (Augendiagramm im Betrieb, Kollisionsverhalten,
PLCA-Timing), sind die Test-Modi ungeeignet — sie senden ein Testmuster statt Frames. Dafür hat
diese Firmware eigene Mittel:

| Mittel | Kommando | Eignung |
|---|---|---|
| Rohe Ethernet-Frames | `noip_send <n> [gap_ms]` | umgeht den TCP/IP-Stack, EtherType `0x88B5`, definierte Anzahl und Lücke — **das beste Mittel für reproduzierbare Bilder am Scope** |
| Zähler dazu | `noip_stat` | TX/RX-Zähler des NoIP-Tests |
| Durchsatzlast | `iperf …` | Dauerlast, siehe [README §7](README.md#7-throughput-testing-with-iperf) |
| Mitlesen | `mirror 1` | SPAN des T1S-Ports nach `eth1` für Wireshark, [README §6](README.md#6-port-mirror-capturing-the-t1s-bus-in-wireshark) |
| Software-Zähler | `stats` | belastet den SPI-Pfad **nicht** — bevorzugt gegenüber Register-Polling |

`mirror` und `noip_send` sind während eines aktiven Test-Modus wirkungslos bzw. sinnlos: der Link
ist dann unterbrochen. Erst zurückstellen, dann Verkehr messen.

---

## 6. Betrieb und Sicherheit

- **Die Test-Modi erzeugen Störaussendungen.** Mode 3 ist genau dafür da, sie zu vermessen. Nur in
  kontrollierter, möglichst isolierter Umgebung betreiben und die einschlägigen EMV-Vorgaben
  beachten.
- **Der T1S-Link ist während Mode 1–4 und Loopback unterbrochen.** Für eine Bridge heißt das: die
  gebrückte Verbindung zwischen T1S-Segment und 100BASE-T-Seite ist tot, solange der Test läuft.
  Wer über die gebrückte Strecke arbeitet (SSH, Telnet, Ping über `eth0`), verliert sie.
- **Der Steuerkanal bleibt.** Die CLI läuft über den EDBG-Virtual-COM-Port, also UART — nicht über
  T1S. Man kann sich mit einem Test-Modus **nicht aussperren**; der Rückweg auf Normalbetrieb ist
  jederzeit erreichbar. Das ist der praktische Grund, warum diese Messungen von der Bridge aus
  überhaupt bequem sind.
- **Andere Knoten am Segment** sehen bei Mode 1–3 ein Dauersignal, das kein gültiger Verkehr ist.
  Auf einem produktiven Bus ist das ein Ausfall, nicht nur eine Störung.
- **Immer aufräumen**, und zwar mit Readback-Kontrolle. Ein vergessenes `TSTCTL ≠ 000` ist später
  schwer zu diagnostizieren: der Link kommt nicht hoch, und nichts im normalen Log deutet auf ein
  Testregister hin.
- **`RST` (Bit 15) nicht mit anderen Bits kombinieren.**

---

## 7. Fallstricke und korrigierte Irrtümer

### 7.1 MMS-Erreichbarkeit nie an einem Register prüfen, das legitim 0 liefert

Die Vorgeschichte dieses Dokuments ist selbst die Lehre. In einer früheren Untersuchung (in einem
anderen Arbeitsverzeichnis, `c:\work\ptp\AN1847\t1s_100baset_bridge\`) wurde geprüft, ob MMS 3
direkt erreichbar ist. Getestet wurden dafür `0x00030001` und `0x00030002` — das sind **PMA/PMD
Status 1** und das **Device-Identifier-Register** aus Clause 45. Beide lasen 0.

Daraus wurde geschlossen, MMS 3 brauche indirekte Adressierung, und die PMD-Register wurden auf ein
Clause-22-Fenster (`0x0000FF20`) umadressiert. **Der Schluss war falsch:** bei einem reinen
10BASE-T1S-PHY dürfen diese Legacy-Register völlig zu Recht 0 liefern. Die Nullen sagten nichts über
die Erreichbarkeit der Bank.

**Merksatz:** Die Erreichbarkeit einer Registerbank nur mit einem **Write-Readback auf ein
beschreibbares Bit** feststellen. Ein Lesezugriff allein kann „Register liest 0" nicht von „Bank
nicht erreichbar" unterscheiden. Genau deshalb ist der Test in [§4](#4-verifikation-am-target-2026-08-10)
so gebaut.

### 7.2 PLCA liegt auf MMS 4, nicht MMS 2

Älterer Dokumentationsstand aus dem AN1847-Umfeld empfahl vor dem Loopback-Test
`lan_write 0x0200004A 0x0000` („PLCA_CTRL_STS deaktivieren"), also **MMS 2 / Offset 0x004A**.

**Für dieses Projekt ist das nachweislich falsch.** Der eigene Code schreibt PLCA_CTRL1 nach
**`0x0004CA02`**, also MMS 4 ([app.c:868](firmware/src/app.c#L868)), und der Hilfetext nennt genau
diese Adresse als Beispiel ([app.c:301](firmware/src/app.c#L301)). Das deckt sich mit der
LAN8650/1-Configuration-App-Note, die `PLCA_CTRL0` auf `0xCA01` und `PLCA_CTRL1` auf `0xCA02`
in MMS 4 verortet.

Praktisch braucht man die Adresse hier ohnehin nicht: für die Loopback-Vorbedingung ist
`plca_node 0` bzw. `setenv plca_id 0` + `saveenv` der richtige Weg.

### 7.3 `protected = true` gilt für Lesen und Schreiben

Ein älteres Analysedokument im AN1847-Umfeld behauptet, `lan_read` rufe
`DRV_LAN865X_ReadRegister(0, addr, false, …)` — also **ohne** Protection. **Das trifft auf diesen
Code nicht zu.** Beide Pfade übergeben `true`:

- Lesen: [app.c:440](firmware/src/app.c#L440)
- Schreiben: [app.c:470](firmware/src/app.c#L470)

Relevant, falls Registerzugriffe einmal unerklärlich fehlschlagen: die Protection-Einstellung ist
hier keine Variable, die zwischen Lesen und Schreiben differiert.

### 7.4 Registerzugriffe konkurrieren mit dem Datenpfad

Control- und Data-Transaktionen teilen dieselbe TC6/SPI-Service-Logik. Im AN1847-Umfeld wurde das
für eine baugleiche Firmware gemessen (23.03.2026, MPU→MCU UDP, 2 Mbit/s, 10 s):

| Fall | Aufbau | Paketverlust |
|---|---|---|
| A Baseline | iperf, keine Eingriffe | 0 % (0/1786) |
| B `lan_read` | iperf + `lan_read 0x00000000` alle 200 ms | **5 % (95/1786)** |
| C Control | iperf + `stats` alle 200 ms (kein SPI) | 0 % (2/1786) |

Dass Fall C sauber bleibt, zeigt: es ist nicht die CLI-Last, sondern der SPI-Zugriff. **Konsequenz
für Messungen:** Test-Mode-Writes und Register-Polling nicht während laufendem Verkehr. Für
Laufzeitbeobachtung `stats` verwenden, nicht zyklisches `lan_read`.

Diese Messung stammt aus einem anderen Arbeitsverzeichnis und ist hier **nicht** nachgemessen; die
Architekturursache (gemeinsamer TC6-Pfad, kleine Queues) gilt aber unverändert auch für diesen Code.

### 7.5 Readback als Standardkontrolle

`LAN865X Write OK` bedeutet „Transaktion lief durch", nicht „Register hat den Wert". Bei jedem
Test-Mode-Wechsel danach lesen. Kostet ein Kommando und erspart die Fehlersuche am falschen Ende.

**Seit 2026-08-10 nimmt `testmode` einem das ab:** es hängt den Verify-Read selbst an den Write und
gibt `[VERIFY] PASS`/`FAIL` aus. Für Handzugriffe mit `lan_write` gilt die Regel unverändert.

### 7.6 `lan_write_callback()` verwirft den `value`-Parameter

Beim Bau von `lan_rmw` zunächst `lan_write_callback` als Callback benutzt — der Treiber liefert dort
laut [drv_lan865x.h:680](firmware/src/config/default/driver/lan865x/drv_lan865x.h#L680) für einen RMW
**den tatsächlich zurückgeschriebenen Wert**, aber dieser Callback ignoriert `value`
([app.c:310](firmware/src/app.c#L310)). Ergebnis: die Ausgabe zeigte `app_lan_reg_read_value`, also
den Wert des *vorherigen* Lesezugriffs — beim Setzen von Mode 1 stand dort `0x00000000`, beim
Zurückstellen `0x00002000`. Beides sah plausibel aus und war um eine Operation verschoben.

**Lösung:** eigener `lan_rmw_callback()` ([app.c:319](firmware/src/app.c#L319)), der `value` in
`app_lan_rmw_final` ablegt. **Merksatz:** ein Wert, der „fast richtig" aussieht, ist der gefährlichste
— hier hat erst der Vergleich zweier aufeinanderfolgender Kommandos den Versatz sichtbar gemacht.

### 7.7 RMW-Maske: `value` wird **nicht** maskiert

Die Konvention des Treibers ist `neu = (alt & ~mask) | value`
([tc6.c:691](firmware/src/config/default/driver/lan865x/src/dynamic/tc6/tc6.c#L691)). Der `value`
wird dabei **nicht** mit der Maske verknüpft: Bits außerhalb der Maske landen ungefragt im Register.
Wer `mask = 0xE000` und `value = 0x2001` übergibt, setzt zusätzlich Bit 0. `lan_rmw` warnt deshalb,
wenn `value & ~mask` ungleich 0 ist, führt den Zugriff aber aus.

Zweiter Punkt zur Verifikation von RMW-Zugriffen: **self-clearing Bits melden zu Recht `FAIL`.**
`T1SPMACTL.RST` (Bit 15) liest nach dem Schreiben 0 zurück — das ist korrektes Verhalten, kein
Fehler. Solche Bits nicht über den Verify-Pfad prüfen.

---

## 8. Ausbaustand und was noch fehlt

### 8.1 Umgesetzt am 2026-08-10

- **`testmode [0..4] [sekunden]`** — setzt `T1STSTCTL`, liest **automatisch zurück**, gibt
  `[VERIFY] PASS`/`FAIL` und den dekodierten Modus aus. Ohne Argument nur Abfrage. Die optionale
  Zeitangabe (1–600 s) bewaffnet einen Auto-Revert, damit ein vergessener Testmodus nicht später als
  defekte Hardware erscheint. Umgesetzt in `app.c`; der Verify-Read ist als Followup an den
  Write-Zustand der bestehenden Einzelslot-Maschine gehängt, es kam kein zweiter Mechanismus dazu.
- **`lan_rmw <addr> <mask> <value>`** — exponiert `DRV_LAN865X_ReadModifyWriteRegister()` und
  verifiziert anschließend maskiert. Damit ist die `T1SPMACTL`-Bitfalle aus
  [§3.2](#32-t1spmactl--pma-control) entschärft.
- **`test_lan8651.py`** — Host-Skript, das die dreistufige Beweiskette aus
  [§4.1](#41-nachtrag-alle-vier-modi-funktional-verifiziert-2026-08-10-später-am-tag) automatisiert.

Zwei Fallstricke, die dabei aufgefallen sind und in [§7](#7-fallstricke-und-korrigierte-irrtümer)
ausführlicher stehen: die Masken-Konvention des Treibers ist `neu = (alt & ~mask) | value` — `value`
wird **nicht** mit der Maske verknüpft; und `lan_write_callback()` verwirft den `value`-Parameter,
weshalb ein RMW-Ergebnis, das diesen Callback benutzt, den *vorherigen* Lesewert anzeigt.

### 8.2 Weiterhin offen

- **SQI-Auslesung.** Bleibt offen, und zwar bewusst: **die frühere Behauptung an dieser Stelle, die
  Init-Sequenz konfiguriere SQI bereits über `0x000400B0 = 0x00000103`, ist nicht belegt.** Eine
  Prüfung des Treibers am 2026-08-10 ergibt, dass `0x000400B0`–`0x000400B4` Teil einer
  undokumentierten Init-Tabelle sind (`MemOp_Write`-Einträge mit Magic-Werten, ohne jeden Bezug zu
  SQI im Code — eine Suche nach „sqi" in `drv_lan865x_api.c` findet **nichts**). Auch `0x000400AD`
  ist dort nur ein Config-Read ohne SQI-Label. Ein `sqi`-Kommando auf dieser Grundlage würde eine
  Zahl liefern, deren Herkunft nicht nachweisbar ist — das ist schlechter als kein Kommando.
  **Voraussetzung für eine Umsetzung: die Registerdefinition des SQI-Ergebnisses aus dem
  LAN8650/51-Datenblatt.** Danach ist es ein reiner `lan_read`-Pfad und belastet den Bus nicht.
- **Kabeldiagnose (CFD).** Ebenfalls in MMS 4 vorhanden, hier nicht angebunden.
- **PMA-Loopback (`LBE`) funktional prüfen.** Registerseitig über `lan_rmw 0x000308F9 0x1 0x1`
  jetzt bequem erreichbar, aber die Wirkung ist noch nicht nachgewiesen. Der Nachweis liefe über
  `noip_send`/`noip_stat` statt über den Endpoint-Verkehr — offen ist dabei, ob der MAC ein Frame
  mit der eigenen Source-MAC annimmt (`noip_send` sendet L2-Broadcast mit eigener MAC als Source).

Im Arbeitsverzeichnis `c:\work\ptp\AN1847\t1s_100baset_bridge\firmware\T1S_100BaseT_Bridge.X\`
existiert eine ältere, experimentelle Sammlung dazu: eine Tkinter-GUI mit Test-Mode-Reiter
(`gui/lan8651_bitfield_gui.py`) sowie SQI-/CFD-Skripte. Diese Dateien sind **nicht Teil dieses
Repositories**. Die GUI enthält keinen eigenen SPI-Treiber — sie schickt über pyserial genau die
`lan_read`/`lan_write`-Textkommandos aus [§2.2](#22-kommandos) und ist damit prinzipiell auch an
diese Firmware anschließbar.

---

## 9. Referenzen

| Quelle | Fundstelle |
|---|---|
| Registerzugriff, Zustandsmaschine, Kommandotabelle | [firmware/src/app.c](firmware/src/app.c) |
| MMS-Adresskodierung (`addr >> 16`) | [tc6.c:885 ff.](firmware/src/config/default/driver/lan865x/src/dynamic/tc6/tc6.c#L885) |
| Init-Sequenz, SQI-Konfiguration | [drv_lan865x_api.c](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c) |
| CLI-Übersicht, Hardware-BOM, Mirror, iperf, `env` | [README.md](README.md) |
| Host-seitiges Kommando-Werkzeug | [cli.py](cli.py) |
| LAN8650/1 Datenblatt, §11 (MMS-3-Register) | Microchip DS60001734 |
| LAN8650/1 Configuration App-Note (PLCA, SQI, Registersequenzen) | Microchip AN60001760 |
| IEEE Std 802.3-2022, Clause 147.5.2 — 10BASE-T1S Test Modes | extern |
| OPEN Alliance 10BASE-T1x MAC-PHY Serial Interface (TC6) | extern |

**Hardware, auf der verifiziert wurde:** SAM E54 Curiosity Ultra (DM320210) mit LAN8740A PHY
Daughter Board (AC320004-3) an `eth1` und MikroElektronika Two-Wire ETH Click mit **LAN8651**
(MIKROE-5543) an `eth0`, SPI CS = PC15, INT = PC14.
