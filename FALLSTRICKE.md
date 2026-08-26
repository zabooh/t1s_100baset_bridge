# t1s_100baset_bridge — Fallstricke (datierte Erkenntnisse)

Ausgelagert aus `CLAUDE.md` Abschnitt 6, um die Haupt-Datei schlank zu halten (Token-Kosten pro
Session). Vor Arbeiten an Register-/Build-/GUI-/Env-Code hier nachlesen — hier stehen konkrete
Fehler samt Lösung und Sackgassen, nicht nur bei explizitem Anlass. Format, Sprache und Routing-
Regel: siehe `CLAUDE.md` Abschnitt 7 „Erkenntnisse festhalten". Neue Einträge datiert
(`YYYY-MM-DD — Fehler/Erkenntnis → Lösung`) hier anhängen.

---

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
  damit *nicht*, dass keine Ignore-Regel existiert.** Genau daran ist eine frühere Aussage in
  `CLAUDE.md` Abschnitt 2 gescheitert („`dist\` ist nicht gitignored"): `.gitignore` enthält seit immer
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
  ist nicht *Build*, sondern MCC „Generate Code" (Mirror-Patch, siehe oben) — danach
  `python test_mirror.py`.
- **2026-08-11 — „Baum danach clean" gilt nur für `build.bat`. Ein Build aus der MPLAB-X-IDE macht
  den Baum dreckig, und beide Symptome sehen schlimmer aus, als sie sind.** Am 2026-08-10 stand nach
  einem IDE-Lauf des Users (1) `firmware\T1S_100BaseT_Bridge.X\dist\default\production\` mit allen
  drei getrackten Artefakten als **gelöscht** im Status — die IDE räumt das Verzeichnis vor dem Bauen
  aus, betroffen sind genau die drei Altlasten aus `6e73b22` (`CLAUDE.md` Abschnitt 2); und (2)
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
  Runde kodierte neu, was die vorige schon verbogen hatte: `Manufacturer's` → `â€™` → `Ã¢â‚¬â„¢`, und
  nie eine Fehlermeldung, weil jede Zwischenstufe **gültiges JSON** blieb. Die Datei war dreifach
  verdorben, als es auffiel. **Lösung:** `encoding="utf-8"` an *jede* JSON-`open()`; Reparatur durch
  Zurückdrehen der Runden (`s.encode('cp1252').decode('utf-8')`, je Runde einmal, bricht von selbst
  ab). **Merksatz: eine Datei, die gültig bleibt, während ihr Inhalt zerfällt, meldet sich nie
  selbst** — ein Roundtrip-Test (laden→speichern→laden muss byteweise gleich sein) ist die einzige
  Prüfung, die das fängt.
- **2026-08-26 — Sprachregel: alles Sichtbare ist englisch.** Modelle, GUI-Texte, README, CLI-Doku;
  deutsch bleiben nur `CLAUDE.md`, diese Datei und die Quelltextkommentare. Durchgesetzt wird das von
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
- **2026-08-26 — Access/Reset für alle 183 Register aus dem Datenblatt extrahiert, dabei zwei
  Fallstricke:** (1) `pdftotext -layout` zerlegt die Bit-Diagramm-Tabellen in Zeilen, die nur noch
  über die Spaltenposition der Zahlen zueinander passen — bei sehr schmalen Spalten (z. B. Register
  mit vielen 1-Bit-Feldern wie `OA_STATUS1`, `CFGPRTCTL`) verrutscht das leicht, und ein LLM, das nur
  den Text sieht, hält eine korrekte Spalte schnell für „uncertain, alignment garbled". Gegenmittel:
  bei jedem als unsicher gemeldeten Feld die Rohseite (`pdftotext`-Ausgabe der Einzelseite, kein
  ganzes Kapitel) selbst gegenlesen, bevor der Wert als `null` stehen bleibt — meist ist die Spalte
  doch eindeutig, nur ungewohnt eng gesetzt. (2) **Ein echter Widerspruch *innerhalb* des Datenblatts
  selbst**, nicht nur ein Extraktionsartefakt: `PPSCTL.PPSPW[4:0]` (§11.6.48, Adresse `0x0239`,
  MMS10) zeigt in der Bit-Diagramm-Reset-Zeile `0,0,0,0,1` (= Feldwert `0x1`), zwei Zeilen darunter
  behauptet die Prosa „The default value of 0 corresponds a pulse width of 640ns." Beide Aussagen
  stehen im selben Absatz derselben Revision F — keine Tippfehler-Korrektur in der Errata
  (DS80001075F) dazu gefunden. Nicht raten, welcher Wert stimmt: im Modell auf `reset: null` mit
  Erklärung belassen, nicht den einen oder anderen Wert erfinden. Ausführliche Prüfung und
  Korrekturanweisung dazu: `LAN8651_Register_Model_Review.md`.
