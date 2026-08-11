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
2. **`FTSE`/`FTSS` sind reine RX-Bits** und gehören deshalb nicht in Phase 1 — siehe
   [§10 Enabling frame timestamps](LAN8651_TIME_SYNC.md#10-enabling-frame-timestamps-two-ways-in).
3. **Die Buslast hängt nicht an der Zahl der Follower**: zwei Frames pro Intervall, egal wie viele
   Knoten mithören.

---

## Phase 1 — Grandmaster auf der Bridge, ohne jeden Treiber-Patch

Ziel: das Gerät sendet 1 Hz `Sync` + `Follow_Up` mit einem *echten* Hardware-TX-Timestamp, und das
ist in Wireshark nachweisbar. Kein Follower nötig, kein Treiber-Patch nötig.

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
| TX-Matcher-Block, `TXMLOC` / `TXMPATH` / `TXMPATL`, Masken auf 0 | erkennt EtherType `0x88F7` an der richtigen Byteposition |
| `MAC_TI` (+ kalibriertes `MAC_TISUBN`) | Tickweite der Wallclock, 25 MHz entspricht 40 ns |
| optional `PADCTRL` + `PPSCTL` | 1PPS auf DIOA4 als sichtbare Referenz fürs Oszilloskop |
| **nicht** `FTSE`/`FTSS` | RX-only, hier wirkungslos — siehe Punkt 2 in Abschnitt 0 |

**1.4 Sendezyklus.** Matcher armen, `Sync` über `DRV_LAN865X_SendRawEthFrame(..., tsc = 1, ...)`
senden, auf die Timestamp-Verfügbarkeit **über den vorhandenen `_OnStatus0`-Callback** warten,
Timestamp lesen, Write-1-Clear, dann `Follow_Up` mit `tsc = 0` und
`preciseOriginTimestamp = t1 + PTP_GM_STATIC_OFFSET`. Ablauf und Begründung des Zwei-Schritt-Verfahrens:
[§5](LAN8651_TIME_SYNC.md#5-why-it-is-two-step-sync--follow_up) und
[§11.2](LAN8651_TIME_SYNC.md#112-what-this-bridge-already-brings).

> **Nicht** zyklisch `lan_read` pollen. Registerzugriffe teilen die TC6/SPI-Service-Logik mit dem
> Datenpfad; gemessen wurden dadurch rund 5 % UDP-Paketverlust unter Last (`CLAUDE.md` Abschnitt 4).
> Der Callback-Weg kostet nichts.

**1.5 Adressierung.** Ethernet-Broadcast `FF:FF:FF:FF:FF:FF`, EtherType `0x88F7`, PTPv2, twoStepFlag,
hochlaufende `sequenceId`. **Keine** PTP-Multicast-Adressen — der RX-Filter des LAN865x ist nicht auf
diese Gruppen konfiguriert und verwirft sie lautlos
([§6](LAN8651_TIME_SYNC.md#6-four-constraints-specific-to-a-multidrop-segment)).

**1.6 Verifikation, in dieser Reihenfolge.**

1. Wireshark am Host `192.168.0.100`: weil das Gerät eine L2-Bridge ist, werden die Broadcasts von
   `eth0` nach `eth1` geflutet — der Mitschnitt kostet keine zusätzliche Hardware
   ([§11.5](LAN8651_TIME_SYNC.md#115-three-things-that-are-different-on-a-bridge)).
2. Plausibilität der Timestamps: aufeinanderfolgende `preciseOriginTimestamp` müssen im Intervall
   sauber weiterlaufen. Achtung auf den 1e9-Überlauf in der Nanosekundenstelle.
3. Lasttest: `stats` parallel zu `iperf`, um zu belegen, dass der PTP-Zyklus den SPI-Pfad nicht
   stört.

**Fertig, wenn** die Frames korrekt aufgebaut über `eth1` sichtbar sind, die Zeitstempel monoton
laufen und der Durchsatz unverändert ist. Bis hierher wurde **keine** generierte Datei angefasst.

---

## Phase 2 — Follower als eigenes Projekt

Eigenes Repo, eigenes Board. Vorlage sind die bereits gemessenen Module aus
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
[test_mirror.py](test_mirror.py).

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
| PTP-Multicast statt Broadcast | Frames werden lautlos verworfen | Broadcast, siehe 1.5 |
| `FTSE` ohne `FTSS` | Empfangsdaten um einen festen Versatz zerlegt | beide Bits gemeinsam, siehe 2.2 |
| Absoluttest ohne Berücksichtigung von `D_const` | Testfehlschlag bei korrektem Verhalten | erwarteten Offset in die Testkriterien, siehe Phase 4 |
