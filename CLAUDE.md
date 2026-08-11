# t1s_100baset_bridge — Arbeitsanweisungen

10BASE-T1S ↔ 100BASE-T Layer-2-Bridge-Firmware für den ATSAME54P20A. Reines MPLAB-X-Projekt
(kein CMake), eigenes Git-Repo (`origin` = `github.com/zabooh/t1s_100baset_bridge.git`, Branch `main`).

---

## 1. Orientierung

| Datei | Zweck |
|---|---|
| `firmware\src\app.c` | App-Zustandsmaschine, Packet-Handler, Packet-Log (Ringpuffer + Drain), `stats`/`meminfo`/`dump`/`ipdump`/`logstat` |
| `firmware\src\lan865x_diag.c` `.h` | **Registerzugriff, Testmodi, PLCA** — `lan_read`/`lan_write`/`lan_rmw`/`testmode`/`plca_node`. Eigenständig und in andere Projekte kopierbar: hängt nur am LAN865x-Treiber und an SYS_CMD/SYS_TIME/SYS_CONSOLE |
| `firmware\src\port_mirror.c` `.h` | **Port-Mirror/SPAN** `eth0` → `eth1` — Kommando `mirror`. **Nicht** frei portierbar: braucht den TCP/IP-Stack, `DRV_GMAC_PacketTx` und den gepatchten LAN865x-Treiber (siehe Abschnitt 6) |
| `firmware\src\noip_test.c` `.h` | **Rohframe-Test** EtherType `0x88B5` ohne IP-Stack — `noip_send`/`noip_stat`. Besitzt EtherType, Frameaufbau, Zähler und Ausgabetexte. **Teilweise gekoppelt:** der Ringpuffer des Packet-Logs bleibt in `app.c` (teilt ihn mit `ipdump`), deshalb ruft `pktEth0Handler()` beim Empfang `NOIP_IsNoIpFrame`/`NOIP_CountRx`/`NOIP_SeqFromFrame` und die Drain-Schleife `NOIP_PrintRxLine()` |
| `firmware\src\env.c` | Persistente Konfiguration (IP/MAC/PLCA) im Emulated EEPROM |
| `firmware\T1S_100BaseT_Bridge.X\` | MPLAB-X-Projekt (Makefiles, `dist\`) |
| `README.md` | Ausführliche Projektdoku (**englisch**): Hardware-BOM, Architektur, CLI, Mirror, iperf, `env` |
| `LAN8651_TEST_MODES.md` | **Vertiefung zu Abschnitt 3+4 dieser Datei** (**englisch**) — die vier Modi, Messaufbau am Bus, generischer Registerweg vs. `testmode`/`lan_rmw`, Messprotokoll |
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
  Build flashen kann). **`dist\` ist gitignored** (`.gitignore`: `**/*.X/dist`, dazu `*.elf`/`*.map`)
  — **genau drei Dateien darunter sind trotzdem getrackt**, weil sie in `6e73b22` an der Regel vorbei
  committet wurden: `…production.elf`, `…production.hex`, `…production.map`. Eine Ignore-Regel greift
  bei getrackten Dateien nicht mehr, also werden diese drei „modified", sobald sich ihr Inhalt wirklich
  ändert — dann schleppt der Commit einen ~22 000-Zeilen-Hex-Diff mit. Ein Build ohne Quelltextänderung
  lässt den Baum dagegen clean (2026-08-10 nachgemessen), und die neuen Artefakte unter
  `dist\…\image\` sind sauber ignoriert. Offen, ob die drei Altlasten getrackt bleiben sollen; solange
  es so ist, mitcommitten. **Frühere Fassung dieser Zeile behauptete das Gegenteil** — Ursache in
  Abschnitt 6 (`git check-ignore` schweigt bei getrackten Pfaden).
- Nach dem Build läuft `build_summary.py` (Flash/RAM, Heap, Interrupt-Handler).
- **Konsole:** EDBG-COM-Port, **115200 8N1**. Host-seitig: `python cli.py --port COM8 --read 1 "<cmd>"`.

Beim Aufruf von `.bat`-Dateien aus Git Bash den **absoluten Pfad** verwenden und `< /dev/null`
anhängen (`MSYS_NO_PATHCONV=1 cmd /c "C:\work\t1s_bridge\t1s_100baset_bridge\build.bat" < /dev/null`)
— sonst wird die Datei nicht gefunden bzw. hängt ein `pause` bis zum Timeout.

---

## 3. Registerzugriff auf den LAN8651: `lan_read` / `lan_write`

**Wohnort seit 2026-08-10: `firmware\src\lan865x_diag.c` — nicht mehr `app.c`.** Das Modul ist
absichtlich eigenständig, damit es in ein anderes LAN865x-Projekt kopiert werden kann: zwei Dateien
übernehmen, `LAN865X_DIAG_Initialize()` einmal und `LAN865X_DIAG_Tasks()` aus der Hauptschleife
aufrufen — mehr enthält `app.c` dazu nicht. Neben der CLI gibt es eine programmatische Schnittstelle
(`LAN865X_DIAG_Read/Write/Rmw/TestMode/ApplyPlca`, siehe Header).

Die Firmware bietet **generischen** Registerzugriff über die serielle Konsole. Gruppenpräfix ist
nicht nötig (`lan_read …` genügt); die Kommandos stehen jetzt in der Gruppe `lan`, **`lanhelp`**
listet sie mit Kurzhilfe auf.

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
4. **Timeout 200 ms** (`LAN_TIMEOUT_MS` in `lan865x_diag.c`).
5. **Read-Modify-Write gibt es seit 2026-08-10: `lan_rmw <addr> <mask> <value>`.** Konvention
   `neu = (alt & ~mask) | value` — `value` wird vom Treiber **nicht** maskiert, Bits außerhalb der
   Maske landen ungefragt im Register (das Kommando warnt). Wichtig bei **T1SPMACTL**, wo mehrere
   Steuerbits in einem Wort liegen. Self-clearing Bits wie `RST` melden dabei zu Recht
   `[VERIFY] FAIL`.
6. **Alle Zugriffe laufen `protected = true`** — Lesen *und* Schreiben (`lan865x_diag.c`). Falls anderswo
   behauptet wird, Lesen liefe mit `false`: das gilt für diesen Code **nicht**.

---

## 4. IEEE-Test-Modi aktivieren (Bus messtechnisch prüfen)

Der LAN8651 hat die Transmitter-Test-Modi aus **IEEE 802.3-2022 §147.5.2** in Hardware. Sie
erzeugen ein **dauerhaftes, definiertes Sendemuster ohne Nutzverkehr** — genau das, was man für
Pegel-, Jitter-, Droop- und Spektrumsmessungen braucht. Es sind reine Registerschreibzugriffe,
**keine Firmware-Änderung nötig.**

**Verifikationsstand (2026-08-10, an diesem Target):** Direkter MMS-3-Zugriff funktioniert, und
**alle vier Modi sind verifiziert** — je auf drei Stufen: Readback, Verkehr des T1S-Endpoints hört
auf, Verkehr kommt nach dem Revert wieder (19 Prüfungen, `test_lan8651.py`, Exitcode 0). Der
**Readback** ist der Beweis, nicht die Write-Bestätigung. **Nicht** belegt: Signalform, Pegel,
Jitter, Spektrum — das entscheidet das Messgerät. Ebenfalls offen: die Wirkung von `LBE`
(PMA-Loopback) und `TXD`.

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
alle vier Modi bestehen alle drei Stufen (19 Prüfungen). Messprotokoll und Messaufbau in
`LAN8651_TEST_MODES.md` §7–§8.

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

Vertiefung — was die vier Modi qualifizieren, Sondierung/Terminierung/Instrument je Modus, was man
vorher abklemmt, generischer Registerweg gegenüber den Komfort-Kommandos, Messprotokoll:
**`LAN8651_TEST_MODES.md`** (englisch)

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
  zurücklesen. `testmode` und `lan_rmw` nehmen das ab, für Handzugriffe mit `lan_write` gilt es
  weiter.
- **2026-08-10 — `lan_write_callback()` verwirft den `value`-Parameter; für RMW ist das ein Fehler.**
  Der Treiber liefert im Callback bei `DRV_LAN865X_ReadModifyWriteRegister()` **den tatsächlich
  zurückgeschriebenen Wert** (`drv_lan865x.h`), aber `lan_write_callback()` ignoriert ihn. Beim
  ersten Bau von `lan_rmw` diesen Callback benutzt → die `Final=`-Ausgabe zeigte
  `app_lan_reg_read_value`, also den Wert des *vorherigen* Lesezugriffs: beim Setzen von Mode 1 stand
  dort `0x00000000`, beim Zurückstellen `0x00002000`. Um eine Operation verschoben und jedes Mal
  plausibel. Sichtbar wurde es erst im Vergleich zweier aufeinanderfolgender Kommandos.
  **Lösung:** eigener `lan_rmw_callback()`, der `value` in `app_lan_rmw_final` ablegt.
  **Merksatz:** ein Wert, der „fast richtig" aussieht, ist gefährlicher als einer, der offensichtlich
  falsch ist — bei Callback-Ergebnissen prüfen, *welcher* Callback den Wert überhaupt speichert.
- **`build.bat` ohne vorherigen IDE-Build** scheitert an fehlenden nbproject-Fragmenten (siehe
  Abschnitt 2), nicht an einem Codefehler.
- **2026-08-10 — Der TX-Zweig des Port-Mirrors hängt an einem Patch in *generiertem* Code.**
  `DRV_LAN865X_PacketTx()` in `drv_lan865x_api.c` deklariert `mirror_eth0_tx_hook` von Hand als
  `extern` und ruft ihn (Zeile ~683). Zwei Folgen: (1) **der Symbolname ist nicht frei** — wer
  `port_mirror.c` umbenennt oder die Funktion umbenennt, bricht den Link; (2) **ein erneutes
  MCC „Generate Code" entfernt die Aufrufstelle lautlos.** Symptom danach: der Mirror zeigt noch
  die Frames *vom* Bus, aber nicht mehr die eigenen des Bridge — sieht wie ein halb funktionierender
  Mirror aus, nicht wie ein fehlender Patch. **`python test_mirror.py` prüft genau das** (Mirror aus
  = 0 Frames, Mirror an > 0, beide Richtungen) und sollte nach jeder MCC-Regenerierung laufen.
- **Ein neues `.c` in `firmware\src\` wird nur gebaut, wenn es in den Projektdateien steht.**
  `nbproject\configurations.xml` ist **getrackt** und die Quelle der Wahrheit (je ein `<itemPath>`
  für `.c` und `.h`, neben den bestehenden Einträgen). `nbproject\Makefile-*.mk` ist **generiert und
  gitignored** — ohne IDE-Lauf muss es von Hand nachgezogen werden: je ein Token in den zwei
  `SOURCEFILES`- und drei `OBJECTFILES`-Zeilen plus die zwei 5-zeiligen Compile-Regeln duplizieren
  (Objektverzeichnis `_ext/1360937237` gilt für alles in `src\`). Selbstheilend: ein IDE-Öffnen
  generiert es korrekt aus `configurations.xml` neu.
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
- **2026-08-10 — `git check-ignore -v <pfad>` schweigt bei *getrackten* Dateien (rc=1) und beweist
  damit *nicht*, dass keine Ignore-Regel existiert.** Genau daran ist die frühere Aussage in
  Abschnitt 2 gescheitert („`dist\` ist nicht gitignored"): `.gitignore` enthält seit immer
  `**/*.X/dist`, aber weil `…production.hex/.elf/.map` in `6e73b22` getrackt wurden, überspringt
  check-ignore sie und liefert leere Ausgabe — was wie „keine Regel" aussieht. **Richtig ist
  `git check-ignore -v --no-index <pfad>`** (zeigt `.gitignore:10:**/*.X/dist`), und für die Frage
  „ist der Pfad getrackt?" `git ls-files -- <pfad>`. Die beiden Fragen sind unabhängig und müssen
  getrennt gestellt werden. **Merksatz:** eine leere Ausgabe mit rc=1 ist ein *Nicht-Ergebnis*, kein
  Gegenbeweis — bei Git-Abfragen vor einer Schlussfolgerung prüfen, ob das Kommando den Fall
  überhaupt abdeckt.
- **2026-08-10 — Bauen mit der MPLAB-X-IDE ist unverändert möglich; `build.bat` beweist es mit.**
  Beide treiben dasselbe von der IDE erzeugte `nbproject\Makefile-default.mk`, ein erfolgreicher
  `build.bat`-Lauf gilt also auch für den IDE-Build. Kontrolliert am 2026-08-10: `configurations.xml`
  **und** das generierte Makefile führen `lan865x_diag`, `port_mirror`, `noip_test` je als `.c`+`.h`;
  Build fehlerfrei (167 525 B Flash / 16,2 %, 49 359 B RAM / 18,8 %), Baum danach clean. Gefährlich
  ist nicht *Build*, sondern MCC „Generate Code" (Abschnitt 6, Mirror-Patch) — danach
  `python test_mirror.py`.
- **2026-08-11 — „Baum danach clean" gilt nur für `build.bat`. Ein Build aus der MPLAB-X-IDE macht
  den Baum dreckig, und beide Symptome sehen schlimmer aus, als sie sind.** Am 2026-08-10 stand nach
  einem IDE-Lauf des Users (1) `firmware\T1S_100BaseT_Bridge.X\dist\default\production\` mit allen
  drei getrackten Artefakten als **gelöscht** im Status — die IDE räumt das Verzeichnis vor dem Bauen
  aus, betroffen sind genau die drei Altlasten aus `6e73b22` (Abschnitt 2); und (2)
  `nbproject\configurations.xml` als **modified**, obwohl `git diff` leer war und der Blob-Hash mit
  HEAD übereinstimmte (`050431e8…`) — nur ein veralteter stat-Eintrag, weil die IDE die Datei
  byteidentisch neu geschrieben hat. Beides ist **kein** Schaden und nichts, was committet werden
  müsste: `git checkout -- firmware/T1S_100BaseT_Bridge.X/dist/ firmware/T1S_100BaseT_Bridge.X/nbproject/configurations.xml`
  stellt den Stand wieder her (für Fall 2 genügt auch `git update-index --refresh`).
  **Merksatz:** vor dem Committen nach einem IDE-Lauf `git diff -- <pfad>` fragen, nicht `git status`
  — sonst committet man einen ~22 000-Zeilen-Hex-Diff oder eine Datei, die sich gar nicht geändert hat.
- **2026-08-11 — Rohframes über `DRV_LAN865X_SendRawEthFrame()` sind auf `eth1` unsichtbar, auch bei
  `mirror 1`.** Es gibt **zwei** `eth0`-Ausgänge: den Stackweg `DRV_LAN865X_PacketTx()`
  (`drv_lan865x_api.c:664`, dort hängt der Mirror-TX-Hook) und den Rohweg
  `DRV_LAN865X_SendRawEthFrame()` → `TC6_SendRawEthernetPacket()` (`:2416`), der an `PacketTx`
  vorbeigeht. Die MAC-Bridge hilft dabei nicht: sie flutet nur, was sie auf einem Port **empfängt**,
  und ein selbst erzeugter Frame wird nie empfangen. Betrifft heute schon `noip_send` — dessen Frames
  tauchen im Mirror nicht auf, was leicht als kaputter Mirror gelesen wird. **Der Kommentar
  „the single eth0 egress point"** an `mirror_eth0_tx_hook` (in `drv_lan865x_api.c:678` *und*
  `port_mirror.c:110`) **ist seit `SendRawEthFrame` falsch.** Wer einen Host-Mitschnitt eigener
  Rohframes braucht, muss aus dem Sender heraus `mirror_ethpkt_to_eth1()` aufrufen. **Merksatz:**
  „die Bridge flutet Broadcasts" gilt für *durchlaufenden* Verkehr, nicht für selbst erzeugten.

---

## 7. Erkenntnisse festhalten

`C:\work` ist bewusster Wegwerf-Arbeitsbereich, und der Auto-Memory-Schlüssel hängt am Pfad — zieht
das Projekt um, verwaist er. **Dauerhaft Wertvolles deshalb in Dateien im Repo ablegen, nicht nur
ins Memory** (dann reist es auch mit einem Klon auf einen anderen Rechner):

- **Test-Modi und Messverfahren** (was gemessen wird, wie der Aufbau aussieht) → `LAN8651_TEST_MODES.md`
  (englisch).
- **Registerzugriff, Callback-/Treiber-Fallstricke, korrigierte Irrtümer** → Abschnitt 6 **dieser**
  Datei. Es gibt kein separates deutsches Registerdokument mehr.
- **Build-/Toolchain-/Flash-Fallstricke, CLI-Verhalten** → Abschnitt 2 bzw. 6 **dieser** Datei.
- **Architektur, Hardware, Bedienung** → `README.md` (englisch).

Format: knapp, ein bis zwei Sätze je Erkenntnis, bei Bedarf ein Snippet, datiert
(`YYYY-MM-DD — Fehler/Erkenntnis → Lösung`). Zieldatei vorher lesen, um Duplikate zu vermeiden.
Besonders festhalten: **Fehler samt richtiger Lösung** und **Sackgassen** („Weg A geht nicht, weil …
→ nicht nochmal versuchen").

**Sprachstand:** `README.md` und `LAN8651_TEST_MODES.md` sind englisch, **nur diese Datei** ist
deutsch. Beim Ergänzen die Sprache der jeweiligen Datei beibehalten.
