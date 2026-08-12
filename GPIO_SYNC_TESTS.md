# GPIO-Gleichzeitigkeit messen — Bedienungsanleitung

> **Was das ist.** Wie man über die Konsole der Boards nachweist, dass mehrere Follower **zum
> gleichen Zeitpunkt** eine Aktion ausführen, und wie man den Versatz mit einem Logic Analyzer
> ausmisst. Reine Bedienung; warum es so gebaut ist, steht in
> [PTP_TIMEBASE_PLAN.md](PTP_TIMEBASE_PLAN.md), was gemessen wurde in
> [test_results.md](test_results.md).
>
> **Ohne Analyzer geht auch etwas:** Abschnitt [3](#3-ohne-analyzer-nur-über-die-konsole) misst mit
> Bordmitteln, wie spät jedes Board gegen sein *eigenes* Ziel feuert. Das ist nützlich, beantwortet
> aber **nicht** die Frage nach Gleichzeitigkeit — dazu siehe den Kasten in
> [Abschnitt 5](#5-warum-die-firmware-zahlen-nicht-genügen).

---

## Inhalt

- [1. Aufbau](#1-aufbau)
- [2. Vorbedingungen prüfen](#2-vorbedingungen-prüfen)
- [3. Ohne Analyzer: nur über die Konsole](#3-ohne-analyzer-nur-über-die-konsole)
- [4. Mit Analyzer: der Versatz zweier Boards](#4-mit-analyzer-der-versatz-zweier-boards)
- [5. Warum die Firmware-Zahlen nicht genügen](#5-warum-die-firmware-zahlen-nicht-genügen)
- [6. Erwartete Werte](#6-erwartete-werte)
- [7. Kommandoreferenz](#7-kommandoreferenz)
- [8. Störungssuche](#8-störungssuche)

---

## 1. Aufbau

**Der Messpunkt ist auf jedem Board derselbe: `PD10` = EXT1 Pin 5** („GPIO1"), ein 2,54-mm-
Durchsteckpin, an dem eine Klemme direkt hält. Masse: jeder GND-Pin von EXT1.

```
Bridge      EXT1 Pin 5 ──── Saleae Ch?     (Grandmaster; ohne Trigger noch stumm)
Follower A  EXT1 Pin 5 ──── Saleae Ch?
Follower B  EXT1 Pin 5 ──── Saleae Ch?
jedes Board EXT1 GND   ──── Saleae GND
```

**Die Kanalnummern stehen hier absichtlich nicht.** Sie werden gemessen, nicht notiert — siehe
[Abschnitt 2](#2-vorbedingungen-prüfen). Beim Aufschreiben ist die Zuordnung in diesem Projekt schon
zweimal falsch geraten worden.

Logic 8 hat eine feste Schwelle von 1,65 V; 3,3-V-Logik passt ohne Pegelwandler.

---

## 2. Vorbedingungen prüfen

### 2.1 Boards

Jeder Follower braucht **eine eigene PLCA-Node-ID und eine eigene IP** — frisch geflasht trägt jedes
`plca_id 7` und `192.168.0.200`, und doppelte IDs kosten gemessene ~17 % Sendeverlust bei
einwandfreiem Empfang, also einen *stillen* Fehler.

```
showenv
```

Erwartung: Bridge `plca id 0`, Follower verschiedene IDs ≠ 0, alle IPs verschieden. Sonst:

```
setenv plca_id 1
setenv ip0 192.168.0.202
saveenv
```

### 2.2 Taktreferenz

Ohne den XOSC0-Taktpatch wandert die MCU-Zeitbasis um Zehner-ppm pro Minute und jede Messung ist
wertlos. Prüfgriff:

```
dump 0x40001038 4
```

Erwartung `40 00 09 00`, also `DPLLCTRLB = 0x00090040` (`DIV = 9`, Referenz XOSC0). Steht dort
`40 00 00 00`, läuft das Board am open-loop DFLL — **erst das Board neu flashen.**

### 2.3 Logic 2

Logic 2 starten, dann **Options → Preferences → Developer → „Enable scripting socket server"** und
Logic 2 neu starten. Einmalig:

```
pip install logic2-automation pyserial
```

### 2.4 Die Messkette validieren, bevor man ihr glaubt

**Das ist kein optionaler Schritt.** Eine Kette, die nie nachweislich ein Signal *und* eine Null
erzeugt hat, ist kein Messgerät.

```
python saleae_wiring_check.py --signature 6 10 14 --seconds 18 --channels 0 1 2 3
```

Das toggelt PD10 **per pyOCD direkt über die PORT-Register** — ohne jede Firmware-Beteiligung,
deshalb funktioniert es auch auf der Bridge, die keinen PD10-Code hat. Jedes Board bekommt eine
andere Flankenzahl in **einer** Aufnahme, und die Ausgabe nennt am Ende die gemessene Zuordnung:

```
  bridge       ( 6 edges) -> Ch3
  follower A   (10 edges) -> Ch1
  follower B   (14 edges) -> Ch0
  first-edge order matches drive order: bridge -> follower A -> follower B
```

Die Flankenzahl identifiziert jeden Kanal, und die Reihenfolge der ersten Flanken schließt einen
Zufallstreffer aus. **Diese Kanalnummern in die folgenden Aufrufe einsetzen.**

---

## 3. Ohne Analyzer: nur über die Konsole

Auf der **Bridge** den Grandmaster starten:

```
ptp interval 100
ptp start
```

Auf **jedem Follower** zuhören und warten, bis die Zeitbasis eingerastet ist:

```
ptpf on
tbase
```

Erwartung `state: LOCKED   usable: yes`. Danach `winners: 16/16` abwarten — das dauert bei 100 ms
Intervall rund **60 s** und ist wichtig, weil die Glättung erst mit vollem Ring greift.

Dann den periodischen Trigger, auf **beiden** Boards, mit derselben Periode:

```
tbase per 20 1
```

`20` ist die Periode in Millisekunden, `1` die Aktions-ID (1 = ISR-Kontext, 2 = nachgelagert und
druckt). Die Phase ist **absolut**: gefeuert wird bei jedem Vielfachen von 20 ms der
Grandmaster-Zeit, unabhängig davon, wann welches Board das Kommando bekam. Genau deshalb können sich
zwei Boards überhaupt treffen.

Nach einer Weile:

```
tbase trig
```

```
[TRIG] backend: E1 (TC1 compare)   PD10: on
[TRIG] armed: yes   mode: STRICT   action: 1   period: 20 ms
[TRIG] fired: 2075   refused: 0   skipped periods: 0   rearm lost: 0
[TRIG] lateness over 2075 fires, ticks (60 ticks = 1 us):
[TRIG]   last 127 (2116 ns)   min 127 (2116 ns)   max 227 (3783 ns)
```

Beenden mit `tbase cancel` auf jedem Board.

---

## 4. Mit Analyzer: der Versatz zweier Boards

Bei laufenden periodischen Triggern (Abschnitt 3), mit den in 2.4 **gemessenen** Kanälen:

```
python saleae_skew.py --ch-a 1 --ch-b 0 --ch-master 3 --seconds 30 --period-ms 20
```

```
FOLLOWER A (Ch1) vs FOLLOWER B (Ch0):
  pairs                : 1500
  median offset        : -160 ns   <- fixed skew
  MAD                  : 1080 ns
  stdev                : 1638 ns
  spread around median : -3560 .. +4320 ns
  peak-to-peak         : 7880 ns
  p90 / p99 |deviation|: 2680 / 3620 ns
```

Die Aufnahme wird zusätzlich als `.sal` gespeichert und kann in Logic 2 nachgesehen werden.

> **Ein Fallstrick, der eine ganze Periode Versatz vortäuscht.** Der Trigger **toggelt** den Pin, und
> damit trägt die Flankenpolarität die **Parität der Auslösezahl**. Zwei Boards können
> auslösungssynchron sein und trotzdem gegenphasige *steigende* Flanken haben. Ein Vergleich nur
> steigender Flanken meldete hier „0 Paare innerhalb 10 ms" und hätte 20 ms Versatz suggeriert, wo
> der echte 160 ns war. `saleae_skew.py` paart deshalb standardmäßig **alle** Übergänge — jede
> Auslösung ist genau einer. `--edges rising` gibt es nur, um den Effekt zu zeigen.

---

## 5. Warum die Firmware-Zahlen nicht genügen

Der `tbase trig`-Zähler misst, wie spät ein Board gegen **sein eigenes Ziel** feuert. Unterscheiden
sich die *Ziele* zweier Boards, sieht er das prinzipiell nicht.

Genau das ist einmal passiert: beide Boards meldeten sauber 2,15…3,27 µs Verspätung, lagen am Draht
aber **21,5 µs** auseinander. Der Unterschied steckte in den Modellen, nicht in der Auslösung.

Und der Analyzer hat drei Firmwarefehler gefunden, die die Zähler nicht diagnostizieren konnten: ein
fehlgeschlagenes Nachladen, das den Trigger dauerhaft tötete (**ein Kanal 20 Übergänge, der andere
1504**), und zwei fehlende Warteschleifen auf schreibsynchronisierte TC-Register, die Auslösungen um
1,49 ms bzw. 1,09 ms verschoben.

**Für Gleichzeitigkeit ist der Analyzer nicht Komfort, sondern die einzige gültige Auskunft.**

---

## 6. Erwartete Werte

Stand 2026-08-12, zwei Follower, 1500 Paare über 30 s, Periode 20 ms:

| Größe | Wert |
|---|---|
| Median-Versatz (fest) | **−160 ns** |
| MAD | **1 080 ns** |
| p90 / p99 | 2 680 / 3 620 ns |
| Spitze-Spitze | **7 880 ns** |
| Verspätung je Board (Zähler) | 127…227 Ticks = 2,12…3,78 µs |

Die 2,1 µs Verspätung sind **fester Pfadaufwand** und auf beiden Boards gleich — für Gleichzeitigkeit
also gleichgültig, sie fallen in der Differenz heraus.

**Was schlechter aussieht, ist ein Symptom:**

| Beobachtung | wahrscheinliche Ursache |
|---|---|
| Spitze-Spitze im Bereich **1,09 ms** | eine TC1-Wrap-Periode — fehlender `SYNCBUSY`-Wait, siehe test_results.md |
| Spitze-Spitze im Bereich **1,5 ms** | Stufe-2-Rückfall feuert zu früh statt zu warten |
| Spitze-Spitze **20 ms** bei „0 Paare" | steigende Flanken statt aller Übergänge verglichen |
| Spitze-Spitze **~20 µs**, MAD ~4 µs | Zeitbasis-Modellsprünge — Ring noch nicht voll, oder Glättung aus |
| ein Kanal viel weniger Übergänge | Trigger auf dem Board hat aufgehört, `rearm lost` prüfen |

---

## 7. Kommandoreferenz

Alles unter der Gruppe `tbase`, weil `MAX_CMD_GROUP` im generierten `sys_command.h` bei 8 liegt und
das Projekt am Limit ist.

| Kommando | Wirkung |
|---|---|
| `tbase` | Zustand der Zeitbasis: Modellzustand, Steigung, Gewinner, Residuum |
| `tbase now` | aktuelle Grandmaster-Zeit aus dem Modell |
| `tbase at <ns>` | Hin- und Rückweg `LocalFor` → `Convert`, zeigt den Umrechnungsfehler |
| `tbase reset` | Modell verwerfen und neu einrasten |
| `tbase trig` | Trigger-Zustand, Auslösezähler, Verspätungsstatistik |
| `tbase fire <ms> [id]` | Einzelauslösung in `<ms>` Millisekunden |
| `tbase per <ms> [id]` | periodisch, absolute Phase 0 |
| `tbase cancel` | Trigger abschalten |
| `tbase hw on\|off` | Auslöse-Backend: `on` = TC1-Compare (E1), `off` = SYS_TIME (Phase C) |
| `tbase pin on\|off` | PD10 bei jeder Auslösung toggeln |
| `tbase mode strict\|free` | `free` feuert auch ohne brauchbare Zeitbasis — **nicht synchronisiert** |

Auf der Bridge zusätzlich `ptp start` / `stop` / `interval <ms>` / `status`.

**`tbase hw off` ist der interessante Schalter:** damit lässt sich Phase C gegen E1 auf derselben
Firmware vergleichen, ohne neu zu flashen. Erwartung C: Verspätung streut über ~25 µs statt ~1,7 µs.

---

## 8. Störungssuche

| Symptom | Ursache und Abhilfe |
|---|---|
| `tbase` zeigt `UNINIT` | kein `Sync` empfangen. `ptpf on` gesetzt? Grandmaster gestartet? PLCA-ID eindeutig? |
| `tbase` zeigt `HOLDOVER` | seit >3 s kein `Sync`. Grandmaster prüfen; `usable: no` ist dann korrekt und der Trigger verweigert |
| `timebase not usable` beim Auslösen | Zeitbasis nicht `LOCKED`. Warten, oder bewusst `tbase mode free` — dann ist es **nicht** synchronisiert |
| `target in the past or too close` | Mindestvorlauf 50 ms unterschritten |
| `a trigger is already armed` | erst `tbase cancel` |
| `rearm lost` > 0 | der periodische Trigger hat aufgegeben; Zeitbasis oder Last prüfen |
| Analyzer sieht nichts | `tbase pin` steht auf `off`, oder falscher Kanal — Abschnitt 2.4 wiederholen |
| Konsole antwortet nicht mehr | ein Dauerlog läuft und `cli.py` hängt im Drain; `tb_capture.py` benutzen, sonst Reset über `flash_same54.py --reset-only` |
| Zahlen ändern sich zwischen Läufen um µs | normal: der Median-Versatz folgt den Neufits und ist **kein** kalibrierbarer Festwert |

**Bench nach der Messung zurückstellen**, sonst laufen Trigger und Grandmaster weiter:

```
[Follower]  tbase cancel
[Bridge]    ptp stop
[Bridge]    ptp interval 500
```
