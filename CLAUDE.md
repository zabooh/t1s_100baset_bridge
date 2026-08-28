# t1s_100baset_bridge — Arbeitsanweisungen

10BASE-T1S ↔ 100BASE-T Layer-2-Bridge-Firmware für den ATSAME54P20A. Reines MPLAB-X-Projekt
(kein CMake), eigenes Git-Repo (`origin` = `github.com/zabooh/t1s_100baset_bridge.git`, Branch `main`).

---

## 1. Orientierung

| Datei | Zweck |
|---|---|
| `firmware\src\app.c` | App-Zustandsmaschine, Packet-Handler, Packet-Log (Ringpuffer + Drain), `stats`/`meminfo`/`dump`/`ipdump`/`logstat`/`uptime`/`history` |
| `firmware\src\lan865x_diag.c` `.h` | **Registerzugriff, Testmodi, PLCA** — `lan_read`/`lan_write`/`lan_rmw`/`testmode`/`plca_node`. Eigenständig und in andere Projekte kopierbar: hängt nur am LAN865x-Treiber und an SYS_CMD/SYS_TIME/SYS_CONSOLE |
| `firmware\src\port_mirror.c` `.h` | **Port-Mirror/SPAN** `eth0` → `eth1` — Kommando `mirror` (nur an die Bridge adressierte Frames) und `sniffer` (**alle** eth0-RX-Frames, auch zwischen zwei anderen Knoten — dieselbe RX-Hook ohne den Ziel-MAC-Filter, möglich weil der LAN865x-Treiber ohnehin promiscuous läuft; schaltet zusätzlich den T1S-Transmitter ab, passiver Tap), dazu `bigframe` (Einzelframe direkt auf `eth1`, Diagnose). Gespiegelte Frames > 1514 Byte werden gekürzt, nicht durchgereicht (PC-seitiger USB-Adapter/Npcap-Aussetzer sonst, siehe `FALLSTRICKE.md` 2026-08-27 und `SNIFFER_1…4_*.md`). **Nicht** frei portierbar: braucht den TCP/IP-Stack, `DRV_GMAC_PacketTx` und den gepatchten LAN865x-Treiber (siehe `FALLSTRICKE.md`) |
| `firmware\src\noip_test.c` `.h` | **Rohframe-Test** EtherType `0x88B5` ohne IP-Stack — `noip_send <n> [gap_ms] [size]`/`noip_stat`. Besitzt EtherType, Frameaufbau, Zähler und Ausgabetexte. **Teilweise gekoppelt:** der Ringpuffer des Packet-Logs bleibt in `app.c` (teilt ihn mit `ipdump`), deshalb ruft `pktEth0Handler()` beim Empfang `NOIP_IsNoIpFrame`/`NOIP_CountRx`/`NOIP_SeqFromFrame` und die Drain-Schleife `NOIP_PrintRxLine()` |
| `firmware\src\env.c` | Persistente Konfiguration (IP/MAC/PLCA) im Emulated EEPROM |
| `firmware\T1S_100BaseT_Bridge.X\` | MPLAB-X-Projekt (Makefiles, `dist\`) |
| `README.md` | Ausführliche Projektdoku (**englisch**): Hardware-BOM, Architektur, CLI, Mirror, iperf, `env` |
| `LAN8651_TEST_MODES.md` | **Vertiefung zu Abschnitt 3+4 dieser Datei** (**englisch**) — die vier Modi, Messaufbau am Bus, generischer Registerweg vs. `testmode`/`lan_rmw`, Messprotokoll |
| `FALLSTRICKE.md` | **Ausgelagerter Abschnitt 6** (**deutsch**) — datierte Register-/Build-/GUI-/Env-Fallstricke, chronologisch. Vor Arbeiten an diesen Bereichen lesen, neue Erkenntnisse dort anhängen |
| `scripts\cli.py` | Kommandos über COM-Port schicken und Antworten einsammeln |
| `scripts\bridge_gui.py` / `run_gui.bat` | **Bedien-GUI** (tkinter, sv-ttk-Theme/Windows-11-Fluent-Optik fest eingebaut, kein Standard-ttk-Fallback mehr seit 2026-08-28): Bridge-Parameter, alle 183 LAN8651-Register mit Bitfeldern, Testmodi, Terminal, dazu Flash/Erase über die EDBG-Sonde (SWD, unabhängig vom COM-Port) — „Flash from release/" spielt `release/*.hex` über `flash_same54.py` auf eine per Dialog gewählte Sonde, „Erase chip..." löscht Firmware **und** EEPROM-Environment komplett und verlangt dafür ein eingetipptes Bestätigungswort (`ERASE_CONFIRM_WORD`). Braucht `sv-ttk` (Pflicht) und `pyserial` (optional, `requirements.txt`), dazu `lan8651_model.json` und `bridge_config.json`, ruft weder `cli.py` noch `test_lan8651.py` auf. sv-ttk löscht beim Aktivieren die roten/grünen Warn-/Erfolgsfarben (Errata-Warnungen, dekodierte Bitfeld-Werte) — `_restore_semantic_colors()` baut sie anhand des Label-Texts wieder auf, siehe `FALLSTRICKE.md` für die Mechanik. `--light` für die helle Variante, sonst dunkel |
| `scripts\gui_term.py` / `term.bat` | **Drei serielle Konsolen in einem Fenster** (dasselbe feste sv-ttk-Theme wie `bridge_gui.py`), ein Klick verbindet alle — Portierung aus dem `t1s_ptp_bridge`-Schwesterprojekt, ohne dessen Sonden-Seriennummer-Auflösung. Welcher COM-Port zu welchem der drei Terminal-Slots gehört, steht in `json\term_ports.json` (gitignored) und wird über das Menü „Setup → Configure Ports" im Tool selbst gepflegt, nicht von Hand. Titelbalken-Dunkelmodus und die Wiederherstellung der (auch vom Nutzer über „Display" gewählten!) Pane-Terminalfarbe und der Verbindungs-Punkt-Farbe laufen einmal nach dem Aufbau der drei Startpanes — ein später (z. B. per Setup-Dialog) geöffnetes Fenster ist davon **nicht** betroffen, sv-ttks Idle-Task-Neufärbung läuft nur einmal, vor dem ersten `root.update()`. `--light` für die helle Variante, sonst dunkel |
| `json\env_model.json` | **Das Environment-Modell** — je Kennung+Version: welche Felder der EEPROM-Datensatz hat, mit welchem Muster sie aus `showenv` gelesen und mit welchem `setenv`-Schlüssel sie geschrieben werden. Die GUI liest die Kennung vom Gerät und deutet die Werte **nur**, wenn sie dazu einen Eintrag findet |
| `scripts\check_gui_language.py` | Prüft, dass **alle sichtbaren Texte** in `bridge_gui.py`, `gui_term.py` und `dep_check.py` englisch sind — über den Syntaxbaum, damit Kommentare unberührt bleiben |
| `scripts\dep_check.py` | **Von beiden GUI-Tools aus `main()` aufgerufen**, bevor irgendetwas gebaut wird — prüft mit `importlib.util.find_spec` (kein echter Import), ob `sv-ttk`/`pyserial` fehlen, und bietet bei einer Lücke einen Tk-Dialog mit „Install now" an (führt `install_dependencies.bat` aus, Ausgabe live gestreamt). `sv-ttk` ist **hart** (kein Weiterlaufen ohne, seit die früheren separaten `_modern`-Varianten am 2026-08-28 in diese beiden Dateien verschmolzen wurden), `pyserial` **optional** (Tool bleibt nutzbar, nur ohne COM-Port). Grund: siehe `FALLSTRICKE.md`, 2026-08-26 |
| `scripts\check_env_model.py` | Prüft das Environment-Modell — und gleicht jeden `cli_key` gegen die `setenv`-Schlüssel in `env.c` ab (beide Richtungen: unbekannter Schlüssel = Fehler, unerreichbare Einstellung = Warnung) |
| `json\lan8651_model.json` | **Das Registermodell** — 183 Register, 538 Bitfelder, je mit Abschnitt und Seite im Datenblatt, dazu Errata-Anmerkungen sowie Access/Reset je Bitfeld. Die GUI **liest** es und schreibt es nie. Fehler werden **hier** korrigiert, nicht im Python-Quelltext, danach `python scripts\check_register_model.py` |
| `scripts\check_register_model.py` | Prüft das Modell gegen sich selbst: MMS gegen Gruppe, doppelte Adressen und Mnemonics, Bitbereiche verdreht/über 31/überlappend, fehlende Namen. Exitcode ≠ 0 bei Fehlern |
| `json\bridge_config.json` | **Nur Sitzungszustand**: COM-Port, Bridge-Parameter, zuletzt gelesene Registerwerte (`values`). Trägt seit 2026-08-25 **keine** Registerkarte mehr |
| `json\README.md` | **Übersicht aller JSON-Dateien** (**englisch**) — je Datei: wer schreibt, wer liest, Zweck; getrackt vs. gitignored getrennt |

**Hardware:** SAM E54 Curiosity Ultra (DM320210) + LAN8740A PHY Daughter Board (AC320004-3) an
`eth1` + MikroElektronika Two-Wire ETH Click mit **LAN8651** (MIKROE-5543) an `eth0`, SPI CS = PC15,
INT = PC14.

**Session in `C:\work\t1s_bridge\bridge\t1s_100baset_bridge` öffnen** — das ist seit 2026-08-25 die
Repo-Wurzel und damit der Auto-Memory-Schlüssel. **Nicht** im Elternverzeichnis `C:\work\t1s_bridge`
(kein Repo, erzeugt einen zweiten, getrennten Schlüssel; die `CLAUDE.md` dort ergänzt diese nur um
die Container-Ebene) und **nicht** in `C:\work\t1s_bridge\t1s_100baset_bridge`: dieser gleichnamige
Ordner ist eine aufgegebene Kopie **ohne `.git`**, der **113 getrackte Dateien fehlen** — das ganze
`firmware\src\config\default\driver\` samt gepatchtem `drv_lan865x_api.c`, dazu `crypto\`,
`default.mhc\`, Teile von `library\tcpip\src\` und `docs\`. Sie baut nicht mehr (letzter Build
2026-08-19) und wird nicht mehr gepflegt; alles daraus Wertvolle steckt in `9d72b26`.

---

## 2. Bauen, Flashen, Konsole

```bat
build.bat                 :: inkrementell (Default)
build.bat rebuild         :: clean + full
build.bat clean
flash.bat                 :: pyOCD über den EDBG-Probe
flash.bat --dry-run
flash.bat --list          :: angeschlossene Probes
install.bat --select      :: welches Board flash.bat programmiert (-> json\bench.json)
setup.bat                 :: einmalig pro Rechner
```

- **`build.bat` baut parallel** mit `-j%NUMBER_OF_PROCESSORS%` und `-Otarget` (Vollbuild an diesem
  Rechner 2 m 02 s → 35 s bei 14 Kernen). `BUILD_JOBS=n` übersteuert, `BUILD_JOBS=1` stellt einen
  Fehler seriell nach. Die Kernzahl kommt aus der Umgebung, damit nichts pro Rechner konfiguriert
  werden muss und nichts veraltet.
- **Welches Board `flash.bat` programmiert, steht in `json\bench.json`** (gitignoriert, pro Rechner):
  `install.bat` fragt bei jedem Lauf, listet die angesteckten Sonden nummeriert auf, Enter behält die
  aktuelle. `flash.bat` liest die Seriennummer über `flash_same54.py --show-probe` und reicht sie als
  `-u` an pyOCD. Ohne Eintrag sucht pyOCD selbst — das geht nur mit **einer** Sonde am USB, und an
  diesem Tisch hängen drei. Übersteuern: `set "PROBE=..."` in `flash.bat` oder `flash.bat --probe <serial>`.

- **Ein frischer Klon baut ohne Vorbereitung.** Die `nbproject\Makefile-*.mk`-Fragmente sind
  gitignoriert (sie tragen absolute Pfade *dieses* Rechners, eingecheckt wären sie anderswo falsch,
  und zwar auf die teure Art: der Link läuft durch, dann scheitert `xc32-bin2hex`). Erzeugt werden
  sie aus der **getrackten** `nbproject\configurations.xml` mit `prjMakefilesGenerator.bat`, dem
  Werkzeug, das MPLAB X selbst mitbringt — verpackt in **`genmk.bat`**, und **`build.bat` ruft es von
  selbst auf**, wenn die Fragmente fehlen. Die IDE muss also nur **installiert** sein, nicht geöffnet.
  Die frühere Anweisung „Projekt einmal in der IDE öffnen und bauen" ist damit **erledigt**.
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
  `FALLSTRICKE.md` (`git check-ignore` schweigt bei getrackten Pfaden).
- Nach dem Build läuft `build_summary.py` (Flash/RAM, Heap, Interrupt-Handler).
- **Konsole:** EDBG-COM-Port, **115200 8N1**. Host-seitig: `python scripts\cli.py --port COM8 --read 1 "<cmd>"`.

Beim Aufruf von `.bat`-Dateien aus Git Bash den **absoluten Pfad** verwenden und `< /dev/null`
anhängen (`MSYS_NO_PATHCONV=1 cmd /c "C:\work\t1s_bridge\bridge\t1s_100baset_bridge\build.bat" < /dev/null`)
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
(PMA-Loopback).

**`TXD` ist seit 2026-08-26 auf PLCA-Ebene verifiziert** (3 Knoten, kein Oszilloskop nötig):
`TXD=1` auf dem Coordinator (Node 0) lässt `PLCA_STS.PST` (`0x0004CA03`, Bit 15) auf **jedem
anderen Knoten** von `1` auf `0` fallen — unmittelbar, im Log kein wahrnehmbarer Verzug — und
`TXD=0` setzt es ebenso zurück auf `1`. Ein parallel laufender TCP-iperf zwischen zwei
Nicht-Coordinator-Knoten (node1↔node2, node0 nicht beteiligt) brach dabei auf ~0,6 % der
Nennrate ein und erholte sich erst 4–15 s nach dem Revert — TXD auf dem Coordinator wirkt also
netzweit, nicht nur lokal. Details: `FALLSTRICKE.md`, 2026-08-26.

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

**Verifikation ohne Oszilloskop:** `python scripts\test_lan8651.py --port COM8` prüft jeden Modus auf drei
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
| Test-Modi automatisch prüfen | `python scripts\test_lan8651.py --port COM8` | Readback + Verkehr-stoppt + Verkehr-kommt-wieder, Exitcode ≠ 0 bei Abweichung |
| Endpoint-Verkehr zählen | `tshark` auf dem `eth1`-Adapter | der Endpoint sendet SOME/IP-SD mit 1 Hz von selbst — bestes Oracle ohne Messgerät |
| Rohe Ethernet-Frames | `noip_send <n> [gap_ms] [size]` / `noip_stat` | EtherType `0x88B5`, umgeht den TCP/IP-Stack, `size` (Gesamtlänge, 60..1518, Default 60) seit 2026-08-27 — **bestes Mittel für reproduzierbare Scope-Bilder**. Bei `count > 1` teilen sich alle Frames einen Puffer (nur ein Zeiger wird eingereiht) — für saubere Wiederholung `noip_send 1 0 <size>` einzeln aufrufen, nicht auf die eingebaute Schleife verlassen (siehe `FALLSTRICKE.md`) |
| Einzelner Rohframe direkt auf `eth1` | `bigframe <total_len>` (Bridge, 60..9000) | Diagnose, umgeht T1S/Mirror-Pool komplett — Ziel Broadcast, EtherType `0xFEED` |
| SPAN nach `eth1` | `mirror [0\|1]` | Bridge-eigener T1S-Verkehr (RX+TX) in Wireshark mitlesen |
| Passiver Bus-Tap | `sniffer [0\|1]` | wie `mirror`, aber ALLER `eth0`-Verkehr inkl. fremder Knoten; schaltet zusätzlich den T1S-Transmitter ab (`T1SPMACTL.TXD`) — Bridge sendet währenddessen selbst nichts, auch kein Forwarding. Frames > 1514 Byte werden vor dem Spiegeln auf 1514 gekürzt (PC-seitiger USB-Adapter/Npcap-Aussetzer bei größeren Frames, siehe `FALLSTRICKE.md` 2026-08-27) |
| Zähler | `stats` | belastet den SPI-Pfad nicht |
| Durchsatz | `iperf …` / `iperfk` | Dauerlast |
| Laufzeit seit Boot | `uptime` | erkennt einen stillen Neustart (Watchdog/Assert-Loop/`pyocd reset`), den sonst nichts anzeigt |
| Bisherige Kommandos | `history` | letzte 20 eingegebene CLI-Kommandos |
| PLCA-Node-ID | `plca_node [id]` | 0 = Coordinator (volatil) |
| Persistente Config | `showenv` / `setenv` / `saveenv` / `readenv` / `resetenv` | IP, MAC, `plca_id`, `plca_cnt` |
| Speicher | `dump <addr> <count>`, `meminfo` | |

`mirror`/`sniffer` und `noip_send` sind während eines aktiven Test-Modus sinnlos — der Link ist
unterbrochen. Erst zurückstellen, dann Verkehr messen.

---

## 6. Fallstricke

Ausgelagert nach **`FALLSTRICKE.md`** (Token-Kosten pro Session; die vielen datierten
Register-/Build-/GUI-/Env-Erkenntnisse werden nicht in jeder Session gebraucht). Vor Arbeiten an
Registerzugriff (`lan865x_diag.c`), Build/Flash-Toolchain, `bridge_gui.py`/`env.c`/`env_model.json`
oder dem Registermodell (`lan8651_model.json`) dort nachlesen — konkrete Fehler samt Lösung und
Sackgassen, chronologisch. Neue Erkenntnisse aus diesen Bereichen dort anhängen (siehe Abschnitt 7).

---

## 7. Erkenntnisse festhalten

`C:\work` ist bewusster Wegwerf-Arbeitsbereich, und der Auto-Memory-Schlüssel hängt am Pfad — zieht
das Projekt um, verwaist er. **Dauerhaft Wertvolles deshalb in Dateien im Repo ablegen, nicht nur
ins Memory** (dann reist es auch mit einem Klon auf einen anderen Rechner):

- **Test-Modi und Messverfahren** (was gemessen wird, wie der Aufbau aussieht) → `LAN8651_TEST_MODES.md`
  (englisch).
- **Register-/Callback-/Treiber-/Build-/GUI-/Env-Fallstricke, korrigierte Irrtümer** → **`FALLSTRICKE.md`**
  (deutsch). Dort chronologisch anhängen, nicht in diese Datei zurückschreiben — Zweck von Abschnitt 6
  war genau, diese Liste aus der Haupt-Datei herauszuhalten.
- **Kurze, stabile Build-/Flash-Grundlagen** (Befehle, `bench.json`-Mechanik) → Abschnitt 2 **dieser**
  Datei; Fallstricke dazu trotzdem in `FALLSTRICKE.md`.
- **Architektur, Hardware, Bedienung** → `README.md` (englisch).

Format: knapp, ein bis zwei Sätze je Erkenntnis, bei Bedarf ein Snippet, datiert
(`YYYY-MM-DD — Fehler/Erkenntnis → Lösung`). Zieldatei vorher lesen, um Duplikate zu vermeiden.
Besonders festhalten: **Fehler samt richtiger Lösung** und **Sackgassen** („Weg A geht nicht, weil …
→ nicht nochmal versuchen").

**Sprachstand:** `README.md` und `LAN8651_TEST_MODES.md` sind englisch, **diese Datei und
`FALLSTRICKE.md`** sind deutsch (beide AI-/Dev-intern, keine Nutzerdokumentation). Beim Ergänzen
die Sprache der jeweiligen Datei beibehalten.
