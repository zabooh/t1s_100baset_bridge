# Messergebnisse — PLCA-Verhalten am T1S-Bus

> **Was hier steht.** Die Ergebnisse der Tests aus
> [PLCA_BOOTSTRAP_TESTS.md](PLCA_BOOTSTRAP_TESTS.md) und ihre Beurteilung. Das Runbook sagt, *wie*
> gemessen wird; diese Datei, *was* herauskam und was daraus für
> [PTP_TIMEBASE_PLAN.md §2](PTP_TIMEBASE_PLAN.md#2-adressierung-und-identität) folgt.
>
> **Ausfüllregeln.** Rohausgaben **wörtlich** einsetzen, nicht zusammenfassen. Übersprungene Tests
> mit **Grund** vermerken. Jeder Durchlauf bekommt einen eigenen Abschnitt; frühere Ergebnisse werden
> nicht überschrieben.

---

## Inhalt

- [Durchläufe](#durchläufe)
- [Kurzfassung](#kurzfassung)
- [T0 — Grundlinie](#t0--grundlinie)
- [T1 — Beacon-Bit in PLCA_STATUS](#t1--beacon-bit-in-plca_status)
- [T2 — Sendet der Follower ohne Beacons?](#t2--sendet-der-follower-ohne-beacons)
- [T3 — Empfängt ein Knoten mit PLCA aus?](#t3--empfängt-ein-knoten-mit-plca-aus)
- [T4 — Doppelte Node-ID](#t4--doppelte-node-id)
- [T5 — Überlebt der Registerschreibzugriff einen Reinit?](#t5--überlebt-der-registerschreibzugriff-einen-reinit)
- [T6 — Kollisionserkennung](#t6--kollisionserkennung)
- [T7 — IP-Konflikt](#t7--ip-konflikt)
- [**Kontrollversuche K1–K3**](#kontrollversuche) — nachträglich, weil die erste Runde keinen blockierten Zustand kannte
- [Nebenbefunde](#nebenbefunde)
- [Gesamtbeurteilung](#gesamtbeurteilung)
- [Konsequenzen für den Plan](#konsequenzen-für-den-plan)
- [Offen](#offen)

---

## Durchläufe

| # | Datum | Repo-Commit | Bridge COM | Follower COM | Dritter Knoten am Bus | Wer |
|---|---|---|---|---|---|---|
| 1 | 2026-08-12 | `7a1354a` | **COM8** (Probe `…1049`) | **COM10** (Probe `…1290`) | **nein** | Claude, im Auftrag |

**Zuordnung empirisch bestimmt**, nicht aus `boards.json`: `stats` nennt auf COM8 `eth0` *und* `eth1`
(Bridge), auf COM10 nur `eth0` (Follower).

**Abweichung:** COM10 trägt die Probe-Seriennummer `ATML3264031800001290`, in
[boards.json](boards.json) steht als Follower `ATML3264031800001103`. Die Zuordnung dort ist also
veraltet oder gilt für ein anderes Board — `flash.bat --project follower` würde eine nicht
angeschlossene Probe adressieren.

**Nicht verifiziert:** ob die auf den Boards laufende Firmware dem Commit `7a1354a` entspricht. Der
Commit beschreibt den Repo-Stand während der Messung, nicht nachweislich den Build im Flash.

**Ausgangszustand nach der Messreihe wiederhergestellt:** beide Boards `PLCA_CTRL0 = 0x8000`,
Bridge Node 0 / Follower Node 7, `testmode 0`, PTP wieder **aus** mit Intervall 500 ms (wie
angetroffen).

---

## Kurzfassung

| Test | Frage | Ergebnis |
|---|---|---|
| **T0** | Messstrecke sauber? | **PASS** — Seriennummern verschieden, 5/5 Frames |
| **T1** | Beacon-Bit? | **`PLCA_STATUS` Bit 15** |
| **T2** | Sendet ohne Beacons? | **JA** — und in K3 **direkt bewiesen**, nicht nur verträglich |
| **T3** | Empfängt mit PLCA aus? | **JA, uneingeschränkt** — Servo blieb `FINE` |
| **T4b** | Doppelte Node-ID? | **Teilausfall ~17 %**, kein Totalausfall |
| **T5** | Reinit-fest? | **JA** — überlebt `testmode` und PMA-Reset |
| **T6** | Kollisionserkennung? | **nicht gefahren** — Datenblatt fehlt |
| **T7** | IP-Konflikt live? | **nein** — Follower steht auf `.201` |
| **K1** | Kann die Messkette eine Null erzeugen? | **JA, 0/5** — Instrument validiert |
| **K2** | `NODE_CNT` auf dem Follower wirksam? | **NEIN** — Nulleingriff, Versuch war fehlentworfen |
| **K2A** | Taktet PLCA das Senden überhaupt? | **JA, 0/5** bei ID außerhalb des Zyklus |
| **K3** | Fallback, nur Beacons verändert | **10/10** — inklusive der zuvor blockierten Frames |

**Die tragenden Annahmen sind bestätigt** (T2/K3, T3) — und anders als im ersten Durchgang liegt jetzt
ein Negativkontrollversuch (K2A) und eine Instrumentenvalidierung (K1) darunter.

---

## T0 — Grundlinie

| Prüfung | Soll | Bridge | Follower | Urteil |
|---|---|---|---|---|
| Serial Word 0, untere 24 Bit | verschieden | `0xCACED9` | `0x9D4C63` | **PASS** |
| abgeleitete MAC | verschieden | `00:04:25:CA:CE:D9` | `00:04:25:9D:4C:63` | **PASS** |
| `PLCA_CTRL0` | `0x00008000` | `0x00008000` | `0x00008000` | **PASS** |
| `PLCA_CTRL1` | Bridge ID 0, Follower ≠ 0 | `0x00000800` (cnt 8, id 0) | `0x00000807` (cnt 8, id 7) | **PASS** |
| `PLCA_STATUS` | Referenz | `0x00008000` | `0x00008000` | — |
| `COL_DET_CTRL0` | `0x00000083` | `0x00000083` | `0x00000083` | **PASS** |
| Verluststrecke `noip_send 5 200` | 5 von 5 | RX `0 → 5` | 5 gesendet | **PASS** |

**Rohausgabe (Auszug)**

```
[BRIDGE] showenv
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  eth1  ip 192.168.0.210  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  eth0  mac 00:04:25:CA:CE:D9
  eth1  mac 00:04:25:CA:CE:DA  (applied at boot)
  plca  id 0  count 8  (eth0/T1S)
  ptp   auto 0  interval 500 ms  (grandmaster on eth0; auto applies at boot)

[FOLLOWER] showenv
  eth0  ip 192.168.0.201  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  eth0  mac 00:04:25:9D:4C:63  (applied at boot)
  plca  id 7  count 8  (eth0/T1S)

[FOLLOWER] dump 0x008061FC 4
008061fc:  63 4c 9d 34                                       cL.4

[BRIDGE] stats (vor allem)
eth0 TX: ok=79 err=0 qFull=0 pend=0
eth0 RX: ok=0 err=0 nobufs=0 pend=0
eth1 TX: ok=0 err=0 qFull=0 pend=0
eth1 RX: ok=83 err=0 nobufs=0 pend=0
```

**Befund.** Messstrecke sauber. Die Seriennummer-Ableitung liefert auf diesen beiden Boards
unterschiedliche MACs — die untere 24-Bit-Annahme aus §2.1 hält, **allerdings bei n = 2**.

**`dump` zählt Bytes, nicht Worte.** `dump 0x008061FC 1` liefert ein einzelnes Byte; für Word 0
braucht es `dump 0x008061FC 4`. Im Runbook steht `1` — **zu korrigieren**.

**Kein dritter Knoten am Bus:** die Bridge hatte `eth0 RX: ok=0`, und es lief kein PTP-Verkehr
(siehe [Nebenbefunde](#nebenbefunde)). Damit ist **T4a nicht durchführbar**.

---

## T1 — Beacon-Bit in `PLCA_STATUS`

| Messpunkt | Wert |
|---|---|
| Bridge, Beacons an | `0x00008000` |
| Follower, Beacons an | `0x00008000` |
| Bridge, Beacons **aus** | `0x00000000` |
| Follower, Beacons **aus** | `0x00000000` |
| Follower, Beacons wieder an | `0x00008000` |
| **Differenzmaske** | **`0x8000` (Bit 15)** |

**Befund.** `PLCA_STATUS` Bit 15 = „PLCA betriebsbereit / Beacons vorhanden". Entscheidend: das Bit
fiel **auch auf dem Follower** auf 0, dessen eigenes `PLCA_CTRL0` dabei auf `0x8000` stehen blieb.
Ein Follower kann seinen Modus also **selbst erkennen**, ohne eigenes Protokoll.

**Einschränkung.** `PLCA_STATUS = 0` bedeutet auch „lokal abgeschaltet" (in T5 gemessen). Das Bit
allein unterscheidet **nicht** zwischen „keine Beacons" und „ich bin aus" — dafür muss `PLCA_CTRL0`
mitgelesen werden.

---

## T2 — Sendet der Follower ohne Beacons?

| Messpunkt | Wert |
|---|---|
| `noip_stat` Bridge, vorher | `RX=5` |
| gesendet vom Follower (PLCA blieb **an**) | 5 |
| `noip_stat` Bridge, nachher | `RX=10` |
| **angekommen** | **5 von 5** |
| `ptpf` samples während des Fensters | `46347 → 46464` (**+117**) |

**Rohausgabe**

```
[BRIDGE] lan_write 0x0004CA01 0x0000
>LAN865X Write OK: Addr=0x0004CA01 Value=0x00000000
>LAN865X Read OK:  Addr=0x0004CA01 Value=0x00000000
[FOLLOWER] noip_send 5 200      -> sent seq 6..10
[BRIDGE] noip_stat -> [NoIP] TX=0  RX=10
```

**Urteil: Fallback funktioniert.** Bei abgeschalteten Beacons konnte der Follower — mit weiterhin
aktiviertem PLCA — vollständig senden. Ein Bootstrap verklemmt also **nicht**, wenn ein Knoten sein
PLCA nicht selbst abschaltet.

**Nebenertrag:** der Follower empfing im selben Fenster weiter (+117 Samples). Ein Sender ohne PLCA
erreicht einen Empfänger mit PLCA.

> **Dieser Test allein war zweideutig** — auf einem leeren Bus mit zwei Knoten hätte der Follower
> auch ohne jeden Fallback senden können, weil PLCA nichts einschränken muss. Aufgelöst durch
> [K2A und K3](#kontrollversuche): PLCA blockiert nachweislich (0/5), und **allein das Wegnehmen der
> Beacons** hebt die Blockade auf (10/10). Erst damit ist der Befund belastbar.

**Grenze des Tests:** geringe Buslast (ein weiterer Sender, PTP mit 100 ms). Erfolg hier belegt keine
Robustheit unter Last.

---

## T3 — Empfängt ein Knoten mit PLCA aus?

| Messpunkt | Wert |
|---|---|
| `ptpf` samples vorher | `46759` |
| PTP-Intervall | **100 ms** (für die Messung von 500 auf 100 gestellt) |
| Wartezeit | 10 s, plus Kommando-Overhead |
| `ptpf` samples nachher | `46900` |
| **Zuwachs** | **+141** |
| Servo danach | `FINE`, Offset **−22 ns**, rate error 0 ppb |

**Urteil: Empfang ist modusunabhängig.** Nicht nur kamen Frames an — die Zeitstempel waren weiterhin
gut genug, dass der Servo im Zustand `FINE` blieb. Die Annahme, dass ein Knoten die
Fensterankündigung **in jedem Modus** hört, ist bestätigt.

**Nebenertrag:** die Zeitbasis übersteht ein Bootstrap-Fenster, ohne aus dem Tritt zu kommen.

---

## T4 — Doppelte Node-ID

**Variante gefahren: T4b (Ersatzvariante).** T4a nicht möglich — kein dritter Knoten am Bus.
Follower auf `plca_node 0` gesetzt, also **Duplikat des Koordinators**, mit zwei Beacon-Quellen.

Gegenkontrolle mit **demselben** Kommando auf gesundem Bus (Follower Node 7):

| Buszustand | gesendet | angekommen | Verlust |
|---|---|---|---|
| **Node 7 (gesund)** | 30 | 30 | **0 %** |
| **Node 0 (Duplikat)** | 30 | 25 | **≈17 %** |

| weitere Messpunkte | Wert |
|---|---|
| `ptpf` samples während des Duplikats | `48922 → 49115` (**+193**) — Empfang **unbeeinträchtigt** |
| Servo während des Duplikats | blieb `FINE` |
| `PLCA_STATUS` Follower als Node 0 | `0x00000000` |
| `PLCA_STATUS` Bridge | `0x00008000` (unverändert) |

**Urteil: Teilausfall, kein Totalausfall.** Der Verlust ist **sporadisch** — ein Burst von 5 kam
vollständig durch, andere verloren je einen Frame. Der Empfang ist gar nicht betroffen.

**Das ist das ungünstigste Fehlerbild:** kein klarer Ausfall, den man bemerkt, sondern gelegentlicher
Sendeverlust bei ansonsten funktionierendem Bus. Genau der Grund, weshalb ein *gültiger*
Compile-Default für `plca_id` die eigentliche Fehlerquelle ist.

**Einschränkungen, die mitgelesen werden müssen:** Ersatzvariante mit **zwei Beacon-Quellen**, nicht
zwei gewöhnlichen Followern; n = 30 Frames je Zustand; geringe Buslast. Die 17 % sind eine
Größenordnung, keine Kennzahl.

---

## T5 — Überlebt der Registerschreibzugriff einen Reinit?

| Störung | `PLCA_CTRL0` danach | `PLCA_CTRL1` danach |
|---|---|---|
| nach `lan_write … 0x0000` | `0x00000000` | — |
| nach `testmode 4` → `testmode 0` | **`0x00000000`** | — |
| nach PMA-Reset (`lan_rmw 0x000308F9 0x8000 0x8000`) | **`0x00000000`** | `0x00000807` (**erhalten**) |
| nach `lan_write … 0x8000` | `0x00008000`, `PLCA_STATUS` sofort `0x8000` | — |

**Urteil: der Treiber schreibt PLCA nicht neu.** Ein einmaliger Registerschreibzugriff hält, auch
über einen Link-Abbruch durch `testmode 4` und über einen PMA-Reset. Der PMA-Reset ließ die Node-ID
in `PLCA_CTRL1` unangetastet (Bank MMS 4 ist davon nicht betroffen).

**Einschränkungen.** `testmode 4` schaltet nur den **eigenen** Sender hochohmig — ob der lokale
Treiber dabei überhaupt ein Link-Down-Ereignis sieht, ist unklar. Ein **physisches Abziehen des
Kabels** ist damit nicht abgedeckt und bleibt offen. Der `[VERIFY] FAIL` beim PMA-Reset ist korrekt:
`RST` ist selbstlöschend (so dokumentiert).

---

## T6 — Kollisionserkennung

**Nicht gefahren.** Grund: Bitbedeutung von `COL_DET_CTRL0` (`0x00040087`, Init-Wert `0x83`) liegt
nicht vor. Das Runbook verlangt ausdrücklich, ohne sie nicht an dem Register zu drehen.

Gemessen wurde nur der Ist-Wert: **`0x00000083` auf beiden Boards**, wie vom Treiber gesetzt.

---

## T7 — IP-Konflikt

| Messpunkt | Wert |
|---|---|
| `showenv` Bridge `eth0` | `192.168.0.200` |
| `showenv` Follower `eth0` | **`192.168.0.201`** |
| `ping 192.168.0.200` | 3/3, 1 ms |
| `ping 192.168.0.201` | 3/3, 1–3 ms |
| `arp -a` | `.200 → 00-04-25-ca-ce-d9`, `.201 → 00-04-25-9d-4c-63` |

**Urteil: kein Konflikt.** Der Follower ist bereits auf `.201` provisioniert (per `saveenv`), die
vorhergesagte Kollision mit `.200` **läuft nicht**. Beide Adressen antworten, die ARP-Einträge sind
eindeutig und passen zu den abgeleiteten MACs.

**Was das nicht heißt.** Die Ursache ist behoben, nicht der Mechanismus: ein **frisch geflashtes**
Board käme weiter mit dem Compile-Default `.200` hoch und kollidierte mit der Bridge. §2.2 bleibt
also nötig — nur nicht als Reparatur eines laufenden Fehlers, sondern als Absicherung für neue
Boards.

**Nebenbefund:** dass `.201` überhaupt antwortet, belegt zugleich, dass die Bridge korrekt von `eth1`
nach `eth0` weiterleitet.

---

## Phase B — Kern der Zeitbasis, auf Hardware verifiziert

Gebaut und gemessen 2026-08-12. Modul
[ptp_timebase.c](follower/firmware/src/ptp_timebase.c) /
[.h](follower/firmware/src/ptp_timebase.h), gefüttert aus `fol_consume()` mit
`(host_sync, t1)`, CLI-Gruppe **`tbase`**.

| Prüfung | Ergebnis |
|---|---|
| Zeit bis `LOCKED` | **< 10 s** (2 Gewinner = 64 Paare bei 100 ms) |
| Steigung nach 60 s | **+62 545 ppb**, nach Ausfall und Erholung **+62 549 ppb** |
| Gegenprobe gegen Phase A (offline, unabhängig) | −62,7 ppm — **0,2 ppm Abweichung**, Vorzeichen erklärt sich aus der Definition |
| letztes Residuum | 9–15 µs — passt zu Phase As Median 14,9 µs |
| Gewinnerspanne, Ring voll | **18,6 µs** (Blöcke von 32, Basis 48 s) |
| verworfene Paare | **0** von 1413 |
| Re-Anker | 0 (erst nach 60 s Tickspanne fällig) |
| `Now()` monoton | ja, 37516,555 s → 37518,075 s bei 1,52 s Wandzeit |
| **Rundreise `LocalFor` → `Convert`** | **−8 ns bei 1 s Vorlauf, −4 ns bei 60 s** |
| Holdover erkannt | `HOLDOVER, usable: no` nach 6 s Masterausfall |
| Erholung | `LOCKED` innerhalb 5 s, Steigung auf 4 ppb identisch |

**Die Rundreise ist das Ergebnis, auf dem Phase C aufsetzt.** `LocalFor()` und `Convert()` stimmen
auch 60 s in die Zukunft auf einstellige Nanosekunden überein — das Q24-Festkomma trägt, und der
Trigger kann sein lokales Ziel ohne nennenswerten Umrechnungsfehler bestimmen.

**Das Vorzeichen der Steigung ist kein Widerspruch.** Das Modul meldet ns pro Tick gegen nominal,
`tb_capture.py` rechnete lokal gegen Master. Ist der Tick um 62,5 ppm *länger* als nominal, läuft der
Zähler entsprechend *langsamer* — dieselbe Größe, andere Blickrichtung. Die Beträge stimmen auf
0,2 ppm, was für zwei unabhängige Implementierungen (Firmware-Festkomma gegen Host-Fließkomma) die
eigentliche Bestätigung ist.

**Ein Nebenbefund zur Kennzahl selbst:** die Gewinnerspanne stand kurz nach dem Einrasten bei
370–397 µs und fiel erst auf 18,6 µs, als der Ring einmal durchgelaufen war. Grund: die ersten
Gewinner wurden noch gegen die **nominale** Steigung bewertet, ihr Residuum trägt also den
aufgelaufenen Modellfehler. Kein Fehler, aber die Zahl ist erst nach einem vollen Ringumlauf ab
`LOCKED` aussagekräftig — beim Ablesen mitdenken.

**Und der Kontrast, der den Sinn des Holdover-Zustands belegt** — beide Zeilen im selben Augenblick,
bei stehendem Grandmaster:

```
[TBASE] state: HOLDOVER   usable: no   age: 8286 ms
[PTPF] servo: FINE for 1292 samples   rate error: 1 ppb   correction: 5104 ppb
```

Damit ist die in Nebenbefund 2 beschriebene Lücke des bestehenden Servos direkt gegenübergestellt: das
neue Modul verweigert sich (`usable: no`, worauf sich der Trigger in C.6 verlässt), der Servo meldet
weiter `FINE`.

---

## Kontrollversuche

Nachträglich gefahren, weil die Ergebnisse der ersten Runde durchweg positiv ausfielen und keiner
davon einen **blockierten** Zustand kannte. Ohne einen solchen ist „5 von 5 angekommen" nicht von
„PLCA schränkt hier ohnehin nichts ein" zu unterscheiden.

Alle Zählerstände einer durchlaufenden Reihe, deshalb geht die Bilanz am Ende auf.

| # | Eingriff | Stimulus-Nachweis | gesendet | angekommen | Bridge `RX` |
|---|---|---|---|---|---|
| **K1** | Follower `testmode 4` (Sender hochohmig) | `T1STSTCTL=0x8000`, `[VERIFY] PASS` | 5 | **0** | `60 → 60` |
| **K2** | Follower `PLCA_CTRL1 = 0x0107` (`NODE_CNT=1`, ID 7) | Rücklesen `0x0107`, `PLCA_STATUS 0x8000` | 5 | **5** | `60 → 65` |
| **K2A** | Follower `PLCA_CTRL1 = 0x0814` (ID **20**, Zyklus 8) | Rücklesen `0x0814`, `PLCA_STATUS 0x8000` | 5 | **0** | `65 → 65` |
| **K3** | wie K2A, **zusätzlich** Bridge `PLCA_CTRL0 = 0x0000` | Follower `PLCA_STATUS 0x0000` | 5 | **10** | `65 → 75` |

Bilanz: Follower `TX 65 → 85` (20 gesendet), Bridge `RX 60 → 75` (15 angekommen). Differenz **5** —
genau die fünf aus K1, bei hochohmigem Sender.

### K1 — die Messkette kann eine Null erzeugen

Bei hochohmigem Sender kam **nichts** an, während der TX-Zähler des Followers weiterlief. Damit ist
belegt, dass der RX-Zähler der Bridge tatsächlich Zustellung über den Draht abbildet und nicht
irgendetwas mitzählt. **Erst dadurch bekommen alle Zählerergebnisse dieser Datei Gewicht** — auch das
30/30 gegen 30/25 aus T4b.

### K2 — der Versuch war fehlentworfen

`NODE_CNT` auf einem **Follower** ist ein Nulleingriff: die Zykluslänge gibt der **Koordinator** vor,
ein Follower braucht nur seine eigene ID. Der Knoten behielt seinen Slot und sendete vollständig. Das
ist kein Messfehler, sondern ein **Befund über PLCA** — und eine Falle für jeden, der einen Knoten
über `plca_cnt` stilllegen will.

### K2A — PLCA taktet das Senden nachweislich

Mit ID **20** bei einem Koordinator-Zyklus von 8 Beats kam **nichts** an. PLCA schränkt in diesem
Aufbau also wirklich ein, und damit ist T2 keine Trivialität mehr.

**Wichtig dabei:** `PLCA_STATUS` blieb `0x00008000`. Das Bit meldet „Beacons vorhanden / betriebs­fähig"
und **nicht** „ich habe einen gültigen Slot". Ein Knoten mit falscher ID sieht im Status **gesund
aus** und kann trotzdem nicht senden — diagnostisch die unangenehmste Kombination.

### K3 — der Fallback, mit nur einer geänderten Variablen

Ausgangspunkt war der in K2A **blockierte** Knoten, unverändert. Einziger Eingriff: die Bridge nahm
die Beacons weg. Ergebnis **10 angekommen bei 5 gesendet** — die fünf in K2A blockierten Frames waren
also nicht verworfen, sondern **zurückgehalten**, und liefen zusammen mit den neuen ab, sobald PLCA
nicht mehr taktete.

Das ist der Beweis, der T2 gefehlt hat: **derselbe Knoten, dieselbe Konfiguration, nur Beacons an
oder aus — 0 gegen 10.**

**Nebenbefund:** blockierte Frames werden **gehalten, nicht verworfen**. Für den Bootstrap heißt das,
dass ein Knoten mit falscher ID beim Öffnen des Fensters einen Rückstau auf den Bus entlässt — beim
Dimensionieren des Fensters mitdenken.

---

## Phase A — (L, t1)-Messreihe für die MCU-Zeitbasis

Durchgeführt 2026-08-12 gemäß [PTP_TIMEBASE_PLAN.md Phase A](PTP_TIMEBASE_PLAN.md#phase-a--messreihe-ohne-einen-algorithmus-zu-bauen).
Instrumentierung: `ptpf tb on` gibt je Zyklus `[TB] seq= L= t1= t2=` aus, `L` ist
`SYS_TIME_Counter64Get()` **beim Sync** (neues Feld `host_sync`, additiv neben dem vom Servo
benutzten `host`). Auswertung mit `tb_capture.py`.

### A.1 Ergebnis: blockiert — aber nicht am erwarteten Engpass

| Größe | 60-s-Fenster | 180-s-Fenster, ~20 min später |
|---|---|---|
| Paare | 599 | 1798 |
| Sequenzlücken | **0** | **0** |
| `t1`-Intervall (Median) | 100,139 ms | 100,133 ms |
| lokale Tickquelle gegen Master-Wallclock | **+601,3 ppm** | **+783,0 ppm** |
| Residuum nach linearem Detrend, Median | 455 µs | 1903 µs |
| Residuum, Maximum | 1110 µs | 3997 µs |
| Gewinnerspanne nach Min-Filter | 882 µs (Blöcke von 32) | 3780 µs (Blöcke von 64) |

**Das Residuum wächst mit der Fensterlänge.** Das ist die Signatur einer **wandernden Rate**, nicht
die von Übergabejitter: Jitter wäre fensterlängenunabhängig. Bestätigt durch Polynomfits über das
60-s-Fenster — Grad 1: Median 478 µs, Grad 2: 523 µs, Grad 3: **302 µs**, Maximum 463 µs. Ein
Übergabejitter ließe sich durch höhere Ordnung nicht wegfitten.

### A.2 Ursache: die MCU hat keine Quarzreferenz

| Ebene | Konfiguration | Beleg |
|---|---|---|
| `GCLK_GENCTRL[2]` | `SRC(6)` = **DFLL48M**, `DIV(48)` → 1 MHz | [plib_clock.c:127](follower/firmware/src/config/default/peripheral/clock/plib_clock.c#L127) |
| DPLL0 | Referenz Generator 2, `LDR = 119` → ×120 → 120 MHz | [plib_clock.c:66-69](follower/firmware/src/config/default/peripheral/clock/plib_clock.c#L66-L69) |
| `GCLK_GENCTRL[1]` | `SRC(7)` = DPLL0, `DIV(2)` → **60 MHz für TC0/SYS_TIME** | [plib_clock.c:117](follower/firmware/src/config/default/peripheral/clock/plib_clock.c#L117) |
| `OSCCTRL_Initialize()` | **leer** — kein XOSC aktiviert | [plib_clock.c:44](follower/firmware/src/config/default/peripheral/clock/plib_clock.c#L44) |
| `DFLL_Initialize()` | **leer**; **null** Schreibzugriffe auf `XOSCCTRL`/`DFLLCTRLA`/`DFLLCTRLB`/`DFLLMUL` | grep über die Datei |

Der DFLL48M läuft also **open loop**, ohne externe Referenz, und die gesamte MCU-Zeitbasis — CPU,
TC0, `SYS_TIME` — hängt daran. Die gemessenen +601 bzw. +783 ppm und die ~180 ppm Wanderung binnen
20 Minuten sind das erwartbare Verhalten eines untrimmten internen Oszillators.

**Gegenprobe, die die Zuordnung sichert:** der Servo korrigiert die **Wallclock** des Followers um nur
~5,1 ppm gegen den Master, bei Ratenfehlern von **1–12 ppb** (`ptpf status`). Die PHY-Uhren sind
quarzbasiert und einwandfrei. Der Fehler sitzt ausschließlich auf der MCU-Seite.

### A.3 Was daraus folgt

**Die Annahme in [B.4](PTP_TIMEBASE_PLAN.md#phase-b--kern-der-zeitbasis) trifft nicht zu.** Dort
steht, zwischen zwei Updates bleibe „nur die Änderung der Rate übrig — im Wesentlichen
Temperaturdrift, Größenordnung 1 ppm pro °C". Gemessen sind **Zehner-ppm pro Minute**. Damit ist die
gemessene Rate praktisch sofort veraltet, und der Holdover-Zweig von
[Phase G](PTP_TIMEBASE_PLAN.md#phase-g--demonstration-und-was-sie-beweist) — „driftet langsam
auseinander, ~10 ns/s" — ist um Größenordnungen zu optimistisch.

**Δ selbst ist noch nicht gemessen.** Es ist von der Uhrenwanderung nicht zu trennen, solange die
Zeitbasis wandert. Beste verfügbare Obergrenze: **Median ~302 µs, Maximum ~463 µs** (Grad-3-Fit über
60 s) — und darin steckt weiterhin Wanderung.

**Der Engpass ist nicht die Hauptschleife.** Phase A war darauf angelegt, zwischen „einstellige µs"
und „hunderte µs durch Hauptschleifentakt" zu entscheiden. Beides trifft nicht: die Zeitbasis selbst
ist unbrauchbar, und das war auf keiner Risikoliste des Plans.

**Nächster Schritt, bevor Phase A wiederholt wird:** DPLL0 auf **XOSC0** referenzieren (das Curiosity
Ultra hat einen Quarz) statt auf den open-loop DFLL. Das ist eine Änderung im MCC-Clock-Configurator,
also mit Regenerierungsgefahr — danach `python test_mirror.py`. Erst mit quarzbasierter Zeitbasis hat
eine Δ-Messung Aussagekraft.

### A.4 Versuch, die Taktreferenz auf XOSC0 zu stellen — **fehlgeschlagen**

**Ground Truth zuerst, risikofrei per pyOCD** (Register schreiben, lesen, zurückstellen, Reset — keine
Firmware-Änderung, kein Takt umgeschaltet). Adressen aus dem DFP-Header: `OSCCTRL` bei `0x40001000`,
`STATUS` bei `+0x10`, `XOSCCTRL[0]` bei `+0x14`, `DPLL[0]` bei `+0x30`.

| Schritt | Ergebnis |
|---|---|
| `XOSCCTRL[0] = 0x00F00002` (ENABLE, XTALEN=0, **STARTUP=0xF**), 4 schnelle Lesevorgänge | `XOSCRDY0` = **0** |
| dasselbe mit **STARTUP=0**, danach 3 s echte Wartezeit | `STATUS = 0x00010101` → `XOSCRDY0` = **1** |
| Clock-Failure-Detector (`CFDEN`, `SWBEN`=0), 4 s beobachtet | `XOSCFAIL0` = **0**, `XOSCRDY0` bleibt 1 |

Erste Lehre: **ein zu großes `STARTUP` lässt „XOSC0 ist nicht da" aussehen, wo nur der Anlaufzähler
noch läuft.** Der erste Versuch wäre als Negativbefund fehlinterpretiert worden.

**Daraufhin ein Handpatch** in [plib_clock.c](follower/firmware/src/config/default/peripheral/clock/plib_clock.c):
XOSC0 im Externtakt-Modus in `OSCCTRL_Initialize()`, und `DPLLCTRLB = REFCLK(XOSC0) | DIV(5)` — bei
12 MHz wären das `12 MHz / (2·(5+1)) = 1 MHz`, also genau die Referenz, die DPLL0 vorher vom DFLL
bekam; `LDR = 119` und alles dahinter unverändert. Dazu ein begrenzter Poll auf `XOSCRDY0` mit
Rückfall auf den DFLL-Weg.

**Ergebnis: das Board bootet nicht.** Zustand am halted Core:

| Register | Wert | Bedeutung |
|---|---|---|
| `OSCCTRL_STATUS` | `0x00000101` | `XOSCRDY0` = 1, DFLLRDY = 1, **DPLL0-Lock = 0** |
| `XOSCCTRL[0]` | `0x00000002` | XOSC0 an, Externtakt — der XOSC0-Zweig **wurde** genommen |
| `DPLLCTRLB` | `0x00050040` | `DIV = 5`, `REFCLK = 2` (XOSC0) — Konfiguration landete wie geplant |
| `DPLLSTATUS` | `0x00000000` | **LOCK = 0, CLKRDY = 0** |

Der Boot hängt also im `while(!LOCK && !CLKRDY)`-Lauf von `FDPLL0_Initialize()`.

**Zwei Lehren:**

1. **`XOSCRDY0` und ein stiller CFD beweisen nicht, dass ein Takt der erwarteten Frequenz anliegt.**
   Entweder führt XIN0 nicht die 12 MHz (auf dem Board sitzen auch ein 32,768-kHz- und ein
   50-MHz-Teil), oder `XOSCRDY0` spiegelt nur den Anlaufzähler. Bei 50 MHz ergäbe `DIV = 5`
   4,17 MHz — über dem DPLL-Referenzbereich, also kein Lock. Das passt zum Befund.
2. **Der Rückfall war unzureichend.** Er deckte „`XOSCRDY0` kommt nicht" ab, aber nicht den
   *häufigeren* Fall „Takt da, falsche Frequenz". **Jeder weitere Versuch muss auch den DPLL-Lock
   begrenzt abwarten und dann zurückfallen** — sonst ist ein Fehlversuch jedes Mal ein nicht
   bootendes Board.

**Wiederhergestellt:** `git checkout -- plib_clock.c`, neu gebaut, geflasht; Board läuft wieder
(`.201`, Node 7, `ptpf tb` erhalten). Die `ptp_follower.c`-Instrumentierung blieb unangetastet.

**Was zum Weitermachen fehlt** — eines von beidem:

- **Der Schaltplan**: welcher der drei Oszillatoren liegt auf XIN0/PA14?
- **Oder eine Frequenzmessung ohne Umschalten**: XOSC0 auf einen freien GCLK-Generator und von dort
  auf einen freien TC (TC1 ist frei), dessen Zählerstand gegen ein bekanntes `SYS_TIME`-Intervall
  verglichen wird. Das liefert die Frequenz numerisch, ohne den Systemtakt anzufassen — also ohne
  Hängerisiko.

**Der dauerhafte Weg bleibt der MCC-Clock-Configurator**, weil er die Board-Frequenz aus dem BSP
kennt. Ein Modell-Patch nach dem MCC-Runbook war nicht möglich: das Modell führt **keine
Taktkomponente** (`components/` hat `core`, `tc0`, `sys_time`, `sercom1` …, aber nichts zum Takt, und
`core.yml` enthält kein einziges `XOSC`/`DFLL`/`DPLL`-Symbol). Der Configurator stand nie auf etwas
anderem als dem Default, ein Patch hätte MCC-Symbole erfinden müssen.

### A.5 XIN0 gemessen, Taktreferenz umgestellt — **Phase A besteht**

**Die Frequenz auf XIN0, gemessen statt angenommen.** Aufbau ohne Firmware-Änderung und ohne
Umschalten des Systemtakts, alles per pyOCD: GCLK-Generator 3 aus XOSC0 (`GENCTRL[3] = 0x00010100`,
`SRC = 0`), von dort auf **TC2** (`PCHCTRL[26] = 0x43` — TC2 hat GCLK-Kanal 26 und ist damit
unabhängig von TC0/`SYS_TIME`; **TC0 und TC1 teilen sich Kanal 9**, TC1 wäre also nicht nutzbar
gewesen), TC2 als 32-Bit-Zähler, `COUNT` über `CTRLBSET.CMD = READSYNC` gelesen.

```
COUNT1 = 0xD06BE91D,  COUNT2 = 0x12038F54  (ein 32-Bit-Umlauf)
Delta  = 1 100 457 527 Zaehlwerte in 21,945 s
==>  50,147 MHz   (+0,29 % gegen 50 MHz, im Rahmen der Host-Zeitmessung)
```

**Auf XIN0 liegt der `DSC1001CI2-050.0000`, also 50 MHz — der RMII-Referenztakt.** Nicht die 12 MHz.
Damit ist der Fehlschlag aus A.4 exakt erklärt: `DIV = 5` ergab 4,167 MHz Referenz, und `LDR = 119`
hätte 500 MHz verlangt.

**Zwei Fallstricke auf dem Weg dorthin:**

- **32-Bit-Modus braucht das TC-Paar.** Mit nur TC2s APB-Takt (`APBBMASK` Bit 13) lief der Zähler
  16-bittig und lief alle 1,3 ms über — die ersten Messwerte waren Unsinn (zweiter Wert kleiner als
  der erste). Erst mit **TC3s APB-Takt** (Bit 14) dazu zählt TC2 echte 32 Bit.
- **DPLL-Lock taugt nicht als Frequenzdetektor.** Der Versuch, über DPLL1 zwischen 12 und 50 MHz zu
  unterscheiden, lieferte in **beiden** Konfigurationen `DPLLSTATUS = 0x3` (Lock + CLKRDY). Die
  32 kHz…3,2 MHz und 96…200 MHz aus dem Datenblatt sind *spezifizierte Betriebsbereiche*, keine
  harten Grenzen — der DPLL rastet auch außerhalb ein. Nur Zählen hilft.

**Die richtige Rechnung und der Patch.** `DIV = 9` → 50 MHz / (2·10) = **2,5 MHz** Referenz (im
Bereich), `LDR = 47` → 2,5 MHz × 48 = **120 MHz exakt**, ohne `LDRFRAC`. GCLK0 = 120 MHz und
GCLK1 = 60 MHz bleiben damit unverändert. Diesmal mit **begrenztem** Lock-Wartelauf und Rückfall auf
den DFLL-Weg, so dass ein falscher Takt höchstens die schlechte Zeitbasis kostet und nie das Board.

Verifiziert nach dem Flashen: `OSCCTRL_STATUS = 0x00010001` (XOSCRDY0 = 1, DPLL0-Lock = 1),
`DPLLCTRLB = 0x00090040` (DIV = 9, REFCLK = XOSC0). Dass die Konsole überhaupt lesbar antwortet, ist
der Zusatzbeweis für die richtige Frequenz — bei falschem Systemtakt wäre die UART-Baudrate daneben.

**Phase A, dieselbe Messung wie in A.1, 180-s-Fenster:**

| Größe | DFLL (open loop) | **XOSC0 (50 MHz)** |
|---|---|---|
| Rate gegen Master-Wallclock | +783,0 ppm | **−62,7 ppm** |
| Residuum nach Detrend, Median | 1903 µs | **14,9 µs** |
| Residuum, Maximum | 3997 µs | **62,7 µs** |
| Residuum, p99 | — | **33,3 µs** |
| Gewinnerspanne nach Min-Filter (Blöcke von 64) | 3780 µs | **9,1 µs** |
| Ratenunsicherheit | ±22,2 ppm | **±0,053 ppm** |
| Holdover-Drift zweier Boards | 2662 µs/min | **6,3 µs/min** |
| Samples bei genau 1,092 ms | 48 | **0** |
| Sequenzlücken | 0 | 0 |

**Urteil: Phase A besteht.** Drei Dinge daran sind wichtig:

1. **Das Residuum wächst nicht mehr mit der Fensterlänge** — über 180 s bleibt das Maximum bei 62,7 µs
   statt auf 4 ms zu laufen. Die Uhrenwanderung ist weg, und damit ist Δ **zum ersten Mal wirklich
   gemessen**: Median 14,9 µs, Streuung 6,0 µs, p99 33,3 µs. Das ist Hauptschleifen-Jitter, wie der
   Plan es als Möglichkeit benannt hatte.
2. **Der Min-Filter funktioniert jetzt.** 9,1 µs Gewinnerspanne ist das „einstellige µs", an dem
   [Phase A](PTP_TIMEBASE_PLAN.md#phase-a--messreihe-ohne-einen-algorithmus-zu-bauen) den Plan
   aufgehängt hat. Vorher war der Filter wirkungslos, weil der Boden selbst wanderte.
3. **Die 48 Samples bei „genau einer `SYS_TIME`-Periode" waren ein Artefakt** der Wanderung, keine
   verpassten Überläufe. Jetzt sind es null. Meine Deutung in A.1 war insoweit falsch.

**Einschränkungen.** Nur **ein** Board ist umgestellt; für Follower-zu-Follower-Gleichzeitigkeit
brauchen alle Knoten den Patch (die Bridge hat **dieselbe** Taktkonfiguration, `diff` über
`plib_clock.c` war identisch). Die −62,7 ppm sind 50-MHz-MEMS gegen LAN8651-Quarz und damit erwartbar.
Und die Ratenunsicherheit skaliert mit der Basislänge — ±0,053 ppm gilt für 172 s, längere Fenster
werden besser.

### A.6 Nebenbefund: `cli.py` kann kein Dauerlog aufnehmen

`drain()` verlängert sein Lesefenster bei **jedem** eintreffenden Byte um 0,5 s
([cli.py:24-33](cli.py#L24-L33)). Gegen ein Log, das alle 100 ms eine Zeile schickt, kehrt es **nie**
zurück — der Aufruf hängt und hält den COM-Port, woraufhin der **nächste** Aufruf beim Öffnen
blockiert. Das sieht wie ein abgestürztes Board aus und ist keines; ein pyOCD-Reset „behebt" es
scheinbar, weil er den Stream stoppt.

**Deshalb gibt es `tb_capture.py`:** hartes Zeitfenster, schaltet das Log im `finally` immer ab.
`--listen` von `cli.py` hat dasselbe Problem und ist für Dauerlogs ebenfalls unbrauchbar.

---

## Nebenbefunde

**1. `noip_send N gap` mit N > 1 liefert falsche Sequenznummern — die Nutzlast wird vor dem Senden
überschrieben.** Die Bridge protokollierte bei einem Burst von 5:

```
[NoIP-RX] #31 seq=36 from 00:04:25:9D:4C:63 len=64 ts=733044 ms
[NoIP-RX] #32 seq=40 from 00:04:25:9D:4C:63 len=64 ts=734044 ms
[NoIP-RX] #33 seq=40 from 00:04:25:9D:4C:63 len=64 ts=734044 ms
[NoIP-RX] #34 seq=40 from 00:04:25:9D:4C:63 len=64 ts=734046 ms
[NoIP-RX] #35 seq=40 from 00:04:25:9D:4C:63 len=64 ts=734046 ms
```

Gesendet wurden `36,37,38,39,40`. Angekommen: `36` und **viermal `40`**, die letzten vier mit
praktisch identischem Zeitstempel. **Gegenkontrolle mit fünf einzelnen `noip_send 1 0`:**

```
[NoIP-RX] #36 seq=41 … ts=796315 ms
[NoIP-RX] #37 seq=42 … ts=797775 ms
[NoIP-RX] #38 seq=43 … ts=799267 ms
[NoIP-RX] #39 seq=44 … ts=800766 ms
[NoIP-RX] #40 seq=45 … ts=802219 ms
```

Einzelsendungen sind korrekt. Deutung: der erste Frame geht synchron raus, die restlichen vier werden
in die Rohsende-Queue eingereiht und **teilen einen Puffer**, dessen Inhalt der Busy-Wait in
`cmd_noip_send()` weiterschreibt, bevor die Queue geleert wird. Das passt zur dokumentierten
Fünf-Frames-Grenze und dazu, dass der Busy-Wait den Treiber nicht bedient (`CLAUDE.md` Abschnitt 6).

**Folge für die Methodik:** Sequenznummern aus Mehrfach-Bursts sind **kein** Verlust-Oracle. Gültig
sind nur die TX-/RX-**Zähler**. Das Runbook behauptet das Gegenteil und ist zu korrigieren. Alle
Verlustaussagen hier beruhen auf Zählern, der Vergleich gesund/Duplikat ist damit gültig (gleiches
Kommando, gleicher Fehler, nur der PLCA-Zustand unterschieden).

**2. `ptpf status` meldet `servo: FINE`, obwohl seit Stunden kein Sync ankam.** Zu Beginn der
Messreihe standen die Zähler bei `samples: 45412`, und sie **änderten sich über 6 s nicht**, während
die Bridge `sending: off, tx sync: 0` meldete. Trotzdem stand dort:

```
[PTPF] servo: FINE for 45262 samples   rate error: -14 ppb   correction: 5118 ppb
[PTPF] offset: -6 ns   change per cycle: 1 ns
```

Alle diese Werte waren Altbestand von vor dem letzten Bridge-Neustart. Der Zustand „frei laufend, seit
X Sekunden kein Sync" wird nicht ausgewiesen — genau die Lücke, die
[PTP_TIMEBASE_PLAN.md B.6](PTP_TIMEBASE_PLAN.md#phase-b--kern-der-zeitbasis) für die neue Zeitbasis
fordert. Hier ist sie im bestehenden Follower-Code belegt. **Kostet Debugzeit**, weil ein
holdover-Follower wie ein synchronisierter aussieht.

**3. Der Grandmaster startet nicht von selbst.** `ptp auto 0`, Intervall 500 ms. Für die Messungen
wurde manuell gestartet und auf 100 ms gestellt, danach zurückgestellt.

---

## Gesamtbeurteilung

> **Zur Belastbarkeit dieser Runde.** Der erste Durchgang fiel durchweg positiv aus und enthielt
> **keinen einzigen blockierten Zustand** — damit war er von „PLCA schränkt hier ohnehin nichts ein"
> nicht zu unterscheiden. Die [Kontrollversuche](#kontrollversuche) schließen diese Lücke: K1
> validiert das Messgerät, K2A zeigt, dass PLCA das Senden wirklich taktet, K3 isoliert den Fallback
> auf eine einzige geänderte Variable. **T2 und T4b ruhen jetzt auf einem Negativbefund, nicht nur
> auf Erfolgen.** T3, T5 und T7 bleiben schwach — siehe [Offen](#offen).

**1. Ist die Messstrecke sauber?** Ja (T0), **und nachgewiesen fähig, eine Null zu erzeugen** (K1).
Verlustmessungen über die Zähler sind gültig; über Sequenznummern **nicht** (Nebenbefund 1).

**2. Trägt der Bootstrap so, wie er im Plan steht?** **Ja, und mit Reserve.**

- Ein Knoten, der sein PLCA **nicht** abschaltet, kann bei fehlenden Beacons trotzdem senden (T2,
  bewiesen in K3) — der Entwurf verklemmt also selbst dann nicht, wenn ein Knoten die Umschaltung
  verpasst. Blockierte Frames gehen dabei nicht verloren, sondern werden nachgeliefert.
- Ein Knoten mit abgeschaltetem PLCA empfängt uneingeschränkt (T3) — die Fensterankündigung erreicht
  jeden, unabhängig vom Modus.
- Der Modus ist **selbst erkennbar** (T1, Bit 15), also lässt sich der Zustand lokal prüfen statt zu
  glauben.
- Ein Registerschreibzugriff genügt, es braucht keinen Firmware-Zustand, der das Fenster hält (T5).

Die Entscheidung, PLCA auf **jedem** Knoten explizit abzuschalten, bleibt trotzdem der richtige
Hauptweg: sie hängt an keiner der oben gemessenen Eigenschaften, und T2 liefert nun ein zweites,
unabhängiges Netz darunter.

**3. Was schlecht aussieht:** das Fehlerbild doppelter Node-IDs (T4b). Sporadischer Sendeverlust bei
funktionierendem Empfang ist im Betrieb kaum als Adressproblem zu erkennen. Damit ist Maßnahme (a) —
`plca_id`-Default **ungültig** machen, unprovisionierte Knoten still und laut meldend — nicht Komfort,
sondern die tragende Absicherung.

---

## Konsequenzen für den Plan

| Test | Ergebnis | Änderung | erledigt |
|---|---|---|---|
| T0 Seriennummer | verschieden bei n = 2 | CRC32 über alle vier Worte bleibt **Empfehlung**, nicht Pflicht; n = 2 vermerken | ☐ |
| T2 Fallback | 5/5 ohne Beacons | §2: Fallback als *zusätzliches Netz* benennen, explizites PLCA-Aus bleibt Hauptweg | ☐ |
| T3 Empfang | +141 Samples, Servo `FINE` | §2: „Ankündigung in jedem Modus hörbar" von *Annahme* zu *gemessen* | ☐ |
| T1 + K2A Beacon-Bit | `PLCA_STATUS` Bit 15, aber **kein** Slot-Indikator | §2: Selbsterkennung aufnehmen — mit **zwei** Einschränkungen: `0` heißt auch „lokal aus", und `0x8000` heißt **nicht** „gültige ID" | ☐ |
| K2 `NODE_CNT` | auf einem Follower wirkungslos | §2 + Störungssuche: einen Knoten kann man **nicht** über `plca_cnt` stilllegen, nur über die ID | ☐ |
| K3 Rückstau | blockierte Frames werden gehalten | §2: Fensteröffnung entlässt einen Rückstau auf den Bus — beim Dimensionieren mitdenken | ☐ |
| Runbook Methodik | positive Kontrolle fehlte | Je Test zwei Pflichtfelder: **Stimulus-Nachweis** und **Gegenprobe, die scheitern muss** | ☐ |
| T5 Reinit | Schreibzugriff hält | §2: Fenster darf ein Registerschreibzugriff sein; Kabelabziehen bleibt offen | ☐ |
| T4b Duplikate | ~17 % Sendeverlust, Empfang ok | §2 + Störungssuche: Fehlerbild „sporadischer TX-Verlust, RX ok" aufnehmen | ☐ |
| T7 IP-Konflikt | nicht live, Follower auf `.201` | §2.2 von „Reparatur" auf „Absicherung für neue Boards" umformulieren | ☐ |
| **§2.4 `eth1` im Follower** | **Behauptung falsch** | Der Follower-Stack hat **nur `eth0`** (`stats` und `showenv` zeigen ein Interface). Der Absatz über `.210`/`00:04:25:01:02:04` auf jedem Follower ist zu streichen | ☐ |
| Runbook T0 | `dump` zählt **Bytes** | `dump 0x008061FC 1` → `dump 0x008061FC 4` | ☐ |
| Runbook Methodik | Sequenznummern unbrauchbar | „Sequenznummern → exakter Verlust" ersetzen durch „TX-/RX-Zähler; Sequenznummern nur bei Einzelsendungen" | ☐ |
| `boards.json` | Follower-Probe veraltet | `…1103` → `…1290`, oder klären, welches Board gemeint ist | ☐ |
| `noip_test.c` | Pufferaliasing im Burst | Fehlerbericht; entweder je Frame ein eigener Puffer oder `NOIP_MAX_COUNT` auf 1 begrenzen | ☐ |
| `ptp_follower.c` | `FINE` ohne Sync | Holdover ausweisen: Alter des letzten Sync, Zustand `HOLDOVER` | ☐ |

---

## Offen

- **`COL_DET_CTRL0` Bitbedeutung** aus dem Datenblatt — Vorbedingung für T6.
- **Echte Doppel-ID-Messung (T4a)** braucht einen dritten Knoten am Bus; derzeit ist keiner
  angeschlossen.
- **Verhalten bei physischem Kabelabzug** (T5 deckt nur `testmode` und PMA-Reset ab).
- **Verhalten unter Last:** T2/K3 und T4b liefen bei geringer Buslast. Ob der Fallback und das
  Duplikat-Fehlerbild bei iperf-Last gleich aussehen, ist ungemessen.
- **T3 bleibt schwach.** Dass PLCA nur Sendegelegenheiten regelt, war vorher schon wahrscheinlich;
  der Test bestätigt etwas, das kaum anders sein konnte. Eine Gegenprobe, in der Empfang
  *nachweislich* ausfällt, gibt es nicht — K1 (`testmode 4`) betrifft nur den Sender.
- **Kein unabhängiger Beobachter.** Jede Messung dieser Reihe wurde von einem der beiden **Teilnehmer**
  gemacht. Ein Knoten, der selbst falsch arbeitet, kann das in seinem eigenen Protokoll nicht zeigen.
  Ein dritter LAN8651 als reiner Mithörer wäre der Ausweg — und würde gleichzeitig T4a freischalten.
- **Kein Drahtbeleg mit ns-Auflösung.** Der `ts=`-Wert der `[NoIP-RX]`-Zeilen ist eine
  **Millisekunde** aus `SYS_TIME` und kann PLCA-Zyklen (hunderte µs) nicht auflösen. Der vorhandene
  ns-Beobachter — der Hardware-RX-Timestamp aus Phase 2 — wird heute nur für `0x88F7` ausgewertet und
  gibt eine Zeile pro Zyklus, nicht pro Frame eines Bursts. Ein Quantisierungsnachweis (PLCA an gegen
  aus, Abstand aufeinanderfolgender Frames) wäre damit erreichbar, braucht aber Code.
- **Firmware-Stand auf den Boards** gegen `7a1354a` verifizieren.
