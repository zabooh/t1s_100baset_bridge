# PTP-Zeitsynchronisation — Umsetzungsplan

> Was zu tun ist, um auf dieser Bridge einen PTP-Grandmaster laufen zu lassen, der `Sync` +
> `Follow_Up` **einweg** auf das 10BASE-T1S-Segment sendet, und daneben ein eigenes
> Follower-Projekt, das seine Wallclock darauf zieht. Kein `Delay_Req`, kein `Delay_Resp`, kein
> Peer-Delay — auf keiner Stufe.
>
> **Wissensspeicher ist [LAN8651_TIME_SYNC.md](LAN8651_TIME_SYNC.md)** (englisch): Registermap,
> Adress-Kodierung, Servo-Zustände, Messzahlen, Fallstricke. Diese Datei wiederholt das nicht,
> sondern verweist darauf. Wer eine Registeradresse oder einen Bitwert braucht, liest dort nach;
> wer wissen will, welcher Schritt als nächstes ansteht, liest hier.
>
> Diese Einführung liegt bewusst auf dem Branch `ptp-time-sync`, getrennt von `main`.

---

## Inhalt

- [0. Vorentscheidung: einweg, und was daraus folgt](#0-vorentscheidung-einweg-und-was-daraus-folgt)
- [Phase 1 — Grandmaster auf der Bridge, ohne jeden Treiber-Patch](#phase-1--grandmaster-auf-der-bridge-ohne-jeden-treiber-patch)
- [Phase 2 — Follower als eigenes Projekt](#phase-2--follower-als-eigenes-projekt)
- [Phase 3 — Laufzeitkonstante: nur ein Schätzwert](#phase-3--laufzeitkonstante-nur-ein-schätzwert)
- [Phase 4 — Testwerkzeuge](#phase-4--testwerkzeuge)
- [Phase 5 — optional: Boundary Clock zur 100BASE-T-Seite](#phase-5--optional-boundary-clock-zur-100base-t-seite)
- [Warum diese Reihenfolge](#warum-diese-reihenfolge)
- [Risiken, die den Plan kippen können](#risiken-die-den-plan-kippen-können)

---

## 0. Vorentscheidung: einweg, und was daraus folgt

Die Bridge sendet, die Follower hören zu und senden **nichts**. Begründung, Kosten und die beiden
Dinge, die dadurch *nicht* schlechter werden (Frequenzlock, Follower-zu-Follower-Ausrichtung), stehen
in [§11.4 One-way only](LAN8651_TIME_SYNC.md#114-one-way-only-what-that-costs-and-what-it-does-not).
Rollenaufteilung als Tabelle: [§11.3 The split](LAN8651_TIME_SYNC.md#113-the-split).

Drei Konsequenzen, die den ganzen Plan prägen — sie sind der Grund, warum Phase 1 so klein ist:

1. **Die Bridge braucht keinen RX-Timestamp.** Damit bleibt der einzige unvermeidliche
   Treiber-Patch ([drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348))
   vollständig auf der Follower-Seite. **Dieses Repo bekommt keine Änderung in generiertem Code.**
2. ~~**`FTSE`/`FTSS` sind reine RX-Bits** und gehören deshalb nicht in Phase 1.~~ **Falsch, korrigiert
   am 2026-08-11 beim Bauen von Phase 1.** Das Datenblatt ist in §5.2.5.1 eindeutig: „Transmit
   timestamping is enabled by setting the Frame Timestamp Enable (FTSE) bit … When the TSC header
   field is zero **or frame timestamping is disabled**, no frame egress timestamp will be captured."
   Ohne `FTSE` bleibt das Capture-Register also leer, egal was in `TSC` steht. Der Grandmaster setzt
   `FTSE` **und** `FTSS` beim Start (`lan_rmw 0x00000004 0xC0 0xC0`, siehe
   [§10.1](LAN8651_TIME_SYNC.md#101-from-application-code-at-runtime--preferred)) und nimmt sie beim
   Stoppen zurück — `FTSS` zwingend mit, sonst frisst der RX-Pfad vier Byte Nutzlast
   ([§10.3](LAN8651_TIME_SYNC.md#103-two-traps-that-apply-either-way)). Das bleibt Anwendungscode,
   Punkt 1 gilt unverändert.
3. **Die Buslast hängt nicht an der Zahl der Follower**: zwei Frames pro Intervall, egal wie viele
   Knoten mithören.

---

## Phase 1 — Grandmaster auf der Bridge, ohne jeden Treiber-Patch

Ziel: das Gerät sendet `Sync` + `Follow_Up` mit einem *echten* Hardware-TX-Timestamp, und das ist in
Wireshark nachweisbar. Kein Follower nötig, kein Treiber-Patch nötig.

**Das Senden ist ausgeschaltet, bis es jemand einschaltet.** Der Grandmaster ist ein Werkzeug, das
per CLI gestartet und gestoppt wird, mit frei einstellbarem Intervall; optional startet er beim Booten
von selbst, wenn das im EEPROM so hinterlegt ist. **Default ist: nicht senden** — Begründung und
Umsetzung in 1.5.

**1.1 Neues Modul `firmware\src\ptp_gm.c` / `.h`.** Aufbau wie
[lan865x_diag.c](firmware/src/lan865x_diag.c): `PTP_GM_Initialize()` einmal, `PTP_GM_Tasks()` aus der
Hauptschleife, dazu eine CLI-Gruppe `ptp` mit `ptphelp`. Selbe Trennung wie dort — das Modul soll für
sich stehen und in ein anderes LAN865x-Projekt kopierbar bleiben.

**1.2 Ins Projekt eintragen.** Ohne diesen Schritt wird die Datei stillschweigend nicht gebaut:
`nbproject\configurations.xml` ist getrackt und die Quelle der Wahrheit (je ein `<itemPath>` für
`.c` und `.h`), das generierte `nbproject\Makefile-default.mk` muss von Hand nachgezogen werden —
zwei `SOURCEFILES`-Zeilen, drei `OBJECTFILES`-Zeilen, zwei Compile-Regeln, Objektverzeichnis
`_ext/1360937237`. Details in `CLAUDE.md` Abschnitt 6.

**1.3 Register setzen, alles aus Anwendungscode.** Adressen und Bitwerte:
[§2 Register map](LAN8651_TIME_SYNC.md#2-register-map), Vorgehen und die zwei Fallen:
[§10.1](LAN8651_TIME_SYNC.md#101-from-application-code-at-runtime--preferred) und
[§10.3](LAN8651_TIME_SYNC.md#103-two-traps-that-apply-either-way).

| Was | Zweck |
|---|---|
| `FTSE` + `FTSS` in `OA_CONFIG0`, RMW mit Maske `0xC0` | schaltet Frame-Timestamping überhaupt ein — **auch für TX**, siehe die Korrektur in Abschnitt 0 |
| **nichts** am TX-Matcher | der Treiber konfiguriert ihn bereits, siehe unten |
| `MAC_TI` (+ kalibriertes `MAC_TISUBN`) | Tickweite der Wallclock, 25 MHz entspricht 40 ns — auf diesem Board schon `0x28`, also nichts zu tun |
| optional `PADCTRL` + `PPSCTL` | 1PPS auf DIOA4 als sichtbare Referenz fürs Oszilloskop |

> **Der TX-Matcher braucht keinen einzigen Schreibzugriff — nachgesehen am 2026-08-11.** Die
> Init-Tabelle des Treibers (`drv_lan865x_api.c`, Zeilen ~1683–1690) schreibt bereits
> `TXMMSKH = 0xFF`, `TXMMSKL = 0xFFFF`, `TXMLOC = 0` und `TXMCTL = 0x0002` (`TXME`), ebenso das
> RX-Pendant. Das ist genau die im Datenblatt §4.5.2.2 beschriebene Standardeinstellung „match every
> packet at the SFD", die *alle* Microchip-Treiber setzen: **Maskenbits bedeuten „ignorieren"**, alle
> auf 1 heißt also „Muster ganz egal". Damit wird jeder Frame gestempelt, **für den `TSC ≠ 0`
> angefordert wird** — normaler Stackverkehr läuft mit `TSC = 0` und bleibt unberührt. Zwei
> Folgerungen: die Vorstellung, man müsse den Detektor **pro `Sync` neu armen**, ist falsch (`TXME`
> ist R/W und nicht selbstlöschend; nur `TXPMDET` ist read-clear), und ein auf PTP verengter Matcher
> wäre eine *Einschränkung*, kein Gewinn. Falls er später doch gewünscht ist: `TXMLOC = 30` (Nibble
> nach dem Muster), Muster `0x88F710` — die 24 Bit umfassen EtherType **plus** das Byte
> `transportSpecific|messageType`, weshalb der Wert von `PTP_TRANSPORT_SPECIFIC` mitentscheidet.

**1.4 Sendezyklus.** `Sync` über `DRV_LAN865X_SendRawEthFrame(..., tsc = 1, ...)` senden, den
Timestamp aus `TTSCAH`/`TTSCAL` holen, dann `Follow_Up` mit `tsc = 0` und
`preciseOriginTimestamp = t1 + PTP_GM_STATIC_OFFSET`. Ablauf und Begründung des Zwei-Schritt-Verfahrens:
[§5](LAN8651_TIME_SYNC.md#5-why-it-is-two-step-sync--follow_up) und
[§11.2](LAN8651_TIME_SYNC.md#112-what-this-bridge-already-brings).

> **Nicht über `TTSCAA` in `OA_STATUS0` synchronisieren, und der `_OnStatus0`-Callback steht nicht zur
> Verfügung — beides am 2026-08-11 im Treiber nachgelesen.** `_OnStatus0()` ist `static` in
> `drv_lan865x_api.c`, also generierter Code, den Phase 1 ausdrücklich nicht anfasst; er liest
> `OA_STATUS0` bei jedem Extended-Status-Ereignis und **schreibt es unverändert zurück**, was
> `TTSCAA` als Write-1-Clear löscht. Ein Verbraucher hier läuft also gegen den Treiber um dasselbe
> Bit — und verliert gelegentlich, was als sporadisch fehlender Timestamp erscheint. Stattdessen
> entscheidet ein **Frischevergleich**: das 64-Bit-Capture wird gegen den Wert des Vorzyklus geprüft,
> ein noch nicht erfolgtes Capture liefert zwangsläufig den alten. Dieser Schatten überlebt
> `stop`/`start` absichtlich — genau das verhindert, dass ein alter Zeitstempel dem ersten `Sync`
> eines neuen Laufs zugeschrieben wird, also den Fehler, den 1.5 beschreibt.
> Nebenbei: der Treiber druckt bei jedem Ereignis ratenbegrenzt
> `Status0.Transmit Timestamp Capture Available A` — bei 1 s Intervall etwa eine Zeile pro Sekunde.
> Das ist erwartetes Rauschen, kein Fehler, und ohne Eingriff in generierten Code nicht abstellbar.

> **Nicht** zyklisch `lan_read` pollen. Registerzugriffe teilen die TC6/SPI-Service-Logik mit dem
> Datenpfad; gemessen wurden dadurch rund 5 % UDP-Paketverlust unter Last (`CLAUDE.md` Abschnitt 4).
> Der Callback-Weg kostet nichts.

**1.5 Betrieb: Start/Stop, Intervall, Autostart.** Drei getrennte Dinge, die man nicht vermischen
darf — *ob* gesendet wird, *wie oft*, und *ob das einen Reset überlebt*.

*CLI.* Vier Kommandos in der Gruppe `ptp`:

| Kommando | Wirkung |
|---|---|
| `ptp start` | beginnt zu senden, mit dem aktuell eingestellten Intervall |
| `ptp stop` | hört auf, ordentlich (siehe unten) |
| `ptp interval <ms>` | setzt das Sendeintervall; ohne Argument zeigt es den Wert |
| `ptp status` | sendet ja/nein, Intervall, Sequenznummer, Zähler, Autostart-Zustand |

*Intervall.* Ein Millisekundenwert, geprüft gegen eine Ober- und Untergrenze, ausgewertet über
`SYS_TIME` in `PTP_GM_Tasks()` — nicht über eine Zählschleife. Eine Änderung im Betrieb greift ab dem
nächsten Zyklus und setzt die `sequenceId` **nicht** zurück; ein Follower, der die Sequenz beobachtet,
sieht dadurch keine Lücke.

> **Das ist keine Stilfrage, sondern am 2026-08-11 nachgemessen.** Der Rohsende-Weg hat eine Queue
> von vier Einträgen (`TC6_TX_ETH_QSIZE = 4` in `tc6-conf.h`), und geleert wird sie erst, wenn die
> Hauptschleife den Treiber wieder bedient. Eine Sendeschleife, die nicht zurückkehrt, bekommt
> deshalb **genau fünf Frames weg** — eines geht synchron über das `serviceData()` innerhalb von
> `TC6_SendRawEthernetPacket()` raus, vier belegen die Queue, das sechste scheitert. `noip_send 20`
> bricht reproduzierbar beim sechsten ab, unabhängig vom `gap_ms` (der Busy-Wait bedient nichts).
> Für den Grandmaster heißt das: **höchstens ein `Sync` + ein `Follow_Up` pro Task-Durchlauf**, dann
> zurück in die Hauptschleife. Zwei Frames passen bequem, ein Burst nicht.

> **Ein Nebeneffekt, der beim Testen mit fremden Werkzeugen auffällt:** das Feld
> `logMessageInterval` im `Sync`-Frame ist ein **Zweierlogarithmus** und kann ein beliebiges
> Millisekundenintervall gar nicht ausdrücken. Der eigene Servo braucht es nicht — er misst den
> Abstand aufeinanderfolgender `Sync`-Nachrichten selbst
> ([§7](LAN8651_TIME_SYNC.md#7-the-software-servo)) —, aber ein PTP-Analysator oder Wireshark
> vergleicht Feld und Realität und meldet eine Abweichung. Also den nächstliegenden Zweierwert
> eintragen und in `ptp status` anzeigen, wenn Feld und tatsächliches Intervall auseinanderfallen.
> Sonst sucht später jemand einen Fehler, den es nicht gibt.

*Sauber stoppen.* `ptp stop` muss den TX-Matcher entwaffnen und einen noch stehenden
Capture-Status per Write-1-Clear abräumen. Sonst liegt beim nächsten `ptp start` ein alter Zeitstempel
bereit und wird dem ersten neuen `Sync` zugeschrieben — ein Fehler, der genau einmal pro Start
auftritt und deshalb schwer zu fassen ist. Ein `Follow_Up` ohne zugehörigen `Sync` darf nicht
hinausgehen: wird mitten im Zyklus gestoppt, wird der Zyklus abgebrochen, nicht halb beendet.

*Autostart über das EEPROM.* Zwei neue Schlüssel in [env.c](firmware/src/env.c), behandelt wie
`plca_id` / `plca_cnt`:

| Schlüssel | Bedeutung | Default |
|---|---|---|
| `ptp_auto` | 0 = nach dem Booten still, 1 = automatisch starten | **0** |
| `ptp_ival` | Sendeintervall in ms, auch für den Autostart | der Startwert des Moduls |

Umsetzung: Felder in die `env_t`-Struktur, in `env_defaults()` seeden, in `cmd_showenv` anzeigen, in
`cmd_setenv` mit Bereichsprüfung parsen — und `ENV_VERSION` erhöhen. Bedienung dann wie gehabt:
`setenv ptp_auto 1`, `saveenv`.

> **Preis der Strukturänderung, vorher wissen:** ein im EEPROM liegender Datensatz der alten Version
> wird ungültig (andere Länge, CRC an anderer Stelle) und beim ersten Boot durch die Compile-Defaults
> ersetzt. Ein Board, das nach dem Update hochkommt, hat also wieder die Standard-IPs und -MACs. Das
> ist gewollt und die einzige ehrliche Variante — aber es gehört in die Release-Notiz, sonst wundert
> sich jemand über eine „verlorene" Konfiguration.

*Wann der Autostart greifen darf.* Nicht in `ENV_Init()`. Registerzugriffe auf den LAN865x werden
erst im eingeschwungenen App-Zustand bedient (`CLAUDE.md` Abschnitt 3), und PLCA wird ohnehin erst über
`env_apply()` gesetzt. Der Autostart gehört deshalb an dieselbe Stelle wie `env_apply()` bzw. hinter
einen Zustandstest in `PTP_GM_Tasks()` — startet man früher, läuft der erste Zyklus ins Leere und es
sieht nach einem Registerproblem aus.

*Warum Default aus.* Zwei Gründe, beide praktisch: die vorhandenen Prüfskripte
[test_lan8651.py](test_lan8651.py) und [test_mirror.py](test_mirror.py) zählen Frames auf dem Bus und
messen gegen den 1-Hz-Verkehr des Endpoints als Oracle — ein von sich aus sendender Grandmaster
verfälscht beides. Und ein Gerät, das nach dem Flashen ungefragt PTP in ein fremdes Netz streut, ist
kein gutes Verhalten für eine Bridge.

*Buslast.* Zwei Frames pro Intervall (Abschnitt 0, Punkt 3), also frei skalierend mit dem
eingestellten Wert. Kurze Intervalle konkurrieren über PLCA mit dem Nutzverkehr — der Lasttest in
1.7 gehört mit dem kleinsten Intervall gefahren, das man freigeben will, nicht mit dem Default.

**1.6 Adressierung und Mitschnitt.** Ethernet-Broadcast `FF:FF:FF:FF:FF:FF`, EtherType `0x88F7`,
PTPv2, twoStepFlag, hochlaufende `sequenceId`. **Keine** PTP-Multicast-Adressen — der RX-Filter des
LAN865x ist nicht auf diese Gruppen konfiguriert und verwirft sie lautlos
([§6](LAN8651_TIME_SYNC.md#6-four-constraints-specific-to-a-multidrop-segment)).

**Der eigene `Sync` erscheint nicht von selbst auf `eth1` — auch mit `mirror 1` nicht.** Zwei
unabhängige Gründe:

- Die MAC-Bridge leitet nur weiter, was sie auf einem Port **empfängt**. Ein selbst erzeugter Frame
  wird nie empfangen, also gibt es nichts zu fluten. Geflutet werden Broadcasts *durchlaufenden*
  Verkehrs — ein *fremder* Master auf `eth1` landet dadurch tatsächlich auf `eth0`, das ist die
  Aussage in [§11.5](LAN8651_TIME_SYNC.md#115-three-things-that-are-different-on-a-bridge); die
  Gegenrichtung für eigene Frames folgt daraus nicht.
- Der Mirror-TX-Hook hängt an
  [`DRV_LAN865X_PacketTx()`](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L664),
  dem Stack-Ausgang. Der TX-Zeitstempel erzwingt aber `tsc = 1` und damit den Rohweg
  [`DRV_LAN865X_SendRawEthFrame()`](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L2416)
  → `TC6_SendRawEthernetPacket()`, der an dieser Funktion vorbeigeht. Aus demselben Grund sind
  `noip_send`-Frames im Mirror unsichtbar. Der Kommentar im Treiber, `PacketTx` sei „the single eth0
  egress point", ist seit `SendRawEthFrame` **falsch**.

**Der Mitschnittweg steht schon — er läuft über den Mirror.** `port_mirror.c` hat seit dem Commit
zu diesem Abschnitt einen dritten Eintrittspunkt
[`MIRROR_RawTx(frame, len)`](firmware/src/port_mirror.h), der die bereits vorhandene, bisher private
Klonfunktion `mirror_ethpkt_to_eth1()` öffnet. `ptp_gm.c` ruft ihn nach jedem erfolgreichen
`DRV_LAN865X_SendRawEthFrame()` auf, genau wie
[`noip_test.c`](firmware/src/noip_test.c) es jetzt tut — dessen Frames sind damit auch der Testfall
für den Weg, bevor es einen Grandmaster gibt.

**Ein Schalter, kein zweiter:** `MIRROR_RawTx()` prüft selbst `mirror [0|1]`, ein eigenes PTP-Gate
gibt es absichtlich nicht. Zwei Flags in Reihe hätten genau das Fehlerbild erzeugt, gegen das dieser
Abschnitt geschrieben ist — leerer Mitschnitt, obwohl nichts defekt ist. Kein MAC-Filter, anders als
in den beiden Stack-Richtungen: der Aufrufer hat den Frame gebaut, er ist per Konstruktion unser.

Die verworfenen Alternativen, zum Nachlesen falls die Frage wiederkommt:

| Weg | Warum nicht |
|---|---|
| Hook direkt in `DRV_LAN865X_SendRawEthFrame()`, fängt jeden Rohsender automatisch | generierte Datei — MCC „Generate Code" entfernt ihn lautlos, dieselbe Falle wie beim bestehenden Mirror-Patch |
| Sniffer direkt am T1S-Bus, also ein zweiter T1S-Knoten promiscuous | zusätzliche Hardware, in Phase 1 nicht vorhanden. Der einzige Weg, der auch das **Wire-Timing** belegt |
| `Sync` über den Stack senden, damit er `PacketTx` durchläuft | wertlos: ohne `tsc = 1` kein TX-Zeitstempel — und der ist der ganze Zweck von Phase 1 |
| Natives Senden auf **beiden** Interfaces statt Klonen | macht die Bridge zur echten PTP-Quelle im 100BASE-T-Netz, also genau das, was der Default „nicht senden" verhindern soll |

Was der Klon **nicht** beweist: das Wire-Timing. Es ist eine Softwarekopie desselben Puffers, über
einen anderen MAC verschickt; Frameaufbau, Inhalt und Kadenz stimmen, die Abstände auf dem Draht
sieht nur ein Messgerät oder der Bus-Sniffer. Und die Kopie ist best-effort — ist der Paketpool
belegt, fehlt sie im Mitschnitt, statt den Sendepfad aufzuhalten.

Ergänzend zählt `ptp status` gesendete `Sync`/`Follow_Up` und abgebrochene Zyklen, damit „sendet
überhaupt" auch ohne Mitschnitt beantwortbar ist.

**1.7 Verifikation — automatisiert, `test_ptp.py` mit pyshark.**

**Der Prüfling ist der Grandmaster in der Bridge, das Messmittel ein PC mit Wireshark an `eth1`.**
Der Weg dorthin ist der Klon aus 1.6 (`mirror 1` + `MIRROR_RawTx()`), und die Auswertung läuft
automatisch: **pyshark** (der Python-Aufsatz auf `tshark`) dissektiert die Frames auf Feldebene,
statt sie nur zu zählen. Das ist der Unterschied zu
[test_lan8651.py](test_lan8651.py) und [test_mirror.py](test_mirror.py), die `tshark` per
`subprocess` mit einem BPF-Filter aufrufen und nur Zeilen zählen — für „ist das ein gültiger
`Sync`?" reicht das nicht.

Beides ist auf dem Entwicklungsrechner vorhanden: `pyshark` ist installiert, `tshark.exe` liegt
unter `C:\Program Files\Wireshark\tshark.exe`. Konventionen werden aus `test_mirror.py` übernommen,
nicht neu erfunden: `TSHARK`-Pfad mit `--tshark`-Override, `IFACE` als NPF-Gerätename
(`\Device\NPF_{5A4D39DB-…}` = „Ethernet 8"), Auflösung eines Adapternamens über `tshark -D` wie in
`resolve_iface()`, `PORT = "COM8"` und die CLI-Anbindung über `serial` + `drain` aus
[cli.py](cli.py).

Was das Skript prüft — jede Zeile eine eigene Zusicherung, damit ein Fehlschlag benennt, *was*
falsch ist:

| Prüfung | Feld / Kriterium | Warum sie drin ist |
|---|---|---|
| Default aus | 0 Frames vor jedem `ptp start` | belegt „sendet ungefragt nicht" (1.5) |
| Frames kommen an | EtherType `0x88F7`, Anzahl > 0 | belegt Sendepfad **und** Klonweg |
| PTP-Version | `ptp.versionptp == 2` | ein falsches Nibble dissektiert Wireshark noch, ein Follower nicht |
| Nachrichtentypen | `ptp.v2.messageid` == `0x0` (Sync) und `0x8` (Follow_Up) | beide müssen da sein, sonst ist der Zyklus halb |
| Paarung | je `sequenceId` genau **ein** Sync und **ein** Follow_Up, Sync zuerst | fängt den „Follow_Up ohne Sync"-Fehler aus 1.5 |
| Sequenz | `ptp.v2.sequenceid` streng monoton, ohne Lücke | prüft den Zähler und indirekt, dass nichts verloren geht |
| Two-Step | `twoStepFlag` gesetzt | ein One-Step-Flag mit Zeitstempel im Follow_Up ist widersprüchlich |
| Zeitstempel vorhanden | `preciseOriginTimestamp` im Follow_Up ≠ 0 | ein Feld voller Nullen heißt: TX-Capture stand nicht bereit |
| Zeitstempel monoton | Sekunden/Nanosekunden aufsteigend, Δ ≈ Intervall | fängt den 1e9-Überlauf in der Nanosekundenstelle |
| Kadenz | Ankunftsabstände ≈ gesetztes `ptp interval`, mit Toleranz | prüft die Zeitbasis, zwei Intervalle nacheinander |
| Stop | nach `ptp stop` 0 Frames im Fenster | „hört auch wirklich auf" |

Ablauf je Prüfschritt wie in `test_mirror.py`: Kommando über die Konsole schicken, `tshark`/pyshark
in einem Thread über ein festes Zeitfenster mitschneiden, danach auswerten. **Exitcode ≠ 0 bei
jeder Abweichung**, damit das Skript ohne Sichtprüfung taugt.

Zwei Fallstricke, die das Skript selbst abfangen muss, sonst meldet es Fehler, die keine sind:

- **`mirror 1` ist Vorbedingung, kein Prüfgegenstand.** Das Skript setzt es zu Beginn und prüft die
  Antwortzeile. Vergisst man es, ist der Mitschnitt leer und sieht wie ein toter Grandmaster aus.
  Am Ende wieder auf den Ausgangszustand stellen.
- **Die Kadenz-Toleranz gehört großzügig.** Gemessen werden Ankunftszeiten der **Klone** am PC —
  best-effort erzeugt und über einen zweiten MAC verschickt. Eine fehlende Kopie (belegter
  Paketpool) darf die Kadenzprüfung nicht umwerfen; deshalb über den Median der Abstände urteilen,
  nicht über den größten.

**Was hiermit nicht geprüft ist:** das Wire-Timing auf dem T1S-Bus (1.6, letzter Absatz) und die
Genauigkeit des Zeitstempels gegenüber einer Referenzuhr. Beides braucht ein Messgerät oder den
Follower aus Phase 2 — `test_ptp.py` belegt Frameaufbau, Vollständigkeit, Reihenfolge und Kadenz.

Zusätzlich, weil ohne Mitschnitt beantwortbar und deshalb der erste Griff bei einer Störung:

1. `ptp status` **vor** `ptp start`: `off`, TX-Zähler `0`. Der Test des Defaults, in zehn Sekunden.
   Ein leerer Mitschnitt taugt dafür **nicht** — der ist auch bei laufendem Grandmaster leer, wenn
   der Klonweg aus 1.6 nicht steht.
2. `ptp status` nach `ptp start`: der TX-Zähler läuft. Läuft er, und der Mitschnitt bleibt leer, ist
   der Fehler im Klonweg, nicht im Grandmaster — diese Reihenfolge spart die Fehlersuche am
   falschen Ende.
3. Start/Stop mehrfach: ein erneutes `ptp start` darf keinen Zeitsprung erzeugen — der Test der
   Abräumlogik aus 1.5.
4. Autostart: `setenv ptp_auto 1`, `saveenv`, Reset — sendet nach dem Booten von selbst. Danach
   `setenv ptp_auto 0`, `saveenv`, Reset — sendet nicht. Beide Richtungen prüfen, nicht nur die
   interessante.
5. Lasttest: `stats` parallel zu `iperf`, mit dem **kleinsten** freigegebenen Intervall, um zu
   belegen, dass der PTP-Zyklus den SPI-Pfad nicht stört.

**Fertig, wenn** `test_ptp.py` mit Exitcode 0 durchläuft, Start/Stop/Intervall/Autostart sich wie
oben verhalten und der Durchsatz unverändert ist. Bis hierher wurde **keine** generierte Datei
angefasst.

### 1.8 Stand: Phase 1 läuft (2026-08-11)

Umgesetzt in [ptp_gm.c](firmware/src/ptp_gm.c) / [ptp_gm.h](firmware/src/ptp_gm.h), Prüfskript
[test_ptp.py](test_ptp.py). Gemessen am Target, `tshark`/pyshark auf dem `eth1`-Adapter:

| Prüfung | Ergebnis |
|---|---|
| `test_ptp.py`, alle 11 Zusicherungen | Exitcode **0**, zweimal gefahren (Bus ohne und mit Koordinator) |
| Frames | 18 `Sync` + 17 `Follow_Up` im Fenster, EtherType `0x88F7`, Quell-MAC die eigene |
| `versionptp` / `messagetype` / `twostep` | 2 / `0x00` und `0x08` / auf `Sync` gesetzt |
| Paarung und Sequenz | je `sequenceId` genau ein Paar, `Sync` zuerst, lückenlos aufsteigend |
| `preciseOriginTimestamp` | echt und laufend, z. B. 152,788900720 s → 156,785543920 s |
| Kadenz | Median 249,79 ms Ankunftsabstand bei 250 ms; Timestamp-Delta Median 249,790 ms |
| `logMessageInterval` | 0 bei 1000 ms, −1 bei 500 ms — Feld und Realität stimmen dort exakt |
| Zyklen ohne Fehler | 364 Zyklen bei **50 ms** (kleinstes Intervall): 0 Timeouts, 0 Sendefehler, 0 Registerfehler, `eth0 TX err=0 qFull=0` |
| Start/Stop mehrfach | Sequenz läuft weiter (kein Reset), Capture springt nicht zurück |
| Autostart | `ptp_auto 1` + Reset sendet von selbst mit `ptp_ival`; `ptp_auto 0` + Reset bleibt still |
| Abstand `Sync` → `Follow_Up` | 0,1–0,3 ms, das ist der SPI-Rückweg für den Timestamp |

Zwei Dinge, die das ausdrücklich **mit**beweist: der Timestamp entsteht am Ende des SFD **auf dem
MDI**, ein frisches Capture pro Zyklus ist also der Beleg, dass der Frame den Draht wirklich
erreicht hat — der Mirror-Klon allein könnte das nicht zeigen (1.6, letzter Absatz). Und der
Nanosekundenwert wandert um etwa 1,02 ms pro Sekunde gegen das Sendeintervall: erwartetes Verhalten,
weil `SYS_TIME` des SAME54 und die Wallclock des LAN8651 zwei unabhängige Quarze sind. Genau diese
Differenz ist das, was der Servo eines Followers später wegregelt.

**Offen aus 1.7:** der Lasttest mit `iperf` parallel (Punkt 5 der Liste) ist nicht gefahren — belegt
ist nur, dass 20 Zyklen/s ohne Fehler und ohne `qFull` laufen, nicht der Durchsatz unter Volllast.

---

## Phase 2 — Follower als eigenes Projekt

> **Korrektur zur ursprünglichen Festlegung (Entscheidung des Nutzers, 2026-08-11): der Follower liegt
> in *diesem* Repo**, als zweites, parallel installiertes MPLAB-Projekt unter
> [follower/](follower/) — nicht in einem eigenen Repo. Abgeleitet wird er mechanisch aus dem
> Bridge-Projekt: [derive_follower.py](derive_follower.py) reproduziert `follower/` bytegleich aus
> `firmware/` und ist damit gleichzeitig die Liste aller Unterschiede. Nur `eth0` (LAN865x/T1S), kein
> Port-Mirror, kein Grandmaster, ein Interface im env-Datensatz. Welches Board welchem Projekt gehört,
> steht in `boards.json` — einmal `python setup_flasher.py`, danach löst jede `flash.bat` ihre Probe
> selbst auf.

Vorlage sind die bereits gemessenen Module aus
`zabooh/net_10base_t1s`, siehe
[§11.1 Prior art](LAN8651_TIME_SYNC.md#111-prior-art-both-roles-already-exist-and-are-measured) und
die beiden Vorbehalte in [§11.7](LAN8651_TIME_SYNC.md#117-two-caveats-about-the-reference-implementation).

**2.1 Der eine Treiber-Patch.**
[drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348)
verwirft den RX-Timestamp; hier liefert er `t2`. **Nach jedem MCC „Generate Code" ist der Patch weg** —
das ist derselbe Mechanismus, der schon den Port-Mirror trifft (`CLAUDE.md` Abschnitt 6), und muss in
die Bring-up-Checkliste des Follower-Projekts.

**2.2 RX-Timestamps freischalten.** `FTSE` **und** `FTSS` gemeinsam setzen — niemals `FTSE` allein,
weil der Empfangspfad eine feste Byteanzahl überspringt. Register, Maske und Wert:
[§10.1](LAN8651_TIME_SYNC.md#101-from-application-code-at-runtime--preferred), die Falle steht in
[§10.3](LAN8651_TIME_SYNC.md#103-two-traps-that-apply-either-way).

**2.3 Frames abgreifen.** Filter auf EtherType `0x88F7` im RX-Callback, Frame samt Timestamp in eine
kleine Queue, Verarbeitung im Task-Kontext.

**2.4 Servo portieren, verkleinert.** Fünf Zustände mit Drift-IIR und Ausreißerverwerfung —
Beschreibung in [§7 The software servo](LAN8651_TIME_SYNC.md#7-the-software-servo), Bewertungsmaßstäbe
in [§8](LAN8651_TIME_SYNC.md#8-judging-the-result-without-a-ptp-analyser). Der Offsetzweig reduziert
sich auf

```
offset = t2 - t1 - D_const
```

Alles, was mit `Delay_Req`, `Delay_Resp`, `t3` oder `t4` zu tun hat, wird beim Portieren **nicht**
übernommen. Der Frequenzzweig bleibt unverändert, er arbeitet aus dem Abstand aufeinanderfolgender
`Sync`-Nachrichten.

**Fertig, wenn** der Follower den Zustand `FINE` erreicht und dort bleibt.

### 2.5 Stand: Messkette steht, Servo fehlt (2026-08-11)

2.1 bis 2.3 sind umgesetzt und am Ziel gemessen, 2.4 (Servo) ist offen. Bewusst in dieser Reihenfolge:
**erst beweisen, dass gemessen wird, dann die Regelschleife schließen** — ein Servo auf einer
unbewiesenen Messung ist nicht debuggbar.

Umgesetzt in [ptp_follower.c](follower/firmware/src/ptp_follower.c) /
[ptp_follower.h](follower/firmware/src/ptp_follower.h), Kommando `ptpf on | off | status | log | reset`:

| Prüfung | Ergebnis |
|---|---|
| Treiber-Patch (2.1) | `ptp_follower_rx_hook()` in `TC6_CB_OnRxEthernetPacket()`; **198 von 198 Syncs mit Zeitstempel**, `sync without timestamp: 0` |
| `FTSE`+`FTSS` (2.2) | beim `ptpf on` gesetzt, beim `ptpf off` zurückgenommen |
| Paarung (2.3) | 198 Sync / 198 Follow_Up / 198 Proben, 0 unmatched, 0 Ring-Überläufe |
| Offset | `t2 − t1 − D_const`, Spanne über ~200 Proben **249.440 ns** |
| Frequenzversatz | **−1280 ns je 250-ms-Zyklus = −5,12 ppm** zwischen den beiden Quarzen, konstant |
| Master-Zyklus, vom Follower aus gemessen | 249.972.800 ns bei kommandierten 250 ms |

Der Absolutoffset ist groß (Differenz der Betriebszeiten beider Boards) und **kein Fehler** — genau den
zieht der Servo im Zustand `HARDSYNC` einmalig weg. Interessant ist die Änderung pro Zyklus, und die ist
inzwischen sauber: die Spanne entspricht der aufsummierten Drift, es liegen keine Ausreißer mehr darin.

**Zwei Fehler in der Grandmaster-Seite hat erst der Follower sichtbar gemacht** — beide in
`CLAUDE.md` Abschnitt 6 und `LAN8651_TIME_SYNC.md` §4 festgehalten: `TTSCAA` sperrt neue Captures (das
Write-1-Clear gehört **vor** jeden Zyklus, nicht nach jeden Erfolg), und das Capture-Paar muss
`TTSCAL` → `TTSCAH` gelesen werden, sonst ist der Offset um genau eine Sekunde falsch. Das ist der
eigentliche Wert eines zweiten Knotens: die Bridge konnte beides über sich selbst nicht feststellen.

**Als nächstes 2.4:** Servo mit den fünf Zuständen, Frequenzzweig aus dem Abstand aufeinanderfolgender
`Sync` (die −5,12 ppm sind sein Eingangssignal), Offsetzweig über `MAC_TA`, Feinregelung über
`MAC_TI`/`MAC_TISUBN`.

---

## Phase 3 — Laufzeitkonstante: nur ein Schätzwert

**Es wird geschätzt, nicht gemessen.** `D_const` bekommt den Referenzwert aus
[§11.4](LAN8651_TIME_SYNC.md#114-one-way-only-what-that-costs-and-what-it-does-not) als
`#define` und bleibt dort. **Kein `Delay_Req`/`Delay_Resp` wird implementiert — auch nicht
vorübergehend, auch nicht „nur zum Einmessen".** Das ist eine bewusste Festlegung: eine
Bring-up-Messung würde genau die Maschinerie zurückholen, deren Vermeidung der Sinn des
Einweg-Verfahrens ist.

Was das bedeutet, in klaren Worten:

- Der Fehler ist **konstant** und liegt allein in der **absoluten Phase**. Er kann per Definition
  nicht zu Drift werden.
- Zwischen zwei Followern hebt er sich weitgehend **auf** — geteiltes Medium, gemeinsamer Offset.
  Übrig bleibt nur die *Differenz* der Laufzeiten: Kabellängenunterschied in der Größenordnung
  weniger Nanosekunden pro Meter plus Exemplarstreuung der PHYs.
- Der Ankerfehler zwischen nIRQ und SFD ist in diesem Entwurf **kein eigenes Problem**: ein fester
  Ankerfehler ist von einer festen Laufzeit nicht unterscheidbar und verschwindet in derselben
  Konstante ([§11.7](LAN8651_TIME_SYNC.md#117-two-caveats-about-the-reference-implementation)).

Wer den Absolutwert später doch prüfen will, hat einen **code-freien** Weg: die 1PPS-Ausgänge von
Master und Follower auf DIOA4 am Oszilloskop vergleichen und die Konstante nachziehen. Das ist eine
Messung, keine Protokollerweiterung.

---

## Phase 4 — Testwerkzeuge

Unterverzeichnis `tools\ptp\` **in diesem Repo**, weil hier schon die Skripte liegen, die beide
Boards über beide COM-Ports bedienen können: [cli.py](cli.py), [test_lan8651.py](test_lan8651.py),
[test_mirror.py](test_mirror.py) — dazu `test_ptp.py` aus 1.7, dessen pyshark-Auswertung hier für die
Follower-Seite wiederverwendbar ist.

Zu prüfen sind:

| Größe | Kriterium |
|---|---|
| Offset: Mittelwert, Standardabweichung, Spitzenwert | Maßstäbe in [§8](LAN8651_TIME_SYNC.md#8-judging-the-result-without-a-ptp-analyser) und [§12](LAN8651_TIME_SYNC.md#12-where-the-numbers-come-from) |
| Zeit bis `FINE` | reproduzierbar, ohne Zurückfallen |
| Follower gegen Follower | soll gut sein — dort hebt sich der Offset auf |
| Follower gegen Absolutzeit | soll **erkennbar verschoben** sein, um `D_const` — das ist erwartetes Verhalten, kein Fehler |

Der letzte Punkt ist wichtig für die Testlogik: ein Test, der absolute Übereinstimmung fordert, würde
den Entwurf zu Unrecht durchfallen lassen. Die Referenzzahlen in
[§12](LAN8651_TIME_SYNC.md#12-where-the-numbers-come-from) stammen aus einem **Zweiweg**-Aufbau und
sind damit die günstigere Grenze.

---

## Phase 5 — optional: Boundary Clock zur 100BASE-T-Seite

Erst sinnvoll, wenn Absolutzeit gebraucht wird — die Grenze des Einweg-Entwurfs, siehe Ende von
[§11.4](LAN8651_TIME_SYNC.md#114-one-way-only-what-that-costs-and-what-it-does-not) und den dritten
Punkt von [§11.5](LAN8651_TIME_SYNC.md#115-three-things-that-are-different-on-a-bridge).

Zwei Bausteine:

1. SNTP von `eth1` zieht die Wallclock der Bridge grob auf die Außenwelt; der T1S-Zweig bleibt davon
   unberührt und behält seine Präzision.
2. Ein Filter auf EtherType `0x88F7` in der Bridge, damit ein fremder Master auf `eth1` **nicht**
   ins T1S-Segment importiert wird. Ein weitergeleiteter PTP-Frame trägt keine
   Residence-Time-Korrektur und wäre schlimmer als kein PTP.

---

## Warum diese Reihenfolge

Phase 1 ist **allein testbar** und lässt dieses Repo frei von Treiberänderungen. Erst wenn der
Grandmaster nachweislich saubere Zeitstempel sendet, lohnt der Aufwand für den Follower — sonst
debuggt man zwei unfertige Seiten gegeneinander und weiß bei jeder Abweichung nicht, welche Seite
schuld ist. Die Stufung als Tabelle steht in
[§11.6](LAN8651_TIME_SYNC.md#116-staging-the-bridge-side-never-needs-a-driver-patch).

---

## Risiken, die den Plan kippen können

| Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|
| MCC „Generate Code" im Follower-Projekt | RX-Timestamp verschwindet, Servo bekommt keine `t2` | Patch in die Bring-up-Checkliste, danach Regressionstest |
| Neue `.c`-Datei nicht in den Projektdateien | wird stillschweigend nicht gebaut | `configurations.xml` **und** generiertes Makefile, siehe 1.2 |
| Zyklisches `lan_read` zur Timestamp-Abfrage | rund 5 % Paketverlust unter Last | `_OnStatus0`-Callback, siehe 1.4 |
| PTP-Multicast statt Broadcast | Frames werden lautlos verworfen | Broadcast, siehe 1.6 |
| `FTSE` ohne `FTSS` | Empfangsdaten um einen festen Versatz zerlegt | beide Bits gemeinsam, siehe 2.2 |
| Absoluttest ohne Berücksichtigung von `D_const` | Testfehlschlag bei korrektem Verhalten | erwarteten Offset in die Testkriterien, siehe Phase 4 |
| `ENV_VERSION` erhöht, ohne es anzukündigen | gespeicherte IPs/MACs fallen auf die Compile-Defaults zurück | in die Release-Notiz, siehe 1.5 |
| `ptp stop` ohne Abräumen des Capture-Status | erster `Sync` nach dem nächsten Start trägt einen alten Zeitstempel | Matcher entwaffnen + Write-1-Clear, siehe 1.5 |
| Mitschnittweg angenommen statt gebaut | leerer Mitschnitt wird als „Grandmaster sendet nicht" gelesen, Fehlersuche am falschen Ende | **erledigt:** `MIRROR_RawTx()` steht und `noip_send` nutzt ihn, siehe 1.6. Default trotzdem über den Zähler prüfen, nicht über Stille |
| `mirror 1` vergessen, bevor gemessen wird | derselbe leere Mitschnitt, derselbe Fehlschluss | `test_ptp.py` setzt den Schalter selbst und prüft die Antwortzeile, siehe 1.7 |
| Kadenz gegen den größten Frameabstand geprüft | eine fehlende best-effort-Klonkopie lässt einen korrekten Grandmaster durchfallen | über den Median urteilen, Toleranz großzügig, siehe 1.7 |
| Mehrere Frames in einer Schleife senden, ohne zur Hauptschleife zurückzukehren | ab dem sechsten Frame `send failed`, Rohsende-Queue hat vier Plätze | ein `Sync` + ein `Follow_Up` je `PTP_GM_Tasks()`-Durchlauf, siehe 1.3 |
