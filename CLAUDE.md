# t1s_100baset_bridge — Arbeitsanweisungen

10BASE-T1S ↔ 100BASE-T Layer-2-Bridge-Firmware für den ATSAME54P20A. Reines MPLAB-X-Projekt
(kein CMake), eigenes Git-Repo (`origin` = `github.com/zabooh/t1s_100baset_bridge.git`, Branch `main`).

---

## 1. Orientierung

| Datei | Zweck |
|---|---|
| `firmware\src\app.c` | App-Zustandsmaschine, **alle CLI-Kommandos**, Registerzugriff — hier passiert das Meiste |
| `firmware\src\env.c` | Persistente Konfiguration (IP/MAC/PLCA) im Emulated EEPROM |
| `firmware\T1S_100BaseT_Bridge.X\` | MPLAB-X-Projekt (Makefiles, `dist\`) |
| `README.md` | Ausführliche Projektdoku (**englisch**): Hardware-BOM, Architektur, CLI, Mirror, iperf, `env` |
| `LAN8651_REGISTER_UND_TESTMODI.md` | **Vertiefung zu Abschnitt 3+4 dieser Datei** — Registerreferenz, Messrezepte, Messprotokoll |
| `cli.py` | Kommandos über COM-Port schicken und Antworten einsammeln |

**Hardware:** SAM E54 Curiosity Ultra (DM320210) + LAN8740A PHY Daughter Board (AC320004-3) an
`eth1` + MikroElektronika Two-Wire ETH Click mit **LAN8651** (MIKROE-5543) an `eth0`, SPI CS = PC15,
INT = PC14.

**Session hier öffnen, nicht im Elternverzeichnis** `C:\work\t1s_bridge` — der Auto-Memory-Schlüssel
hängt an der Repo-Wurzel, und das Elternverzeichnis ist kein Repo, erzeugt also einen zweiten,
getrennten Schlüssel. Die `CLAUDE.md` dort ergänzt diese nur um die Container-Ebene.

---

## 2. Bauen, Flashen, Konsole

```bat
build.bat                 :: inkrementell (Default)
build.bat rebuild         :: clean + full
build.bat clean
flash.bat                 :: pyOCD über den EDBG-Probe
flash.bat --dry-run
flash.bat --list          :: angeschlossene Probes
setup.bat                 :: einmalig pro Rechner
```

- **Einmalige Voraussetzung:** die `nbproject\Makefile-*.mk`-Fragmente entstehen erst, wenn das
  Projekt **einmal in der MPLAB-X-IDE geöffnet und gebaut** wurde. Ohne sie bricht `build.bat` mit
  einer erklärenden Meldung ab. Das ist kein Fehler im Skript.
- **Nie `MP_CC_DIR` auf der make-Kommandozeile mitgeben.** `build.bat` lässt das absichtlich weg:
  `nbproject\Makefile-local-default.mk` setzt es korrekt, und ein Wert von der Kommandozeile — auch
  ein leerer — hat Vorrang und lässt `xc32-bin2hex` **stillschweigend** scheitern (Link läuft
  fehlerfrei durch, dann „file not found"). Der Kommentar dazu steht in `build.bat`.
- Ergebnis: `firmware\T1S_100BaseT_Bridge.X\dist\default\production\T1S_100BaseT_Bridge.X.production.hex`,
  zusätzlich kopiert nach `release\T1S_100BaseT_Bridge.hex` (getrackt, damit ein frischer Klon ohne
  Build flashen kann). **Achtung, 2026-08-10 korrigiert: `dist\` ist *nicht* gitignored** —
  `.hex`, `.elf` und `.map` dort sind seit `6e73b22` getrackt (`git check-ignore` liefert nichts).
  Jeder Build macht sie also „modified", und jeder Commit schleppt einen ~22 000-Zeilen-Hex-Diff mit.
  Offen, ob das so bleiben soll; solange es so ist, mitcommitten, sonst ist der Baum dauerhaft dreckig.
- Nach dem Build läuft `build_summary.py` (Flash/RAM, Heap, Interrupt-Handler).
- **Konsole:** EDBG-COM-Port, **115200 8N1**. Host-seitig: `python cli.py --port COM8 --read 1 "<cmd>"`.

Beim Aufruf von `.bat`-Dateien aus Git Bash den **absoluten Pfad** verwenden und `< /dev/null`
anhängen (`MSYS_NO_PATHCONV=1 cmd /c "C:\work\t1s_bridge\t1s_100baset_bridge\build.bat" < /dev/null`)
— sonst wird die Datei nicht gefunden bzw. hängt ein `pause` bis zum Timeout.

---

## 3. Registerzugriff auf den LAN8651: `lan_read` / `lan_write`

Die Firmware bietet **generischen** Registerzugriff über die serielle Konsole. Gruppenpräfix ist
nicht nötig (`lan_read …` genügt, `Test lan_read …` geht auch).

```
lan_read  <addr_hex>
lan_write <addr_hex> <value_hex>
```

```
LAN865X Read OK: Addr=0x000308FB Value=0x00002000
LAN865X Write OK: Addr=0x000308FB Value=0x00002000
```

### Adress-Kodierung

**Obere 16 Bit = MMS (Memory Map Selector), untere 16 Bit = Registeroffset.**
Beleg: `SET_VAL(HDR_C_MMS, (addr >> 16), tx_buf)` in
`firmware\src\config\default\driver\lan865x\src\dynamic\tc6\tc6.c`.

| MMS | Bank | Beispiel |
|---|---|---|
| 0 | OA Standard (OA_CONFIG0, OA_STATUS0/1) | `0x00000000` |
| 1 | MAC (Wall Clock, MAC_TI, TX-Timestamps) | `0x00010077` |
| 2 | PHY PCS | `0x00020000` |
| **3** | **PHY PMA/PMD — Test-Modi** | `0x000308FB` |
| 4 | PHY Vendor-Specific (PLCA, ACMA, CBS, SQI, CFD) | `0x0004CA02` = PLCA_CTRL1 |
| 10 (0x0A) | Misc (Event Capture/Gen, 1PPS, PADCTRL, DEVID) | `0x000A0094` |

### Verhalten und Grenzen — beim Skripten beachten

1. **Asynchron.** Der Kommando-Handler setzt nur einen Zustand; die TC6-Transaktion führt die
   App-Task aus, die Antwort kommt per Callback. Also nach dem Kommando auf die Antwortzeile warten
   (`cli.py --read 1`), nicht sofort nachschieben.
2. **Bedient wird nur in `APP_STATE_IDLE`.** In anderen App-Zuständen liegt das Kommando brach —
   es sieht dann aus, als passiere nichts.
3. **Nur eine Operation gleichzeitig.** Ein zweites Kommando davor wird mit
   `ERROR: Previous LAN operation still in progress` **abgewiesen**, nicht eingereiht.
4. **Timeout 200 ms** (`APP_LAN_TIMEOUT_MS` in `app.c`).
5. **Read-Modify-Write gibt es seit 2026-08-10: `lan_rmw <addr> <mask> <value>`.** Konvention
   `neu = (alt & ~mask) | value` — `value` wird vom Treiber **nicht** maskiert, Bits außerhalb der
   Maske landen ungefragt im Register (das Kommando warnt). Wichtig bei **T1SPMACTL**, wo mehrere
   Steuerbits in einem Wort liegen. Self-clearing Bits wie `RST` melden dabei zu Recht
   `[VERIFY] FAIL`.
6. **Alle Zugriffe laufen `protected = true`** — Lesen *und* Schreiben (`app.c`). Falls anderswo
   behauptet wird, Lesen liefe mit `false`: das gilt für diesen Code **nicht**.

---

## 4. IEEE-Test-Modi aktivieren (Bus messtechnisch prüfen)

Der LAN8651 hat die Transmitter-Test-Modi aus **IEEE 802.3-2022 §147.5.2** in Hardware. Sie
erzeugen ein **dauerhaftes, definiertes Sendemuster ohne Nutzverkehr** — genau das, was man für
Pegel-, Jitter-, Droop- und Spektrumsmessungen braucht. Es sind reine Registerschreibzugriffe,
**keine Firmware-Änderung nötig.**

**Verifikationsstand (2026-08-10, an diesem Target):** Direkter MMS-3-Zugriff funktioniert —
`lan_write 0x000308FB 0x2000` → `lan_read 0x000308FB` liefert `0x00002000`. Der **Readback** ist der
Beweis, nicht die Write-Bestätigung. Getestet wurde bisher **nur Mode 1**; Modi 2–4 und Loopback
sind registerseitig plausibel, aber nicht gegengeprüft. Signalform/Pegel/Spektrum sind damit
ausdrücklich **nicht** belegt — das entscheidet das Messgerät.

### T1STSTCTL — `0x000308FB`, Bits 15:13, Reset `0x0000`

`wert = (modus & 0x7) << 13`

| Wert | Modus | Was geprüft wird | Messmittel |
|---|---|---|---|
| `0x0000` | Normal Operation | — | — |
| `0x2000` | Test Mode 1 | Output Voltage, Timing Jitter | Oszilloskop |
| `0x4000` | Test Mode 2 | Output Droop | Oszilloskop |
| `0x6000` | Test Mode 3 | PSD Mask (Störaussendung) | Spektrumanalysator |
| `0x8000` | Test Mode 4 | Transmitter High Impedance | Kabel-/Busmessung ohne diesen Sender |

### T1SPMACTL — `0x000308F9`, Reset `0x0000`

| Bit | Maske | Name | Funktion |
|---|---|---|---|
| 15 | `0x8000` | `RST` | PMA Reset, self-clearing — **nicht** mit anderen Bits kombinieren |
| 14 | `0x4000` | `TXD` | Transmit Disable |
| 11 | `0x0800` | `LPE` | Low Power Enable |
| 10 | `0x0400` | `MDE` | Multidrop Enable (laut Datenblatt ohne Effekt) |
| 0 | `0x0001` | `LBE` | PMA Loopback Enable |

`T1SPMASTS` = `0x000308FA` (read-only).

### Ablauf — bevorzugt `testmode`

Seit 2026-08-10 gibt es ein dediziertes Kommando, das den Readback selbst anhängt und
`[VERIFY] PASS`/`FAIL` plus den dekodierten Modus ausgibt:

```
testmode              # aktuellen Modus zeigen
testmode 1            # Mode 1, mit automatischem Verify
testmode 1 30         # ... und Auto-Revert nach 30 s
testmode 0            # zurück in den Normalbetrieb
```

Der rohe Weg bleibt gültig und ist der, mit dem ursprünglich verifiziert wurde:

```
lan_read  0x000308FB          # Ausgangszustand, erwartet 0x00000000
lan_write 0x000308FB 0x2000   # Mode 1
lan_read  0x000308FB          # IMMER kontrollieren
# ... messen ...
lan_write 0x000308FB 0x0000
lan_read  0x000308FB          # muss 0x00000000 zeigen
```

**Verifikation ohne Oszilloskop:** `python test_lan8651.py --port COM8` prüft jeden Modus auf drei
Stufen — Readback, „Verkehr des T1S-Endpoints hört auf", „Verkehr kommt nach dem Revert wieder" — und
endet bei jeder Abweichung mit Exitcode ≠ 0. Setzt voraus, dass die Bridge PLCA-Coordinator ist
(Node-ID 0), sonst ist Stufe 2 wertlos; das Skript prüft es und bricht sonst ab. Stand 2026-08-10:
alle vier Modi bestehen alle drei Stufen (19 Prüfungen). Details in
`LAN8651_REGISTER_UND_TESTMODI.md` §4.1.

**Readback nach jedem Wechsel.** Weicht er ab, hat der PHY den Modus nicht übernommen — das sieht
man sofort am Register, während man es am Oszilloskop leicht für ein Messproblem hält.

### PMA-Loopback (`LBE`)

Schleift intern zurück (MAC → PCS → 4B/5B → PMA Manchester → zurück), trennt also PHY-Problem von
Verkabelungsproblem. Vorbedingungen:

- **PLCA aus oder dieser Knoten ist Coordinator (Node-ID 0).** Dafür **nicht** am PLCA-Register
  hantieren, sondern `plca_node 0` (volatil) bzw. `setenv plca_id 0` + `saveenv` (persistent).
- Keine externe Kommunikation während des Loopback.
- **Nach `LBE = 0` ist ein Reset erforderlich**, damit der Normalbetrieb sauber anläuft.
- Vorher `0x000308F9` lesen (kein RMW in der CLI, siehe 3.5).

### Sicherheit und Betrieb

- **Störaussendungen.** Mode 3 ist dazu da, sie zu messen. Nur in kontrollierter, isolierter
  Umgebung, EMV-Vorgaben beachten.
- **Der T1S-Link ist während Mode 1–4 und Loopback unterbrochen** — für eine Bridge heißt das: die
  gebrückte Verbindung ist tot. Wer über `eth0` arbeitet, verliert die Verbindung.
- **Man kann sich nicht aussperren:** die CLI läuft über UART (EDBG-COM), nicht über T1S. Der
  Rückweg auf Normalbetrieb ist jederzeit erreichbar.
- **Nie während laufendem Verkehr.** Registerzugriffe teilen die TC6/SPI-Service-Logik mit dem
  Datenpfad; für eine baugleiche Firmware wurden ~5 % UDP-Paketverlust gemessen, wenn `lan_read`
  alle 200 ms parallel zu iperf lief (`stats` ohne SPI-Zugriff dagegen 0 %). Für Laufzeitbeobachtung
  `stats` verwenden, nicht zyklisches `lan_read`.
- **Ein vergessenes `TSTCTL ≠ 000` ist später schwer zu finden:** der Link kommt nicht hoch, und
  nichts im normalen Log deutet auf ein Testregister hin.

Vertiefung — Registerreferenz, Messrezepte, vollständiges Messprotokoll, korrigierte Irrtümer:
**`LAN8651_REGISTER_UND_TESTMODI.md`**

---

## 5. Weitere Bordmittel für Messungen

| Mittel | Kommando | Eignung |
|---|---|---|
| IEEE-Test-Modi | `testmode [0..4] [sek]` | setzt + verifiziert + dekodiert, optionaler Auto-Revert |
| Einzelne Bits setzen | `lan_rmw <addr> <mask> <val>` | `neu = (alt & ~mask) \| val`, danach maskierter Verify |
| Test-Modi automatisch prüfen | `python test_lan8651.py --port COM8` | Readback + Verkehr-stoppt + Verkehr-kommt-wieder, Exitcode ≠ 0 bei Abweichung |
| Endpoint-Verkehr zählen | `tshark` auf dem `eth1`-Adapter | der Endpoint sendet SOME/IP-SD mit 1 Hz von selbst — bestes Oracle ohne Messgerät |
| Rohe Ethernet-Frames | `noip_send <n> [gap_ms]` / `noip_stat` | EtherType `0x88B5`, umgeht den TCP/IP-Stack — **bestes Mittel für reproduzierbare Scope-Bilder** |
| SPAN nach `eth1` | `mirror [0\|1]` | T1S-Verkehr in Wireshark mitlesen |
| Zähler | `stats` | belastet den SPI-Pfad nicht |
| Durchsatz | `iperf …` / `iperfk` | Dauerlast |
| PLCA-Node-ID | `plca_node [id]` | 0 = Coordinator (volatil) |
| Persistente Config | `showenv` / `setenv` / `saveenv` / `readenv` / `resetenv` | IP, MAC, `plca_id`, `plca_cnt` |
| Speicher | `dump <addr> <count>`, `meminfo` | |

`mirror` und `noip_send` sind während eines aktiven Test-Modus sinnlos — der Link ist unterbrochen.
Erst zurückstellen, dann Verkehr messen.

---

## 6. Fallstricke

- **PLCA liegt auf MMS 4, nicht MMS 2.** Der eigene Code schreibt PLCA_CTRL1 nach `0x0004CA02`
  (Bits 15:8 = NODE_CNT, 7:0 = NODE_ID). Älterer Dokumentationsstand aus dem AN1847-Umfeld nennt
  `0x0200004A` — für dieses Projekt nachweislich falsch.
- **Die Erreichbarkeit einer Registerbank nie an einem Register prüfen, das legitim 0 liefern
  darf.** Genau daran ist eine frühere Analyse gescheitert: sie las `0x00030001` (PMA/PMD Status 1)
  und `0x00030002` (Device ID) — Legacy-Clause-45-Register, die bei einem reinen 10BASE-T1S-PHY zu
  Recht 0 lesen — und schloss daraus fälschlich, MMS 3 brauche indirekte Adressierung. Richtig ist
  ein **Write-Readback auf ein beschreibbares Bit**.
- **`LAN865X Write OK` heißt „Transaktion lief durch", nicht „Register hat den Wert".** Immer
  zurücklesen.
- **`build.bat` ohne vorherigen IDE-Build** scheitert an fehlenden nbproject-Fragmenten (siehe
  Abschnitt 2), nicht an einem Codefehler.
- **2026-08-10 — Host-PC am `eth1`-Port: `192.168.0.200` und `.210` sind vergeben, `.100` nehmen.**
  Beide Bridge-Interfaces sind fest statisch konfiguriert (`TCPIP_NETWORK_CONFIG_IP_STATIC`,
  `configuration.h` IDX0 = `LAN865x`/`eth0` = `.200`, IDX1 = `GMAC`/`eth1` = `.210`) und geben ihre
  Adresse nicht frei. Weil das Gerät eine **L2-Bridge** ist, liegen beide im selben Segment — der PC
  sieht also beide und erzeugt mit `.200` *oder* `.210` einen Adresskonflikt. Zwei Symptome, die man
  auseinanderhalten muss: `ipconfig` zeigt **nur** `Autoconfiguration IPv4 Address` (169.254.x.x) und
  gar keine `IPv4 Address`-Zeile → die statische Config wurde nie übernommen (im GUI-Dialog `OK`
  nicht in *beiden* Fenstern gedrückt). Steht dort die statische Adresse mit dem Zusatz
  `(Duplicate)` → Adresskonflikt. Verlässlicher als der GUI-Weg, als Administrator:
  `netsh interface ip set address name="Ethernet 8" static 192.168.0.100 255.255.255.0`, danach
  `netsh interface ip show addresses name="Ethernet 8"` als Kontrolle. Das ist persistent (schreibt
  dieselben Registry-Werte wie der GUI-Dialog), hängt aber an der Adapter-**Instanz**-GUID, nicht am
  Namen — ein anderer USB-NIC ergibt eine neue Instanz auf DHCP. Der Entwicklungsrechner läuft auf
  `.100`; die README-Beispiele nutzen `.220` — beides ist frei, die Zahl ist beliebig.
  Ausführliche, englische Fassung: Abschnitt „Host PC: giving the `eth1` adapter a static address"
  in `README.md`. Diagnosewert der Pings:
  `.210` antwortet = Link zum GMAC-Port steht; `.200` antwortet = die Bridge forwardet wirklich nach
  `eth0`.

---

## 7. Erkenntnisse festhalten

`C:\work` ist bewusster Wegwerf-Arbeitsbereich, und der Auto-Memory-Schlüssel hängt am Pfad — zieht
das Projekt um, verwaist er. **Dauerhaft Wertvolles deshalb in Dateien im Repo ablegen, nicht nur
ins Memory** (dann reist es auch mit einem Klon auf einen anderen Rechner):

- **Test-Modi, Registerzugriff, Messverfahren** → Abschnitt „Fallstricke und korrigierte Irrtümer"
  in `LAN8651_REGISTER_UND_TESTMODI.md`.
- **Build-/Toolchain-/Flash-Fallstricke, CLI-Verhalten** → Abschnitt 2 bzw. 6 **dieser** Datei.
- **Architektur, Hardware, Bedienung** → `README.md` (englisch).

Format: knapp, ein bis zwei Sätze je Erkenntnis, bei Bedarf ein Snippet, datiert
(`YYYY-MM-DD — Fehler/Erkenntnis → Lösung`). Zieldatei vorher lesen, um Duplikate zu vermeiden.
Besonders festhalten: **Fehler samt richtiger Lösung** und **Sackgassen** („Weg A geht nicht, weil …
→ nicht nochmal versuchen").

**Sprachstand:** `README.md` ist englisch, `LAN8651_REGISTER_UND_TESTMODI.md` und diese Datei sind
deutsch. Beim Ergänzen die Sprache der jeweiligen Datei beibehalten.
