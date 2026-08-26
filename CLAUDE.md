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
| `bridge_gui.py` | **Bedien-GUI** (tkinter): Bridge-Parameter, alle 183 LAN8651-Register mit Bitfeldern, Testmodi, Terminal. **Standalone** — braucht nur `pyserial`, `lan8651_model.json` und `bridge_config.json`, ruft weder `cli.py` noch `test_lan8651.py` auf (Abschnitt 6) |
| `env_model.json` | **Das Environment-Modell** — je Kennung+Version: welche Felder der EEPROM-Datensatz hat, mit welchem Muster sie aus `showenv` gelesen und mit welchem `setenv`-Schlüssel sie geschrieben werden. Die GUI liest die Kennung vom Gerät und deutet die Werte **nur**, wenn sie dazu einen Eintrag findet |
| `check_gui_language.py` | Prüft, dass **alle sichtbaren Texte** in `bridge_gui.py` englisch sind — über den Syntaxbaum, damit Kommentare unberührt bleiben |
| `check_env_model.py` | Prüft das Environment-Modell — und gleicht jeden `cli_key` gegen die `setenv`-Schlüssel in `env.c` ab (beide Richtungen: unbekannter Schlüssel = Fehler, unerreichbare Einstellung = Warnung) |
| `lan8651_model.json` | **Das Registermodell** — 183 Register, 535 Bitfelder, je mit Abschnitt und Seite im Datenblatt, dazu Errata-Anmerkungen. Die GUI **liest** es und schreibt es nie. Fehler werden **hier** korrigiert, nicht im Python-Quelltext, danach `python check_register_model.py` |
| `check_register_model.py` | Prüft das Modell gegen sich selbst: MMS gegen Gruppe, doppelte Adressen und Mnemonics, Bitbereiche verdreht/über 31/überlappend, fehlende Namen. Exitcode ≠ 0 bei Fehlern |
| `bridge_config.json` | **Nur Sitzungszustand**: COM-Port, Bridge-Parameter, zuletzt gelesene Registerwerte (`values`). Trägt seit 2026-08-25 **keine** Registerkarte mehr |

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
install.bat --select      :: welches Board flash.bat programmiert (-> bench.json)
setup.bat                 :: einmalig pro Rechner
```

- **`build.bat` baut parallel** mit `-j%NUMBER_OF_PROCESSORS%` und `-Otarget` (Vollbuild an diesem
  Rechner 2 m 02 s → 35 s bei 14 Kernen). `BUILD_JOBS=n` übersteuert, `BUILD_JOBS=1` stellt einen
  Fehler seriell nach. Die Kernzahl kommt aus der Umgebung, damit nichts pro Rechner konfiguriert
  werden muss und nichts veraltet.
- **Welches Board `flash.bat` programmiert, steht in `bench.json`** (gitignoriert, pro Rechner):
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
  Abschnitt 6 (`git check-ignore` schweigt bei getrackten Pfaden).
- Nach dem Build läuft `build_summary.py` (Flash/RAM, Heap, Interrupt-Handler).
- **Konsole:** EDBG-COM-Port, **115200 8N1**. Host-seitig: `python cli.py --port COM8 --read 1 "<cmd>"`.

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

- **2026-08-26 — Ein Windows-Precision-Touchpad erzeugt bei Tk-Fenstern oft KEIN
  `WM_MOUSEWHEEL`, egal wie gebunden wird.** Der Nutzer hat ein Dell-Notebook-Touchpad, keine
  Maus. Die Zwei-Finger-Scroll-Geste scrollt in jeder anderen Anwendung, aber `bridge_gui.py`
  reagierte auf keine Bindungsvariante — drei Anläufe (direkte Bindung je Kind-Widget,
  Enter/Leave rekursiv, ein globaler `bind_all`-Handler mit `winfo_containing`) blieben wirkungslos.
  Ein isoliertes Testfenster (nur ein Canvas, sonst nichts) klärte es: Hunderte `Enter`/`Motion`-
  Ereignisse beim Scrollen, **kein einziges** `MouseWheel`. Das Ereignis kommt auf diesem Geraet
  nie an — keine Bindungsfrage, keine im Code loesbare Sache. **Merksatz: `widget.event_generate("<MouseWheel>", …)`
  beweist nur, dass eine vorhandene Bindung feuert, nicht ob das Betriebssystem das Ereignis je
  zustellen würde** — genau das hat mich zu den ersten beiden falschen "Fixes" verleitet, die im
  synthetischen Test bestanden und real nichts taten. **Lösung:** `Page Up`/`Page Down` als
  Tastatur-Fallback, unabhängig von der fraglichen Zustellung — scrollt das gerade sichtbare Canvas,
  nachgeführt über `<<NotebookTabChanged>>` auf Haupt- und Register-Untertab-Notebook. Auf einer
  kompakten Dell-Tastatur ohne eigene PgUp/PgDn-Tasten: **Fn + Pfeil hoch/runter**.
- **2026-08-26 — `<<NotebookTabChanged>>` feuert nicht synchron, sondern erst beim nächsten
  Event-Loop-Durchlauf — eine explizite Zuweisung danach kann trotzdem zu spät sein.** Ein
  `ttk.Notebook` wählt beim Aufbau seinen ersten Tab automatisch aus und löst dabei sein eigenes
  `<<NotebookTabChanged>>` aus, verarbeitet wird es aber erst beim ersten `mainloop()`/`update()`
  — also nachdem der ganze Tab-Aufbau längst durchgelaufen ist. Eine explizite Zuweisung
  `self._active_scroll_canvas = self._bridge_scroll_canvas` am Ende von `setup_ui()` sah beim
  Debuggen korrekt aus, wurde aber vom nachgeholten Ereignis lautlos überschrieben, sobald die
  Event-Loop das erste Mal lief. Verschärft dadurch, dass **auch das Register-Untertab-Notebook**
  beim eigenen Aufbau seinen ersten MMS-Tab wählt und sein `<<NotebookTabChanged>>` feuert —
  **unabhängig davon, ob „LAN8651 Registers" überhaupt der sichtbare Haupttab ist.** Ohne Prüfung
  überschrieb dieses interne Ereignis das korrekte Canvas des Bridge-Parameter-Tabs. **Lösung:**
  die Handler sind die einzige Quelle der Wahrheit (früh binden, nicht am Ende zuweisen), und
  `_on_reg_tab_changed` prüft zuerst, ob sein Haupttab überhaupt sichtbar ist, bevor er etwas
  überschreibt.
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
- **2026-08-18 — Ein neues Feld im `env`-Datensatz ohne Migration ist ein stiller Werksreset, und der
  kostet die Koordinatorrolle.** `env_read_valid()` verlangt **exakte** Versionsgleichheit
  (`out->version == ENV_VERSION`), ein Versionssprung macht also jeden gespeicherten Datensatz ungültig
  und `ENV_Init()` fällt auf die einkompilierten Vorgaben zurück. Die sind hier nicht harmlos:
  `DRV_LAN865X_PLCA_NODE_ID_IDX0` ist **7**, während die Bridge als Koordinator **0** fährt — ein Wert,
  der nur aus dem EEPROM kommen kann. Ohne Migration hört die Bridge nach einem Firmware-Update also
  still auf, Beacons zu senden, der Bus hat keinen Koordinator mehr, `test_lan8651.py` bricht ab, und
  nichts im Log zeigt aufs EEPROM — man sucht in der PLCA-Konfiguration. **Lösung:
  `env_migrate_v3()`**: alte Struktur behalten, CRC über das alte Layout prüfen, Felder übernehmen,
  neues Feld auf Vorgabe, als neue Version zurückschreiben. Zwei Dinge dabei nicht weglassen: die alte
  Struktur (sie ist die Migrationsvorschrift, kein toter Code) und die **Compile-Zeit-Zusicherung** auf
  `sizeof`/`offsetof` — Padding würde den Datensatz beschädigen, ohne dass je ein Fehler gemeldet wird,
  weil die CRC in sich stimmig bliebe.
- **2026-08-18 — Ein Branchwechsel macht die generierten Makefiles still falsch, und das ist teurer
  als ein fehlendes Makefile.** `nbproject\configurations.xml` unterscheidet sich zwischen `main` und
  den Arbeitszweigen (gemessen: Blob `050431e8` gegen `a70ac13e`), die Fragmente sind aber gitignoriert
  und bleiben beim Wechsel unangetastet. `build.bat` erzeugt sie nur, wenn sie **fehlen** — nicht, wenn
  sie veraltet sind. Ein Lauf danach kann fehlerfrei linken und dann an `xc32-bin2hex` scheitern, oder
  eine Datei bauen, die nicht zum ausgecheckten Stand gehört. **Nach jedem Branchwechsel, der
  `configurations.xml` anfasst, `genmk.bat firmware\T1S_100BaseT_Bridge.X` aufrufen.** Die Prüffrage
  in einer Zeile: `git rev-parse <branch>:firmware/T1S_100BaseT_Bridge.X/nbproject/configurations.xml`
  für beide Zweige vergleichen.
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
- **2026-08-25 — Der parallele Build ist einmal in ~15 Läufen gescheitert, Ursache nicht gefunden.**
  Symptom: der Linker startete, bevor `app.o` geschrieben war — `pic32c-gcc.exe: error:
  build/…/app.o: No such file or directory`, `Error 1` auf der `…production.hex`-Regel. Nicht
  reproduzierbar: danach 15 Vollbuilds am Stück sauber, auch mit derselben Vorgeschichte
  (inkrementelle Läufe, dann `rebuild`). `-Otarget` ist **nicht** der Auslöser (drei Vollbuilds damit
  fehlerfrei). Warum das hinnehmbar ist: der Fehler ist ein **harter Abbruch mit Exitcode ≠ 0**, keine
  stillschweigend falsche HEX — er kostet einen erneuten Lauf, er liefert kein falsches Binary aus.
  Wer ihn sieht: `BUILD_JOBS=1 build.bat rebuild` baut seriell und beweist, dass es der Wettlauf war.
  Ein Vergleich seriell/parallel ergab damals eine HEX, die sich in **genau einer Zeile** unterschied
  — dem einkompilierten `Build Timestamp` —, sonst Byte für Byte gleich.
- **2026-08-26 — Ein Werkzeug darf nichts ablehnen, was die Firmware annimmt — und nur das Flashen
  hat es gezeigt.** Die neue Firmware akzeptierte den vorhandenen Datensatz mit der Alt-Kennung
  `'LANE'` (Version und CRC passen zu diesem Layout) und lieferte gültige Werte; die GUI kannte nur
  `'EBRG'` und schrieb *„dafür gibt es kein Modell, die Werte sind NICHT gedeutet"* über ein Feld
  voller korrekter Werte. Gegen **erfundene** Testausgaben hatte alles gepasst — weil ich die
  erfundene Ausgabe mit der neuen Kennung geschrieben hatte. **Lösung:** `accepts_ids` im
  Environment-Modell führt die Kennungen, die die Firmware selbst noch liest. **Merksatz: Testdaten
  nicht aus derselben Annahme bauen wie den Code**, sonst prüfen sie die Annahme statt des Codes.
  Zweiter Fund derselben Runde: die Kopfzeile meldete „nicht gedeutet", während die Felder gefüllt
  waren — der Worker deutete mit dem Modelleintrag, der galt, *bevor* die gerade gelesene Kennung
  ankam. Wer Kennung und Nutzdaten aus **einer** Antwort zieht, muss beides mit **demselben** Stand
  auswerten (`env_entry_for(ident)` statt `env_entry()`).
- **2026-08-25 — `bridge_config.json` verlor bei jedem Speichern eine weitere Kodierungsrunde.**
  `save_config()` schreibt UTF-8, die drei Lesestellen nahmen den Windows-Standard **cp1252**. Jede
  Runde kodierte neu, was die vorige schon verbogen hatte: `Manufacturer’s` → `â€™` → `Ã¢â‚¬â„¢`, und
  nie eine Fehlermeldung, weil jede Zwischenstufe **gültiges JSON** blieb. Die Datei war dreifach
  verdorben, als es auffiel. **Lösung:** `encoding="utf-8"` an *jede* JSON-`open()`; Reparatur durch
  Zurückdrehen der Runden (`s.encode('cp1252').decode('utf-8')`, je Runde einmal, bricht von selbst
  ab). **Merksatz: eine Datei, die gültig bleibt, während ihr Inhalt zerfällt, meldet sich nie
  selbst** — ein Roundtrip-Test (laden→speichern→laden muss byteweise gleich sein) ist die einzige
  Prüfung, die das fängt.
- **2026-08-26 — Sprachregel: alles Sichtbare ist englisch.** Modelle, GUI-Texte, README, CLI-Doku;
  deutsch bleiben nur **diese Datei** und die Quelltextkommentare. Durchgesetzt wird das von
  `check_gui_language.py` (liest die Zeichenketten über den **Syntaxbaum**, damit Kommentare außen vor
  bleiben) und den Sprachprüfungen in `check_register_model.py`/`check_env_model.py`. Erfahrungswert
  aus dem ersten Durchgang: eine Liste aus **Funktionswörtern** fand 34 von 39 Texten — die fehlenden
  fünf waren kurze Statuszeilen aus Substantiven („Verbindung verloren", „ohne Antwort"), also
  gerade die, die ein Benutzer am häufigsten sieht. Deshalb stehen Substantive mit in der Liste.
- **2026-08-25 — Zwei Firmware-Varianten schrieben denselben EEPROM-Datensatz mit derselben
  Kennung und derselben Version, aber anderem Layout.** `t1s_100baset_bridge` und
  `t1s_ptp_bridge` hatten beide `ENV_MAGIC 0x4C414E45` (`'LANE'`), beide `ENV_VERSION 4`, beide
  Offset 0 — die Bridge hat `mirror` an Byte 60, der PTP-Zweig dort `ptp_auto`. Dass daraus keine
  falsch gedeuteten Werte wurden, war **Zufall**: 68 gegen 72 Byte, die CRC liegt woanders und
  schlägt fehl → Datensatz verworfen → stiller Werksreset mit `plca_id = 7`. Bei gleicher Größe
  wären es falsche Werte gewesen. **Lösung: die Magic IST die Kennung der Variante** — hier jetzt
  `'EBRG'`, `'LANE'` wird beim Lesen noch akzeptiert (Version und CRC über *dieses* Layout beweisen
  die Herkunft) und beim nächsten `saveenv` umgeschrieben. Neue Varianten nehmen vier eigene
  Zeichen und benutzen nie eine fremde.
- **2026-08-25 — Die im EEPROM gefundene Version lässt sich nicht aus der RAM-Kopie ablesen.**
  `env_read_valid()` verlangt exakte Gleichheit; nach einer Abweichung steht in `s_env` die
  Vorgabe, und `s_env.version` ist **immer** die der Firmware. Ein `showenv`, das dieses Feld
  ausgibt, könnte einen Versionskonflikt also nie zeigen. Deshalb merkt sich `env_read_valid()`
  Kennung, Version und CRC-Status des **gefundenen** Datensatzes, bevor er verworfen wird, und
  `showenv` gibt beides nebeneinander aus:
  `env id EBRG version 4 crc ok | firmware id EBRG version 4 t1s_100baset_bridge`.
  Ein verworfener Datensatz meldet sich beim Booten außerdem laut, samt der `plca_id`, die dann
  gilt — sonst hört die Bridge wieder unbemerkt auf, Beacons zu senden.
- **2026-08-25 — Die Registerkarte gehört in eine Modelldatei, nicht in die Konfiguration — und ein
  Abgleich gegen das Datenblatt fand drei Fehler, die keine Strukturprüfung je gefunden hätte.**
  `lan8651_model.json` ist seither die Referenz (nur lesen), `bridge_config.json` nur noch Zustand.
  Der Abgleich gegen **DS60001734F (Aug-2025), Kapitel 11** ergab: 183 Register im PDF gegen 182 im
  alten Stand, alle Mnemonics und alle Bitgrenzen identisch — aber (1) **205 von 524 Bitfeldnamen
  waren um ihren Index verkürzt** (`SPD_SEL` statt `SPD_SEL[0]`/`[1]`, `OUI` statt `OUI[2:9]`/`[10:17]`),
  weil das ursprüngliche Suchmuster `(?:\[[\d:]+\])?` **außerhalb** der Namensklammer stehen hatte;
  (2) **PADCTRL (`0x000A0088`) fehlte komplett** — im PDF-Textlayer steht dort `x0088` **ohne führende
  Null**, das Muster verlangte `0x…`, und die elf Bitfelder landeten beim Vorgängerregister, was
  schlimmer ist als ein fehlender Eintrag, weil es plausibel aussieht; (3) ein Titel hatte einen
  kompletten Abschnittsabzug von 3 836 Zeichen geschluckt, weil das Muster mit `re.S` über
  Zeilengrenzen griff — Titel sind **einzeilig**. **Merksatz: bei einer PDF-Extraktion nicht die
  Trefferzahl prüfen, sondern die Gegenrichtung** — was steht im Dokument, das im Ergebnis fehlt.
- **2026-08-25 — Die Errata ist für ein Diagnosewerkzeug so wichtig wie das Datenblatt.**
  `LAN8650-1-Errata-80001075.pdf` (**DS80001075F**) enthält registerbezogene Klarstellungen, die man
  einem Registerwert nicht ansieht: **s1** — `OA_PHYID` identifiziert **nicht** das LAN8650/1, sondern
  nur den eingebauten PHY-Block, dafür ist `DEVID` (`0x000A0094`) zuständig; **s6** — `SLPCTL0` liefert
  beim Lesen `SLPCAL = 1`, das Feld **muss aber immer als 0 geschrieben werden**, ein „Bulk Write All"
  würde also den Sleep-Modus lahmlegen; **s5** — `UNEXPB` in `STS1` heißt: zweiter PLCA-Koordinator am
  Segment. Diese Punkte stehen im Modell unter `errata` und werden in der GUI rot markiert.
- **2026-08-25 — Registernamen und -adressen NICHT aus der Firmware ableiten und schon gar nicht
  raten: die vollständige Karte steht im Datenblatt, Kapitel 11.** Die Firmware kennt nur die fünf
  `#define`s in `lan865x_diag.h:57-61`, die der Code selbst braucht (`T1STSTCTL`, `T1SPMACTL`,
  `T1SPMASTS`, `PLCA_CTRL1`); das Schwesterprojekt `t1s_ptp_bridge` hat zusätzlich SQI (`0x000400A0/A1/AA/AC`)
  und Wallclock/1PPS (`MAC_TSL 0x00010074`, `MAC_TN 0x00010075`, `PADCTRL 0x000A0088`,
  `PPSCTL 0x000A0239`). Eine *Sammlung* ist das nicht. Ich habe für die GUI Namen und Adressen aus
  dem Gedächtnis ergänzt und dabei **vier nicht existierende Register erfunden** (`0x00010078`
  „MAC_TO", `0x0002000C` „PHY_PCS_STATUS", `0x00030001` „PMA_STATUS1", `0x00030002` „DEVICE_ID") und
  **zwei falsch benannt** (`0x0004CA03` ist **PLCA_STS**, nicht PLCA_CTRL0; `0x0004CA04` ist
  **PLCA_TOTMR**, nicht PLCA_STATUS — PLCA_CTRL0 liegt auf `0x0004CA01`). Symptom: `lan_read` liefert
  für die Fantasieadressen nichts, und die falschen Namen fallen gar nicht auf, weil der Wert
  plausibel aussieht. **Quelle ist `…\OneDrive\Documents\W\WNET\LAN865x\LAN8650-1-Data-Sheet-60001734.pdf`.**
  Zwei Stellen darin: **Tabelle 4-6** (S. 49–51) listet die schützbaren Nicht-PHY-Register kompakt als
  MMS/Adresse/Mnemonic/Name; **Kapitel 11** enthält alle 182 Register einzeln. Extraktionsrezept mit
  `fitz`: Kopf- und Fußzeilen wegwerfen, alle Seiten zu *einem* Textstrom zusammenfügen (Registerblöcke
  laufen über Seitengrenzen), dann Blöcke über
  `(11\.\d+\.\d+)\.\s*(.+?)\nName:\s*\n\s*(\w+)\s*\nAddress:\s*\n\s*(0x[0-9A-Fa-f]+)` greifen und die
  Bitfelder je Block über `^Bits?\s+(\d+)(?::(\d+))?\s*[-–—]\s*(\S+?)(?:\[[\d:]+\])?\s+(.*)$`.
  Die Abschnittsnummer gibt die MMS: 11.1→0, 11.2→1, 11.3→2, 11.4→3, 11.5→4, 11.6→10; volle Adresse
  ist `MMS<<16 | offset`. Ergebnis: MMS 0 = 22, MMS 1 = 38, MMS 2 = 4, MMS 3 = 5, MMS 4 = 62,
  MMS 10 = 51 Register. **Zwei Fallstricke beim Parsen:** die Adressen im PDF haben uneinheitliche
  Breite (`0x000`, `0x03`, `0x0002`) — über `int(x, 16)` normalisieren, nicht über die Zeichenzahl;
  und der Trennstrich vor dem Feldnamen ist mal `-`, mal `–`, mal `—` — alle drei in die Zeichenklasse
  aufnehmen, sonst fehlen Bitfelder lautlos.
- **2026-08-25 — Der COM-Port ist exklusiv, deshalb darf in `bridge_gui.py` NICHTS über einen
  Unterprozess laufen.** `cli.py:47` macht `serial.Serial(args.port, ...)` und öffnet den Port selbst;
  dasselbe gilt für `test_lan8651.py`. Solange die GUI verbunden ist, bekommt jeder solche Aufruf
  „Zugriff verweigert" — und zwar **im Unterprozess**, während das Fenster im Vordergrund normal
  aussieht. Die GUI führt alle Kommandos deshalb über ihre eine offene Verbindung aus
  (`BridgeGUI.send_command_via_link`); `cli.py` und `test_lan8651.py` werden nicht mehr aufgerufen.
  Wer die Testsuite fahren will, drückt vorher **Disconnect**. **Merksatz für jedes Werkzeug an dieser
  Hardware: wer den Port hält, hält ihn allein — zwei Zugriffswege im selben Programm sind ein
  Entwurfsfehler, kein Komfortmerkmal.**
- **2026-08-25 — Zwei Konsumenten an derselben Queue zerreißen die Antwort, und das Symptom sieht aus
  wie ein Geräteproblem.** In `bridge_gui.py` lasen `terminal_process_queue()` (Main-Thread, alle
  30 ms) und der Kommando-Worker aus **derselben** Queue. Beim Bulk-Read hat mal der eine, mal der
  andere einen Chunk erwischt; einzelne Register blieben leer, andere nicht, und zwar bei jedem Lauf
  andere. Ich habe erst die Registeradressen verdächtigt — falsch. **Lösung:** die serielle Weiche in
  `process_queue()` legt jeden Datenblock in **beide** Queues (`terminal_q` zur Anzeige,
  `cmd_response_q` nur solange `cmd_pending` gesetzt ist), dazu ein `cmd_lock`, damit nicht zwei
  Kommandos gleichzeitig laufen. **Merksatz: ein sporadisch leeres Ergebnis bei mehreren Lesern einer
  Queue ist fast immer der Wettlauf, nicht das Gerät.**
- **2026-08-25 — `lambda e: canvas.configure(...)` in einer Tab-Schleife bindet die Schleifenvariable,
  nicht das jeweilige Widget.** In `create_registers_tab()` bekam dadurch **nur der zuletzt erzeugte
  Tab** eine gültige `scrollregion`; alle anderen ließen sich nicht über die sichtbare Höhe hinaus
  scrollen, und weil oben trotzdem Register standen, sah es nach „Tab ist eben kurz" aus statt nach
  einem Fehler. Betrifft jede Bindung im Schleifenrumpf, auch die Mausrad-Funktion selbst
  (`lambda e, fn=_on_wheel: ...`). **Richtig ist ein Default-Argument je Bindung** (`def _on(event, cv=canvas)`).
  Prüfen lässt es sich ohne Klicken: Tab für Tab anwählen, `root.update()`, dann
  `canvas.cget("scrollregion")` gegen `canvas.bbox("all")` vergleichen — die Region muss die
  Inhaltshöhe abdecken, in **jedem** Tab.
- **2026-08-25 — „Save to JSON" in der GUI hat die Registerkarte vernichtet, und zwar lautlos.**
  `save_registers_json()` setzte `self.config["registers"] = {}` und schrieb dann `{Adresse: Wert}`
  zurück. Ein Klick genügte: Mnemonic, Beschreibung und Bitfelder aller 182 Register waren weg,
  Register ohne gelesenen Wert fielen ganz aus der Datei. Beim nächsten Start griff dann der
  Alt-Format-Zweig (`isinstance(info, dict)` ist bei einem String falsch), und die GUI zeigte nur noch
  nackte Adressen — **ohne Fehlermeldung, ohne dass die Datei kaputt aussah**, sie war ja gültiges
  JSON. Drei Lehren: (1) **eine Speicherfunktion, die ihre Struktur aus den Widgets neu aufbaut,
  verliert alles, was nicht in einem Widget steht** — die Karte lebt jetzt in `self.register_meta`,
  und gespeichert wird nur noch das Feld `value`; (2) `open(pfad, 'w')` leert die Datei beim Öffnen,
  deshalb `json.dumps(...).encode()` zuerst und dann `os.replace()` über eine Nachbardatei — ein
  Serialisierungsfehler fliegt so, bevor irgendetwas angefasst wird; (3) `load_registers_json()` hatte
  denselben Denkfehler in der Gegenrichtung und schrieb bei der neuen Struktur das **ganze
  Registerobjekt** ins Wertfeld. **Prüfung dagegen:** Roundtrip auf einer Kopie der Config —
  Registeranzahl, Bitfeldanzahl und ein Stichproben-Mnemonic vor und nach `save_registers_json()`
  vergleichen (`scratchpad/roundtrip.py`, 11 Prüfungen).
- **2026-08-25 — `addr.upper()` normalisiert eine Hex-Adresse NICHT, es zerstört sie:** aus
  `0x0004CA02` wird `0X0004CA02`, und jeder Vergleich gegen `0x…` schlägt fehl. Genau daran ist die
  Wertübernahme beim Neuerzeugen von `bridge_config.json` gescheitert — stillschweigend, das Ergebnis
  war eine Datei mit vollständiger Karte und lauter leeren Werten. Für Adressen als Schlüssel
  `int(a, 16)` vergleichen oder `"0x%08X" % int(a, 16)` formatieren, nie `.upper()` auf den ganzen
  String.

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
