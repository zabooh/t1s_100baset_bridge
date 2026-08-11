# PTP über 10BASE-T1S vorführen — Bedienungsanleitung für die Konsole

Zwei Boards, zwei Terminals, keine Messgeräte: die Bridge verteilt ihre Uhrzeit über den
Zweidrahtbus, der Follower zieht seine eigene Uhr darauf und meldet, wie genau. Alles Folgende
läuft über die serielle Konsole der Boards; die einzige Stelle, an der ein PC mehr tut als zusehen,
ist der optionale Wireshark-Mitschnitt in Abschnitt 4.

Sprache: deutsch wie [PTP_IMPLEMENTATION_PLAN.md](PTP_IMPLEMENTATION_PLAN.md), auf den sich diese
Anleitung bezieht. Die Wissensdokumente [LAN8651_TIME_SYNC.md](LAN8651_TIME_SYNC.md) und
[README.md](README.md) sind englisch.

**Alle Kommandoblöcke sind zum Kopieren gebaut: reines ASCII, ein Kommando pro Zeile, keine
Kommentare dahinter.** Das ist kein Stil, sondern Erfahrung — beim Einfügen in ein Terminal wird aus
einem geschützten Leerzeichen sonst ein `unknown command`, und die Zeile sieht dabei völlig richtig
aus.

---

## Inhalt

1. [Der Aufbau](#1-der-aufbau)
2. [Einmal vorbereiten](#2-einmal-vorbereiten)
3. [Ausgangslage: zwei Uhren, die nichts voneinander wissen](#3-ausgangslage-zwei-uhren-die-nichts-voneinander-wissen)
4. [Demo 1: Grandmaster einschalten](#4-demo-1-grandmaster-einschalten)
5. [Demo 2: die Frames in Wireshark](#5-demo-2-die-frames-in-wireshark)
6. [Demo 3: den Follower einrasten lassen](#6-demo-3-den-follower-einrasten-lassen)
7. [Demo 4: der Beweis, dass beide dieselbe Zeit haben](#7-demo-4-der-beweis-dass-beide-dieselbe-zeit-haben)
8. [Demo 5: Servo aus, Drift sichtbar machen, wieder einrasten](#8-demo-5-servo-aus-drift-sichtbar-machen-wieder-einrasten)
9. [Live mitlesen: eine Zeile pro Zyklus](#9-live-mitlesen-eine-zeile-pro-zyklus)
10. [Störungssuche](#10-störungssuche)
11. [Kommandoreferenz](#11-kommandoreferenz)
12. [Was diese Demo nicht zeigt](#12-was-diese-demo-nicht-zeigt)

---

## 1. Der Aufbau

| Rolle | Projekt | Board | Konsole | eth0-IP | PLCA |
|---|---|---|---|---|---|
| **Grandmaster** | `firmware/T1S_100BaseT_Bridge.X` | SAM E54 Curiosity Ultra + LAN8740A-Tochterkarte + Two-Wire ETH Click | z. B. `COM8` | 192.168.0.200 | Node-ID **0** = Koordinator |
| **Follower** | `follower/firmware/T1S_Follower.X` | SAM E54 Curiosity Ultra + Two-Wire ETH Click | z. B. `COM10` | 192.168.0.201 | Node-ID 7 |

Verbunden sind die beiden über die **Zweidrahtleitung der Click-Boards** (10BASE-T1S). Die Bridge
hat zusätzlich `eth1` (100BASE-T) — für die Demo braucht man das nur, wenn man in Wireshark
zusehen will (Abschnitt 5).

**Die COM-Ports nicht raten.** Beide Boards melden sich als derselbe EDBG-Probe-Typ; welcher Port zu
welchem Board gehört, sagt das Flasher-Werkzeug:

```
python setup_flasher.py --show
```

```
attached boards:
  1) ATML3264031800001049   Atmel Corp. EDBG CMSIS-DAP     console COM8    -> bridge
  2) ATML3264031800001290   Atmel Corp. EDBG CMSIS-DAP     console COM10   -> follower
```

Die Spalte `console` ist die Antwort. Sie stammt aus der Seriennummer des Probes im Hardware-ID des
virtuellen COM-Ports, ist also nicht geraten. Alle Beispiele unten verwenden **COM8 = Bridge** und
**COM10 = Follower** — bei dir können die Nummern andere sein.

**Zwei Terminals** mit **115200 8N1** öffnen, eines je Board. Tera Term, PuTTY oder was da ist.
Alternativ ohne Terminalprogramm vom PC aus, ein Kommando je Aufruf:

```
python cli.py --port COM8 --read 2 "ptp status"
```

**Jedes Board sagt beim Booten, was es ist** — und wenn man den Start verpasst hat, liefert
`timestamp` dasselbe noch einmal. Die Bridge:

```
======================================================
 T1S Bridge + PTP Grandmaster
 10BASE-T1S (eth0) <-> 100BASE-T (eth1) L2 bridge, PTP master on eth0
 Build: Aug 11 2026 18:56:08
 Commands: help | lanhelp | ptp status | mirror | stats | showenv
======================================================
```

Der Follower:

```
======================================================
 T1S PTP Follower
 single interface eth0 = 10BASE-T1S, wall clock steered from Sync/Follow_Up
 Build: Aug 11 2026 18:56:13
 Commands: help | lanhelp | ptpf status | stats | showenv
======================================================
```

Der Zeitstempel ist die Bauzeit der geladenen Firmware — der schnellste Weg, um zu klären, ob auf dem
Board wirklich der Stand läuft, den man gerade gebaut hat.

---

## 2. Einmal vorbereiten

Nur nötig, wenn die Boards noch nicht die passende Firmware tragen. Zuordnung festlegen (einmal pro
Rechner, schreibt `boards.json`), dann jedes Projekt bauen und flashen:

```
python setup_flasher.py
build.bat
flash.bat
follower\build.bat
follower\flash.bat
```

`flash.bat` holt sich das richtige Board aus `boards.json` — deshalb ist die Reihenfolge egal und man
kann nicht versehentlich das falsche Board programmieren. Wer es überstimmen will:
`flash.bat --probe <seriennummer>`.

**Voraussetzung für den Build:** das jeweilige Projekt muss **einmal** in der MPLAB-X-IDE geöffnet und
gebaut worden sein, damit die `nbproject\Makefile-*.mk`-Fragmente existieren. Fehlen sie, sagt
`build.bat` das mit einer erklärenden Meldung; das ist kein Skriptfehler.

Grundzustand nach dem Flashen — **beide Seiten senden bzw. hören nichts**, und das ist Absicht:

```
python cli.py --port COM8 --read 2 "ptp status"
python cli.py --port COM10 --read 2 "ptpf status"
```

```
[PTP] sending: off   interval: 500 ms   seq: 0
[PTPF] listening: off   samples: 0   last seq: 0
```

---

## 3. Ausgangslage: zwei Uhren, die nichts voneinander wissen

Beide LAN8651 haben eine eigene Wallclock, die bei ihrem Reset bei Null anfängt. Vor dem Einschalten
von PTP laufen sie also getrennt. Auf beiden Boards die Sekunden lesen:

```
lan_read 0x00010074
```

Gemessen an diesem Aufbau, Bridge zuerst:

```
bridge    Value=0x000000D2      ->  210 s
follower  Value=0x000000D8      ->  216 s
```

Sechs Sekunden auseinander. **Achtung bei der Interpretation:** die beiden Lesevorgänge liegen
zeitlich 2–3 s auseinander (Konsolen-Umlauf), davon ist der Unterschied also ein Teil. Diese
Gegenüberstellung ist deshalb nur der *grobe* Blick — die genaue Zahl liefert später `ptpf status`.
Als Einstieg taugt sie trotzdem: die Uhren haben nichts miteinander zu tun.

---

## 4. Demo 1: Grandmaster einschalten

Auf der **Bridge** (COM8):

```
ptp interval 125
ptp start
ptp status
```

```
[PTP] sending: on   interval: 125 ms   seq: 187
[PTP] logMessageInterval: -3 (125 ms)
[PTP] tx sync: 187   follow_up: 187   ts timeouts: 0   send fails: 0   reg errors: 0
[PTP] last capture: 55 s 757494960 ns
[PTP] autostart (env ptp_auto): off
```

Zeile für Zeile:

| Zeile | Bedeutung |
|---|---|
| `sending / interval / seq` | sendet ja/nein, Zykluszeit, laufende `sequenceId` |
| `logMessageInterval` | das PTP-Feld, ein Zweierlogarithmus. `-3` = 125 ms; steht dort ein anderer Wert als das echte Intervall, sagt die Zeile das ausdrücklich |
| `tx sync` / `follow_up` | **müssen zusammenpassen** (Abweichung 1 = der Zyklus, der gerade läuft) |
| `ts timeouts` | **muss 0 bleiben.** Zählt hoch, wenn der Hardware-Zeitstempel des gesendeten `Sync` nicht rechtzeitig auftaucht |
| `last capture` | der zuletzt akzeptierte Sendezeitstempel, also die Uhrzeit der Bridge |

**Der erste Blick geht immer auf `ts timeouts`.** Steht dort etwas anderes als 0, ist der
Zeitstempelpfad gestört und alles Weitere ist wertlos — Abschnitt 10 sagt, was dann zu prüfen ist.

Wieder ausschalten geht jederzeit mit `ptp stop`; die Sequenznummer läuft danach weiter, sie wird
nicht zurückgesetzt.

**Automatisch nach dem Booten senden** (etwa für eine Vorführung ohne Tastatur), auf der Bridge:

```
setenv ptp_auto 1
setenv ptp_ival 125
saveenv
```

Wirkt ab dem nächsten Reset. Zurück mit `setenv ptp_auto 0` und `saveenv`. Der Default ist
absichtlich *aus*: ein Gerät, das nach dem Flashen unaufgefordert PTP in ein fremdes Netz streut, ist
kein gutes Verhalten, und die Frame-zählenden Prüfskripte würden dadurch verfälscht.

---

## 5. Demo 2: die Frames in Wireshark

Optional, braucht einen PC am `eth1`-Port der Bridge. **Die eigenen Frames der Bridge erscheinen dort
nicht von selbst** — der Rohsendeweg umgeht den Mirror-Hook, und eine MAC-Bridge flutet nur, was sie
*empfängt*. Deshalb den Mirror einschalten:

```
mirror on
```

Dann auf dem PC mitschneiden, EtherType `0x88F7` ist PTP:

```
python test_ptp.py
```

Das Skript setzt den Mirror selbst, schneidet mit `tshark`/pyshark mit und prüft zwölf Dinge
einzeln — Version, Nachrichtentypen, Paarung je `sequenceId`, Lückenlosigkeit, `twoStepFlag`,
Zeitstempel vorhanden und plausibel, Kadenz, dazu die Zähler des Geräts über den ganzen Lauf. Am Ende:

```
sync: 18   follow_up: 17
median arrival gap: 249.90 ms (min 248.84, max 251.07)
sync=480 follow_up=480 timeouts=0 send_fails=0 reg_errors=0

PASS: Sync/Follow_Up pairs valid, sequence intact, timestamps live, cadence right
```

Exitcode 0 heißt: alles in Ordnung. Wer von Hand zusehen will, nimmt in Wireshark den Filter `ptp`
und sieht abwechselnd `Sync` und `Follow_Up` mit steigender Sequenznummer.

Danach den Mirror wieder ausschalten, er kostet Paketpuffer:

```
mirror off
```

---

## 6. Demo 3: den Follower einrasten lassen

Auf dem **Follower** (COM10), während die Bridge sendet:

```
ptpf on
```

Der Regler meldet jeden Zustandswechsel von selbst. So sieht ein Einrasten aus (echte Ausgabe):

```
[PTPF] servo UNINIT -> MATCHFREQ  (offset -2027168127668 ns, rate 5115 ppb)
[PTPF] servo MATCHFREQ -> HARDSYNC  (offset -2027168127673 ns, rate 5115 ppb)
[PTPF] servo HARDSYNC -> COARSE  (offset 158 ns, rate 6382 ppb)
[PTPF] servo COARSE -> FINE  (offset 93 ns, rate 5621 ppb)
```

Das dauert etwa **vier Sekunden** bei 125 ms Zyklus. Was die fünf Zustände tun:

| Zustand | Was passiert |
|---|---|
| `UNINIT` | nominales Inkrement geschrieben, 16 Proben sammeln, Frequenzfehler schätzen |
| `MATCHFREQ` | erste Frequenzkorrektur anwenden, **dann** die Uhr hart auf die Zeit des Masters setzen |
| `HARDSYNC` | grobe Einmalschritte über `MAC_TA` plus Frequenznachtrimmung |
| `COARSE` | Restfehler unter 300 ns, gefilterte Halbschritte |
| `FINE` | Restfehler bei oder unter 150 ns — **hier soll er bleiben** |

Die Reihenfolge ist der Kern: **erst Frequenz, dann Uhr stellen.** Eine auf falscher Rate gestellte
Uhr läuft sofort wieder weg.

Danach der Status (echte Ausgabe im eingerasteten Zustand):

```
ptpf status
```

```
[PTPF] listening: on   samples: 259   last seq: 2903
[PTPF] rx sync: 259   follow_up: 259   sync without timestamp: 0
[PTPF] unmatched follow_up: 0   ring overflows: 0
[PTPF] offset: 170 ns   change per cycle: -26 ns
[PTPF] offset min/max: -85 / 244 ns   span: 329 ns
[PTPF] t1 (master): 363394912080 ns   t2 (ours): 363394916038 ns
[PTPF] master cycle: 125080000 ns   (D_const assumed: 3788 ns)
[PTPF] servo: FINE for 259 samples   rate error: 265 ppb   correction: 5656 ppb
[PTPF] increment: MAC_TI 40 ns + 3795/2^24 ns   steps: 234   rate writes: 8   outliers: 0
```

Worauf man schaut, in dieser Reihenfolge:

| Zeile | Was sie beweist |
|---|---|
| `sync without timestamp: 0` | jeder empfangene `Sync` trug einen Hardware-Zeitstempel. **Steht hier etwas > 0, fehlt der Treiber-Patch** — siehe Abschnitt 10 |
| `rx sync` = `follow_up` = `samples` | jedes Paar wurde gefunden und verrechnet |
| `offset` | Restfehler gegen die *angenommene* Ausrichtung, hier 170 ns |
| `offset min/max` / `span` | die Streuung über alle Proben seit dem letzten `ptpf reset`. **329 ns ist das Ergebnis, das zählt** |
| `change per cycle` | Restfrequenzfehler als Phasenänderung je Zyklus; nahe Null heißt: die Rate stimmt |
| `servo: FINE for N samples` | wie lange er schon oben ist. Große N ohne Rückfall = stabil |
| `increment` | was tatsächlich in `MAC_TI`/`MAC_TISUBN` steht: 40 ns plus Bruchteil, hier +5656 ppb |

Für eine saubere Messung die Statistik zurücksetzen, **nachdem** `FINE` erreicht ist — sonst
enthalten min/max noch den riesigen Offset vom Anfang:

```
ptpf reset
```

Dann 30 s warten und `ptpf status` erneut lesen. Erwartung: `span` im Bereich einiger hundert
Nanosekunden, `servo: FINE`.

---

## 7. Demo 4: der Beweis, dass beide dieselbe Zeit haben

Zwei Belege, ein grober und ein genauer.

**Genau, aus einer Zeile:** in `ptpf status` stehen `t1` (Sendezeitpunkt beim Master) und `t2`
(Empfangszeitpunkt bei uns) für **denselben Frame**:

```
[PTPF] t1 (master): 363394912080 ns   t2 (ours): 363394916038 ns
```

Differenz: **3958 ns**. Genau das ist zu erwarten — es ist die angenommene Laufzeit
`D_const = 3788 ns` plus die 170 ns Restfehler. Vor dem Einrasten liegen die beiden Werte um die
Betriebszeitdifferenz der Boards auseinander, also um Sekunden.

**Grob, aber unmittelbar:** dieselbe Wallclock-Abfrage wie in Abschnitt 3, jetzt im eingerasteten
Zustand, auf beiden Boards:

```
lan_read 0x00010074
```

| | vorher | eingerastet |
|---|---|---|
| Bridge | 210 s | 357 s |
| Follower | 216 s | 360 s |
| Unterschied | **6 s** | **3 s** — und das ist vollständig der Konsolen-Umlauf zwischen den zwei Abfragen |

Aussagekräftig ist nicht die 3, sondern dass der Unterschied auf die Latenz zusammenschrumpft: die
Uhr des Followers zeigt nicht mehr ihre eigene Betriebszeit, sondern die Zeit des Masters. Wer es
genauer will, nimmt `t1`/`t2` von oben — die Konsole ist als Zeitmessgerät ungeeignet.

---

## 8. Demo 5: Servo aus, Drift sichtbar machen, wieder einrasten

Das ist die überzeugendste Vorführung, weil man den Regler gegen die Physik arbeiten sieht. Auf dem
Follower:

```
ptpf servo off
ptpf reset
```

30 s warten, dann:

```
ptpf status
```

Jetzt läuft die Uhr frei. Gemessen an diesem Aufbau wandert der Offset um **etwa 640 ns pro
125-ms-Zyklus**, also rund 5 µs pro Sekunde — die beiden Quarze unterscheiden sich um ungefähr
5 ppm. Die Zeile `change per cycle` zeigt genau diesen Wert, und `offset min/max` läuft
auseinander.

Wieder einschalten:

```
ptpf servo on
```

Er sammelt erneut 16 Proben, korrigiert die Frequenz, stellt die Uhr und ist in wenigen Sekunden
zurück in `FINE`. **Wichtig:** ein `ptpf servo off` überlebt `ptpf off` und `ptpf on` — wenn der
Regler nichts tut, sagt `ptpf status` das ausdrücklich mit
`servo: OFF - measuring only, the clock is not touched`.

Zweiter Teil derselben Demo: **den Master abschalten** (`ptp stop` auf der Bridge). Der Follower
bekommt keine Proben mehr, hält sein letztes Inkrement und driftet mit dessen Restfehler weiter —
`samples` steigt nicht mehr. Nach `ptp start` läuft es weiter, ohne dass am Follower etwas zu tun ist.

---

## 9. Live mitlesen: eine Zeile pro Zyklus

```
ptpf log on
```

Danach schreibt der Follower je Zyklus eine Zeile:

```
[PTPF] seq=6788  offset=-573 ns  delta=-1 ns  HARDSYNC  rate=-569 ppb
[PTPF] seq=6789  offset=-573 ns  delta=0 ns  HARDSYNC  rate=-569 ppb
```

In einem Terminalprogramm ist das genau richtig zum Zusehen. **Vom PC aus dagegen nicht mit `cli.py`
mitlesen:** das Werkzeug wartet auf eine Pause in der Ausgabe, die bei laufendem Log nie kommt, und
bleibt hängen. Dafür gibt es:

```
python serial_capture.py COM10 30
```

Liest 30 Sekunden und hört auf; die Ausgabe landet in `_capture.out`. Log wieder abschalten mit
`ptpf log off`.

---

## 10. Störungssuche

| Symptom | Ursache | Prüfung / Abhilfe |
|---|---|---|
| `ts timeouts` zählt auf der Bridge hoch | der Sendezeitstempel wird nicht freigegeben | `lan_read 0x00000008` — steht dort `0x100`, klemmt `TTSCAA`. Der Zyklus löscht es eigentlich vor jedem Senden; bleibt es stehen, ist die Firmware nicht die aktuelle |
| `sync without timestamp` > 0 am Follower | **der Treiber-Patch für den RX-Zeitstempel fehlt** — typischerweise nach einem MCC „Generate Code" | in `follower/firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c` muss `ptp_follower_rx_hook` in `TC6_CB_OnRxEthernetPacket()` aufgerufen werden |
| Follower empfängt gar nichts (`rx sync: 0`) | Bus, PLCA oder Master aus | auf der Bridge `ptp status` (sendet sie?), auf dem Follower `stats` (steigt `eth0 RX`?), `plca_node` auf beiden — **genau ein Knoten muss Node-ID 0 haben** |
| `netinfo` sagt `Interface is down`, `lan_read` antwortet `result=-5` | der LAN865x-Treiber ist nie `READY` geworden, meist falsch aufgesteckte oder fehlende Click-Karte | Bootmeldungen ansehen: kommt `LAN865X Write OK: Addr=0x0004CA02`, ist der Treiber da |
| Servo pendelt zwischen `COARSE` und `FINE` | normal in den ersten Sekunden | wenn es dauerhaft bleibt: `ptpf reset`, 30 s beobachten. Dauerhaftes Pendeln mit großer `span` deutet auf eine gestörte Leitung |
| Servo bleibt in `HARDSYNC` | Frequenzfehler wird nicht korrigiert | `ptpf status`: passen `rate error` und `correction` zusammen? Ein `correction` weit unter dem anfänglichen `rate` deutet auf eine alte Firmware ohne die Filterinitialisierung |
| `unknown command` beim Einfügen | unsichtbares Zeichen aus dem Dokument | von Hand tippen oder `python cli.py --port <port> --read 1 "<kommando>"` verwenden |
| Beide Boards antworten nicht mehr am selben Port | falscher COM-Port nach Umstecken | `python setup_flasher.py --show` |

---

## 11. Kommandoreferenz

**Bridge (Grandmaster):**

```
ptp status
ptp start
ptp stop
ptp interval 125
mirror on
mirror off
stats
lan_read 0x00010074
```

**Follower:**

```
ptpf status
ptpf on
ptpf off
ptpf servo on
ptpf servo off
ptpf log on
ptpf log off
ptpf reset
stats
lan_read 0x00010074
```

**Beide, persistente Konfiguration:** `showenv`, `setenv <key> <wert>`, `saveenv`, `readenv`,
`resetenv`. Die Bridge kennt zusätzlich `ptp_auto` und `ptp_ival`.

**Vom PC:**

```
python setup_flasher.py --show
python cli.py --port COM8 --read 2 "ptp status"
python serial_capture.py COM10 30
python test_ptp.py
python test_rawtx_mirror.py
```

---

## 12. Was diese Demo nicht zeigt

Drei Dinge, damit die Zahlen nicht überinterpretiert werden:

- **Das Timing auf dem Draht.** Alles hier gemessene stammt aus Zeitstempeln der beiden MAC-PHYs und
  aus Zählern. Die Abstände der Frames auf der Zweidrahtleitung belegt nur ein Messgerät oder ein
  dritter, mithörender T1S-Knoten.
- **Die absolute Genauigkeit.** Der Servo regelt den *gemessenen* Offset auf Null, und diese Messung
  ist per Konstruktion um `echte Laufzeit − D_const` verschoben (angenommen: 3788 ns). Dieser
  konstante Anteil bleibt drin und wird bewusst nicht gemessen — Begründung in
  [LAN8651_TIME_SYNC.md §11.4](LAN8651_TIME_SYNC.md#114-one-way-only-what-that-costs-and-what-it-does-not).
  Zwischen zwei *Followern* hebt er sich weitgehend auf, gegen eine externe Referenz nicht.
- **Verhalten unter Volllast.** Gemessen ist, dass 20 Zyklen pro Sekunde ohne Fehler laufen. Der
  Durchsatztest mit `iperf` parallel zum PTP-Betrieb ist offen.

Der ehrliche Prüfstein für die absolute Ausrichtung ist der 1PPS-Ausgang auf DIOA4 beider Boards am
Oszilloskop, Flanke gegen Flanke — beschrieben in
[LAN8651_TIME_SYNC.md §8](LAN8651_TIME_SYNC.md#8-judging-the-result-without-a-ptp-analyser). Das ist
noch nicht umgesetzt.
