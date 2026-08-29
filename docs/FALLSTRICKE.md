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
- **2026-08-28 — Auch ein Win32-Fensterprozedur-Hook (Subclassing) fängt die
  Touchpad-Zwei-Finger-Geste nicht ab — Ursache ist eine Ebene unterhalb jeder
  Fensternachricht.** Versuch, den Befund vom 2026-08-26 zu umgehen: `SetWindowLongPtrW`
  tauscht die echte Win32-Fensterprozedur aus (Subclassing), noch vor Tk selbst.
  Erster Anlauf hookte nur den äußeren Toplevel-„Wrapper" (via `GetParent`, wie in
  `_apply_dark_titlebar`) und wertete nur `WM_POINTERWHEEL` aus — wirkungslos. Zweiter
  Anlauf hookte zusätzlich das innere „Content"-HWND (`root.winfo_id()`, das HWND, auf
  dem Tk selbst seine Fensterprozedur installiert und den Fokus hält), wertete zusätzlich
  rohes `WM_MOUSEWHEEL` aus und loggte jede Nachricht aus der ganzen Touch-/Pointer-/
  Gesten-Familie (`WM_GESTURE`, `WM_TOUCH`, `WM_POINTERUPDATE/-DOWN/-UP/-WHEEL`,
  `WM_MOUSEHWHEEL`, …) auf beiden HWNDs mit. **Ergebnis: leeres Log — auf keinem der
  beiden HWNDs kam während der ganzen Sitzung eine einzige Nachricht aus dieser Familie
  an.** Das ist ein stärkerer Beweis als der von 2026-08-26 (der nur Tks virtuelle
  Events prüfte): Die Geste erzeugt auf diesem Gerät gar keine klassische
  Win32-Fensternachricht, sie läuft vermutlich über Direct Manipulation (eine
  COM-Schnittstelle, an der Tk und auch ein Fensterprozedur-Hook vorbeigehen). **Nicht
  nochmal versuchen** — ein Fix bräuchte volle `IDirectManipulationViewport`-COM-
  Integration, Aufwand jenseits dessen, was für dieses Tool sinnvoll ist. Der Hook wurde
  wieder entfernt (Datei ist wieder identisch mit dem committeten Stand). **Lösung bleibt
  die vom 2026-08-26 eingebaute Kombination:** `Page Up`/`Page Down` (bzw. `Fn` +
  Pfeil hoch/runter) und der Scrollbalken per Maus-Drag.
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
  Mirror aus, nicht wie ein fehlender Patch. **`python scripts\test_mirror.py` prüft genau das** (Mirror aus
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
  `python scripts\test_mirror.py`.
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
- **2026-08-26 — sv-ttk (`bridge_gui_modern.py`) ist viel aggressiver als ein normales ttk-Theme,
  zwei Punkte, beide durch Ausprobieren gefunden, nicht aus der Doku:** (1) Es rekoloriert beim
  Aktivieren nicht nur ttk-Widgets, sondern auch **einfache tk-Widgets** (`tk.Canvas`, `tk.Text`)
  und das Root-Fenster selbst — vermutlich über Tks Options-Datenbank, nicht über `ttk.Style`. Ein
  manueller Farb-Patch für Connection-Dot, Scroll-Canvases, Command-Output und Terminal war deshalb
  **nicht** nötig; erst angenommen, dann mit einem Dreizeiler widerlegt. (2) Es wendet seine Palette
  **rückwirkend über einen Idle-Task** an, der beim ersten Event-Loop-Durchlauf feuert — und der
  überschreibt **jede** zur Erzeugungszeit gesetzte `ttk.Label`-`foreground=`, auch die, die in
  `bridge_gui.py` echte Bedeutung tragen (rote Errata-Warnungen, rote „AFTER RESET"/„NEXT BOOT"-
  Hinweise, grüne dekodierte Bitfeld-Werte). Ein Farbe-nach-Konstruktion-Patch, der **vor** dem ersten
  `root.update()`/`update_idletasks()` läuft, wird von diesem Idle-Task also sofort wieder
  überschrieben — mit einem Dreizeiler verifiziert (Patch vor vs. nach `root.update()`, nur Letzteres
  hält). **Lösung:** `update_idletasks()` einmal erzwingen, danach patchen. Da die ursprüngliche Farbe
  zu diesem Zeitpunkt bereits weg ist, lässt sie sich nur noch über den **Text-Inhalt** der Labels
  rekonstruieren (`_restore_semantic_colors()` in `bridge_gui_modern.py`) — fragil gegenüber
  Wortlautänderungen in `bridge_gui.py`, aber die einzige Option ohne die Tab-Aufbaumethoden
  komplett zu duplizieren.
- **2026-08-26 — Zwei Nachträge zu sv-ttk, beim Bau von `gui_term_modern.py` gefunden:**
  (1) Der Idle-Task aus dem Eintrag oben feuert genau **einmal**, beim ersten `root.update()` nach
  `sv_ttk.set_theme(...)`. Ein Fenster/Dialog, der **danach** neu entsteht (z. B. der
  Setup-„Configure Ports"-Dialog, erst durch einen Klick zur Laufzeit geöffnet), ist **nicht**
  betroffen — seine zur Erzeugungszeit gesetzten Farben (z. B. das Farbmuster-Label im Dialog)
  bleiben unangetastet, mit einem Dreizeiler geprüft statt angenommen. Patchen muss man deshalb nur
  die Widgets, die schon vor dem ersten `update()` existieren — bei `gui_term_modern.py` sind das
  genau die drei Panes aus `App.__init__`, nicht der später geöffnete Dialog.
  (2) **Der native Windows-Titelbalken lässt sich per DWM dunkel färben** (`ctypes`,
  `DwmSetWindowAttribute` mit Attribut 20, unter `GetParent(root.winfo_id())`, nicht
  `root.winfo_id()` direkt — das liefert nur das eingebettete Kind-Fenster, nicht das echte HWND).
  Der Aufruf allein reicht aber nicht: Rückgabewert 0 (Erfolg) und sogar ein Rücklesen per
  `DwmGetWindowAttribute` bestätigten den gesetzten Wert, sichtbar dunkel wurde der Balken trotzdem
  erst nach einem erzwungenen Neuzeichnen (`SetWindowPos` mit `SWP_FRAMECHANGED`) — DWM zeichnet den
  Nicht-Client-Bereich sonst erst beim nächsten eigenen Anlass (Resize, Fokuswechsel) neu.
- **2026-08-26 — Zwei weitere Windows-native Elemente, die sv-ttk und auch direkte Tk-Farboptionen
  NICHT erreichen, beide erst am laufenden Screenshot entdeckt, nicht vorher vermutet:**
  (1) Der Scrollbalken von `scrolledtext.ScrolledText` ist ein eingebautes, schlichtes
  `tk.Scrollbar` (`self.vbar` in cpython `tkinter/scrolledtext.py`) — rendert als native Windows-
  Bildlaufleiste und ignoriert jede Tk-Farboption, anders als `tk.Canvas`/`tk.Text`, die sv-ttk
  automatisch mitfärbt. **Lösung:** das alte `vbar` zerstören, ein `ttk.Scrollbar` an dieselbe
  `yview`-Kommando-Funktion binden, `yscrollcommand` neu verdrahten — dabei unbedingt erst
  `pack_forget()` auf dem Text-Widget, dann Scrollbar mit `side=right`, dann Text mit `side=left`
  neu packen, sonst beansprucht das (bereits mit `expand=True` gepackte) Text-Widget den ganzen
  Platz und für die neue Scrollbar bleibt nichts übrig. (2) Der native Windows-Menübalken
  (`tk.Menu` + `root.config(menu=...)`) ignoriert `background`/`foreground` auf dem Menu-Objekt
  **komplett** — gesetzt, per Screenshot geprüft, keinerlei sichtbare Wirkung. Keine bekannte
  Tk-Option behebt das; der native Menüstreifen wird vom Windows-eigenen Renderer gezeichnet, den
  Tk nicht kontrolliert. **Lösung:** den Menübalken durch einen `ttk.Menubutton` in einer eigenen
  Leiste ersetzen (`root.config(menu="")`, dann eigene Leiste bauen) — das Dropdown selbst bleibt
  ein natives `tk.Menu`-Popup (kein ttk-Äquivalent dafür vorhanden), aber die durchgehend
  sichtbare Leiste ist jetzt korrekt themed, und genau die war das gemeldete Problem.
- **2026-08-26 — "Flash from release/" und "Erase chip..." aus dem `t1s_ptp_bridge`-Schwesterprojekt
  nach `bridge_gui.py` portiert, `flash_same54.py` brauchte dafür zuerst eine `--erase`-Option
  (gab es hier noch nicht, dort schon).** `pyocd erase --chip` ist der richtige Modus fürs
  vollständige Löschen (Firmware **und** emuliertes EEPROM, beide im selben physischen Flash) —
  nicht `--sector`/`--mass`, das mit `-c`/`--chip` verwechselt eine harmlosere Teillöschung
  auslöst. Mitportiert, weil im Schwesterprojekt schon schmerzhaft gefunden: **`sys.executable`
  darf für den `flash_same54.py`-Subprozess-Aufruf nicht blind übernommen werden.** Läuft diese
  GUI über `pythonw.exe` (kein zweites Konsolenfenster), verliert der ENKEL-Prozess
  (`pythonw.exe -m pyocd erase --chip ...`) irgendwo in der Kette seine Standardausgabe — das
  Kommando wird noch echot, dann kommt nichts mehr, auch nicht „Chip erase complete", obwohl der
  Erase am echten Board durchlief. `_console_python()` sucht deshalb neben `pythonw.exe` nach
  `python.exe` und nimmt das. Bei uns lösen `run_gui.bat`/`run_gui_modern.bat` aktuell **beide**
  mit normalem `python`, nicht `pythonw` — der Fehler tritt hier also (noch) nicht auf, aber
  die Absicherung bleibt drin, falls das mal über einen anderen Weg gestartet wird.
- **2026-08-26 — Analyse „würde ein frischer Checkout die GUI voll funktionsfähig starten" fand
  eine echte stille Lücke, `dep_check.py` behebt sie.** `bridge_gui.py`/`gui_term.py` degradieren
  ohne `pyserial` sauber (Try/Except bzw. lazy Import), aber `bridge_gui_modern.py`/
  `gui_term_modern.py` hatten ein **hartes** `import sv_ttk` auf Modulebene. `run_gui_modern.bat`
  prüft das vorher ab und zeigt eine klare Meldung — `term_modern.bat` **nicht**, und startet
  zusätzlich per `pythonw` über ein losgelöstes `start`, dessen Exitcode nie ausgewertet wird.
  Ergebnis: ein frischer Checkout ohne vorheriges `pip install -r requirements.txt` ließ
  `term_modern.bat` beim Doppelklick **komplett lautlos** scheitern — kein Fenster, keine Meldung,
  nichts. Behoben durch `dep_check.py`: prüft mit `importlib.util.find_spec` (kein echter Import,
  kann selbst nicht abstürzen), zeigt bei fehlenden Paketen einen Tk-Dialog mit „Install now"
  (führt `install_dependencies.bat` aus, Ausgabe wird live gestreamt — gleiches
  Thread+Queue+`after()`-Polling-Muster wie `_run_pyocd_op` beim Flash/Erase). Dafür musste
  `import sv_ttk` aus dem Modulkopf von `bridge_gui_modern.py`/`gui_term_modern.py` **in**
  `main()` wandern, hinter den Dependency-Check — sonst wäre der Absturz schon vor dem Check
  passiert. Nach einem erfolgreichen Install immer „bitte neu starten" statt Weitermachen im
  laufenden Prozess: ein bereits fehlgeschlagener `import serial` auf Modulebene ist zu dem
  Zeitpunkt schon ausgewertet und als fehlgeschlagen im Modul verankert (`serial = None`), ein
  `pip install` danach ändert daran im selben Prozess nichts mehr.
- **2026-08-26 — `T1SPMACTL.TXD` auf dem PLCA-Coordinator wirkt netzweit, nicht nur lokal —
  jetzt an drei echten Knoten gemessen, vorher nur aus der Spezifikation vermutet.** Testaufbau:
  Node 0 = Coordinator, langer TCP-`iperf` zwischen Node 1 und Node 2 (Node 0 selbst nicht am
  Transfer beteiligt), auf Node 0 zweimal `lan_rmw 0x000308F9 0x4000 0x4000` (Sender aus) gefolgt
  von `lan_rmw 0x000308F9 0x4000 0x0000` (Sender an). Ergebnis in zwei unabhängigen Belegen:
  (1) **`PLCA_STS.PST`** (`0x0004CA03`, Bit 15) auf Node 1 folgte dem Sendezustand von Node 0
  exakt und ohne im Log erkennbare Verzögerung — `TXD=1` → `PST` fällt von `0x8000` auf `0x0000`,
  `TXD=0` → `PST` zurück auf `0x8000`. (2) Der `iperf`-Durchsatz zwischen Node 1 und Node 2 brach
  in beiden Toggle-Durchläufen auf ~35 Kbps (≈0,6 % der Nennrate ~5840 Kbps) ein und erholte sich
  erst 4–15 s **nach** dem Zurücksetzen von `TXD` wieder vollständig — keine sofortige Erholung,
  echte Resync-Latenz. TCP überstand beide Aussetzer ohne Verbindungsabbruch (Gesamtdurchschnitt
  4782 statt ~5840 Kbps über 100 s); bei UDP wären das schlicht verlorene Pakete ohne
  Wiederholung. **Schlussfolgerung:** der Coordinator (Node-ID 0) abzuschalten legt die
  PLCA-Koordination für **das ganze Segment** lahm, nicht nur die eigene Verbindung — ein reiner
  Listening-Modus über `TXD` ist für den Coordinator selbst also nur dann praxistauglich, wenn
  vorher die Coordinator-Rolle an einen anderen Knoten übergeben wurde (siehe Abschnitt 4
  „PMA-Loopback" in `CLAUDE.md` zur selben Vorbedingung). **Noch offen:** ob die verbliebenen
  ~35 Kbps während des Aussetzers echter CSMA/CD-Fallback-Verkehr sind oder etwas anderes — dafür
  fehlt noch ein `PLCA_STS`-Log mit exakter Zeitkorrelation zum iperf-Verlauf.
- **2026-08-27 — Fortsetzung des TXD/PLCA-Befunds, jetzt mit `test_txd_impact.py` automatisiert
  und über 21 s statt nur wenige Sekunden beobachtet — zwei neue Erkenntnisse, eine davon
  widerspricht der ersten Vermutung.** Aufbau: Bridge = Coordinator, `TXD` von vor dem
  iperf-Start bis 21 s **nach** Verbindungsaufbau durchgehend aus, `STATS6`/`STATS7`
  (`0x0001020E`/`0x0001020F`) einmal pro Sekunde mitgeloggt. (1) **Der ~35-Kbps-Zustand ist ein
  stabiler Dauerzustand, kein Übergangs-Blip** — 21 s am Stück exakt `0/3 (0%) 35 Kbps` auf beiden
  Followern, ohne jede Tendenz zur Erholung, solange `TXD=1` bleibt. (2) **Die Bridge empfängt
  während des Stalls weiter fehlerfrei, nur seltener** — `TFRX` und `FRX` laufen fast durchgehend
  gleichauf (z. B. 9/9, 9/10, 12/9), kein Auseinanderlaufen wie man es bei CRC-Müll aus
  Kollisionen erwarten würde. Erstes Indiz (kein Beweis) für einen sauberen Fallback-Mechanismus
  statt Chaos — für den Beweis fehlt weiterhin die `PLCA_STS`-Korrelation. (3) **Widerspruch zur
  ersten Messung: die Erholungszeit nach `TXD=0` ist nicht fest.** Hier war der Durchsatz binnen
  **unter 2 s** wieder bei ~5840 Kbps (t=34,86s Reset, t=36,42s schon 4679 Kbps, t=37,43s voll);
  beim ersten, manuellen Lauf (Eintrag oben) brauchte die erste Erholung ~13 s. Zwei unabhängige
  Messungen mit deutlich unterschiedlicher Erholungsdauer bei ansonsten gleichem Handgriff —
  spricht für eine timing-abhängige Re-Synchronisation (Position im PLCA-Zyklus beim Umschalten),
  nicht für eine feste Reconnect-Verzögerung. **Nicht überinterpretieren:** die allererste
  Zählerzeile eines Laufs (hier `TFRX=711 FRX=712`) ist der aufgelaufene Reststand seit dem
  letzten Lesen vor Testbeginn, kein echter Burst — erst ab der zweiten Zeile ist der Wert
  aussagekräftig.
- **2026-08-27 — Gegenprobe zu den beiden Einträgen oben: ein unbeteiligter Nicht-Coordinator
  abzuschalten stört niemanden — jetzt gemessen, nicht nur aus dem PLCA-Modell angenommen.**
  Aufbau mit `test_follower_txd_impact.py`: iperf-Server auf der Bridge (Coordinator), iperf-Client
  auf Follower A, `TXD` wird stattdessen auf **Follower B** getoggelt — der an diesem Transfer gar
  nicht beteiligt ist. Ergebnis über den vollen 60-s-Transfer (30000 Pakete): **0 % Paketverlust**,
  von iperf selbst gemeldet auf beiden Seiten (`0/30000 (0%) 5839 Kbps` Server, `0/29998 (0%)
  5840 Kbps` Client) — kein aus eigenen Zählern abgeleiteter Wert, sondern iperfs eigene
  Verlustbilanz. Während der ~12 s, die Follower B's `TXD` aus war, blieb die Sekundenrate auf
  beiden Seiten ohne jede Auffälligkeit bei den üblichen ~5840 Kbps; Follower B's eigener Empfang
  (`TFRX`/`FRX`) blieb ebenfalls unverändert bei ~1065–1080/s vor, während und nach dem Toggle —
  das eigene RX ist vom eigenen TX unabhängig, wie erwartet. **Damit ist die Asymmetrie aus den
  beiden Einträgen oben empirisch geschlossen:** der Coordinator abzuschalten legt das ganze
  Segment lahm (Werkzeug `test_txd_impact.py`), ein unbeteiligter Mitgliedsknoten abzuschalten hat
  keinerlei messbaren Effekt (Werkzeug `test_follower_txd_impact.py`) — ein Knoten ohne eigene
  Sendeabsicht musste PLCA-seitig nie aktiv "an die Reihe" kommen, also fehlt seine Abwesenheit
  auch niemandem.
- **2026-08-27 — `PLCA_STS.PST` (`0x0004CA03`, Bit 15) ist die Selbstdiagnose eines
  Nicht-Coordinator-Knotens für "gibt es gerade einen aktiven Coordinator auf dem Bus".**
  Datenblattdefinition (DS60001734F, §11.5.60, S. 283): "This field indicates that the PLCA
  reconciliation sublayer is active and a BEACON is being regularly transmitted or received." —
  `1` = Beacon wird regelmäßig empfangen (bzw. auf dem Coordinator selbst: gesendet), `0` = nicht.
  An echter Hardware bestätigt (Eintrag oben, 2026-08-26): `PST` auf Node 1 folgte dem
  Sendezustand des Coordinators exakt und ohne im Log erkennbaren Verzug. **Einschränkung:**
  `PST=0` sagt nur "ich empfange gerade kein regelmäßiges Beacon", nicht wieso — aus Sicht eines
  einzelnen Knotens sehen "kein Coordinator vorhanden", "Coordinator vorhanden, aber sein Sender
  ist aus" und "nur meine eigene Verbindung zum Bus ist gestört" identisch aus. Für die praktische
  Frage "kann ich mich gerade auf einen aktiven Coordinator verlassen?" ist das unerheblich — in
  allen drei Fällen ist die richtige Antwort "nein, gerade nicht", und genau die liefert `PST`
  zuverlässig.
- **2026-08-27 — Neuer `sniffer`-Modus (`port_mirror.c`) spiegelte anfangs 0 Frames bei
  iperf-Rate, obwohl die Firmware jeden einzelnen Schritt als erfolgreich meldete — Ursache war
  Heap-Fragmentierung, nicht Adressfilterung.** `sniffer` hebt bei `MIRROR_Eth0Rx()` den
  Ziel-MAC-Filter auf, den `mirror` hat (Frames zwischen zwei *anderen* Knoten werden jetzt auch
  gespiegelt — möglich, weil `DRV_LAN865X_PROMISCUOUS_IDX0=true` das MAC schon vorher alles
  annehmen lässt, siehe `configuration.h:138`). Bei iperf-Rate (~500 fps) kam auf `eth1` aber fast
  nichts an. **Diagnoseweg:** eigene Zähler in `mirror_ethpkt_to_eth1()` (`rx_hook`,
  `pool_empty`, `tx_submitted`) UND der längst vorhandene `stats`-Befehl (`eth1 TX: ok=8134
  qFull=0`) zeigten beide **erfolgreiche** Übergabe an den GMAC-Treiber — der Verlust lag also
  nicht im eigenen Code, sondern weiter unten. `TCPIP_PKT_PacketAlloc()` zieht aus dem
  gemeinsamen 65 536-Byte-TCPIP-Heap (`TCPIP_STACK_DRAM_SIZE`, `configuration.h:332`) — bei
  ~1650 Byte je Kopie und **Fragmentierung durch alles andere, was denselben Heap benutzt** (TCP,
  DHCP, ARP, …), schlägt die Allokation lange vor der nominellen Kapazität fehl. **Lösung:**
  eigenes, festes Pool aus 8 vorab allokierten `TCPIP_MAC_PACKET`s (`TCPIP_PKT_PacketAlloc(...,
  TCPIP_MAC_PKT_FLAG_STATIC)`, über eine `PROTECTED_SINGLE_LIST` verwaltet), genau das Muster,
  das `tcpip_mac_bridge.c` für dieselbe Traffic-Rate bereits erfolgreich nutzt
  (`TCPIP_MAC_BRIDGE_PACKET_POOL_SIZE=8`) — ein Pool aus gleich großen Blöcken kann nicht
  fragmentieren, ein allgemeiner Heap mit gemischten Lebensdauern schon. **Wichtige Falle dabei:**
  `TCPIP_MAC_PKT_FLAG_STATIC` schützt ein Paket **nicht** automatisch vor `TCPIP_PKT_PacketFree()`
  — `_TCPIP_PKT_PacketAllocInt()` streicht dieses Flag bei jeder Allokation unbedingt
  (`tcpip_packet.c`). Der einzige Schutz ist Disziplin im eigenen Code: nie `PacketFree`
  aufrufen, nur über die eigene Ack-Funktion ins eigene Pool zurücklegen — exakt das Muster, das
  `drv_lan865x_api.c`s `_RxPacketAck()` für die eigenen RX-Puffer schon vorlebt.
- **2026-08-27 — Fixed-Pool-Fix für `sniffer` an echter Hardware nachgetestet: 0 Drops.**
  PC↔Follower-Dauerlast über die Bridge erzeugt (`bandwidth_ramp_client.exe`, ~2 Mbit/s,
  ~13 s, bis die Verbindung am bekannten PC↔Follower-Ceiling mit `WSAECONNRESET` abbrach —
  ein separater, bereits dokumentierter Bug, hier irrelevant), `sniffer 1` auf der Bridge
  parallel an. Debug-Zähler vor/nach: `rx_hook=477 passed_filter=477 pool_empty=0 no_eth1=0
  tx_submitted=477` — jeder einzelne Frame, der den Filter passierte, wurde auch tatsächlich
  an eth1 übergeben, kein einziger Pool-Engpass. Bestätigt den Fix aus dem Eintrag direkt
  darüber unter realer Bus-Last, nicht nur aus dem Code geschlossen.
- **2026-08-27 — Neues, eigenes Modul `testserver.c` (TCP-Echo-Server für Bandbreitentests)
  hatte zwei unabhängige, schwerwiegende Bugs, beide erst durch einen bewusst
  thread-freien C-Referenzclient von einem unzuverlässigen Python-Client unterscheidbar.**
  (1) **Nach dem ersten Client nahm der Server nie einen zweiten an** — `TCPIP_TCP_IsConnected()`/
  `WasDisconnected()` zeigten den Verbindungsabbau korrekt, aber der Socket blieb für neue
  Verbindungen taub. `tcp.h`s eigener Kommentar zu `TCPIP_TCP_Disconnect()` sagt es explizit:
  *"The server socket will return to listen state."* — genau dieser Aufruf fehlte. **(2) Weit
  schwerwiegender: echter, stiller Datenverlust bei niedrigen Bandbreiten (10–40 Kbps), auch
  ganz ohne T1S/PLCA (reproduziert direkt gegen die Bridge selbst, kein Follower beteiligt).**
  `TCPIP_TCP_ArrayPut()` kann **weniger** Bytes annehmen als übergeben (TX-Puffer momentan voll)
  — der Rückgabewert ist kein Logging-Detail, sondern die tatsächlich angenommene Menge. Der
  Code behandelte den kompletten `got`-Wert als „gesendet" und verwarf den nicht angenommenen
  Rest ersatzlos. **Erst nach beiden Fixes:** `sent == received`, byte-genau, bei jeder einzelnen
  Stufe von 10 Kbps bis in den Mbps-Bereich, sowohl bridge-direkt als auch über den vollen
  T1S-Pfad. **Merksatz, der zum Fund führte:** bei unerwartetem Datenverlust nie nur einen
  einzigen Client vertrauen — ein zweiter, unabhängig implementierter Client (hier: `select()`-
  basiertes C statt Python-Threads, siehe `scripts/bandwidth_ramp_client.c`) trennt zuverlässig
  Client-Bug von Server-Bug, wenn beide dasselbe Verlustmuster zeigen oder eben nicht.
- **2026-08-27 — Eine doppelte PLCA-Node-ID (Bridge und ein Follower beide auf ID 7) erzeugte
  genau das Bild eines Firmware-Bugs: mal ging eine Verbindung durch, mal nicht, ganz ohne
  erkennbares Muster.** Erst als der Nutzer den betroffenen Follower auf eine freie ID (1)
  umkonfigurierte, wurde klar, dass die vorher beobachtete Instabilität (Timeouts, ein einmaliger
  Runaway-Output auf der Konsole nach einem SWD-Reset) eine **Umgebungsursache** hatte, keine im
  Code. **Merksatz:** bei sporadischem, nicht reproduzierbarem Verhalten auf einem Mehrknoten-T1S-
  Bus zuerst die PLCA-Node-IDs alle beteiligten Knoten gegenprüfen (`plca_node` bzw. `showenv`),
  bevor man im eigenen Code sucht.
- **2026-08-27 — Das `follower/`-Projekt aus dem Schwesterprojekt `t1s_ptp_bridge` in dieses
  Repo kopiert, um `testserver.c` auch dort zu bauen.** Reiner Ordner-Kopiervorgang
  (`follower/firmware/T1S_Follower.X`, eigenes MPLAB-X-Projekt) — funktioniert selbstständig,
  weil `nbproject\configurations.xml` keine Pfade oberhalb des eigenen Projektordners referenziert
  und `follower/build.bat` nur `..\genmk.bat`/`..\setup_compiler.config` **im neuen Elternordner**
  braucht, die hier schon vorhanden sind (identisch bis auf Sprache). **Vor dem ersten Build
  aufräumen:** `dist/`, `build/`, `debug/`, `.generated_files/` und die `nbproject\Makefile-*`-
  Fragmente aus der alten Kopie sind gitignored, aber physisch mitkopiert und tragen absolute
  Pfade des **alten** Speicherorts — erst löschen, dann `genmk.bat` neu erzeugen lassen, sonst
  scheitert `xc32-bin2hex` auf die bekannte Art (siehe Abschnitt 2 in `CLAUDE.md`).
  **Board-Zuordnung nicht aus einer Sonden-Seriennummer allein ableiten:** Sonde (SWD) und
  COM-Port (USB-CDC) sind zwei unabhängige USB-Schnittstellen desselben oder verschiedener
  Boards — ein Flash auf eine vom Nutzer benannte Sonde, gefolgt von einer Prüfung über den
  *erwarteten* COM-Port, zeigte zweimal **keine Änderung**, weil die genannte Sonde zu einem
  anderen physischen Board gehörte als der COM-Port. Verlässlich war erst der Ausschluss über
  alle drei bekannten Sonden aus `bench.json` plus Gegenprobe (`help` zeigt die neu geflashte
  Kommandogruppe am erwarteten COM-Port, oder eben nicht).
- **2026-08-27 — Der ~4,3-Mbit/s-Deckel von `testserver.c` lag NICHT an der Zykluszeit der
  Hauptschleife — eine Vermutung, die sich mit echter Messung als falsch herausstellte, statt
  eine weitere Sackgasse blind auszubauen.** Erste Theorie: `TESTSERVER_Tasks()` liest nur einen
  `TESTSERVER_CHUNK` (512 Byte) pro `APP_Tasks()`-Durchlauf, also limitiert die Zyklusrate der
  Schleife den Durchsatz. **Gegenprobe statt Umbau ins Blaue:** ein Zähler
  (`s_idle_cycle_count`, einmal pro `APP_STATE_IDLE`-Durchlauf erhöht, per 1-Hz-Timer-Callback
  in Zyklen/Sekunde umgerechnet, ausgegeben über `stats`) zeigte **75 548 Zyklen/s im Leerlauf**
  und **38 000–59 000 Zyklen/s unter voller Last** — um Größenordnungen schneller, als nötig
  wäre. Ein Umbau von `testserver.c` auf ein 8-KB-Budget pro Aufruf (statt 512 Byte) bestätigte
  das: **keine Änderung am Deckel**, weiterhin ~4,3 Mbit/s. **Der eigentliche Hinweis steckt in
  `eth1 TX: ok=`** (bereits vorhandener `stats`-Zähler): über 27 Sekunden von 718 auf 28382, also
  **~1024 Frames/Sekunde, auffällig konstant** — passt größenordnungsmäßig zu ~4,3 Mbit/s bei
  ~1 KB Nutzlast pro Frame. Das deutet auf eine Frames-pro-Sekunde-Grenze im GMAC-Treiber oder
  der TCPIP-Stack-Verarbeitung selbst, unabhängig von `testserver.c` und der App-Schleife — nicht
  weiter verfolgt, da das eine Ebene tiefer (Treiber-/Interrupt-Overhead pro Frame) liegt als für
  eine Diagnose-Sonde sinnvoll ist. **Merksatz: bei einer Performance-Vermutung erst einen
  Zähler einbauen und messen, bevor man den Code umbaut** — der Umbau hier kostete eine
  Baue-Flash-Test-Runde und brachte nichts, weil die Theorie falsch war; die Messung hätte das
  vorher gezeigt.
- **2026-08-27 — `sniffer`/`mirror` an `eth1` gespiegelte Frames über ~1514 Byte legten
  wiederholt die USB-Ethernet-Schnittstelle des PCs lahm (Npcap "adapter no longer
  attached", Wireshark-Capture bricht ab) — Ursache liegt nachweislich AUSSERHALB dieser
  Firmware.** Volle Untersuchung in `SNIFFER_1_HYPOTHESEN.md` bis `SNIFFER_4_ERGEBNISSE.md`.
  Kernbeweis: neue Zähler in `port_mirror.c` (`ack_ok`/`ack_fail`, ausgewertet aus
  `pkt->ackRes`, dem vom GMAC-Treiber selbst beim TX-Abschluss gesetzten Ergebniscode, nicht
  nur "API aufgerufen" wie der alte `tx_submitted`) zeigten **100 % treiberbestätigten
  Sendeerfolg für jeden einzelnen Frame, auch die größten** — während im PC-Mitschnitt im
  selben Zeitfenster **0 von 985** derselben Frames ankamen. Kein Windows-Ereignisprotokoll-
  Eintrag (`Kernel-PnP`) belegt eine echte USB-Trennung. Ein direkter PC→Bridge-iperf-Test
  (realer `iperf2`-Client vom PC, **nicht** über den Mirror-Pfad, sondern normaler
  Netzwerkstack) lief bei identischer Framegröße fehlerfrei (0/852). **Schlussfolgerung: die
  Ursache ist eine Eigenheit von Npcap/dem USB-Ethernet-Adapter dieses PCs beim Rohpaket-
  Mitschnitt großer Frames — kein Firmware-Bug.** **Umgesetzte Abmilderung** (kein echter
  Fix, da extern): `mirror_ethpkt_to_eth1()` kürzt gespiegelte Frames jetzt auf
  `MIRROR_SAFE_FRAME_LEN` (1514 Byte) statt sie unverändert durchzureichen — wirkt nur auf
  die Diagnose-Kopie (beide Aufrufer von `mirror_ethpkt_to_eth1()` sind durch
  `if (!s_mirror_on && !s_sniffer_on) return;` bzw. `if (!s_mirror_on) return;` abgesichert),
  die eigentliche PC↔T1S-Weiterleitung läuft komplett getrennt über
  `tcpip_mac_bridge.c` und ist strukturell unberührt. Verifiziert: 1840/1840 gekürzte Frames
  kamen an, keine Adapter-Aussetzer mehr.
  **Nebenbefund, separater Bug:** `TC6_SendRawEthernetPacket()` (genutzt von `noip_send`/
  `bigframe`, der unsegmentierte Rohframe-Pfad) verliert Frames über ~1514 Byte spurlos — der
  Sender meldet Erfolg (auch der eigene `eth0 TX`-Hardwarezähler zeigt `ok`, `err=0`), aber
  niemand empfängt etwas, nicht mal als Fehler gezählt. Vermutlich unzureichendes Nachziehen
  von `serviceData()` bei einem einzelnen, großen Chunk-Batch (`tc6.c:304`, nur ein Aufruf
  direkt beim Einreihen). **Nicht behoben, nicht root-caused** — betrifft einen anderen
  Sendepfad als der Mirror-Bug (segmentierter Stack-Pfad über `iperf`/TCP/UDP war die ganze
  Zeit fehlerfrei bis 1468 Byte Nutzlast).
  **Sackgasse, nicht weiter verfolgen:** `noip_send <n> <gap> <size>`s eigene Mehrfachschleife
  reicht nur einen **Zeiger** auf einen einzigen, wiederverwendeten Puffer an
  `TC6_SendRawEthernetPacket()` weiter (keine Kopie) — bei `count > 1` überschreibt der
  nächste Aufruf den Puffer, bevor der vorherige Frame fertig übertragen ist. Für
  Wiederholungstests stattdessen `noip_send 1 0 <size>` einzeln aus einem Skript aufrufen
  (wie `NOIP_SendOne()` es für den PTP-Trigger-Pfad bereits vormacht), nicht die eingebaute
  Schleife.
- **2026-08-27 — Echter PC→Bridge-UDP-Flood (`iperf -b <bandbreite>` von einem realen
  PC-Client, nicht die Bridge-eigene Test-Suite) legt `eth1`-RX ab irgendwo zwischen 20 und
  30 Mbit/s dauerhaft lahm — kein Zählerartefakt, keine Selbstheilung, nur ein Reset hilft.**
  Reproduziert mit `iperf.exe -c 192.168.0.210 -B 192.168.0.100 -b 10000000..100000000`
  (`-b` schaltet automatisch auf UDP, kein `-u` nötig). Bis 20 Mbit/s (auch dreimal
  hintereinander gegen dieselbe Server-Instanz) lief alles sauber (`stats` `err=0`,
  `eth1 RX` zählt normal mit). Nach den Läufen bei 30/60/100 Mbit/s: `eth1 RX` **eingefroren**
  auf dem letzten Wert vor der Eskalation, auch bei einem anschließenden Lauf mit nur
  1 Mbit/s — keine Erholung. **Konsole bleibt die ganze Zeit voll erreichbar** (`uptime`/
  `stats` antworten normal, kein Absturz) — nur der `eth1`-Datenempfang ist tot. Per
  `tshark`-Mitschnitt bestätigt: ab ca. 4,5 s nach Beginn der Eskalation beantwortet die
  Bridge **keine einzige ARP-Anfrage mehr**, für den Rest des 120-s-Mitschnitts — nicht nur
  UDP-Port-5001-spezifisch, `eth1`-RX ist grundsätzlich tot (bestätigt auch per `ping`:
  "Destination host unreachable" von der PC-eigenen Adresse, also keine ARP-Antwort). Nach
  `pyocd reset` sofort wieder normal (`eth1 RX` zählt, `ping` antwortet mit `<1ms`). **Nicht
  root-caused** — offener Punkt für eine Folge-Session: vermutlich eine echte
  GMAC-Ressourcenerschöpfung oder ein Deskriptor-/Interrupt-Problem im
  `drv_gmac.c`/`drv_gmac_lib_samE5x.c`-Pfad bei sehr hoher, ungebremster eingehender
  Paketrate auf `eth1` — unabhängig vom Sniffer/Mirror-Thema (`SNIFFER_1…4_*.md`), da hier
  gar kein Mirror/Sniffer aktiv war und die Bridge selbst der Zielport war, nicht T1S.
  **Nachtrag — Root Cause per Live-Speicherauslesen gefunden (`dump <addr> <count>` über
  die Konsole, während die Bridge im toten Zustand war; Adressen aus der `.map`-Datei und
  den Struct-Definitionen berechnet, siehe unten):**
  1. **Der RX-Deskriptor-Ring für `eth1` ist fest auf 8 Einträge hartkodiert**
     (`initialization.c:454`, `.nRxDescCnt = 8`) — die in `configuration.h` konfigurierten
     "dynamischen Zusatzpuffer"-Konstanten (`TCPIP_GMAC_RX_ADDL_BUFF_COUNT_QUE0`,
     `RX_BUFF_COUNT_THRESHOLD_QUE0`, `RX_BUFF_ALLOC_COUNT_QUE0`) werden **nirgends im
     Treiber (`drv_gmac.c`, `drv_gmac_lib_samE5x.c`) referenziert** — toter Code, keine
     echte dynamische Nachlieferung existiert.
  2. **`nRxBuffNotAvailable`** (das `stats` als `eth1 RX nobufs` anzeigt) **wird im
     GMAC-Treiber nirgends beschrieben** (verifiziert: kein einziges Vorkommen in
     `drv_gmac.c` außer der Nullinitialisierung) — dieser Zähler kann für `eth1` strukturell
     nie etwas anderes als 0 zeigen, unabhängig vom tatsächlichen Zustand. Erklärt, warum
     beim Hänger nirgends ein Fehler sichtbar war.
  3. **Live-Speicherauslesen während des reproduzierten Hängers** (RX-Deskriptor-Array bei
     `0x2000C5A8`, `gmac_queue[0]` bei `0x2000C810`, beide Adressen aus der `.map`-Datei +
     Struct-Layout von Hand berechnet, per `dump` bestätigt): **alle 8 RX-Deskriptoren**
     zeigen `rx_desc_buffaddr = 0x00000001` (bzw. `0x3` beim Wrap-Eintrag) — Ownership-Bit
     software-eigen gesetzt, aber **Adressteil komplett gelöscht** (`& ~GMAC_RX_ADDRESS_MASK`,
     passiert beim Extrahieren eines Pakets aus dem Ring, siehe
     `drv_gmac_lib_samE5x.c:1298`) — alle 8 warten auf Neubefüllung, die nie kommt.
  4. **Der Puffer-Rückgabepool `_RxQueue` selbst ist korrumpiert:** `head=0x00000000`
     (NULL), aber `tail=0x20016100` und `nNodes=9` — für die (korrekte!)
     Single-Linked-List-Logik in `DRV_PIC32CGMAC_SingleListTailAdd()`/`HeadRemove()`
     eigentlich unmöglich ohne Nebenläufigkeitsfehler. **9 freigegebene Puffer sitzen
     unerreichbar in der Liste** (`SingleListHeadRemove()` liest nur `head`, findet `NULL`,
     gibt sofort auf), obwohl genug Material zum Nachfüllen da wäre.
  5. **Vermuteter Mechanismus:** `_DRV_GMAC_RxLock()`/`_DRV_GMAC_RxUnlock()`
     (`drv_gmac_local.h:519-533`) sind **komplette No-Ops**, wenn `_synchF == 0` — in diesem
     RTOS-losen Bare-Metal-Projekt mit hoher Wahrscheinlichkeit nie gesetzt. Ohne echten
     Lock kann ein Interrupt mitten in `SingleListTailAdd()`/`HeadRemove()` dazwischenfunken
     und die Liste zerreißen — passt exakt zum beobachteten Muster (`head` verloren,
     `tail`/`nNodes` noch intakt).
  6. **Selbst wenn `_RxQueue` intakt wäre, gäbe es noch das strukturelle Problem:** das
     Nachfüllen der Hardware-Deskriptoren aus `_RxQueue`
     (`DRV_PIC32CGMAC_LibRxBuffersAppend()`) wird nur als **Nebeneffekt erfolgreicher
     Paketverarbeitung** angestoßen (`DRV_PIC32CGMAC_LibRxGetPacket()`, Aufrufe bei
     `drv_gmac_lib_samE5x.c:1305/1330/1450`). Ist der Ring einmal komplett leer, findet
     `_SearchRxPacket()` nie wieder ein gültiges Paket — und damit wird auch das Nachfüllen
     nie wieder ausgelöst, selbst wenn `_RxQueue` gesunde Puffer enthielte. Ein
     struktureller Teufelskreis, unabhängig vom Race-Condition-Befund unter Punkt 4/5.
  **Fazit:** mindestens zwei, vermutlich zusammenwirkende Treiberfehler (Race Condition in
  der ungesicherten Liste + fehlender Retrigger-Mechanismus fürs Nachfüllen) — kein
  Anwendungscode dieses Projekts beteiligt, Ursache sitzt tief im MPLAB-Harmony-eigenen
  `drv_gmac`-Treiber. **Nicht behoben** — ein echter Fix bräuchte entweder einen
  funktionierenden Lock (`_synchF` wirklich verdrahten) oder einen expliziten
  Retrigger-Mechanismus fürs Nachfüllen unabhängig vom Paketverarbeitungspfad, oder beides.
  **Nachtrag — zwei Fix-Versuche im Treiber selbst, Ergebnis: deutliche Verbesserung, kein
  vollständiger Fix.**
  1. **`_DRV_GMAC_RxLock()`/`_DRV_GMAC_RxUnlock()`** (`drv_gmac_local.h`) fallen jetzt auf
     `SYS_INT_Disable()`/`SYS_INT_Restore()` zurück, wenn `_synchF==0` (No-RTOS-Fall dieses
     Projekts) — echter globaler kritischer Abschnitt statt No-Op.
  2. **`DRV_PIC32CGMAC_LibRxBuffersAppend()`** (`drv_gmac_lib_samE5x.c`) wird jetzt
     zusätzlich **unconditional, einmal pro Ring-Index einzeln**, am Anfang von
     `DRV_PIC32CGMAC_LibRxGetPacket()` aufgerufen — nicht mehr nur als Nebeneffekt
     erfolgreicher Paketverarbeitung (bricht den unter Punkt 6 oben beschriebenen
     Teufelskreis).
  3. **Nachbesserung:** die ursprünglich **zwei getrennten** Lock/Unlock-Fenster in
     `LibRxBuffersAppend()` (eines um `SingleListHeadRemove()`, eines um den
     Deskriptor-Schreibzugriff, mit kurz wieder aktivierten Interrupts dazwischen) zu
     **einem** durchgehenden kritischen Abschnitt zusammengefasst (Pufferentnahme +
     Deskriptor-Schreiben + `pRxPckt[]`-Buchführung).

  **Verifiziert mit demselben PC→Bridge-UDP-Flood-Test, mehrere Runden:**

  | Zustand | Schwelle bis zum Hänger | Betroffene Deskriptoren |
  |---|---|---|
  | vor jedem Fix | ~20–30 Mbit/s | 8/8 |
  | nach Fix 1+2 | 60 Mbit/s sauber, 100 Mbit/s bricht | 2/8 |
  | nach Fix 3 (zusammengefasster Lock) | ~85–100 Mbit/s | 1/8 |

  **Wichtiger Zusatzbefund, warum selbst 1/8 kaputte Deskriptoren die Bridge komplett
  taub macht:** Datenblatt zu `GMAC_RSR.BNA` (§24.9.9): „The DMA will re-read the
  **pointer** each time an end of frame is received until a valid pointer is found" —
  die Hardware bleibt an der **aktuellen Ringposition** hängen und prüft nur genau
  diesen einen Zeiger erneut, sie springt nicht zu den anderen, gesunden Deskriptoren
  weiter. Ein einziger korrupter Slot genügt deshalb für einen kompletten
  Empfangsausfall, unabhängig davon, wie viele der übrigen 7 noch frei wären.

  **Fazit:** Die Race Condition wurde mit jedem Schritt kleiner (8/8 → 2/8 → 1/8
  betroffene Deskriptoren, Schwelle 20–30 → 85–100 Mbit/s), aber **nicht vollständig
  beseitigt** — es gibt offenbar noch mindestens eine weitere, nicht gefundene
  ungesicherte Zugriffsstelle (vermutlich im GMAC-eigenen Interrupt-Handler, der in
  diese Untersuchung noch nicht einbezogen wurde). Für den praktischen Betrieb bereits
  ein sehr deutlicher Gewinn (Alltagslast bricht das nicht mehr aus), aber kein
  beweisbar vollständiger Fix. Beide Patches sind Hand-Patches an generiertem
  Harmony-Code (wie der LAN865x-TX-Hook) — gehen bei MCC-Neugenerierung verloren.
- **2026-08-27 — Echter Follower→PC-TCP-Transfer (JPerf/`iperf.exe` als Server auf dem PC,
  Follower als Client, normaler Netzwerkstack, kein Mirror/Sniffer) verlor bei
  Standard-MSS 1460 mitten im Transfer ganze Segmente spurlos — `-M 1400` (JPerf: Checkbox
  „Max Segment Size" unter TCP, Wert 1400) behebt es.** Per `tshark`-Mitschnitt auf dem PC
  belegt: bei MSS 1460 blieb die Verbindung nach dem ersten Burst (1460 Byte, sauber
  ge-ACKt) **35 s** stehen, danach tauchte beim PC ein Segment ab `Seq=4381` auf, markiert
  `[TCP Previous segment not captured]` — die dazwischenliegenden **2920 Byte (exakt
  2×1460)** sind nie angekommen, der PC quittiert bis zum Schluss nur `Ack=1461`; der
  Follower gibt nach einem erfolglosen Retransmit-Versuch mit `RST` auf. `stats` auf der
  Bridge zeigte zeitgleich **`eth1 TX err=7`** (nicht 0) — dieser Zähler wird laut
  `drv_gmac.c:667-671`/`DRV_PIC32CGMAC_LibTxClearUnAckPacket()` (`drv_gmac_lib_samE5x.c:753`)
  **ausschließlich** erhöht, wenn der GMAC-Treiber einen Link-Down auf `eth1` erkennt und
  daraufhin **alle gerade wartenden TX-Pakete kommentarlos verwirft** (kein Retry, keine
  Fehlermeldung an den TCP/IP-Stack außer dem stillen `ACK_LINK_DOWN`-Callback) — passt
  exakt zum Bild im Mitschnitt. Mit `-M 1400` (Frame ~1454 statt ~1514 Byte) lief derselbe
  Testaufbau danach fehlerfrei durch: **4.620.001 Byte in 10,0 s, keine Retransmits, keine
  Dup-ACKs, sauberer beidseitiger FIN/ACK-Abschluss** (verifiziert per erneutem
  `tshark`-Mitschnitt). **Nicht root-caused, warum ausgerechnet Frames nahe 1514 Byte den
  Link-Down-Trigger auf `eth1` auslösen** (passt aber zur selben ~1514-Byte-Grenze wie beim
  Mirror-Bug und bei `TC6_SendRawEthernetPacket()` oben) — offener Punkt für eine
  Folge-Session, insbesondere ob es dieselbe Ursache wie die GMAC-RX-Race-Condition
  (`drv_gmac_lib_samE5x.c`, siehe oben) im TX-Pfad ist. **Praktische Konsequenz für alle
  künftigen iperf-TCP-Tests über die Bridge: `-M 1400` (JPerf-Feld „Max Segment Size")
  routinemäßig setzen**, sonst können Transfers ohne erkennbaren Grund minutenlang hängen
  bleiben.
  **Korrektur (2026-08-27, selber Tag, per echten Bridge-internen Zählern widerlegt):**
  Die `eth1 TX err=7`-Erklärung oben war ein Fehlschluss — der Zähler stand schon direkt
  nach jedem Boot/Reset auf einem kleinen, stabilen Wert (vermutlich PHY-Aufstartverhalten)
  und hatte mit dem eigentlichen Verlust nichts zu tun. Die echte Ursache: siehe den Eintrag
  weiter unten zu `TCPIP_MAC_BRIDGE`/`failMtu` — **`-M 1400` hat zufällig genau deshalb
  geholfen, weil es die Framegröße unter dieselbe 1518-Byte-Grenze drückt**, an der die
  generische Harmony-Bridge fälschlich alles Größere verwarf. Kein Link-Down, keine
  GMAC-Race — ein simpler Off-by-Header/FCS-Fehler in `tcpip_mac_bridge.c`.
- **2026-08-27 — Follower→PC-UDP-Flood (`iperf -c 192.168.0.100 -u -b ...`) kam beim echten
  PC-Server mit 0 von 2510 Paketen an (per echtem `iperf`-UDP-Server verifiziert, nicht nur
  Wireshark) — Ursache: generische Harmony-`tcpip_mac_bridge.c` verwirft beim Weiterleiten
  jeden Frame > 1500 Byte, obwohl Standard-Ethernet bis 1518 Byte erlaubt.** Root-Cause per
  neu aktiviertem `TCPIP_MAC_BRIDGE_STATISTICS`/`EVENT_NOTIFY` (`configuration.h:540f`,
  vorher beide `false` — deshalb war das eingebaute `bridge stats`-Kommando bis dahin nutzlos,
  „failed to get stats!") zweifelsfrei belegt: `bridge stats` zeigte `failMtu: 2511` (exakt
  die Paketzahl), `bridge register` + Testpaket lieferte den echten Wert
  `Bridge event: fail MTU, size: 1516` (= 14 Byte Ethernet-Header + 20 IP + 8 UDP + 1470
  Nutzlast + 4 Byte FCS).
  **Mechanismus** (`tcpip_mac_bridge.c:1257-1271`): `linkMtu = _TCPIPStackNetLinkMtu(pOutIf)`
  liefert `TCPIP_MAC_LINK_MTU_DEFAULT = 1500` (`tcpip_mac.h:1657`) — das ist die **IP-Schicht-MTU**
  (an anderer Stelle im selben Stack, `ipv4.c:3450`, wird dieselbe Größe als
  `linkMtu - sizeof(IPV4_HEADER)` benutzt, um die maximale Nutzlast zu berechnen — dort also
  korrekt als „ohne Ethernet-Header" behandelt). `pFDcpt->pktLen` kommt aber aus
  `TCPIP_PKT_PayloadLen()`, das stumpf `segLen` aller Segmente aufaddiert — bei diesem
  Projekt (patched LAN865x-Treiber auf `eth0`) ist das der **komplette erfasste Frame
  inklusive 14-Byte-Header und 4-Byte-FCS**, nicht nur die Nutzlast. Laut
  `tcpip_mac.h:358-367` sollte der MAC-**Treiber** Header+FCS beim Empfang eigentlich schon
  abziehen, bevor er das Paket an den Stack übergibt — passiert hier nicht (Indiz:
  `pktEth0Handler()` liest die Quell-MAC über `pMacLayer[6]`, `pktEth1Handler()` dagegen über
  `pDSeg->segLoad[6]` — unterschiedliche Zeigerkonvention zwischen LAN865x- und
  GMAC-Empfangspfad). Der Vergleich `pktLen <= linkMtu` (1500) verwirft dadurch **jeden**
  Standard-1500-Byte-IP-Frame (der als Ethernet-Frame 1518 Byte groß ist), nicht nur
  Ausreißer — trifft TCP mit MSS 1460 (1514 Byte) genauso wie UDP mit Default-Datagrammgröße
  1470 (1516 Byte).
  **Fix, bewusst am Symptom statt an der Ursache** (Entscheidung: der eigentliche Fehler säße
  im bereits mehrfach gepatchten, kritischen LAN865x-Treiber — riskanter Eingriff mit
  potenziell weiteren Auswirkungen auf den Stack; der lokale Patch im generischen
  Bridge-Modul ist die pragmatische, schnell verifizierbare Lösung):
  ```c
  // tcpip_mac_bridge.c, ~Zeile 1263
  if(pFDcpt->pktLen <= linkMtu + sizeof(TCPIP_MAC_ETHERNET_HEADER) + 4u)  // + FCS
  ```
  Nach Fix: `failMtu: 0`, `fwd_direct`/`fwd ucast` zählen die Pakete korrekt, `eth1 TX ok`
  (GMAC-Treiberebene) steigt passend mit. **Nicht behoben, nur umgangen:** der eigentliche
  Vertragsbruch im LAN865x-RX-Pfad (Header+FCS werden entgegen `tcpip_mac.h`-Doku nicht
  abgezogen) bleibt bestehen — falls anderer Stack-Code sich ebenfalls auf die dokumentierte
  Kontrakt-Länge verlässt, könnten dort verwandte Symptome auftauchen. Patch ist ein
  Hand-Patch an generiertem Harmony-Code (wie die GMAC-/LAN865x-Fixes weiter oben) — geht bei
  MCC-Neugenerierung verloren.
  **`bridge stats`/`bridge register` sind ab jetzt scharf** (`TCPIP_MAC_BRIDGE_STATISTICS`
  und `TCPIP_MAC_BRIDGE_EVENT_NOTIFY` auf `true`, RAM-Kosten minimal, ~0,5 KB) — für jede
  künftige „Pakete kommen nicht durch die Bridge"-Diagnose zuerst `bridge stats` prüfen,
  nicht erst raten.
  **Nebenbefund/Sackgassen bei der Diagnose:**
  - Mehrere Hintereinander-Starts von `jperf-2.0.2/bin/iperf.exe -s` auf dem PC binden sich
    NICHT alle scheiternd — der alte Prozess läuft weiter (dieses iperf-1.7-Binary beendet
    sich nach einer Session normalerweise selbst, tat es aber in dieser Sitzung mehrfach
    nicht) und mehrere Instanzen laufen parallel weiter, ohne Fehler beim Neustart. Wer nur
    das Log der *neuen* Instanz prüft, sieht scheinbar „nichts angekommen", obwohl eine
    ältere Instanz den Port hält. **Vor jedem Reproduktionsversuch `taskkill /F /IM
    iperf.exe` (alle Instanzen), dann neu starten**, nicht nur die zuletzt gestartete PID
    killen — Windows-PIDs aus Git-Bash-`ps`/`$!` stimmen nicht zuverlässig mit denen von
    `taskkill` überein.
  - Firewall/fehlende ARP-Einträge waren **keine** Ursache: passende Freigaberegeln für
    genau dieses `iperf.exe` (TCP und UDP) existierten bereits, `bridge fdb show` zeigte die
    PC-MAC korrekt gelernt.
  - **Zwischenzeitlich offener Punkt, seitdem aufgeklärt (siehe unten):** Trotz `failMtu: 0`
    kam beim echten PC-Server weiterhin nichts an — der `failMtu`-Fix allein hat das Problem
    nicht gelöst, siehe Fortsetzung.
- **2026-08-27 — Fortsetzung/Abschluss des `failMtu`-Funds oben: `failMtu`-Fix allein reichte
  nicht — der eigentliche Fehler steckte in der `pktLen`-Berechnung selbst, jetzt vollständig
  behoben und mit einem echten `iperf`-UDP-Server bei 0% Verlust verifiziert.** Auch nach dem
  `failMtu`-Fix kam beim Follower→PC-UDP-Test weiterhin **0 von 2510** Paketen an (mit
  eigenem `tshark` **und** der eigenen Wireshark-GUI des Nutzers gegengeprüft — kein
  Mitschnitt-Artefakt). `bridge stats` zeigte aber `fwd ucast`/`fwd direct` korrekt hochzählen
  und `eth1 TX ok` (GMAC-Ebene) passend mitsteigen — die Software glaubte auf allen Ebenen,
  erfolgreich gesendet zu haben.
  **Zwischenschritt 1 (Idee des Nutzers): kopierloses Weiterleiten abschalten.** Bei nur
  einem Zielport reicht `tcpip_mac_bridge.c` (`_MAC_Bridge_ForwardPacket`, ~Zeile 855-859)
  den **originalen** eth0-Empfangspuffer ungekopiert an `_TCPIPStackPacketTx()` für eth1
  weiter (Zähler `fwd_direct`) — anders als `sniffer`, der immer in einen eigenen Puffer
  kopiert. Umgebaut auf denselben Copy-Mechanismus wie beim `TCPIP_MAC_BRIDGE_PKT_RES_HOST_PROCESS`-Zweig
  (`_MAC_Bridge_GetFwdPkt()` + `_MAC_Bridge_PacketCopy()`), inklusive sofortiger
  `TCPIP_PKT_PacketAcknowledge(pRxPkt, TCPIP_MAC_PKT_ACK_RX_OK)` fürs Original (sonst geht dem
  LAN865x-Treiber der RX-Puffer aus, da der `BRIDGE_OWN`-Rückgabepfad am Ende von
  `_MAC_Bridge_ForwardPacket` das Original nie quittiert). **Allein brachte das noch keine
  Besserung** — weiterhin 0 Pakete beim Server.
  **Zwischenschritt 2, der eigentliche Rootcause:** `_TCPIPStackPacketTx()` (`tcpip_manager.c:4514`)
  ruft letztlich exakt dieselbe `pMacObj->TCPIP_MAC_PacketTx()` (= `DRV_GMAC_PacketTx()`) wie
  `sniffer` auf — der Transportweg ist identisch, das Problem lag also nicht im Aufrufpfad,
  sondern weiterhin in der Länge. `_MAC_Bridge_PacketCopy()` kopiert `pktLen` Byte ab
  `pNetLayer` (das schon HINTER dem Ethernet-Header liegt) — mit dem (aus dem `failMtu`-Fund
  bekannten) fehlerhaften `pktLen` von 1516 (statt korrekt 1498) wurden dabei **18 Byte zu
  viel** kopiert (in den alten FCS-Bereich und 14 Byte danach liegenden Heap-Speicher hinein),
  und die für beide Zweige geltende Zeile `pFwdPkt->pDSeg->segLen += sizeof(TCPIP_MAC_ETHERNET_HEADER)`
  machte die Länge zusätzlich falsch (1530 statt korrekt 1512 für die Übertragung). Die Wurzel
  ist **`fwdDcpt.pktLen = TCPIP_PKT_PayloadLen(pRxPkt)`** selbst (`tcpip_mac_bridge.c:805`) —
  dieselbe Fehlmessung, die schon den `failMtu`-Bug verursacht hat, wird hier ein zweites Mal
  wirksam. **Fix, direkt an der Quelle statt an jeder einzelnen Verwendungsstelle:**
  ```c
  // tcpip_mac_bridge.c, direkt nach fwdDcpt.pktLen = TCPIP_PKT_PayloadLen(pRxPkt);
  if(inPort == 0)  // eth0/LAN865x - hier steckt der Vertragsbruch, eth1/GMAC ist korrekt
  {
      fwdDcpt.pktLen -= (uint16_t)(sizeof(TCPIP_MAC_ETHERNET_HEADER) + 4u);  // Header + FCS
  }
  ```
  **Ergebnis, mit echtem `iperf -s -u` auf dem PC verifiziert:**
  ```
  [612]  0.0- 5.0 sec  3.51 MBytes  5.88 Mbits/sec  2.045 ms    0/ 2501 (0%)
  ```
  0 % Verlust, alle Datagramme angekommen — vorher, mit demselben Testaufbau: 0 von 2510.
  **Zusammenspiel der drei Fixes:** Der `failMtu`-Schwellenwert-Patch (`+18`) ist mit dieser
  Korrektur eigentlich überflüssig geworden (echtes `pktLen` liegt jetzt sowieso unter 1500),
  bleibt aber als harmlose zusätzliche Sicherheitsmarge stehen. Der Umkopieren-Patch war
  **notwendig**, nicht nur der `pktLen`-Fix allein: der kopierlose Pfad verwendet
  `pDSeg->segLen` direkt (weiterhin roh und falsch vom Treiber), nicht `fwdDcpt.pktLen` — nur
  über `_MAC_Bridge_PacketCopy()` wirkt die `pktLen`-Korrektur überhaupt. Alle drei Änderungen
  zusammen ergeben den vollständigen Fix.
  **Bewusst nicht am LAN865x-Treiber selbst behoben** (Nutzerentscheidung, siehe oben) — falls
  der Treiber später doch korrigiert wird (Header+FCS beim RX korrekt abziehen, wie
  `tcpip_mac.h` es vorschreibt), müssen alle drei Patches hier wieder rückgebaut werden, sonst
  zieht `fwdDcpt.pktLen` dann fälschlich nochmal 18 Byte zu viel ab.
- **2026-08-28 — `PC -> Follower` UDP mit dem echten `iperf.exe` unter Windows bricht schon
  bei 5 Mbit/s Zielrate spürbar ein (3-9 % Verlust je nach Lauf), obwohl dieselbe T1S-Strecke
  in jeder anderen Richtung (`Bridge <-> Follower`, `Follower <-> Follower`, sogar
  `Follower -> PC`) sauber ~9,4 Mbit/s schafft — kein Bridge-/Firmware-Bug, sondern eine
  Windows-Eigenheit des PC-seitigen `iperf.exe`.** Per `tshark`-Mitschnitt (Feld
  `frame.time_relative`) belegt: bei nominell 5 Mbit/s (erwarteter Abstand ~2,4 ms/Paket)
  schickt der Windows-Client tatsächlich Schübe von 4-7 Paketen in **unter 1 ms**, dann eine
  ungleichmäßige Pause, immer wieder — der Mittelwert stimmt, die Momentanlast schießt aber
  weit über die Zielrate hinaus. Bekannte Ursache: iperf 1.x/2.x pacet über eine
  Sleep-Schleife, und Windows' grobe Standard-Timer-Auflösung (~15,6 ms) lässt diese Schleife
  hinterherhinken und in Bursts aufholen. Ein Einzeltest zeigte serverseitig real
  `75/1277 (5,9 %)` Verlust bei 5 Mbit/s Ziel. TCP ist davon nicht betroffen, weil es sich
  über ACK-Rückkopplung selbst gleichmäßig taktet statt über eine Sleep-Schleife — die
  eingebauten iperf-Clients (Bridge/Follower) pacen offenbar ebenfalls sauber genug, um das
  Problem nicht auszulösen. **Praktische Konsequenz:** Für belastbare PC→T1S-UDP-Zahlen ein
  anderes Windows-Tool mit hochauflösendem Timer verwenden — mit `jperf-2.0.2/bin/iperf.exe`
  als Quelle sind PC→Follower-UDP-Ergebnisse mit Vorsicht zu genießen, alle anderen
  Richtungen sind davon nicht betroffen. Details/Mitschnitt-Beleg: `IPERF_TEST_MATRIX.md`.
  **Nachtrag — Fix gefunden und verifiziert, kein anderes Tool nötig:**
  `winmm.timeBeginPeriod(1)` (Windows-API, systemweit wirksam solange irgendein Prozess sie
  hält) zwingt die System-Timer-Auflösung auf 1 ms. Per erneutem `tshark`-Mitschnitt bestätigt:
  Pakete kommen danach gleichmäßig alle ~2-3 ms statt in Schüben, `0/1276 (0%)` Verlust bei
  5 Mbit/s statt vorher 5,9 %. `iperf_matrix_test.py` ruft das jetzt für die komplette
  Laufzeit auf (`ctypes.WinDLL("winmm").timeBeginPeriod(1)`/`timeEndPeriod(1)`) — betrifft
  auch den `iperf.exe`-Subprozess, weil die Wirkung maschinenweit gilt. `PC -> Follower A/B`
  UDP erreicht damit jetzt ebenfalls ~8 Mbit/s statt 2 Mbit/s, siehe `IPERF_TEST_MATRIX.md`.
- **2026-08-28 — `bridge_gui_modern.py`/`gui_term_modern.py` (die separaten sv-ttk-Vergleichsbauten)
  in `bridge_gui.py`/`gui_term.py` verschmolzen und gelöscht, auf Nutzerentscheidung: kein
  Vergleich mehr nötig, sv-ttk ist jetzt fester Bestandteil beider Tools.** Die
  `BridgeGUIModern(BridgeGUI)`/`AppModern(App)`-Unterklassen konnten nicht einfach verschwinden —
  sie erbten von den jetzt-zu-löschenden Dateien, also mussten `_apply_dark_titlebar()`,
  `_tighten_button_style()`, `_restore_semantic_colors()` (bzw. `_restore_pane_colors()`,
  `_replace_pane_scrollbars()`) als normale Methoden in die Basisklassen wandern, `main()`
  bekam `--light` plus den harten `sv-ttk`-Dependency-Check, den vorher nur die
  `_modern`-Varianten hatten. Bei `gui_term.py` zusätzlich die Menüleiste **direkt** als
  `ttk.Menubutton`-Leiste gebaut statt (wie in `AppModern`) erst ein natives `tk.Menu` zu
  bauen und es danach durch `_replace_menu_bar()` zu ersetzen — der nachträgliche
  Umbau-Schritt hatte nur Sinn, solange `App.__init__` unverändert von `AppModern` geerbt
  wurde. Alle hart erarbeiteten sv-ttk-Eigenheiten aus den `_modern`-Dateien (Idle-Task-Reihenfolge,
  DWM-Titelleisten-Dunkelmodus, native Scrollbar/Menüleiste ignorieren Tk-Farboptionen unter
  Windows) sind dabei unverändert übernommen, nicht neu hergeleitet — siehe die
  ausführlichen Docstrings/Methodenkommentare direkt in `bridge_gui.py`/`gui_term.py` für die
  Details, die vorher in den jetzt gelöschten Dateien standen. `check_gui_language.py`,
  `dep_check.py`, `requirements.txt` und `CLAUDE.md` entsprechend nachgezogen. Verifiziert:
  beide Tools starten fehlerfrei mit dem Theme (kein Traceback) — die tatsächliche Optik
  nicht per Screenshot geprüft, nur die Abwesenheit eines Absturzes.
- **2026-08-28 — Alle 19 Python-Dateien aus dem Repo-Root nach `scripts\` verschoben (`git mv`),
  auf Nutzerwunsch ("ich will das alle python dateie in scripts\ liegen").** Betroffen:
  `bridge_gui.py`, `build_summary.py`, `check_env_model.py`, `check_gui_language.py`,
  `check_register_model.py`, `cli.py`, `cli_doc_check.py`, `dep_check.py`, `flash_same54.py`,
  `gui_term.py`, `install_prereqs.py`, `iperf_matrix_test.py`, `setup_compiler.py`,
  `setup_debug.py`, `smoketest.py`, `sniffer_capture_test.py`, `test_lan8651.py`,
  `test_mirror.py`, `test_terminal_input.py`. Reine Verschiebung, kein Verhaltensunterschied —
  aber jede `Path(__file__)`-relative Referenz auf eine Repo-Root-Datei (`bridge_config.json`,
  `bench.json`, `lan8651_model.json`, `env_model.json`, `setup_compiler.config`,
  `install_dependencies.bat`) musste von `.parent` auf `.parent.parent` wechseln — sonst hätte
  z. B. `flash_same54.py` sein `bench.json` plötzlich unter `scripts\bench.json` gesucht statt
  im Root, wo `install.bat --select` es tatsächlich schreibt. **Skripte, die sich nur auf sich
  selbst oder auf einen jetzt ebenfalls mitgezogenen Nachbarn beziehen, brauchten dagegen KEINE
  Änderung** — `test_lan8651.py`s eigener `sys.path.insert(0, os.path.dirname(...))`,
  `install_prereqs.py`s `from flash_same54 import ...` (beide jetzt Geschwister in `scripts\`),
  `gui_term.py`s `term_ports.json` (bewusst neben sich selbst, wandert einfach mit). Alle
  `.bat`-Aufrufstellen (`build.bat`, `flash.bat`, `install.bat`, `run_gui.bat`, `setup.bat`,
  `term.bat`, `follower\build.bat`) und alle Doku-Dateien (`CLAUDE.md`, `README.md`,
  `BRIDGE_GUI_README.md`, `CLI_COMMANDS.md`, `LAN8651_TEST_MODES.md`,
  `SNIFFER_CAPTURE_VALIDATION.md`) entsprechend nachgezogen — **mit Ausnahme reiner
  Prosa-Erwähnungen eines Dateinamens ohne Aufrufsyntax**, die blieben unverändert, und mit
  Ausnahme der eigenen historischen Einträge in dieser Datei (nur echte Kommandozeilen-Beispiele
  wie `python test_mirror.py` wurden hier auf `scripts\test_mirror.py` korrigiert, der
  erzählende Text drumherum nicht). Verifiziert: `check_register_model.py`, `check_env_model.py`,
  `check_gui_language.py` (0 Fehler), `gui_term.py --selftest` (14/14) und `flash_same54.py
  --show-probe`/`--list` liefen fehlerfrei aus `scripts\` heraus, `build.bat` lief komplett durch
  (inkl. `scripts\build_summary.py`-Aufruf am Ende) und hinterließ einen sauberen Baum.
- **2026-08-28 — Alle JSON-Dateien im Repo-Root (auch die per-Rechner-gitignorten) nach `json\`
  verschoben, auf Nutzerwunsch ("alle json dateien müssen in einen unterordner json").**
  Getrackt: `boards.json`, `bridge_config.json`, `env_model.json`, `lan8651_model.json`
  (`git mv`). Gitignored/temporär: `bench.json`, `term_ports.json` (plain `mv` — waren nie
  getrackt, die unverankerten `.gitignore`-Muster (`bench.json`/`term_ports.json` ohne
  führenden `/`) greifen unverändert auch unter `json\`, per `git check-ignore -v` bestätigt).
  **Bewusst NICHT verschoben:** `.main-meta\main.json` in den beiden `.X`-MPLAB-Projektordnern
  — von MPLAB X selbst an fester Stelle relativ zum Projekt erzeugt/gelesen, kein eigenes
  Tooling-Artefakt (analog zu `nbproject\`, das ebenfalls unangetastet bleibt). Ebenfalls NICHT
  verschoben: `setup_compiler.config` — enthält zwar JSON, trägt aber bewusst eine andere
  Endung, und der Nutzerwunsch war wörtlich "json dateien" (Erweiterung, nicht Inhalt).
  Betroffene `Path(__file__)`-Referenzen bekamen ein zusätzliches `/ "json"` eingefügt
  (`bridge_gui.py`: `CONFIG_FILE`, `MODEL_FILE`, `ENV_MODEL_FILE`; `check_env_model.py`:
  `MODEL_PATH`; `check_register_model.py`: `MODEL_PATH`; `flash_same54.py`: `BENCH_PATH`;
  `flash_boards.py`: `BOARDS_JSON`). **`gui_term.py`s `term_ports.json` war die einzige
  Ausnahme mit echtem Verhaltenswechsel:** es lag bisher `HERE`-relativ (neben dem Skript
  selbst, siehe Eintrag oben vom selben Tag), jetzt zeigt `CONFIG` auf
  `os.path.dirname(HERE)/json/term_ports.json` — ein Repo-Root-Pfad wie die anderen fünf.
  **Nebenbefund dabei:** die physische `term_ports.json` war seit dem `scripts\`-Umzug (Eintrag
  oben) verwaist — `gui_term.py` suchte sie schon unter `scripts\term_ports.json`, die reale
  Datei lag aber noch unverändert im Root, weil nur Python-Dateien verschoben wurden. Fiel nicht
  auf, weil eine fehlende `term_ports.json` von `gui_term.py` als "noch nicht konfiguriert"
  behandelt wird (leere Zuweisung, kein Fehler) — der `--selftest`-Lauf direkt nach dem
  `scripts\`-Umzug zeigte entsprechend `1=-(-), 2=-(-), 3=-(-)` statt der echten, gespeicherten
  Portbelegung, ohne dass das als Fehlschlag auffiel. Mit dem `json\`-Umzug jetzt behoben und
  verifiziert: derselbe `--selftest`-Lauf zeigt wieder die echte Belegung
  (`1=Bridge 192.168.0.210(COM8), 2=Follower A ..., 3=Follower B ...`). **Lehre:** ein
  Pfad-Umzug, der eine Konfigurationsdatei "verliert", muss nicht als Fehler auffallen, wenn der
  Aufrufer eine fehlende Datei still als Erstlauf interpretiert — nach jedem Pfad-Umzug den
  *Inhalt* einer geladenen Konfiguration prüfen, nicht nur den Exitcode. Eine `\ ` (Backslash
  gefolgt von Leerzeichen) in einem Python-Docstring erzeugte dabei eine `SyntaxWarning:
  invalid escape sequence` (`bridge_gui.py`s Modul-Docstring, "liegt in json\ im Repo-Root") —
  in `#`-Kommentaren harmlos, in echten String-Literalen (Docstrings, `"..."`) nicht: mit einem
  Wort statt Backslash umschrieben ("liegt im json-Ordner"). Alle 25 Skripte in `scripts\`
  danach mit `py_compile.compile(..., doraise=True)` unter `warnings.simplefilter("error")`
  gegengeprüft — keine weiteren Treffer. Verifiziert: `check_register_model.py`,
  `check_env_model.py`, `gui_term.py --selftest`, `flash_same54.py --show-probe`,
  `flash_boards.py --list` liefen fehlerfrei, `build.bat` komplett durchgelaufen, Baum danach
  sauber (bis auf die erwarteten `git mv`/Edit-Änderungen).
- **2026-08-28 — Alle Markdown-Dateien im Repo-Root außer `README.md` nach `docs\` verschoben
  (`git mv`), auf Nutzerwunsch.** Betroffen: `BRIDGE_GUI_README.md`, `CLI_COMMANDS.md`,
  `FALLSTRICKE.md` (diese Datei), `IPERF_TEST_MATRIX.md`, `LAN8651_TEST_MODES.md`,
  `SNIFFER_1_HYPOTHESEN.md` … `SNIFFER_4_ERGEBNISSE.md`, `SNIFFER_CAPTURE_VALIDATION.md`.
  **Zwei bewusste weitere Ausnahmen, vorher per Nachfrage geklärt statt geraten:**
  `CLAUDE.md` bleibt im Root, weil es Claude Codes eigene, automatisch pro Session geladene
  Projektanweisungsdatei ist — ein Umzug nach `docs\` hätte das automatische Laden beendet;
  `json\README.md` (eigenes Verzeichnis-Readme für `json\`, nicht am Root) und
  `follower\firmware\T1S_Follower.X\KEIN_MCC_MODELL.md` (gehört direkt neben das MPLAB-X-Projekt,
  das es erklärt, analog zu `.main-meta\main.json`) blieben ebenfalls, wo sie waren.
  **Ein echter Funktionsbruch, kein bloßer Kommentar:** `scripts\cli_doc_check.py` hatte
  `DOC = 'CLI_COMMANDS.md'` als CWD-relative Konstante (nicht `Path(__file__)`-basiert wie die
  meisten anderen Skripte — dieses eine erwartet von jeher CWD=Repo-Root beim Aufruf, genau wie
  sein `SRC = os.path.join('firmware', 'src')`) — nach dem Umzug sofort mit
  `FileNotFoundError: 'CLI_COMMANDS.md'` reproduziert, dann auf
  `DOC = os.path.join('docs', 'CLI_COMMANDS.md')` korrigiert und erneut verifiziert (`PASS,
  26/26 Kommandos`), auch über `follower\build.bat`s Aufruf (`..\scripts\cli_doc_check.py
  --quiet`) hinweg. **Alle sonstigen Fundstellen waren reine Prosa-Verweise** („siehe
  FALLSTRICKE.md, 2026-08-27" o. ä.) in Kommentaren/Docstrings über den ganzen Baum verteilt —
  `firmware\src\*.c/.h`, `follower\firmware\src\*.c/.h`, `scripts\*.py`, `requirements.txt` —
  jeweils um `docs\`/`docs/` ergänzt (Konvention der jeweiligen Datei beibehalten). **Eine
  Ausnahme davon war kein Kommentar, sondern kompilierter Konsolentext:**
  `port_mirror.c`s `SYS_CONSOLE_PRINT(...)` bei `dump`/`stats` enthält wörtlich „see
  SNIFFER_4_ERGEBNISSE.md" im Format-String — dort auf `docs/SNIFFER_4_ERGEBNISSE.md` geändert,
  **das braucht einen Neu-Flash**, um auf dem Gerät sichtbar zu werden (reines Kosmetikum, keine
  Funktionsänderung). Interne Querverweise **zwischen** den verschobenen Dateien selbst
  brauchten unterschiedliche Behandlung: `LAN8651_TEST_MODES.md` ↔ `CLI_COMMANDS.md`
  (beide jetzt Geschwister in `docs\`) blieben unverändert bare Links; Links von dort auf
  `README.md`/`CLAUDE.md` (am Root geblieben) sowie auf `firmware\...`/`scripts\...` bekamen
  ein `../` davor. **Explizit NICHT angefasst:** `sniffer_capture_results.log` — enthält als
  Testmitschnitt denselben „see SNIFFER_4_ERGEBNISSE.md"-String, aber als historisches Zitat
  dessen, was eine ältere Firmware tatsächlich ausgegeben hat, nicht als lebender Verweis;
  Ändern hätte das Protokoll verfälscht. Verifiziert: `cli_doc_check.py` (PASS),
  `check_register_model.py`/`check_env_model.py` (0 Fehler), `build.bat` **und**
  `follower\build.bat` liefen komplett durch (inkl. des `cli_doc_check.py`-Aufrufs am Ende von
  `follower\build.bat`), abschließende Grep-Suche über `*.c`/`*.h`/`*.py`/`*.txt`/`*.bat` fand
  keine unpräfixierten Treffer mehr außer der eigenen `os.path.join('docs', ...)`-Zeile (falsch
  positiv der Grep-Suche selbst, keine echte Lücke).
- **2026-08-28 — `boards.jpg`, `iperf_matrix_results.log` und `sniffer_capture_results.log` aus
  dem Repo-Root ebenfalls nach `docs\` verschoben (`git mv`), auf Nutzerwunsch.** `boards.jpg`
  war nur von `README.md` verlinkt (`![...](boards.jpg)` → `![...](docs/boards.jpg)`). Die
  beiden Logs sind Lauf-Ergebnisse von `iperf_matrix_test.py`/`sniffer_capture_test.py` — anders
  als die JSON-/Modell-Dateien beim `scripts\`-Umzug hängt ihr Zielpfad **nicht** an
  `Path(__file__)`, sondern an argparse-`--log`-Defaults, die als reiner Dateiname CWD-relativ
  aufgelöst werden (`ap.add_argument("--log", default="iperf_matrix_results.log")` bzw.
  `"sniffer_capture_results.log"`) — beide Defaults auf `docs/…` umgestellt, damit ein Lauf ohne
  `--log`-Override (Aufruf weiterhin von der Repo-Wurzel aus, wie überall sonst in diesem
  Projekt) automatisch wieder neben der `.md`-Datei landet, die ihn zitiert
  (`docs\IPERF_TEST_MATRIX.md`, `docs\SNIFFER_CAPTURE_VALIDATION.md`). Deren eigene
  Prosa-Erwähnung des Logdateinamens brauchte **keine** Änderung — Log und zitierende `.md`-Datei
  sind jetzt Geschwister im selben Ordner, ein bare Dateiname stimmt weiterhin. Der bestehende
  historische Hinweis weiter oben in dieser Datei („sniffer_capture_results.log … explizit NICHT
  angefasst" beim `docs\`-Umzug der Markdown-Dateien selbst) bleibt unverändert stehen — er
  beschreibt eine damals richtige Entscheidung (Inhalt nicht ändern), nicht den Dateipfad, und
  bleibt auch nach diesem Umzug wahr. Verifiziert: `--help` beider Skripte zeigt den neuen
  Default korrekt, `py_compile` beider Dateien fehlerfrei, `open(..., "a")` gegen beide neuen
  Zielpfade erfolgreich getestet (leerer Schreibzugriff, kein Inhalt verändert).
- **2026-08-28 — `requirements.txt` aus dem Repo-Root nach `scripts\` verschoben (`git mv`),
  auf Nutzerwunsch.** Einzige echte Funktionsstelle: `install_dependencies.bat` las es bisher
  über `set REQUIREMENTS=%~dp0requirements.txt` (repo-root-relativ zu sich selbst) — auf
  `%~dp0scripts\requirements.txt` umgestellt und real durchgetestet (`install_dependencies.bat`
  fand die Datei, `pip install -r ...` lief durch, alle drei Pakete bereits vorhanden). Narrative
  Erwähnungen mit `pip install -r requirements.txt`/`requirements.txt` als reinem Namen in
  `scripts\dep_check.py`s Docstring und `CLAUDE.md`s Dateitabelle um `scripts\` ergänzt; die drei
  Treffer in den eigenen historischen Einträgen weiter oben in dieser Datei bewusst unverändert
  gelassen (Regel wie immer: datierte Einträge beschreiben, was zum jeweiligen Zeitpunkt galt).
- **2026-08-28 — Projekteigenes `.venv` eingeführt, alle `.bat`-Dateien darauf umgestellt, kein
  `activate`/`deactivate` nötig.** Auslöser: `pyocd` ist mit Begründung auf `0.43.0` gepinnt
  (0.44.1 bricht die SWD-Verbindung) — global installiert reicht ein `pip install --upgrade` in
  einem *anderen* Python-Projekt auf demselben Rechner, um das unbemerkt kaputtzumachen.
  `install_dependencies.bat` legt jetzt `.venv\` im Repo-Root an (`python -m venv`, idempotent —
  ein vorhandenes `.venv` wird nur mit `pip install -r scripts\requirements.txt` nachgezogen) statt
  global zu installieren. Jedes andere `.bat` (`build.bat`, `flash.bat`, `install.bat`,
  `run_gui.bat`, `term.bat`, `follower\build.bat`, `follower\flash.bat`) löst
  `.venv\Scripts\python(w).exe` selbst per Pfad auf (Fallback aufs globale `python`, falls `.venv`
  fehlt) — der Nutzer aktiviert nie etwas von Hand, jeder Doppelklick/Aufruf läuft automatisch im
  venv. Funktioniert reibungslos, weil `flash_same54.py`/`install_prereqs.py`/`flash_boards.py`
  schon vorher konsequent `sys.executable` statt eines bare `"python"` für `subprocess`-Aufrufe an
  `pip`/`pyocd` verwendeten — zwei Ausreißer (`sniffer_noip_investigation.py`,
  `test_sniffer_repro.py`, beides Ad-hoc-Diagnoseskripte mit bare `["python", "-m", "pyocd", ...]`
  für den Board-Reset) wurden dabei auf `sys.executable` korrigiert. `.gitignore` bekam `.venv/`
  (maschinenspezifisch, u. a. plattform-kompilierte Pakete wie `cmsis-pack-manager`). Real
  durchgetestet: `install_dependencies.bat` frisch (`.venv` angelegt, 28 Pakete installiert),
  danach `flash.bat --list`, `install.bat`, `follower\flash.bat --list` (alle drei fanden pyocd im
  venv und die drei EDBG-Sonden), `term.bat --selftest` und `build.bat help` liefen fehlerfrei.
  **Offen/bewusst nicht angefasst:** Ad-hoc-Terminalaufrufe wie `python scripts\cli.py …` oder
  `python scripts\test_lan8651.py …` (siehe `CLAUDE.md` Abschnitte 2–5, dutzendfach so
  dokumentiert) laufen weiterhin über das im PATH gefundene `python`, nicht über `.venv` — dafür
  gibt es noch keinen `.bat`-Wrapper (z. B. `cli.bat`); bislang ungefragt keinen gebaut, um nicht
  in die laufende Aufräumaktion (Verzeichnisumbau, siehe Einträge direkt darüber) hineinzupfuschen.
- **2026-08-28 — `install_dependencies.bat` renamed to `setup_venv.bat` (`git mv`), plus a
  "what do I actually run?" quick-reference table added to `README.md` and `CLAUDE.md`.** User
  feedback: with six setup-related root `.bat` files (`setup.bat`, `install_dependencies.bat`,
  `install.bat`, `genmk.bat`, plus `build.bat`/`flash.bat` themselves auto-calling two of them),
  the near-identical names `setup.bat` / `install.bat` / `install_dependencies.bat` made it unclear
  which one a user should ever type. Root cause wasn't redundant logic - it's that only two scripts
  are ever meant to be run by hand (`setup.bat` once per machine, `install.bat --select` when
  switching probe/board); `setup_venv.bat` (this rename), `install.bat --install` and `genmk.bat`
  are internal building blocks other scripts already call automatically. Fixed by (a) renaming the
  least self-explanatory one so its name matches what it does (creates/populates `.venv`, no longer
  "just pyserial" like the pre-`.venv` version this replaced), and (b) documenting the "you type
  this / this runs automatically" split explicitly instead of just listing files. Every reference
  updated: `build.bat`/`follower\build.bat` header comments (also trimmed from a stale 4-line
  manual-steps list down to "run setup.bat" - they'd drifted out of sync with `setup.bat`'s actual
  5 steps already), `install.bat`'s fallback comment, `setup.bat`'s own call plus a rewritten header
  making the run-this-yourself/internal split explicit, `scripts\dep_check.py` (`INSTALL_SCRIPT`
  path, docstring, dialog button text), and `scripts\bridge_gui.py`'s module docstring (kept in
  German - existing comment, only the filename changed). `docs\FALLSTRICKE.md`'s own **older**
  entries mentioning `install_dependencies.bat` (the `.venv` entry directly above this one, the
  `scripts\`-move and `requirements.txt`-move entries further up) were deliberately **left
  unchanged** - they describe what was true at the time, per this file's own rule. Verified via a
  full repo grep for the old name after the rename: zero hits outside this file's historical
  entries. From this point on, new content in this session (including this entry) is written in
  English per explicit user instruction, overriding the file's normal German convention for new
  additions - existing German entries were not retroactively translated.
- **2026-08-28 — `genmk.bat` and `setup_venv.bat` moved into a new `batch\` folder (`git mv`), on
  user request: "everything the user doesn't call directly themselves belongs in a `batch\`
  directory."** Applying the same you-run-it/it-runs-automatically split documented in the previous
  entry: `install.bat` stayed at the repo root despite also being called automatically (`--install`
  from `setup.bat`) because it has a genuine direct-use case too (`install.bat --select`, switching
  probe/board) - moving it would have broken that. **Real bug caught by this move, not just a path
  rename:** `setup_venv.bat` located its own `.venv` and `requirements.txt` via `%~dp0` (batch's
  `%~dp0` is the *executing script's* own directory) - `%~dp0.venv` and `%~dp0scripts\requirements.txt`
  silently resolved to `batch\.venv` and `batch\scripts\requirements.txt` once the file moved, both
  wrong. `genmk.bat` had no such problem - it's fully self-contained, taking its target as an
  explicit argument and never touching a repo-relative path. Fixed by introducing
  `REPO_ROOT=%~dp0..` in `setup_venv.bat` and rebuilding `VENV_DIR`/`REQUIREMENTS` from that.
  General lesson: moving a `.bat` file is not just updating callers' paths to it - audit what it
  resolves via its *own* `%~dp0` first, or a working script starts silently creating a duplicate,
  shadow `.venv` inside its own new folder. Every caller's `call` path was updated too
  (`build.bat`, `follower\build.bat`, `setup.bat`, `scripts\dep_check.py`'s `INSTALL_SCRIPT`,
  README.md, CLAUDE.md); `install.bat`'s comment about `setup_venv.bat` was updated for accuracy
  though it's prose, not a live path. Verified for real: deleted `.venv` entirely, ran
  `batch\setup_venv.bat` fresh (created `.venv` at the repo root, confirmed via `ls .venv/Scripts/
  python.exe` succeeding and `ls batch/.venv` failing - i.e. NOT inside `batch\`), then a full
  `setup.bat` run end to end (all 5 steps green, including `batch\genmk.bat` regenerating the
  nbproject Makefiles), then `build.bat help`, `flash.bat --list`, `follower\flash.bat --list` all
  still green afterward.
- **2026-08-28 — `term.bat` renamed to `run_term.bat` (`git mv`), on user request, for consistency
  with `run_gui.bat`.** No path-resolution risk like the `batch\` move above - `term.bat` addressed
  everything through its own `%~dp0` and a plain rename doesn't change that. Every reference updated
  (its own header comments and usage examples, `batch\setup_venv.bat`'s docstring-style file list,
  `scripts\dep_check.py`'s comment, `README.md`'s and `CLAUDE.md`'s quick-reference tables). This
  file's own older entries mentioning `term.bat` (the `batch\` move entry above, and the one further
  up about the `docs\`/`scripts\` reorg) were deliberately left unchanged, per this file's own rule.
  Verified: `run_term.bat --selftest` still exits 0.
- **2026-08-28 — `nbproject/configurations.xml` trug einen falschen `heap-size`-Wert
  (`98304` = 96 KiB), der Wert des *tatsächlich funktionierenden* Hex war `163840`
  = 160 KiB. Jeder frische Build aus dem Repo heraus (egal ob `build.bat`, `build.bat
  rebuild`, egal auf welchem Rechner/Worktree) erzeugte damit einen Hex, dessen
  Firmware in `CLOCK_Initialize()`/`FDPLL0_Initialize()` permanent in der
  `DPLLSYNCBUSY.DPLLRATIO`-Wartschleife hängen blieb (`plib_clock.c`) — noch bevor
  die Konsole überhaupt initialisiert ist, deshalb keinerlei CLI-Ausgabe. Nach
  stundenlanger Fehlsuche in die falsche Richtung (Fuses, pyOCD-Pack-Bug,
  Board-Zustand, Reset-Typ Hardware vs. Software, Toolchain-Version) fand sich die
  Ursache über einen präzisen Bit-für-Bit-Vergleich: der einzige inhaltliche
  Unterschied zwischen dem funktionierenden, committeten Hex und jedem Rebuild war
  ein einzelner 32-Bit-Zeiger, der exakt um 65536 Byte verschoben war — an einer
  Adresse kurz hinter dem letzten benannten RAM-Symbol, genau am Ende der
  linker-eigenen Heap-Reservierung (`_min_heap_size`, sichtbar im Build-Summary als
  „Heap"). Der Zeiger selbst liegt in einer für den Hang irrelevanten
  iperf-Reporting-Funktion (`ReportBW_Jitter_Loss`) — der eigentliche
  Boot-kritische Code (`CLOCK_Initialize`) war in jedem Vergleich byte-identisch.
  Der ursächliche Zusammenhang zwischen einer um 64 KiB kleineren Heap-Reservierung
  und dem DPLL-Hang ist **nicht** vollständig verstanden (kein offensichtlicher
  RAM-Overlap gefunden) — die Korrektur ist trotzdem eindeutig belegt: mit
  `heap-size=163840` bootet jeder frische Rebuild zuverlässig (mehrfach mit
  normalem Software-Reset über pyOCD getestet, kein Hardware-Reset-Workaround
  nötig). **Lösung:** `nbproject/configurations.xml`, Property `heap-size`, auf
  `163840` korrigiert, Makefiles danach neu generiert (die generierten
  Makefile-Fragmente sind gitignored und übernehmen den Wert nicht automatisch
  nach). **Merksatz: bei „Hex A funktioniert, identischer Quelltext + Rebuild
  funktioniert nicht" nicht nur den ausgeführten Code vergleichen, sondern JEDEN
  Byte-Unterschied im ganzen Image — auch scheinbar irrelevante Konstanten in
  unbeteiligtem Code können auf eine falsche Linker-/Projekteinstellung zeigen,
  die an ganz anderer Stelle wirkt.**
- 2026-08-29 -- Neuer Befehl plca_stat (lan865x_diag.c) fand sofort einen echten
  Doppel-Coordinator auf dem Testbus. Motivation: IP-Frame-Werkzeuge (mirror/sniffer/
  noip_send) haengen an der RX-Frame-Hook, aber BEACON/COMMIT sind Reconciliation-
  Sublayer-Steuersymbole unterhalb der MAC-Framebildung -- sie erreichen diese Hook nie,
  egal welcher Filter entfernt wird. plca_stat liest stattdessen eine feste Sequenz aus
  PLCA-Registern (PLCA_STS, STS1-Event-Bits, STS3.ERRTOID, PRSSTS.MAXID, dazu die ueber
  CTRCTRL.TOCTRE/BCNCTRE einmalig freigeschalteten Zaehler TOCNT/BCNCNT) und druckt einen
  dekodierten Report; da TOCNTH/L und BCNCNTH/L read-clear sind, zeigt jeder Aufruf die
  Deltas seit dem letzten. Erster Testlauf am Bench (Bridge COM8 + Follower COM10/COM23):
  Bridge zeigte OUT OF RANGE, staendig UNEXPB und unplausibel hohe TO-/BEACON-Zaehlerstaende
  (>100000 in 3 s) -- zunaechst faelschlich als "offener/unterminierter Bus, PHY interpretiert
  Rauschen als Symbole" gedeutet. Tatsaechliche Ursache: plca_node auf allen drei Boards
  abgefragt zeigte Bridge und Follower auf COM23 beide mit persistenter plca_id 0
  (Coordinator) -- zwei Coordinatoren am selben Bus, genau das Bild, das das Datenblatt fuer
  UNEXPB beschreibt (PHY geht fuer zwei PLCA-Zyklen in Recovery, sendet waehrenddessen
  nichts). Loesung: Bridge per setenv plca_id 1 + saveenv auf eine freie ID umgestellt
  (Follower A = 7, Follower B/COM23 blieb Coordinator ID 0) -- danach plca_stat sauber: in
  range, kein UNEXPB mehr. Sackgasse: aus rohen Zaehlerstaenden allein nicht auf
  "Rauschen/offener Bus" schliessen -- erst die Cross-Check-Abfrage von plca_node auf jedem
  Knoten zeigt eine ID-Kollision zuverlaessig.

  Nachtrag 2026-08-29 (spaeter am selben Tag) -- PRSSTS.MAXID ist kein Bus-Zensus, sondern
  ein Konfigurations-Echo. Der Wert stand in JEDEM Testlauf exakt auf 8 (== NODE_CNT dieses
  Knotens) -- egal ob die Bridge allein am Bus haengte, ob der Doppel-Coordinator-Konflikt
  aktiv war, oder ob der Bus sauber mit drei Knoten (IDs 0/1/3, hoechste tatsaechlich aktive
  ID also 3) lief. Waere MAXID eine live beobachtete hoechste Node-ID, haette sich der Wert
  zwischen diesen sehr unterschiedlichen Bus-Zustaenden aendern muessen -- tat es aber nicht.
  Das ATSAME54-Datenblatt-Tool (mcp__mchp-docs__search_datasheet) fuehrt den LAN8651 nicht in
  seinem Geraete-Katalog, ein Volltext-Zitat aus DS60001734F war damit nicht moeglich; die
  Deutung "Konfigurations-Echo von PLCA_CTRL1.NODE_CNT, nicht Live-Erkennung" stuetzt sich
  daher auf die Konsistenz der eigenen Messreihen, nicht auf den Datenblatttext. Korrigiert:
  lan865x_diag.c/.h melden das Feld jetzt als "configured segment size (PRSSTS.MAXID, mirrors
  NODE_CNT - not a bus census)" statt "highest active node ID seen". TOCNT/BCNCNT blieben
  bei diesem Nachtrag unangetastet -- deren 1:1-Verhaeltnis in einem sauberen, leisen Report
  (kein anderer Knoten sendet) ist mit "ein Beacon pro leerem Zyklus, ein TO pro leerem
  Zyklus" plausibel erklaerbar und wurde nicht als offene Frage weiterverfolgt.

  Zweiter Nachtrag 2026-08-29 (noch spaeter am selben Tag) -- auch "Konfigurations-Echo" war
  falsch, direkt durch den naechsten Testschritt widerlegt. Beide Follower per setenv
  plca_cnt 12 + saveenv auf NODE_CNT=12 umgestellt, die Bridge dabei bewusst NICHT
  angefasst (blieb lokal NODE_CNT=8, von plca_node bestaetigt). plca_stat auf der Bridge
  zeigte danach PRSSTS.MAXID=12 -- den Wert der ANDEREN Knoten, nicht den eigenen. Damit ist
  klar widerlegt, dass das Feld die eigene NODE_CNT spiegelt; es folgt stattdessen der auf
  dem Bus tatsaechlich aktiven Segmentgroesse, vermutlich vom Coordinator (COM23) per BEACON
  verteilt. Der fruehere Befund (Wert blieb konstant bei 8 ueber sehr unterschiedliche
  Bus-Zustaende) war kein Widerspruch dazu, sondern schlicht Zufall: bis zu diesem Schritt
  hatten alle drei Boards dieselbe NODE_CNT=8, der Unterschied zwischen "eigene Konfiguration"
  und "live vom Bus" war also nicht beobachtbar. Merksatz: bei einem Statusregister, dessen
  Wert zufaellig mit einem Konfigurationsregister uebereinstimmt, erst einen bewussten
  Mismatch zwischen beiden herbeifuehren, bevor man "spiegelt die eigene Konfiguration"
  behauptet -- sonst bleibt Korrelation von Kausalitaet ununterscheidbar. Korrigiert:
  lan865x_diag.c/.h melden das Feld jetzt als "active segment size (PRSSTS.MAXID, from the
  bus - may differ from our own NODE_CNT)". Weiterhin ungeklaert und ohne Datenblatt-Volltext
  nicht abschliessend zu klaeren: ob der Wert wirklich vom aktuellen Coordinator kommt oder
  von einer anderen Quelle (z. B. dem Knoten mit der zuletzt geaenderten Konfiguration).
- 2026-08-29 -- Sackgasse: die PLCA-Register des LAN8651 geben KEINE Liste tatsaechlich
  vorhandener Node-IDs her, auch nicht mit dem neuen plca_stat. Frage war, ob sich aus den
  PLCA-Registern ablesen laesst, wieviele Knoten am Bus haengen und welche IDs sie haben.
  Durchgesehen: PRSSTS.MAXID ist nur die konfigurierte Segmentgroesse (aktuell vom Bus
  uebernommen, siehe Eintrag oben), nicht wieviele der Slots real belegt sind. STS1.EMPCYC
  ist ein einzelnes Sticky-Bit "mindestens ein leerer Zyklus kam vor", kein Zaehler und kein
  Bitmuster pro Slot -- daraus laesst sich weder eine Anzahl noch welche IDs betroffen waren
  ablesen. STS3.ERRTOID nennt nur die ID des LETZTEN fehlerhaften Slots, keine laufende Liste.
  TOCNT/BCNCNT sind Summenzaehler ueber den ganzen Bus ohne Aufschluesselung nach ID. Die
  "PLCA Multiple ID 0..3"-Register (11.5.12-15) sind kein Erkennungsmechanismus fuer fremde
  Knoten, sondern erlauben diesem einen PHY, zusaetzliche eigene IDs zu beanspruchen (mehrere
  logische Knoten auf einem Chip). Der Reconciliation-Sublayer sieht pro Zyklus offenbar nur
  "meine eigene Transmit Opportunity kam/kam nicht" -- keine Slot-fuer-Slot-Belegungstabelle.
  Nicht nochmal auf Registerebene nach einer Node-Enumeration suchen. Fuer eine echte
  Knotenzahl/-liste bleiben nur Mittel oberhalb von PLCA: IP-Discovery (ARP-Scan/Ping-Sweep
  auf eth0, setzt eine antwortende IP je Knoten voraus) oder manuell je Board ueber dessen
  eigene Konsole (plca_node/showenv), wie beim Doppel-Coordinator-Fund oben -- Letzteres nur
  moeglich, weil wir UART-Zugriff auf jedes Testboard haben, kein Bus-Mechanismus.

  Zusammenfassung des ganzen 2026-08-29-PLCA-Tagesdurchlaufs (plca_stat, Kollisionserkennung
  und die Node-Enumeration-Sackgasse oben), was ein einzelner Knoten ueber PLCA-Register also
  tatsaechlich herausfinden kann: Im **Sniffer-Modus** (passiv, T1SPMACTL.TXD aus, kein
  eigenes Senden) laesst sich PRSSTS.MAXID trotzdem lesen -- die Segmentgroesse kommt
  offenbar per BEACON vom Coordinator herein, auch rein passiv mitgehoert. Im **aktiven**
  Modus mit eigener ID wird ein Konflikt auf der EIGENEN ID sichtbar (UNEXPB bei Duplikat auf
  der Coordinator-ID 0, RXINTO bei Duplikat auf einer reguellaren ID) -- aber das sagt nur
  "meine ID ist belegt", nichts ueber andere IDs. Eine freie ID finden geht nur INDIREKT durch
  Durchprobieren (auf eine Kandidaten-ID setzen, auf UNEXPB/RXINTO pruefen) -- und das ist
  nicht nebenwirkungsfrei: eine besetzte ID zu belegen loest beim Coordinator-Fall laut
  Errata zwei PLCA-Zyklen Recovery aus (kein Verkehr fuer alle), im regulaeren Fall
  kollidierende Frames. Ein "leises" Scannen aller IDs gibt es auf dieser Registerebene nicht.
- 2026-08-29 -- SQI-Register (Signal Quality Indication, MMS 0x0A-Bank: SQICTL 0x000400A0,
  SQICFG0.TOID 0x000400AA, SQISTS0 0x000400A1) reagieren nur auf ECHTEN Sendeverkehr in dem
  beobachteten Slot, nicht auf reine Knoten-Praesenz oder PLCA-Haushalt (BEACON/Leerzyklen).
  Test: SQICFG0.TOID auf eine unbelegte ID (7) gesetzt, SQIEN+SQIRST gepulst -- SQISTS0 blieb
  ueber mehrere Sekunden konstant 0x0000 (kein SQIERR, kein SQIVLD, kein SQIVAL -- schlicht
  keine Messung, kein Fehler). Dieselbe Prozedur fuer ID 3 (zu dem Zeitpunkt real belegt: die
  Bridge selbst UND Follower A, siehe Doppel-Node-Test oben) UND fuer ID 0 (Follower B/
  Coordinator, real, einzeln, nicht die eigene ID) ergab ebenfalls 0x0000 -- solange der Bus
  im Leerlauf war ("empty PLCA cycle" in jedem plca_stat-Aufruf parallel bestaetigt). Erst
  nachdem gezielt echter Verkehr erzeugt wurde (iperf-Client auf dem beobachteten Knoten,
  Follower B/ID 0 sendet 5 s TCP an Follower A), aenderte sich SQISTS0 auf 0x00009F00 --
  deutlich von Null verschieden, klar mit dem echten Sendeverkehr korreliert.
  Wichtiger Modell-Fund dabei: die im Registermodell (lan8651_model.json) dokumentierten Felder
  SQIERR (Bit 7), SQIVLD (Bit 6), SQIVAL[2:0] (Bit 5:3), SQIERRC[2:0] (Bit 2:0) liegen alle im
  UNTEREN Byte -- das war in diesem Ergebnis 0x00. Die eigentliche Aktivitaet steckte im
  OBEREN, im Modell nicht dokumentierten Byte (0x9F = 1001 1111). Das Modell deckt dieses
  Register also nicht vollstaendig ab; ohne Volltext-Zugriff auf DS60001734F (LAN8651 ist im
  MCP-Datenblatt-Tool mcp__mchp-docs__search_datasheet nicht gelistet, gleiche Einschraenkung
  wie beim PRSSTS.MAXID-Fund oben) liess sich das nicht verifizieren. Unbestaetigte Arbeits-
  hypothese, NICHT als Tatsache verwendet: falls dieselben vier Felder einfach eine Byte-
  Position hoeher liegen (Bit 15=SQIERR, 14=SQIVLD, 13:11=SQIVAL, 10:8=SQIERRC), ergaebe 0x9F
  -> SQIERR=1, SQIVLD=0, SQIVAL=3, SQIERRC=7 (ein gemeldeter Fehler statt eines sauberen
  Werts) -- reine Spekulation, nicht verifiziert. Vor einer produktiven Nutzung von SQI zur
  Bus-Diagnose: (a) SQIVLD explizit mitpruefen, sonst wird ein Nicht-Ergebnis (Knoten still)
  mit einem echten Messwert verwechselt, siehe Fund oben; (b) die Bit-Position der Felder in
  SQISTS0 im Datenblatt-Volltext nachschlagen statt dem Modell-Extrakt zu vertrauen, da genau
  hier eine Luecke nachgewiesen ist.

  Nachtrag 2026-08-29 (noch spaeter am selben Tag) -- Datenblatt-PDF liegt auf diesem Rechner
  lokal vor (`C:\work\ptp\check4\_pdf\LAN8650-1-Data-Sheet-60001734.pdf`, DS60001734F; das
  Errata-PDF `LAN8650-1-Errata-80001075.pdf` liegt daneben), das MCP-Tool
  mcp__mchp-docs__search_datasheet deckt den LAN8651 zwar nicht ab, das lokale PDF aber schon
  -- direkt per Read-Tool mit `pages` einlesbar. Volltext-Abgleich Abschnitt 11.5.52-11.5.55
  (Seiten 274-278) zeigt: **die Byte-Shift-Vermutung oben war falsch.** Das Registermodell
  (lan8651_model.json) hatte die Bit-Positionen fuer SQISTS0 korrekt: SQIERR(7)/SQIVLD(6)/
  SQIVAL[2:0](5:3)/SQIERRC[2:0](2:0). Bits 15:8 sind im Datenblatt explizit als reserviert
  dokumentiert (Access RO, Reset 0 fuer alle acht, keine Namen).

  Trotzdem zeigte sich real, wiederholbar Inhalt genau dort: zwei aufeinanderfolgende Reads
  direkt nach echtem Sendeverkehr ergaben 0x9F00 dann 0x1F00 -- Bits 12:8 blieben bei beiden
  Reads identisch 0x1F, was exakt dem Reset-Default von SQICFG2.SQIINTTHR[4:0] (0x000400AC,
  ein voellig anderes Register) entspricht. Ein zweiter Reproduktionsversuch ruehrte
  SQICFG0/TOID (0x000400AA) diesmal gar nicht an und zeigte trotzdem wieder dasselbe 0x1F-
  Muster -- das schwaecht die naheliegende "Reste von einem kurz zuvor angefassten
  Nachbarregister"-Erklaerung eher ab, als sie zu stuetzen.

  Einordnung nach Ruecksprache mit dem Nutzer: An diesem Tag liefen sehr viele andere
  Register-Zugriffe ueber dieselbe Kette (lan_read/lan_write/lan_rmw, DRV_LAN865X_*) ueber
  drei verschiedene MMS-Baenke (0, 1, 3, 4) durchgehend konsistent und mehrfach gegen die
  physische Realitaet verifiziert (testmode-Verify-Rundlauf, UNEXPB/RXINTO korrelierten exakt
  mit absichtlich erzeugten ID-Kollisionen, TOCNT/BCNCNT skalierten plausibel). Ein
  systemischer Fehler im Zugriffspfad ist damit unwahrscheinlich -- ein lokaler Effekt,
  begrenzt auf dieses eine Register bzw. den SQI-Registercluster, bleibt dagegen offen.
  Plausibelste Arbeitshypothese, NICHT verifiziert: SQISTS0 Bits 15:8 spiegeln undokumentiert
  den aktuell konfigurierten SQICFG2-Schwellwert neben dem Status zurueck (Debug-Komfort ohne
  Dokumentation) -- also eine Datenblatt-Luecke, kein Treiberfehler. Nicht mit Registerpoken
  allein abschliessend zu klaeren; ein echter SPI-Bus-Mitschnitt waere der naechste Schritt,
  falls das je operativ relevant wird. Fuer die praktische Nutzung von SQI reicht es, sich
  auf die dokumentierten Bits 7:0 zu verlassen und Bits 15:8 zu ignorieren.

  Nachtrag 2026-08-29 (dritter Durchgang, nach Nutzer-Vorschlag "nur Bitfelder betrachten,
  die im Datenblatt mit Funktion beschrieben sind") -- Volltext-Abgleich der zentralen
  plca_stat-Register (PLCA_STS 11.5.60 S.283, STS1 11.5.2 S.218-220, STS2 11.5.3 S.221,
  STS3 11.5.4 S.222, PRSSTS 11.5.16 S.236) deckte zwei echte Fehler in den eigenen
  Code-Kommentaren auf, nicht im Registermodell:
  1. **STS1.TXCOL war falsch beschrieben.** Datenblatt woertlich: "Physical collision on the
     network was detected. This does not include logical collisions due to normal operation
     of PLCA." Der Code-Kommentar hatte behauptet, ein Doppel-Coordinator/Doppel-Knoten-
     Konflikt wuerde TXCOL setzen -- das Gegenteil ist der Fall, und passt sogar zu den
     eigenen Messdaten von vorhin: TXCOL feuerte in KEINEM der beiden ID-Konflikt-Tests,
     nur RXINTO bzw. UNEXPB. TXCOL meldet ausschliesslich echte physikalische Kollisionen
     ausserhalb der PLCA-Arbitrierung, keine ID-Adresskonflikte.
  2. **STS1.UNCRS war zu generisch beschrieben** ("unerwartete Traegererkennung"). Laut
     Datenblatt ACMA-Modus-spezifisch ("carrier sense during this PHY's transmit slot when
     ACMA is asserted") -- auf diesem Projekt (kein ACMA) praktisch nie relevant.
  Bestaetigt (keine Korrektur noetig): STS1.RXINTO ("could indicate multiple nodes being
  assigned the same Local ID" -- exakt unser Fund), STS1.UNEXPB (exakt unser Fund),
  PRSSTS.MAXID (siehe oben, jetzt mit Wortlaut belegt statt nur Verhaltensvermutung).
  Neu entdeckt, vorher nicht bekannt: STS3.ERRTOID ist laut Datenblatt "only accurate if one
  unmasked interrupt status bit is set in the Status 1 register. If multiple interrupt
  status bits are set, then this field represents the transmit opportunity for only the
  most recent" -- plca_stat zeigt aber routinemaessig mehrere STS1-Events gleichzeitig, in
  genau diesen Faellen ist ERRTOID also nicht verlaesslich. Alle vier Punkte in
  lan865x_diag.c/.h korrigiert bzw. mit Datenblatt-Zitat belegt. Merksatz: Ein Registermodell,
  das nur Bitnamen und -positionen extrahiert (ohne die Fliesstext-Funktionsbeschreibung),
  verleitet dazu, aus dem Namen allein eine plausibel klingende, aber falsche Bedeutung zu
  erraten (TXCOL "klingt" nach jeder Art Kollision) -- vor einer Interpretation eines Bits
  in einem neuen Diagnose-Feature den Fliesstext im echten Datenblatt lesen, nicht nur den
  Bitnamen im Modell-Extrakt.

  Abgehakt 2026-08-29, nicht weiter verfolgt: SQI (SQICTL/SQICFG0/SQICFG2/SQISTS0, siehe
  Eintraege oben) bleibt ein offener, ungeklaerter Befund -- SQIVLD wurde in keinem Testlauf
  gültig (auch nicht mit mehrsekuendigem echtem TCP-Verkehr vom beobachteten Knoten), die
  dokumentierten Bits blieben immer 0x00, nur die ungeklaerten Bits 15:8 aenderten sich. Kein
  produktiver plca_stat-Code fuer SQI geschrieben. Bei Bedarf hier wieder aufsetzen, statt neu
  zu ermitteln -- nicht ohne neuen Anlass (z. B. echten SPI-Mitschnitt) weiterbohren.

  Korrektur, spaeter am selben Tag: **"abgehakt" war falsch -- SQI funktioniert, wir hatten nur
  das falsche Vorgehen.** Beim Portieren von plca_stat in das follower\-Projekt stellte sich
  heraus, dass follower/firmware/src/lan865x_diag.c bereits eine eigene, laengst funktionierende
  SQI-Implementierung enthielt (LAN865X_DIAG_SqiStart/Stop/Active, Befehl `sqi`), die wir bei der
  urspruenglichen Untersuchung nicht kannten. Zwei Unterschiede zu unserem gescheiterten
  Ein-Schuss-Versuch:
  1. **Kontinuierliches Polling (~1x/s), nicht ein einzelner Lesevorgang** -- SQIVLD ist
     read-clear und rearmt sich selbst, die Statistik-Akkumulationsdauer haengt vom Verkehr ab.
  2. **Automatische Fehler-Erholung**: bei gesetztem SQIERR zyklisch SQIEN aus- und wieder
     einschalten (SQI_RECOVER_CLEAR -> SQI_RECOVER_SET), sonst blieb die Messung fuer immer
     haengen, ohne dass das nach aussen sichtbar war.
  Trotzdem lieferte `sqi all` auf dem Follower zunaechst weiterhin keine validen Samples, obwohl
  Follower A <-> Follower B UDP-Verkehr lief -- bis klar wurde: **SQI akkumuliert nur aus
  Verkehr, den der beobachtende Knoten nicht selbst erzeugt hat.** Eigener Sendeverkehr (A -> B,
  beobachtet auf A) lieferte nie ein Sample; Fremdverkehr auf demselben Bus (Bridge -> B,
  beobachtet auf A, das an diesem Austausch gar nicht beteiligt ist) lieferte sofort gueltige
  Werte (`7 best`, SNR >= ~18dB). Spaeter bestaetigt: auch selbst EMPFANGENER (nicht selbst
  gesendeter) Verkehr zaehlt genauso (Bridge -> Follower A, beobachtet auf A selbst).
  `sqi`/`LAN865X_DIAG_SqiStart/Stop/Active` wurde daraufhin identisch in beide anderen Kopien des
  Moduls uebernommen (firmware/src/lan865x_diag.c der Bridge bekam SQI, follower bekam
  plca_stat) -- alle drei Knoten haben jetzt denselben vollen Funktionsumfang. Dabei einen
  echten Bug gefunden und in beiden Kopien behoben: lan_abort() setzte s_plcastat_active bei
  einem Timeout nie zurueck.

  Anschluss-Test, selber Tag: Kann man mit einem der IEEE-Testmodi (testmode 1..4) gezielt eine
  *abgestufte* Signalverschlechterung erzeugen, um SQI mit einem schlechteren als dem besten
  Wert zu sehen? Drei Versuche, alle mit demselben Ergebnis in unterschiedlicher Deutlichkeit:
  Testmodi sind ein Alles-oder-Nichts-Stoerer, kein feiner Regler.
  1. Coordinator (Follower B) in testmode 1 -> gesamter Bus sofort `OUT OF RANGE`
     (plca_stat auf der Bridge), kein SQI-Sample moeglich. Naheliegend: ohne Coordinator kein
     BEACON, keine Synchronisation -- aber:
  2. Ein NICHT-Coordinator (Follower A) in testmode 1 -> genauso fatal: die Bridge konnte
     Follower B's MAC-Adresse nicht mal per ARP aufloesen. Das kontinuierliche, unarbitrierte
     Testsignal blockiert den ganzen Bus fuer alle, unabhaengig von der Coordinator-Rolle --
     vermutlich weil jeder andere Knoten per Carrier-Sense staendig "Bus belegt" sieht und nie
     eine saubere Transmit-Opportunity-Grenze erkennen kann.
  3. Testmodus nur kurz (3 s) MITTEN in einem bereits laufenden, 15+ s langen Bridge->Follower-B-
     UDP-Strom aktiviert (statt von Anfang an): Der iperf-Client meldete durchgehend 0 % Verlust
     (Sender-Eigenmeldung, nicht unbedingt aussagekraeftig fuer UDP ohne Rueckkanal). Der
     SQI-Report auf dem Empfaenger (Follower B) zeigte aber nur **7 Samples statt der bei ~1/s
     erwarteten ~14-15** -- und alle sieben vorhandenen Samples blieben `7 best` (min=max=7),
     kein einziger schlechterer Wert. Deutung: SQI verhaelt sich unter dieser Art Stoerung eher
     binaer als abgestuft -- entweder die Messung in diesem Poll-Zyklus gelingt sauber (dann
     praktisch immer der beste Wert), oder sie schlaegt ganz fehl (kein Sample), aber ein
     "mittelmaessiger" Wert kam nie vor.
  Fazit: Die eingebauten Testmodi sind fuer Oszilloskop-Messungen am ruhenden Bus gedacht
  (siehe Abschnitt 4 der CLAUDE.md), nicht fuer eine kontrollierte, graduelle Stoersimulation im
  laufenden Betrieb. Fuer eine echte Zwischenstufe brauchte es vermutlich physikalische Mittel
  (Fehlanpassung, externe Stoerquelle, Daempfungsglied), nicht Register-Pokes.

  Methodik-Merksatz aus dem ganzen 2026-08-29-PLCA-Durchlauf: Der eigentliche Hebel war die
  Kombination aus **drei physisch unabhaengigen Knoten mit je eigenem Konsolenzugang**
  (Bridge COM8, Follower A COM10, Follower B COM23), **Register-CLI** (lan_read/lan_rmw/
  plca_stat) und **lokalem Datenblatt-Volltext** (siehe lan8651-datenblatt-lokal.md im
  projektuebergreifenden Memory). Zwei falsche Theorien an diesem Tag (SQISTS0 Byte-Shift,
  PRSSTS.MAXID als Echo der eigenen NODE_CNT) wurden nicht durch Nachdenken widerlegt,
  sondern durch gezielt herbeigefuehrte, physisch reale Bus-Zustaende (Doppel-Coordinator,
  Doppel-Knoten, NODE_CNT-Mismatch, Verkehr an/aus). Bei kuenftigen unklaren Registern:
  zuerst eine Hypothese formulieren, die sich mit einem am Bench herstellbaren Zustand
  unterscheidbar pruefen laesst -- nicht aus dem Bitnamen allein eine Bedeutung erraten.

  Nachtrag 2026-08-29 (Verifikation der restlichen plca_stat-Register gegen den Volltext,
  Seiten 198/227-231/280-282): CTRCTRL und STATS10.XCOL bestaetigt wie im Code angenommen.
  Zwei neue, wichtige Funde:
  1. **TOCNTH/TOCNTL und BCNCNTH/BCNCNTL muessen in dieser Reihenfolge gelesen werden** (High
     vor Low). Datenblatt woertlich (TOCNTH): "the contents of the 32-bit... counter will be
     latched into the high and low counter register pair... The 32-bit counter will be reset
     when the contents are latched." Und (TOCNTL): "The contents of this register will be
     latched upon reading of the [High] register." Liest man Low zuerst oder in falscher
     Reihenfolge, bekommt man Haelften aus unterschiedlichen Latch-Ereignissen. Unser
     s_plcastat_addr in lan865x_diag.c hatte zufaellig schon die richtige Reihenfolge (High
     vor Low fuer beide Paare) -- ohne diese Regel gekannt zu haben. Jetzt als Kommentar in
     lan865x_diag.h festgehalten, damit eine kuenftige Umsortierung das nicht stillschweigend
     bricht.
  2. **PLCA_CTRL1.NCNT wird nur auf dem Coordinator (ID=0) ausgewertet.** Datenblatt woertlich:
     "This field must be configured correctly on the node with ID=0 (Controller). Nodes
     configured with ID other than zero (Followers) ignore this field." Das erklaert sauber,
     warum PRSSTS.MAXID immer den Coordinator-Wert zeigt -- es ist der einzige, der ueberhaupt
     zaehlt. Praktische Konsequenz: unser `setenv plca_cnt 12` auf Follower A (COM10, ID 3)
     weiter oben war auf echter Hardware wirkungslos, nur die Aenderung auf dem Coordinator
     (COM23, ID 0) hat tatsaechlich etwas bewirkt. `setenv`/`env.c` warnt davon bisher nicht --
     ggf. Kandidat fuer eine spaetere CLI-Hinweismeldung ("plca_cnt wird nur auf dem
     Coordinator ausgewertet"), aber noch nicht umgesetzt.

  Bonus-Fund, ebenfalls nicht umgesetzt: PLCA_CTRL0.EN empfiehlt explizit, bei aktivem PLCA
  die physische Kollisionserkennung (CDCTL0.CDEN) zu deaktivieren ("recommended to disable
  physical layer collision detection to achieve a higher level of noise tolerance"). CDEN
  steht auf allen Boards noch auf Werksdefault (1 = aktiv).
- 2026-08-29 -- PLCA Burst Mode (PLCA_BURST, 0x0004CA05, MAXBC[15:8]/BTMR[7:0]) live verifiziert,
  aber erst der zweite Testaufbau war eindeutig. Hintergrund: aus der Audio-Bandbreitenrechnung
  vom selben Tag stellte sich die Frage, ob Burst Mode (mehrere Frames pro eigener Transmit
  Opportunity, statt nur einem) tatsaechlich etwas bringt.
  **Erster Versuch, nicht eindeutig:** Follower A (COM10) mit MAXBC=4 konfiguriert, iperf
  Follower A -> Follower B ohne weitere Buslast. TCP: ~5,76 vs. ~5,80 Mbit/s (kein Unterschied
  -- TCP wartet nach jedem Paket auf ACK, es baut sich nie ein Rueckstau auf, den Burst Mode
  abbauen koennte). UDP-Flood (10 Mbit/s Ziel) ohne/mit Burst: 9.338 vs. 9.502 Kbps, nur +1,76 %
  -- zu klein, um sicher von Messrauschen zu unterscheiden (nur je ein Lauf), weil beide schon
  nahe an der 10-Mbit/s-Leitungsgrenze lagen (nur 2 echte Sender auf 12 Slots, kaum Konkurrenz
  um die eigene Transmit Opportunity).
  **Sackgassen auf dem Weg zu einem dritten, echt konkurrierenden Sender:** `noip_send <n> <gap>
  <size>` mit n>1 scheidet aus (schon dokumentierte Sackgasse oben, geteilter Puffer). Ein
  dritter gleichzeitiger `iperf`-Fluss von der Bridge scheiterte zunaechst an
  `TCPIP_IPERF_MAX_INSTANCES=1` (`configuration.h:313`) -- jeder Knoten kann nur eine iperf-
  Rolle gleichzeitig halten, Follower A und B waren beide schon durch den eigentlichen Testlauf
  belegt.
  **Loesung:** UDP braucht keine Gegenstelle, die eine Verbindung aktiv annimmt (kein Handshake
  wie TCP). Die Bridge kann also `iperf -c 192.168.0.202 -u -b 10000000 -t 30` Richtung
  Follower B schicken, OHNE dass dort ein iperf-Server dafuer laeuft oder eine zweite Instanz
  belegt wird -- die Pakete landen einfach unbeachtet an, solange ARP das Ziel-MAC aufloesen
  kann. Damit nutzt jeder der drei Knoten nur seine eine eigene Instanz: Bridge = Client
  (ignorierte Flut), Follower A = Client (gemessen), Follower B = Server (bedient A, verwirft
  die Bridge-Pakete stillschweigend).
  **Zweiter Testaufbau, eindeutig:** Bridge flutet Follower B im Hintergrund (`-t 30`, 4 s
  Vorlauf vor dem eigentlichen Testlauf, um Ueberlappung sicherzustellen -- ein erster Versuch
  mit nur 1 s Vorlauf und `-t 14` lief der Bridge-Flut zeitlich davon und ergab einen falsch
  gemischten Messwert). Follower A -> Follower B UDP, 10 s, unter dieser Konkurrenz:
  - Burst aus (MAXBC=0): **4.698 Kbps**, ueber die volle Messdauer absolut konstant.
  - Burst an (MAXBC=4): **7.913 Kbps**, ebenso konstant.
  - **+68 % Durchsatzgewinn.** Zweimal komplett wiederholt (Server/Flood neu gestartet, Burst
    aus/an neu gesetzt) -- beide Male exakt dieselben Werte bis auf die Kbps-Stelle. Kein
    Rauschen, ein robuster, deterministischer Effekt.
  Merksatz: Burst Mode bringt nur etwas, wenn am eigenen Knoten tatsaechlich ein Rueckstau
  entsteht (die eigene Transmit-Opportunity-Wiederkehr also langsamer ist, als neue Daten
  ankommen) -- ohne echte Buskonkurrenz (oder TCP-Flusskontrolle, die von sich aus keinen
  Rueckstau zulaesst) gibt es nichts zu "bursten", und die Messung bleibt im Rauschen. Fuer
  einen eindeutigen Test aktiv Konkurrenz erzeugen, nicht nur Ziel-Bandbreite hochsetzen.

  Anschluss-Messreihe, selber Tag: NCNT-Erhoehung allein (ohne Burst, ohne Bridge-Flut) senkt
  den erreichbaren Durchsatz zwischen Follower A und Follower B messbar und reproduzierbar,
  weil mehr leere Slots pro Zyklus die eigene Wiederkehr verlangsamen -- derselbe Mechanismus,
  den Burst Mode kompensieren kann, hier aber isoliert ohne Burst betrachtet (UDP, 10 Mbit/s
  Ziel, jeweils 10 s, NCNT auf dem Coordinator/Follower B ueber setenv plca_cnt gesetzt,
  danach wieder auf 12 zurueckgesetzt):

  | NCNT | Durchsatz | 1/Durchsatz |
  |---|---|---|
  | 12  | 9.338 Kbps | 1,0709e-4 |
  | 100 | 7.572 Kbps | 1,3207e-4 |
  | 150 | 6.864 Kbps | 1,4569e-4 |
  | 200 | 6.277 Kbps | 1,5931e-4 |

  Der Rueckgang ist NICHT linear in NCNT, sondern linear im KEHRWERT des Durchsatzes -- die
  Steigung von 1/Durchsatz pro zusaetzlichem NCNT-Schritt ist ueber alle drei Intervalle fast
  identisch (2,84e-7 fuer 12->100, 2,72e-7 fuer 100->150, 2,72e-7 fuer 150->200, die letzten
  beiden praktisch deckungsgleich). Das passt exakt zum physikalischen Modell: die Zykluszeit
  waechst linear mit NCNT (jeder leere Slot kostet einen festen TOTMR-artigen Betrag), aber
  Durchsatz ist umgekehrt proportional zur Zykluszeit (Durchsatz ~ konstant / (a + b*NCNT)) --
  eine reziproke, keine lineare Kurve. Merksatz: bei einer vermuteten "mehr X kostet linear
  mehr Zeit"-Ursache eher die Kehrwerte der gemessenen Rate pruefen, nicht die Rate selbst auf
  Linearitaet -- Rate und zugrundeliegende Zeit verhalten sich genau umgekehrt zueinander.

  Durch Burst-Mode-Test + NCNT-Messreihe verhaltensmaessig verifizierte Register (zusaetzlich
  zur reinen Text-Verifikation weiter oben): **PLCA_BURST.MAXBC** (reproduzierbarer +68%-
  Durchsatzgewinn unter echtem Rueckstau, exakt in die vom Datenblatt beschriebene Richtung),
  **PLCA_CTRL1.NCNT** (Coordinator-only-Wirkung mehrfach ueber vier verschiedene Werte
  reproduziert, plus der reziproke statt lineare Zusammenhang zur Zykluszeit als neue,
  quantitative Erkenntnis), und **PRSSTS.MAXID** (zeigte bei allen vier NCNT-Werten exakt den
  gesetzten Wert, nicht nur im einzelnen Mismatch-Fall von vorhin). Nur als Messwerkzeug
  mitgelaufen, dabei aber nicht neu geprueft: PLCA_STS.PST sowie die restlichen plca_stat-
  Register (STS1, TOCNT/BCNCNT) -- die liefen im Hintergrund korrekt, ohne dass dieser Test
  speziell auf sie abzielte.
- 2026-08-29 -- Wall Clock (TSU, MMS 1: MAC_TSH 0x00010070, MAC_TSL 0x00010074, MAC_TN
  0x00010075, MAC_TI 0x00010077) live verifiziert -- Datenblatt-Abschnitt "Wall Clock" (Seite
  ~74/375 im PDF) beschreibt einen 94-Bit-Timer, der bei jedem Tick der 25-MHz-Referenzuhr um
  den in MAC_TI konfigurierten Wert erhoeht wird (Standard: 0x28 = 40 ns/Tick, 25 MHz x 40 ns =
  exakt 1 s -- ein sofort nachrechenbarer Cross-Check). MAC_TI stand beim ersten Nachsehen
  bereits auf 0x28, obwohl diese Firmware kein PTP implementiert -- vermutlich vom Harmony-
  Treiber beim Boot gesetzt, nicht von eigenem Code.
  **Erster Messversuch, methodisch unbrauchbar:** MAC_TSH/TSL/TN auf 0 gesetzt (3 einzelne
  CLI-Aufrufe in einem Bash-Call), 10 s gewartet (separater Bash-Call), zurueckgelesen (3
  weitere einzelne Aufrufe in einem dritten Bash-Call) -- Ergebnis 48,7 s statt 10 s erwarteter
  Zeit. Ursache: die unkontrollierte Luecke ZWISCHEN den drei separaten Bash-Tool-Aufrufen
  (inklusive der Zeit, die das Modell selbst zwischen den Aufrufen braucht) ist genauso Teil
  der gemessenen Spanne wie das eigentliche `sleep 10` -- eine Zeitmessung ueber mehrere
  getrennte Tool-Aufrufe hinweg ist damit grundsaetzlich unzuverlaessig.
  **Zweiter Versuch, besser:** Zwei Zeitstempel (vor/nach `sleep 10`) innerhalb EINES
  einzigen Bash-Aufrufs genommen, nur die Differenz betrachtet (unabhaengig vom Nullpunkt).
  Delta 14,36 s statt 10 s -- deutlich besser (die grosse Bash-Call-uebergreifende Luecke ist
  weg), aber die vier einzelnen `lan_read`-Aufrufe (TSL+TN je zweimal) innerhalb dieses einen
  Calls addieren immer noch ~4,4 s eigene Round-Trip-Zeit drauf.
  **Dritter Versuch, sauber:** 10 Messpunkte ueber ~20 s Zielintervall (real durch CLI-Overhead
  eher ~5,6 s pro Iteration), aber diesmal mit **Host-Zeitstempel** (`date +%s.%N` direkt vor
  jedem Register-Lesevorgang, innerhalb desselben Bash-Skripts) als Referenz statt eines
  angenommenen `sleep`-Werts -- macht die Messung unabhaengig von der tatsaechlichen
  CLI-Latenz, weil Host-Zeit und Register-Wert am selben Punkt erfasst werden.
  Ergebnis (Host-Delta vs. Wallclock-Delta, jeweils gegen Sample 1, ppm-Abweichung):
  Sample 2: +186.035 ppm, Sample 5: +44.847 ppm, Sample 8: +25.278 ppm -- drei klare
  Ausreisser. Alle anderen sechs Punkte (3,4,6,7,9,10): zwischen -351 und +1.358 ppm, klar
  im Rahmen normaler Quarztoleranz.
  **Erklaerung der Ausreisser, kein Fehler der Wall Clock selbst:** TSL und TN werden ueber
  ZWEI GETRENNTE lan_read-Aufrufe gelesen, nicht atomar. Faellt zwischen den beiden ein
  Sekundenuebertrag (TN laeuft ueber 1s in TSL's naechste Sekunde), entsteht ein
  inkonsistentes (TSL, TN)-Paar -- das erzeugt genau diese Art grosser Einzelausreisser,
  unabhaengig von der tatsaechlichen Taktgenauigkeit. Bei ~5,6 s Iterationsabstand (kein
  glatter Teiler von 1 s) trifft das pseudo-zufaellig einzelne Samples, hier 3 von 9
  Intervallen.
  **Fazit:** Wall Clock laeuft nachweislich mit der im Datenblatt beschriebenen Rate, praezise
  auf ±100-1.400 ppm zur Host-Uhr (normale Quarztoleranz, kein Register-/Konfigurationsfehler).
  Merksatz fuer kuenftige Zeitmessungen ueber die serielle Konsole: (a) niemals ueber mehrere
  getrennte Tool-/Bash-Aufrufe hinweg messen, die dazwischenliegende Luecke ist unkontrolliert
  und kann die Messung um ein Vielfaches verfaelschen (48,7s-Artefakt oben); (b) Host-
  Zeitstempel direkt am Messpunkt nehmen, nicht einen angenommenen Schlafwert als Referenz
  verwenden; (c) bei mehrteiligen Registern (High+Low, Sekunden+Nanosekunden), die nicht
  atomar ueber eine serielle Schnittstelle gelesen werden koennen, Ausreisser durch
  Sekunden-/Bit-Uebertrag einplanen und per Wiederholung/Ausreisser-Filterung herausrechnen,
  nicht einzelne Messpunkte fuer bare Muenze nehmen.
- 2026-08-29 -- Voller LAN8651-Chip-Reset gefunden und verifiziert: **OA_RESET.SWRESET**
  (0x00000003, Bit 0, MMS 0, self-clearing). Datenblatt woertlich (11.1.4, S. 124): "Writing a
  '1' to this bit will fully reset the device including the integrated PHY." Ein einziger
  lan_write reicht, kein vorheriges Abschalten von TX/RX noetig.
  **Test:** Einmal-Skript (nicht im Projekt getrackt, liegt im Scratchpad dieser Session)
  haelt die serielle Verbindung offen, loest den Reset aus, liest danach alle 183 Register aus
  lan8651_model.json einzeln per lan_read und vergleicht gegen die im Modell dokumentierten
  Bitfeld-Reset-Werte (nur Bits mit bekanntem Reset-Wert werden verglichen, unbekannte/Luecken-
  Bits werden ignoriert).
  **Ergebnis, zweimal wiederholt:** 147/183 bzw. 145/183 Register zeigen exakt den
  dokumentierten Silizium-Default -- der Reset findet also nachweislich wirklich statt. Die
  verbleibenden ~35 Register weichen aber ab, und zwar **reproduzierbar dieselben** in beiden
  Laeufen (nur STATS11/STATS12 und ein paar Zaehlerstaende unterschieden sich minimal
  zwischen den Durchgaengen).
  **Ursache der Abweichungen: der Harmony-LAN865X-Treiber heilt einen externen PHY-Reset
  selbststaendig, viel schneller als wir per Konsole nachlesen koennen.** Fund im Treiber-
  Quelltext (`drv_lan865x_api.c`, Funktion `_OnClearStatus0`, ~Zeile 2342): OA_STATUS0-Bit 4
  ("Loss of Framing Error") setzt bei Erkennung `reinit = true`, was `initState =
  DRV_LAN865X_INITSTATE_RESET` ausloest und die komplette Treiber-Initialisierung erneut
  anstoesst -- inklusive Neuschreiben von MAC-Adresse, PLCA-Konfiguration (aus dem
  persistierten Environment), Wall-Clock-Inkrement, Kollisionserkennung deaktivieren, TX/RX-
  Match-Konfiguration und Interrupt-Masken. Betroffene Register (reproduzierbar in beiden
  Laeufen): OA_CONFIG0, OA_IMASK0, MAC_NCR, MAC_NCFGR, MAC_SAB1/SAB2/SAT2 (eigene MAC-Adresse),
  MAC_TSL/TN/TI (Wall Clock laeuft wieder), STS1/STS3/PRSSTS/STATS6/STATS7 (plausible
  Nebeneffekte des laufenden Resync -- neue Events, ein paar empfangene Frames), TXMCTL/
  TXMMSKH/TXMMSKL/TXMLOC, RXMCTL/RXMMSKH/RXMMSKL/RXMLOC, SLPCTL0/SLPCTL1, CDCTL0 (CDEN wird
  vom Treiber explizit AUSgeschaltet -- genau die Datenblatt-Empfehlung "disable collision
  detection when PLCA is active" von weiter oben, hier real umgesetzt gefunden), PLCA_CTRL0/
  PLCA_CTRL1/PLCA_STS (PLCA laeuft nach dem Reset innerhalb von Sekundenbruchteilen wieder),
  MISC, ECCCTRL, ECCLKSL, ECCLKNS, ECRDTSn, SEVIM.
  **Warum sich "alle echten Reset-Werte" architektonisch nicht auslesen lassen:** Das TC6/OPEN-
  Alliance-SPI-Protokoll traegt Statusinformationen (inklusive Bit 4 "Loss of Framing Error")
  im Footer JEDER SPI-Transaktion, nicht ueber einen separaten Poll-Zyklus. Der erste
  `lan_read`-Befehl nach dem Reset IST bereits eine SPI-Transaktion und liefert damit selbst
  den Trigger fuer den Reinit, bevor sein Ergebnis überhaupt zurückkommt -- es gibt kein
  Zeitfenster, das man mit schnellerem Auslesen "gewinnen" koennte. Um wirklich an die reinen
  Silizium-Reset-Werte dieser ~35 Register zu kommen, muesste man die Footer-Auswertung im
  TC6-Treiber selbst patchen (Kernprotokoll-Logik einer Vendor-Bibliothek) -- fuer einen
  einmaligen Test nicht sinnvoll, daher nicht gemacht.
  **Nicht abschliessend geklaert, moeglicherweise falsche Reset-Werte im eigenen Modell:**
  TXMMSKH/TXMMSKL/RXMMSKH/RXMMSKL zeigen nach dem (Treiber-reinitialisierten) Reset durchgaengig
  alle Bits gesetzt (0xFF/0xFFFF), TXMLOC/RXMLOC durchgaengig 0 -- beides das genaue Gegenteil
  der im Modell dokumentierten Default-Werte. Das koennte Treiber-Konfiguration sein (z. B.
  "alle Typ-ID-Bits als don't-care maskieren" als sinnvolle Startkonfiguration), oder es koennte
  bedeuten, dass lan8651_model.json hier falsche Reset-Werte dokumentiert. Nicht gegen den
  Datenblatt-Volltext geprueft; falls diese Register spaeter fuer Diagnose relevant werden,
  vor einer Interpretation den Volltext (Abschnitt zu TXMMSKH/L, RXMMSKH/L, TXMLOC/RXMLOC)
  nachlesen statt dem Modell-Extrakt zu vertrauen.
  **Positive Erkenntnis aus dem Ganzen:** Die urspruengliche Warnung, ein externer PHY-Reset
  wuerde den Treiber dauerhaft verwaisen lassen und eth0 bis zu einem MCU-Reset kaputt machen,
  war zu pessimistisch -- dieser Treiber erkennt und heilt einen unerwarteten LAN8651-Reset im
  laufenden Betrieb selbststaendig und zuverlaessig, ohne dass ein `reset` der Bridge noetig
  waere.
  **Merksatz zur Modellpflege, direkt aus einem Nutzer-Einwand entstanden:** Von den 145
  "MATCH"-Registern hatten nur 37 einen nicht-trivialen (nicht durchgehend Null) Reset-Wert --
  die restlichen 108 sind reine Alles-Null-Register, bei denen eine Uebereinstimmung kaum
  Aussagekraft hat (jedes ungenutzte Register liest nach praktisch jedem Reset 0, unabhaengig
  davon, ob die im Modell dokumentierten Bitgrenzen ueberhaupt stimmen). Nur die 37 Register mit
  echtem, spezifischem Reset-Muster wurden im Modell als "hardware-confirmed" markiert; die 108
  trivialen blieben unveraendert. Erster Versuch, alle 145 pauschal als bestaetigt zu markieren,
  waere irrefuehrend gewesen -- vor einem Verifikations-Upgrade im Modell pruefen, ob der
  Vergleichswert ueberhaupt aussagekraeftig ist (nicht-trivial), nicht nur ob er uebereinstimmt.
  Zusaetzlich technischer Fallstrick beim Modell-Editieren selbst: ein erster Versuch, die
  verified-Felder per vollem `json.load`/`json.dump`-Roundtrip zu aktualisieren, hat die
  GESAMTE Datei umformatiert (906 statt der erwarteten ~57 geaenderten Zeilen), weil json.dump
  das urspruengliche Datei-Layout nicht exakt reproduziert -- Aenderung verworfen (`git
  checkout`) und stattdessen wie beim ersten Modell-Update per gezielter Text-/Regex-Ersetzung
  auf der Rohdatei gemacht (siehe fruehere Eintraege). Bei handgepflegten JSON-Dateien mit
  festem Layout: nie ueber vollen Parse+Dump aktualisieren, wenn nur einzelne Felder geaendert
  werden sollen.
