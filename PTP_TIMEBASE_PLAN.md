# Zeitgesteuerte Aktionen auf den Followern — Umsetzungsplan

> **Was das ist.** Eine **korrigierte MCU-Zeitbasis** im Follower, die Grandmaster-Zeit auf lokale
> Timer-Ticks abbildet, plus eine **Trigger-Einrichtung**: der Master kommandiert „Aktion N zum
> Zeitpunkt `Tx`", und mehrere Follower führen sie **gleichzeitig** aus. Die Korrektur kommt aus
> zwei austauschbaren Quellen — heute aus dem `t1`-Strom der PTP-Frames, später, wenn ein Draht
> gezogen ist, aus dem 1PPS-Signal des LAN8651.
>
> **Steuerung und Zeit sind getrennt.** Die Zeit bleibt rohes L2 (`0x88F7`); die Kommandos gehen über
> **UDP im SOME/IP-Format**, entdeckt wird über **mDNS**. Adressen und Identität regelt
> [§2](#2-adressierung-und-identität) — MAC aus der Seriennummer, IP aus der PLCA-Node-ID.
>
> Sichtbar gemacht wird das in [Phase G](#phase-g--demonstration-und-was-sie-beweist): zwei Follower
> toggeln ein GPIO, die Synchronisation wird vom Host ein- und ausgeschaltet, und am Oszilloskop
> laufen die Flanken zusammen bzw. driften wieder auseinander. Diese Vorführung ist gleichzeitig die
> genaueste Messung des Plans.
>
> **Dieser Plan hängt nicht am Servo.** Phase 2 aus
> [PTP_IMPLEMENTATION_PLAN.md](PTP_IMPLEMENTATION_PLAN.md) ist abgeschlossen und bleibt es —
> gebraucht wird sie hier erst in der letzten Phase, als Voraussetzung für das 1PPS. Bis dahin
> werden **kein RX-Timestamp, kein `FTSE`/`FTSS` im Follower, kein Treiber-Patch und kein
> generierter Code** angefasst.
>
> Vorausgesetzt wird nur **Phase 1**: `Sync` + `Follow_Up` mit echtem Hardware-TX-Timestamp auf dem
> Bus. Das steht und ist mit `test_ptp.py` nachgewiesen.
>
> Registerwissen und Messzahlen stehen in [LAN8651_TIME_SYNC.md](LAN8651_TIME_SYNC.md) (englisch);
> diese Datei wiederholt sie nicht.

---

## Inhalt

- [0. Vorentscheidungen](#0-vorentscheidungen)
- [1. Fehlerbudget und die eine unbewiesene Annahme](#1-fehlerbudget-und-die-eine-unbewiesene-annahme)
- [2. Adressierung und Identität](#2-adressierung-und-identität)
- [Phase A — Messreihe, ohne einen Algorithmus zu bauen](#phase-a--messreihe-ohne-einen-algorithmus-zu-bauen)
- [Phase B — Kern der Zeitbasis](#phase-b--kern-der-zeitbasis)
- [Phase C — Trigger, Stufe Software](#phase-c--trigger-stufe-software)
- [Phase D — Kommando auf dem Master](#phase-d--kommando-auf-dem-master)
- [Phase E — Trigger, Stufe Hardware](#phase-e--trigger-stufe-hardware)
- [Phase F — 1PPS als Quelle, wenn der Draht liegt](#phase-f--1pps-als-quelle-wenn-der-draht-liegt)
- [Phase G — Demonstration, und was sie beweist](#phase-g--demonstration-und-was-sie-beweist)
- [Validierung](#validierung)
- [Warum diese Reihenfolge](#warum-diese-reihenfolge)
- [Risiken, die den Plan kippen können](#risiken-die-den-plan-kippen-können)
- [Fallstricke](#fallstricke)

---

## 0. Vorentscheidungen

### 0.1 Gefittet wird gegen `t1`, nicht gegen `t2`

`t2` ist ein Wert der **eigenen** Wallclock, und die verstellt der Servo laufend (`MAC_TA`-Sprünge,
`MAC_TI`-Ratenänderungen). Jeder Servo-Eingriff verfälschte den Fit. `t1` dagegen kommt aus der
`Follow_Up`-Nutzlast, ist Grandmaster-Zeit und wird lokal nie angefasst.

Drei Folgen, alle groß:

- **Vollständig entkoppelt vom Servo** — er darf springen, wie er will.
- **Funktioniert unabhängig davon, ob der Servo konvergiert ist** oder überhaupt läuft.
- **Kein Treiber-Patch.** `t2` wird nicht gebraucht, also auch nicht die Änderung an
  [drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348).
  Damit kann ein MCC „Generate Code" dieses Feature nicht kaputt machen — der Fallstrick, der Mirror
  und Servo gleichermaßen bedroht (`CLAUDE.md` Abschnitt 6), greift hier nicht.

Preis: `Δ` (der Weg vom Frame-Empfang bis zum Auslesen des lokalen Zählers) wird größer, weil auf
App-Ebene gestempelt wird statt im Treiber. `Δ_min` ist aber eine Konstante, und der Filter in
Phase B interessiert sich nur für die Streuung.

### 0.2 Umgerechnet wird, nicht gestellt

Die lokale Zeitbasis bleibt unangetastet. An ihr hängen Timer, Timeouts und `SYS_TIME` — springt
sie, brechen die mit. Stattdessen führt der Kern eine affine Abbildung:

```
grandmaster_ns  =  anchor_ns + (lokale_ticks − anchor_L) · slope
```

Damit lässt sich **jeder** lokale Zeitstempel in Grandmaster-Zeit ausdrücken, auch rückwirkend, und
die Umkehrung liefert das lokale Ziel für einen Trigger.

### 0.3 Quellenunabhängiger Kern

Beide Korrekturquellen liefern **dasselbe Datum**: ein Paar aus lokalem Zählerstand und
Referenzzeit. Nur die Güte unterscheidet sich.

| Quelle | Paar | Güte | brauchbare Rate |
|---|---|---|---|
| `t1` aus `Follow_Up` | (`L` per Software, `t1`) | µs-Klasse, Min-Filter nötig | ~1 guter Wert / 3 s |
| 1PPS über TC-Capture | (`L` per Hardware-Capture, Sekundengrenze) | **ns-Klasse, kein Filter** | 1/s, jeder Wert gut |

Der 1PPS-Weg ist also nicht nur präziser, er liefert auch **mehr** brauchbare Information pro Zeit.

Aufbau entsprechend, damit der spätere Tausch ein Dateizuwachs und kein Umbau ist:

```
ptp_timebase.c    Fit, Convert, Trigger — quellenunabhängig, ändert sich nie
   ^  PTP_TB_SubmitPair(uint64_t local_ticks, uint64_t ref_ns)
   |
   +-- ptp_src_frames.c   (L, t1), Min-Filter, seqId-Paarung        [Phase B]
   +-- ptp_src_1pps.c     TC-Capture, Sekunde beschriften, filterlos [Phase F]
```

Der Min-Filter ist ausdrücklich **keine** Eigenschaft des Zeitmodells, sondern des Frame-Wegs — nur
dort entsteht einseitiges Rauschen.

### 0.4 Die Kennzahl ist Gleichzeitigkeit, nicht Absolutzeit

Verlangt ist „alle Follower zum selben Zeitpunkt", nicht „genau zur Grandmaster-Sekunde". Damit ist
die Zielgröße eine **Differenz zwischen Knoten**, und der gemeinsame Fehleranteil — `Δ_min`,
`D_const`, die ganze µs-Klasse — **fällt heraus**. Dasselbe Argument, das
[§11.4](LAN8651_TIME_SYNC.md#114-one-way-only-what-that-costs-and-what-it-does-not) schon für
`D_const` macht.

Konsequenz für die Messtechnik: **Zweikanal-Oszilloskop an zwei Follower-GPIOs.** Keine externe
Zeitreferenz nötig, kein Draht zum PHY. Das Messmittel misst genau die Größe, die interessiert.

### 0.5 Was der Ansatz grundsätzlich nicht kann

> **Ein Hardware-Zeitstempel auf der PHY-Seite ist auf der MCU-Seite nur so genau, wie die
> Gleichzeitigkeit herstellbar ist. Gleichzeitigkeit herstellen kann nur ein Draht.**

Daraus folgt, dass der naheliegende Mittelweg — den hardware-präzisen `t2` nehmen *und* in MCU-Zeit
umrechnen — nicht funktioniert: für die Umrechnung braucht man zwei Uhrenstände zum **selben**
Augenblick, und bis die CPU nach dem lokalen greift, ist der Augenblick von `t2` vorbei.

Die kurze Fassung des Unterschieds zwischen Servo und diesem Plan:

| | Servo (Phase 2) | Affiner Fit (dieser Plan) |
|---|---|---|
| Korrektur lebt in | der PHY-Hardware | Software |
| liefert Präzision an | den **Draht** | die **CPU** |
| braucht | RX-Timestamp, Treiber-Patch, `FTSE`/`FTSS` | nichts Besonderes |
| verwundbar durch MCC | ja | nein |

Die Anforderung hier ist CPU-seitig. Der Fit ist dafür nicht das billigere Werkzeug, sondern das
richtige; der Servo ist nicht schlechter, er zielt woandershin.

---

## 1. Fehlerbudget und die eine unbewiesene Annahme

Erwartet zwischen zwei Followern, Frame-Weg, Hardware-Trigger:

| Beitrag | Größe | Anmerkung |
|---|---|---|
| Differenz der `Δ_min` beider Boards | **dominiert**, vermutlich einstellige µs | **unbewiesen** — siehe unten |
| Steigungsfehler × Vorlaufzeit | 10 ns bei 1 s, 1 µs bei 100 s | über die Vorlaufzeit steuerbar |
| Buslaufzeit (Position am Multidrop) | ~5 ns/m, bei 25 m ~125 ns | notfalls kalibrierbar |
| Hardware-Compare + Pin | ~17 ns | vernachlässigbar |
| *stattdessen Software-Trigger* | *einige µs bis ms* | *würde alles andere erdrücken* |

Absolut gegen die Grandmaster-Zeit bleibt dagegen `Δ_min + D_const` ≈ **100 µs** stehen. Beides ist
konstant, beides zusammen nicht trennbar — und für diesen Anwendungsfall auch nicht nötig.

**Die Annahme, auf der das günstige Urteil ruht:** dass `Δ_min` zwischen zwei Boards mit gleicher
Firmware und gleicher Hardware sich weitgehend aufhebt. Plausibel, weil die dominierenden Anteile
identisch sind — die Übertragungszeit des Frames auf dem Draht (64 Byte bei 10 Mbit/s = **51 µs**)
und der SPI-Chunk-Transfer. Aber nicht gemessen. **Phase A misst genau das.**

---

## 2. Adressierung und Identität

Voraussetzung für Phase D, weil die Kommandos über UDP laufen. Ausgangslage: **alle Follower tragen
dieselbe Firmware, also per Compile-Default dieselbe Adresse** — und zwar dieselbe wie die Bridge.

### 2.1 Was heute schon stimmt: die MAC

Beide Projekte leiten die MAC aus der SAME54-Seriennummer ab, das Problem existiert dort nicht mehr:

| | Funktion | Ergebnis |
|---|---|---|
| Bridge | [env.c:67](firmware/src/env.c#L67) `env_derive_mac(m0, m1)` | `eth0` = OUI + Serial[2..0], `eth1` = `eth0`, niedrigstes Byte +1 |
| Follower | [follower/…/env.c:66](follower/firmware/src/env.c#L66) `env_derive_mac(m0)` | nur `eth0` |

Zwei Nachbesserungen, beide klein:

- **U/L-Bit setzen** — `ENV_OUI[0] = 0x02` statt `0x00`. Dann ist die Adresse nach IEEE 802
  *lokal verwaltet* und damit **standardkonform**, statt eine nie zugewiesene Adresse in Microchips
  OUI zu belegen. Kosten: eine Zeile Code und eine Zeile Doku (`CLAUDE.md` Abschnitt 6 nennt die
  konkrete `00:04:25:ca:ce:d9`; **kein** Testskript tut das). Nicht dringend, aber vor jedem Einsatz
  außerhalb des Labors fällig.
- **CRC32 über alle vier Serial-Worte** statt der unteren 3 Bytes von Word 0. Die Eindeutigkeit
  hängt derzeit an der ungeprüften Annahme, dass dort die Variation sitzt und nicht ein Lot-Code.
  **Vorher messen:** `dump 0x008061FC 1` auf beiden Boards vergleichen. Unterscheiden sich die
  unteren 24 Bit, ist nichts zu tun; sonst räumt der CRC die Frage ein für alle Mal aus dem Weg.

### 2.2 Die IP kommt aus der PLCA-Node-ID

```
IP       = 192.168.0.(64 + plca_id)     ->  .65, .66, .67 …   (id 0 = Koordinator = Bridge)
Hostname = t1s-follower-<plca_id>
```

Begründung: **jeder Knoten auf einem 10BASE-T1S-Multidrop-Segment braucht ohnehin eine eindeutige
PLCA-Node-ID**, sonst funktioniert PLCA selbst nicht — zwei Knoten würden im selben Beat senden. Die
Eindeutigkeit wird also von derselben Bedingung erzwungen, die den Bus überhaupt zum Laufen bringt,
und `plca_id` ist bereits pro Board provisioniert (`plca_node`, `setenv plca_id`). Damit sinkt die
Provisionierung von drei Werten je Board auf **einen** — den, den man sowieso setzen muss.
Nebeneffekt: die Adresse verrät die Busposition, was beim Vergleich zweier Follower am Oszilloskop
praktisch ist.

**Verworfene Alternative: IP ebenfalls aus der Seriennummer** (oder IPv4 Link-Local). Kostet keine
Provisionierung, macht die Adresse aber unvorhersagbar und **mDNS damit zwingend**. Das ist der
Grund gegen sie: mDNS hängt auf dieser Hardware an einer einzigen Zeile (2.4), und bei unbekannter
Adresse *und* streikender Entdeckung ist ein Board über IP überhaupt nicht mehr erreichbar — nur
noch über die serielle Konsole. Eine berechenbare Adresse bleibt auch dann erreichbar.

### 2.3 ARP-Probe als Netz, nicht als Mechanismus

Beim Start die abgeleitete Adresse per ARP anfragen. Antwortet jemand: nächste freie nehmen **und
laut auf der Konsole klagen**. Das fängt genau den Fall, der schon einmal einen toten Bus gekostet
hat — `ENV_VERSION` erhöht, alle persistenten Werte zurück auf Compile-Defaults, `plca_id` bei allen
Knoten gleich (`CLAUDE.md` Abschnitt 6). Ohne Probe ist das ein stiller Fehler.

### 2.4 Zwei Altlasten, die vorher weg müssen

**`eth1` im Follower ist unversorgt.** `env` verwaltet dort nur *ein* Interface, `configuration.h`
bringt aber weiter zwei Netzwerke mit — `eth1` behält also **`192.168.0.210` und
`00:04:25:01:02:04` auf jedem Follower**
([follower/…/configuration.h:272-275](follower/firmware/src/config/default/configuration.h#L272-L275)).
Solange dort kein Kabel steckt, passiert nichts; am Tag, an dem eines hineingeht, kollidieren alle
Follower untereinander und mit der Bridge. Entweder `eth1` im Follower nicht mehr konfigurieren oder
in die Ableitung aufnehmen.

**Multicast-Empfang hängt an einer einzigen Zeile.**
`DRV_LAN865X_RxFilterHashTableEntrySet()` ist ein **Stub** und gibt `TCPIP_MAC_RES_OP_ERR` zurück
([drv_lan865x_api.c:603](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L603)) —
der Treiber kann **keine** Multicast-Gruppe in den Empfangsfilter eintragen. Dass mDNS
(`224.0.0.251`) und SOME/IP-SD (`224.244.224.245`) trotzdem ankommen, liegt allein an
`DRV_LAN865X_PROMISCUOUS_IDX0 = true`
([configuration.h:138](follower/firmware/src/config/default/configuration.h#L138)), und es gibt
**keinen Rückfallweg**. Wer die Zeile zum CPU-Sparen auf `false` setzt, killt jede
Multicast-Entdeckung lautlos.

**Folge für den Entwurf:** der **kritische** Pfad (Kommandos) läuft über **Broadcast** und ist damit
von dieser Zeile unabhängig; nur der **Komfort**pfad (mDNS) darf Multicast benutzen. Siehe D.3.

---

## Phase A — Messreihe, ohne einen Algorithmus zu bauen

Die Machbarkeit hängt an drei Zahlen, die niemand kennt. Sie kosten fast nichts, weil der
Grandmaster längst sendet.

**A.1 Mitschreiben.** Im Follower einen Packet-Handler auf `frameType == 0x88F7` registrieren — der
Weg existiert schon, `noip_test.c` macht es für `0x88B5`, Registrierung in
[app.c:375](firmware/src/app.c#L375). Beim `Sync`: `SYS_TIME_Counter64Get()` lesen und mit der
`sequenceId` merken. Beim `Follow_Up`: über die `sequenceId` zuordnen und die Rohwerte über die
Konsole ausgeben.

Ausgabeformat bewusst maschinenlesbar und in **rohen Ticks**, nicht in µs:

```
[TB] seq=1234 L=123456789012 t1=1691... 
```

**A.2 Auswerten, auf dem PC.** Ein paar hundert Paare mit `cli.py` einsammeln, in Python rechnen.
Gefragt sind:

| Zahl | wozu |
|---|---|
| Minimum, Median, Streuung von `L − t1` | die Δ-Verteilung; **die** Sizing-Grundlage |
| Streuung des Minimums über Blöcke von 32 | was nach dem Filter übrig bleibt |
| Ausreißerstruktur | scharfe Häufung bei genau **1,092 ms** = verpasster `SYS_TIME`-Überlauf, kein Scheduling-Jitter |
| dasselbe auf **zwei** Boards, `Δ_min` verglichen | die dominierende Größe des Fehlerbudgets |

**A.3 Den ganzen Algorithmus offline durchrechnen.** Min-Filter, Zwei-Punkt-Fit, Extrapolation,
erwartete Gleichzeitigkeit — alles in Python auf den geloggten Paaren, bevor eine Zeile
Firmware-Algorithmus existiert. Die Firmware muss dafür nichts können außer mitschreiben.

**Fertig, wenn** die drei Zahlen vorliegen und die Offline-Rechnung zeigt, welche Gleichzeitigkeit
erreichbar ist. Ergibt die Δ-Streuung nach Filter einstellige µs, trägt der Plan weit; ergibt sie
hunderte µs, ist die Hauptschleife die Grenze — dann braucht es keinen feineren Algorithmus, sondern
ein früheres Lesen von `L`.

---

## Phase B — Kern der Zeitbasis

**B.1 Modell und Festkomma.**

```c
static uint64_t anchor_L;    /* lokale Ticks               */
static uint64_t anchor_ns;   /* Grandmaster-ns             */
static uint64_t slope_q24;   /* ns pro Tick, Q24           */

uint64_t PTP_TB_Convert(uint64_t L)
{
    return anchor_ns + (((L - anchor_L) * slope_q24) >> 24);
}
```

Nominal 16,667 ns/Tick (`SYS_TIME` läuft auf TC0, 16 Bit, **60 MHz**, GCLK-Generator 1 —
[plib_tc0.c:128](firmware/src/config/default/peripheral/tc/plib_tc0.c#L128),
[initialization.c:650](firmware/src/config/default/initialization.c#L650)) → `slope_q24 ≈ 2,80e8`.
Bei Neuankern alle ~3 s ist `L − anchor_L ≤ 1,8e8`, das Produkt also ~5e16, komfortabel unter
`2^64`.

**Q32 wäre knapp** (1,3e19 gegen 1,8e19 Grenze), und `unsigned __int128` gibt es auf 32-Bit-ARM
**nicht** — es gibt kein Ausweichen. Auflösung von Q24: ~0,004 ppm, besser als die Messung hergibt.

**B.2 Min-Filter auf dem Residuum, nicht auf der Rohdifferenz.**

```c
int64_t r = (int64_t)(PTP_TB_Convert(L) - t1);   /* wie viel später als vorhergesagt */
```

Grund: über 32 Samples (3,2 s) läuft ein Gangfehler von 10 ppm um 32 µs weg. Bei ~10 µs Δ-Streuung
würde die Rohdifferenz systematisch immer das erste oder letzte Sample eines Blocks wählen —
abhängig vom Driftvorzeichen. Das Residuum entfernt die Drift und lässt nur Δ-Ausschläge stehen.

**B.3 Ablauf.** Pro Block von **N = 32** Syncs das Sample mit kleinstem `r` behalten, die anderen
verwerfen. Gewinner in einen Ring von **16** Einträgen → Basis ~50 s. Steigung aus **ältestem und
neuestem** Gewinner:

```
slope_q24 = ((ns_neu − ns_alt) << 24) / (L_neu − L_alt)
```

Eine 64/64-Division alle 3 s, das ist billig. Anker auf den neuesten Gewinner setzen.

**Zwei-Punkt-Fit statt Ausgleichsgerade**, weil das Ergebnis von der Qualität der Endpunkte
dominiert wird und der Min-Filter genau die zu den besten verfügbaren gemacht hat. Eine
Ausgleichsgerade zieht Samples mit schlechterem Δ herein.

**Basislänge ~50 s** ist ein Kompromiss: nach oben begrenzt sie die Temperaturdrift des Quarzes
(~1 ppm/°C), nach unten die Δ-Streuung. Die Zahlen aus Phase A sagen, wo er wirklich liegt.

**B.4 Was `Sync`-Intervall und Drift wirklich bedeuten.** Weil die **Rate** mitbestimmt wird, ist
die Drift zwischen zwei Updates kein Fehler, sondern ein bekannter Betrag. 10 ppm unkorrigiert wären
über 100 ms genau 1 µs; ist die Rate bekannt, bleibt nur die **Änderung** der Rate übrig, und die
ist über 100 ms nichts. **Das Intervall ist nicht die begrenzende Größe** — man könnte die `Sync` auf
1/s ausdünnen und kaum etwas verlieren.

**B.5 API.**

```c
void     PTP_TB_SubmitPair(uint64_t local_ticks, uint64_t ref_ns);
bool     PTP_TB_Convert   (uint64_t local_ticks, uint64_t *ns);   /* beliebiger Zeitpunkt  */
bool     PTP_TB_Now       (uint64_t *ns);
bool     PTP_TB_LocalFor  (uint64_t ns, uint64_t *local_ticks);   /* Umkehrung, für Trigger */
PTP_TB_STATUS PTP_TB_Status(void);   /* konvergiert? Alter des letzten Fits? Steigung? */
```

`PTP_TB_Convert()` ist der wertvolle Teil: ein Ereignis wird im Moment des Geschehens mit
`SYS_TIME_Counter64Get()` gestempelt — ein Registerlesevorgang, keine SPI-Transaktion — und
**später** in Grandmaster-Zeit umgerechnet. Die Genauigkeit der Ereigniszeit hängt damit nicht daran,
wann die Firmware zur Verarbeitung kommt.

**B.6 Monotonie und Holdover.** Nach einem Neufit kann `Convert()` für denselben `L` einen minimal
kleineren Wert liefern — wer Reihenfolgen daraus ableitet, muss den letzten ausgegebenen Wert
festhalten und nicht darunter gehen. Und bleiben die `Sync` aus, läuft `Convert()` auf der alten
Steigung weiter: richtig, aber zunehmend falsch. Ein Alter-Zeitstempel und ein Statuswert („frei
laufend seit X s") gehören dazu, sonst liefert die Funktion stillschweigend Unsinn.

**Fertig, wenn** `PTP_TB_Status()` „konvergiert" meldet, die Steigung stabil bleibt und ein über die
Konsole ausgegebener `PTP_TB_Now()` gegen die Offline-Rechnung aus Phase A passt.

---

## Phase C — Trigger, Stufe Software

Ein Callback ist Software, also kommt der Interrupt-Weg ins Budget: Exception-Entry auf
Cortex-M4 @120 MHz ~100 ns, ISR-Prolog und Cache-Miss einige 100 ns — und im Worst Case die Dauer
der **längsten kritischen Sektion der gesamten Firmware**, die im TCP/IP-Stack und im
LAN865x-Treiber steckt. Nicht abschätzbar, nur messbar.

**C.1 Zwei Schichten in einem Ereignis.** Damit man sich nicht entscheiden muss:

```
Auslösung bei Tx
   |
   +--> Hardware: Pin setzen              (ab Phase E: ~17 ns, immer exakt)
   |
   +--> ISR: registrierten Callback rufen (~1 µs typisch, macht die Arbeit)
```

**C.2 Der Callback bekommt seine Verspätung mitgeliefert.** Beim Eintritt den Zähler lesen; die
Differenz zum Zielwert ist die Verspätung. Zwei Folgen: der Callback kann sie **herausrechnen**, und
Maximum und Histogramm über Stunden sind eine harte Zahl für den Worst Case — **ohne Messgerät**,
und gleichzeitig ein Gesundheitscheck für den Interrupt-Haushalt.

**C.3 ISR- oder Task-Kontext, klar getrennt.**

| | ISR-Kontext | Task-Kontext (nachgelagert) |
|---|---|---|
| Jitter | ~1 µs | Hauptschleife, 10er µs bis ms |
| erlaubt | Register, GPIO, Flags, Puffer | alles |
| **verboten** | SPI, Frames senden, `SYS_CONSOLE`, Blockieren | — |

Das Verbot ist hier nicht theoretisch: die Projektgewohnheit ist, alles über die Konsole auszugeben,
und ein Frame-Versand braucht die Task-Ebene ohnehin (`TC6_TX_ETH_QSIZE = 4`, bedient aus
`SYS_Tasks()` — `CLAUDE.md` Abschnitt 6).

**C.4 NVIC-Priorität** des auslösenden Interrupts über GMAC, SERCOM und EIC setzen — dann verdrängt
er andere ISRs statt zu warten. Eine Zeile, nimmt den größten Teil des Worst Case weg. Kritische
Sektionen (`__disable_irq()`) kann er nicht verdrängen; das ist der irreduzible Rest und genau der,
den C.2 beziffert.

**C.5 Registrierung über Aktions-IDs.** Über den Draht geht keine Funktionsadresse. Der Follower
führt eine kleine Tabelle:

```c
typedef void (*PTP_TRIG_Handler)(uint64_t scheduled_ns, uint32_t late_ticks, uintptr_t ctx);

bool PTP_TRIG_Register  (uint16_t action_id, PTP_TRIG_Handler h, uintptr_t ctx, bool isr_ctx);
bool PTP_TRIG_ArmPin    (bool enable);
bool PTP_TRIG_ScheduleAt(uint16_t action_id, uint32_t cmd_seq, uint64_t tx_ns);

/* periodisch, Phase in ABSOLUTER Grandmaster-Zeit — Begründung in Phase G */
bool PTP_TRIG_SchedulePeriodic(uint16_t action_id, uint32_t cmd_seq,
                               uint64_t period_ns, uint64_t phase_ns);

void PTP_TRIG_Cancel    (void);
PTP_TRIG_STATUS PTP_TRIG_Status(void);
```

**C.6 Vier Verweigerungsgründe, alle ausdrücklich** — jeder von ihnen führt sonst zu einem stillen
Fehlschlag:

| Grund | warum |
|---|---|
| Aktions-ID nicht registriert | sonst schaltet nichts und niemand weiß es |
| Zeitbasis nicht konvergiert / zu lange kein `Sync` | die Zeit ist nicht vertrauenswürdig |
| `Tx` liegt in der Vergangenheit | zu spät zugestellt |
| `Tx` zu weit in der Zukunft | Compare lässt sich nicht schärfen (siehe E.2) |

**Der zweite Grund ist umschaltbar** — Phase G braucht genau den Fall „läuft ohne Synchronisation":

| Modus | Verhalten ohne konvergierte Zeitbasis |
|---|---|
| `strict` (Default, Produktivfall) | verweigern |
| `free` (Demonstration) | trotzdem feuern, auf der **nominalen** Zeitbasis, und den Zustand melden |

Der Modus **muss von außen sichtbar sein** — über die Statusleitung aus
[Risiken](#risiken-die-den-plan-kippen-können). Sonst wird irgendwann ein `free`-Lauf für einen
synchronisierten gehalten, und das ist der teuerste Messfehler, den dieses Feature erzeugen kann.

**C.7 Genau einmal feuern.** Gleiche Aktions-ID + gleiche `cmd_seq` = schon gesehen, verwerfen. Ein
wiederholtes Kommando darf nicht zweimal schalten.

**C.8 Nur ein anstehender Trigger, nicht n.** Ein Compare-Kanal, ein Ziel. Eine Warteschlange wie bei
`SYS_TIME` ist machbar, kauft hier aber nichts. Wenn später mehrere Ereignisse nötig sind, ist der
zweite Kanal des 32-Bit-TC frei.

**C.9 Periodische Variante.** Für Phase G nötig und billig: nach dem Feuern `Tx += period` rechnen
und neu schärfen. Wichtig ist nicht das Nachladen, sondern **wie die Phase definiert ist** — siehe
G.1. Zwei Details:

- Das Nachladen gehört **in denselben ISR**, nicht in die Hauptschleife, sonst wandert die Periode
  mit der Schleifenlast.
- Kommt das Nachladen zu spät (Zielzeit schon vorbei), **eine Periode überspringen** statt sofort
  nachzufeuern — sonst holt der Trigger nach einer Störung mit einem Schwall von Impulsen auf.

**C.10 Mehrere Handler pro ID serialisieren** — der zweite läuft nach dem ersten, ist also nicht mehr
„präzise". Entweder auf einen begrenzen oder es dokumentieren und jedem seine eigene Verspätung
mitgeben. Letzteres ist ehrlicher.

**Fertig, wenn** ein per Konsole gesetzter Trigger feuert, die Verspätung protokolliert wird und
alle vier Verweigerungsgründe nachweislich greifen.

---

## Phase D — Kommando auf dem Master

Setzt [§2 Adressierung](#2-adressierung-und-identität) voraus. **Der Zeitstrom bleibt L2** — `Sync`
und `Follow_Up` bleiben rohes `0x88F7` und wandern **nicht** auf UDP. Nur die *Steuerung* wird
IP-basiert, damit sie adressierbar, entdeckbar und mit gewöhnlichem Werkzeug lesbar ist.

**D.1 Zeitverhalten ist unkritisch.** Ein Kommando muss nur *rechtzeitig* ankommen, nicht *pünktlich*.
Jitter des UDP-Wegs, Stack-Latenz und Hauptschleife sind deshalb gleichgültig — es zählt allein die
Vorlaufzeit. Wer hier Präzision sucht, sucht an der falschen Stelle: die Präzision steckt in `Tx`,
nicht in der Zustellung.

**D.2 Nachrichtenformat: SOME/IP.** Header nach SOME/IP, big endian, 16 Byte:

```
Offset  Breite  Feld
0       u16     Service ID        = 0x0865   (privat gewählt, nicht AUTOSAR-registriert)
2       u16     Method ID         = 0x0001 SCHEDULE
                                  | 0x0002 CANCEL
                                  | 0x0003 SCHEDULE_PERIODIC
4       u32     Length            = 8 + Nutzlast
8       u16     Client ID
10      u16     Session ID        <- dient gleichzeitig als Dedup-Schlüssel (C.7)
12      u8      Protocol Version  = 0x01
13      u8      Interface Version = 0x01
14      u8      Message Type      = 0x01  REQUEST_NO_RETURN
15      u8      Return Code       = 0x00  E_OK
16      ...     Nutzlast
```

Nutzlast:

```
0       u16     action_id
2       u16     reserved = 0
4       u64     tx_ns       Grandmaster-ns                    (SCHEDULE)
4       u64     period_ns                                     (SCHEDULE_PERIODIC)
12      u64     phase_ns    absolute Phase, siehe G.1         (SCHEDULE_PERIODIC)
```

Zwei Dinge, die SOME/IP hier geschenkt beisteuert:

- **`REQUEST_NO_RETURN` (0x01) ist die Fire-and-Forget-Betriebsart des Standards.** Die Einbahnregel
  des Projekts ist damit keine Notlösung, sondern ein dokumentierter Nachrichtentyp. Was fehlt,
  bleibt trotzdem das, was in [Risiken](#risiken-die-den-plan-kippen-können) steht: eine Rückmeldung.
- **Die `Session ID` ist der Dedup-Schlüssel**, den C.7 verlangt — ein eigenes `cmd_seq`-Feld
  entfällt.

**D.3 Transport: Broadcast für die Gruppe, Unicast für den Einzelnen.**

| Zweck | Ziel | Grund |
|---|---|---|
| „alle gleichzeitig" | **Broadcast** `192.168.0.255`, UDP **30509** | ein Frame für alle, und **unabhängig** von der Promiscuous-Zeile aus §2.4 |
| einzelner Follower | Unicast `192.168.0.(64+id)` | Diagnose, Einzelkonfiguration |

**Kein Multicast für Kommandos.** Broadcast wird vom MAC-Filter immer angenommen; IPv4-Multicast
hängt auf dieser Hardware am Stub-Filter und damit an `promiscuous = true`. Der kritische Pfad soll
nicht an einer Zeile hängen, die jemand aus Performancegründen umstellen könnte.

**D.4 Senden über den Stack, nicht über den Rohweg.** UDP geht durch `DRV_LAN865X_PacketTx()` —
damit greift der Mirror-TX-Hook automatisch, `MIRROR_RawTx()` ist **nicht** nötig, und die
Fünf-Frames-Grenze der Rohsende-Queue (`TC6_TX_ETH_QSIZE = 4`) gilt hier **nicht**. Der Wechsel von
rohem EtherType auf UDP nimmt also eine Einschränkung weg, statt eine hinzuzufügen.

**D.5 Mindestvorlauf.** Der Master plant großzügig — Vorschlag **200 ms** —, der Follower verweigert
unter **50 ms**. Beides unkritisch für die Extrapolation (1 s Vorlauf kostet 10 ns) und weit über
jeder plausiblen Zustellzeit.

**D.6 Entdeckung über mDNS.** Neues MCC-Modul (`TCPIP_STACK_USE_ZEROCONF_MDNS_SD` ist in **keinem**
der beiden Projekte aktiviert) — also ein Eingriff mit der bekannten Regenerierungsgefahr, danach
`python test_mirror.py`.

```
Instanz:  t1s-follower-<plca_id>
Dienst:   _t1strig._udp.local   Port 30509
TXT:      plca=<id>  ver=1  actions=<bitmaske>
```

Nur **statische** Angaben ins TXT. Der Synchronisationszustand gehört *nicht* hinein — er ändert
sich, und jede Änderung erzwingt ein erneutes Announce, also Verkehr auf einem 10-Mbit/s-Bus. Für
den Zustand ist die Statusleitung aus [Risiken](#risiken-die-den-plan-kippen-können) zuständig.

**Bewusste Doppelung, die keine sein soll:** SOME/IP bringt mit **SOME/IP-SD** eine eigene
Entdeckung mit (`OfferService`/`FindService`, UDP 30490). Die wird hier **nicht** benutzt.
Aufteilung stattdessen: **mDNS entdeckt, SOME/IP formatiert.** Grund ist Werkzeug — `dns-sd`,
avahi und Wireshark beherrschen mDNS ohne AUTOSAR-Umgebung. Das ist eine Festlegung, damit nicht
später jemand SOME/IP-SD daneben baut.

**D.7 CLI auf dem Master.** `trig <action_id> <ms_in_future>`, absolut `trigat <action_id> <tx_ns>`,
periodisch `trigper <action_id> <period_ms>` mit `phase_ns = 0` (Ausrichtung auf die glatte
Grandmaster-Zeit, siehe G.1). Zusätzlich `trigto <ip> …` für den Unicast-Fall.

**Fertig, wenn** ein Kommando in Wireshark auf dem Mirror als SOME/IP dissektiert erscheint, ein
`dns-sd -B _t1strig._udp` beide Follower zeigt, und beide darauf feuern.

---

## Phase E — Trigger, Stufe Hardware

Erst hier entsteht ein MCC-Eingriff. Vorher prüfen, ob er nötig ist: **ist der Worst Case aus C.2
klein gegen die Anforderung, entfällt diese Phase vollständig.**

**E.1 Ein eigener 32-Bit-TC als einzige Zeitbasis** für lesen (stempeln) und Compare (triggern) —
sonst hat man zwei Zähler und muss ihr Verhältnis bestimmen.

Belegung heute: **TC0 ist voll** — `CC0` ist die Periode, `CC1` der Compare von `SYS_TIME`
([plib_tc0.c:176-192](firmware/src/config/default/peripheral/tc/plib_tc0.c#L176-L192)). Es existiert
nur `plib_tc0.c`, also sind **TC1…TC7 und alle TCC frei**. EVSYS ist im Projekt, aber mit **null
konfigurierten Kanälen**.

**Zu klären, bevor die Instanz gewählt wird** (in MCC bzw. im Datenblatt, nicht raten):

- Ein 32-Bit-TC entsteht auf dieser Familie aus einem **Instanzpaar** — welches Paar ist frei?
- Kann **ein** TC gleichzeitig einen Kanal als Compare und einen als Capture betreiben (`CAPTEN` pro
  Kanal)? Das entscheidet, ob Phase F dieselbe Instanz weiterbenutzt.
- Rückfallebene, falls nein: **zwei Timer am selben GCLK**. Sie laufen ratengleich, der konstante
  Versatz ist einmal durch zwei kurz aufeinanderfolgende Lesevorgänge bestimmbar. Kostet eine
  Instanz, keine Genauigkeit.

**Wichtig für die Auswahl heute:** die Capture-Fähigkeit wird noch nicht gebraucht, aber der TC soll
sie später haben. Also ein Paar mit **beiden Kanälen frei** wählen, nicht den ersten, der übrig ist.

**E.2 Überlauf und Schärfen.** 32 Bit bei 60 MHz laufen alle **71,6 s** über. Also nur schärfen, wenn
das Ziel im laufenden Fenster liegt — daraus folgt die Obergrenze in C.6. Den Compare-Wert **kurz
vor dem Feuern** berechnen, nicht beim Annehmen des Kommandos: der Extrapolationsfehler wächst mit
der Vorlaufzeit (10 ns bei 1 s, 1 µs bei 100 s bei 0,01 ppm Steigungsgenauigkeit).

**E.3 Pin, zwei Wege.**

1. Direkt ein Waveform-Ausgang (`WO[x]`) des TC. Setzt voraus, dass so ein Pin auf einem Header des
   Curiosity Ultra frei liegt — reine Pinout-Recherche, **kein Löten**.
2. **Compare → EVSYS → PORT-Event mit `SET`.** Pinwahl frei, Trigger vollständig in Hardware, und die
   Einmaligkeit ist sauber gelöst (ein `SET` setzt, es toggelt nicht bei jedem Durchlauf). Preis:
   EVSYS neu einrichten.

Mit 1 anfangen, wenn ein Pin da ist; 2 als Rückfallebene führen.

**E.4 Nach dem MCC-Eingriff** gilt wieder die Hausregel: `python test_mirror.py`, und prüfen, dass
der Mirror-TX-Hook und die `<itemPath>`-Einträge überlebt haben (`CLAUDE.md` Abschnitt 6).

**Fertig, wenn** am Zweikanal-Oszilloskop zwei Follower-GPIOs innerhalb der aus Phase A
vorhergesagten Spanne schalten.

---

## Phase F — 1PPS als Quelle, wenn der Draht liegt

Erst ab hier wird der Servo aus Phase 2 gebraucht: **ohne nachgeführte Wallclock ist das 1PPS
wertlos** — es käme pünktlich zu einer falschen Zeit. Das ist der Wert der abgeschlossenen Phase 2 in
diesem Plan: sie liegt bereit, bis der Draht kommt.

**F.1 Freischalten.** `PPSCTL` und `PADCTRL` gemeinsam — ohne das `PADCTRL`-Bit bleibt der Impuls
intern und erreicht DIOA4 nie. Register und Werte in
[LAN8651_TIME_SYNC.md §2](LAN8651_TIME_SYNC.md). Der Servo schaltet 1PPS erst im Zustand `FINE` ein.

**F.2 Neuer Provider `ptp_src_1pps.c`.** TC-Capture auf der Flanke, Paar an
`PTP_TB_SubmitPair()`. **Kein Min-Filter** — es gibt kein Δ.

**F.3 1PPS liefert Präzision, aber keine Identität.** Der Impuls sagt „jetzt ist eine
Sekundengrenze", nicht *welche*. Es braucht weiterhin eine grobe Zeitquelle zum Beschriften:
`MAC_TSL` lesen oder eben `t1`. **Der Frame-Weg verschwindet nie**, er wird von der Präzisionsquelle
zum Etikettierer. Die beiden Provider sind also **kombinierbar, nicht alternativ** — so bauen.

**F.4 Beim Umschalten springt die Absolutzeit** um die Größenordnung `Δ_min` (~100 µs): der
Frame-Weg trägt `Δ_min + D_const`, der 1PPS-Weg nur den Servo-Restfehler (93…256 ns) plus
`D_const`. Für „alle gleichzeitig" harmlos — **aber nur, wenn alle Knoten zusammen umgestellt
werden.** Gemischter Betrieb ist der schlechteste Fall von allen, weil die gemeinsame
Fehlerkomponente dann nicht mehr gemeinsam ist. **Regel: entweder alle mit Draht oder alle ohne.**

**Fertig, wenn** dieselbe Zweikanalmessung wie in Phase E eine messbar kleinere Spanne zeigt.

---

## Phase G — Demonstration, und was sie beweist

Zwei Follower toggeln ein GPIO; am Oszilloskop wird sichtbar, wie die Flanken bei eingeschalteter
Synchronisation zusammenlaufen und bei ausgeschalteter wieder auseinanderdriften. Das ist nicht nur
eine Vorführung — es ist die **aussagekräftigste Messung des ganzen Plans**, siehe G.3.

Voraussetzung ist der **periodische** Trigger (C.9) und der Modus `free` (C.6). Phase E ist nicht
nötig: die Demo funktioniert schon mit dem Software-Trigger, nur mit dessen Jitter als Bodensatz.

> **Verhältnis zu [PTP_DEMO.md](PTP_DEMO.md).** Dort zeigt §8 dieselbe Idee — Synchronisation aus,
> Drift sichtbar machen, wieder einrasten — aber über die **Konsole** und am **Servo-Offset der
> Wallclock**. Das hier ist die Variante mit **zwei GPIOs am Oszilloskop**, und sie misst etwas
> anderes: nicht den Servo, sondern die MCU-Zeitbasis und den Ausführungspfad. Die beiden ersetzen
> sich nicht, sie prüfen zwei verschiedene Ketten. Wer eine Bedienungsanleitung sucht, liest
> `PTP_DEMO.md`; wer wissen will, was noch zu bauen ist, liest hier.

### G.1 Die Phase muss absolut definiert sein — daran hängt alles

Ein Kommando „toggle ab jetzt alle 100 ms" richtet die Follower **nie** aus, auch bei perfekter
Synchronisation nicht: die Phase hinge daran, wann das Kommando bei jedem Knoten eintraf. Richtig ist
die Bedingung

```
feuere bei jedem Tx mit    Tx ≡ phase_ns   (mod period_ns)
```

gerechnet in **absoluter** Grandmaster-Zeit. Mit `phase_ns = 0` und 100 ms Periode feuern alle Knoten
auf der glatten Zehntelsekunde — unabhängig davon, wann sie das Kommando bekamen, und unabhängig von
Neustarts. Beim Empfang:

```
n  = (now_ns − phase_ns) / period_ns + 1
Tx = phase_ns + n · period_ns
```

### G.2 Drei Zustände, drei Bilder

| Zustand | Versatz A↔B | Driftrate | Bild am Scope |
|---|---|---|---|
| nie synchronisiert (`free`) | beliebig, bis zu **einer ganzen Periode** | volle Quarzdifferenz, ~10–20 ppm → **10–20 µs/s** | weit auseinander, wandert sichtbar |
| Sync an, konvergiert | wenige µs (Budget in §1) | ~0 | Flanken überlappen und stehen |
| Sync aus, Holdover | bleibt, wo er war | Differenz der Ratenfehler, ~0,01 ppm → **~10 ns/s** | driftet langsam auseinander |

Beim Einschalten sind **zwei Stufen** zu sehen, und das macht die Demo instruktiver als erwartet:

1. **Zusammenschnappen** in wenigen Sekunden — sobald das erste gefilterte Paar liegt, springt der
   Offset.
2. **Restdrift läuft aus** über ~50 s — so lange, wie der Ring braucht, bis die Steigung gut ist.

### G.3 Die Driftrate im Holdover ist eine Messung, kein Effekt

Was dort auseinanderläuft, ist die **Differenz der Ratenschätzungen** beider Follower. 1 µs in 100 s
sind 0,01 ppm. Damit liest man am Oszilloskop **direkt die Qualität des Fits aus Phase B ab** — eine
Zahl, die sonst kein Messmittel im Haus liefert. Wer die Synchronisation abschaltet, führt also nicht
bloß etwas vor, er kalibriert.

Umgekehrt diagnostisch: driftet es im Holdover **schnell** auseinander, also µs/s statt ns/s, dann ist
die Steigung schlecht bestimmt, und die Ursache liegt in Phase A/B — nicht am Trigger.

### G.4 Bedienung

Auf der Master-Seite ist nichts Neues nötig, `ptp start` / `ptp stop` / `ptp interval` existieren:

```
ptp stop            # Synchronisation aus
trigper 1 100       # beide Follower toggeln alle 100 ms
                    #   -> Flanken weit auseinander, wandern schnell
ptp start           #   -> schnappen zusammen, Restdrift läuft aus
ptp stop            #   -> driften langsam auseinander
```

### G.5 Scope-Einstellung

Auf Kanal A triggern, Kanal B beobachten — dann steht A und die Flanke von B wandert durchs Bild.
Zeitbasis so wählen, dass einige µs Versatz auflösbar sind; die 100 ms Periode ist nur die
Wiederholrate, nicht die Messgröße. Ein kurzer Impuls (~1 µs) gibt sauberere Vergleichsflanken als
ein 50-%-Toggle, ist aber Geschmackssache.

### G.6 Zwei Fallstricke der Demo selbst

- Im Modus `free` läuft die Zeitbasis auf nominaler Steigung mit **beliebiger Epoche** (Ticks seit
  Boot). Der Versatz kann bis zu einer ganzen Periode betragen und sieht in einem kurzen
  Scope-Fenster aus wie „auf B kommt gar nichts". Erst herauszoomen, dann hereinzoomen.
- `free` **muss** an der Statusleitung erkennbar sein (C.6) — sonst ist am nächsten Tag nicht mehr
  entscheidbar, ob ein Bild aus einem synchronisierten Lauf stammt.

**Fertig, wenn** die drei Bilder aus G.2 reproduzierbar sind und die Holdover-Driftrate zur
Steigungsgenauigkeit aus Phase A passt.

---

## Validierung

| Was | Mittel | braucht Messgerät? |
|---|---|---|
| Δ-Verteilung, `Δ_min` zweier Boards | Konsolen-Log + Python (Phase A) | nein |
| Callback-Verspätung, Worst Case | `late_ticks`-Histogramm über Stunden | nein |
| Zeitbasis konvergiert, Steigung stabil | `PTP_TB_Status()` | nein |
| **Gleichzeitigkeit zweier Follower** | **Zweikanal-Oszilloskop an zwei GPIOs** | ja, aber nur ein Scope |
| **Güte der Ratenschätzung** | Driftrate im Holdover, `ptp stop` (G.3) | dito |
| Fortschritt Frame-Weg → 1PPS-Weg | dieselbe Messung, vorher/nachher | dito |

Drei der sechs Zahlen kommen über die Konsole. Für die eigentliche Zielgröße genügt ein Zweikanal-
Oszilloskop an zwei GPIOs — **keine externe Zeitreferenz, kein Draht zum PHY.**

Bemerkenswert ist die letzte Zeile: das **Abschalten** der Synchronisation ist eine Messung, nicht
nur eine Vorführung. Die Geschwindigkeit, mit der die beiden Flanken auseinanderlaufen, *ist* die
Differenz der Ratenfehler — ein direkter Blick auf die Qualität von Phase B.

---

## Warum diese Reihenfolge

1. **Phase A zuerst**, weil die Machbarkeit an drei ungemessenen Zahlen hängt und die Messung fast
   nichts kostet. Ein Algorithmus, der auf geschätzten Zahlen ausgelegt ist, wird zweimal gebaut.
2. **B und C ohne jeden Peripherie-Eingriff.** Damit ist die vollständige Kette testbar — grob, aber
   vollständig — und die Frage „brauche ich Hardware?" wird durch eine Messung entschieden statt
   durch eine Annahme.
3. **D nach C**, weil ein Trigger über die Konsole leichter zu debuggen ist als einer über den Bus.
4. **E nur, wenn C.2 es verlangt.** Der MCC-Eingriff ist die einzige Stelle, an der dieser Plan
   dauerhaft Pflegeaufwand erzeugt — er soll begründet sein, nicht vorsorglich.
5. **G direkt nach D** — nicht am Ende. Die Demonstration braucht **kein** E und **kein** F, sondern
   nur den periodischen Trigger und den Modus `free`. Sie liefert damit früh das beste
   Erkenntnis-pro-Aufwand-Verhältnis des ganzen Plans: sie zeigt, ob die Kette überhaupt
   funktioniert, *und* misst über die Holdover-Drift die Güte der Ratenschätzung.
6. **F ganz am Ende**, weil es ein Hardware-Eingriff am Board ist und der Nutzen erst dann
   quantifizierbar ist, wenn E gemessen wurde.

Reihenfolge in der Praxis also: **A → B → C → D → G → (E) → (F)**, wobei E und F beide von Messungen
abhängen, nicht von Absichten.

---

## Risiken, die den Plan kippen können

| Risiko | Symptom | Gegenmittel |
|---|---|---|
| `Δ_min` hebt sich zwischen Boards **nicht** auf | Gleichzeitigkeit deutlich schlechter als erwartet | Phase A.2 auf zwei Boards, **vor** allem anderen |
| Hauptschleife dominiert Δ | Streuung nach Filter in hunderten µs | `L` früher lesen (Treiber-Callback statt Packet-Handler) — dann doch ein Patch |
| Längste kritische Sektion zu lang | einzelne Trigger stark verspätet | `late_ticks`-Histogramm, NVIC-Priorität, notfalls Phase E |
| Kein `WO`-Pin frei und EVSYS-Weg zu aufwändig | Phase E blockiert | Stufe-2-Trigger behalten, Anforderung neu bewerten |
| Kein TC-Paar mit zwei freien Kanälen | Phase F kann die Instanz nicht weiterbenutzen | zweiter Timer am selben GCLK (E.1) |
| Verlorene Kommandos bleiben unentdeckt | ein Follower schaltet nicht, keiner merkt es | Statusleitung (siehe unten) |
| `promiscuous` wird auf `false` gestellt | mDNS und jede Multicast-Entdeckung sterben **lautlos** | §2.4; Kommandos laufen deshalb über Broadcast |
| `ENV_VERSION` erhöht → `plca_id` überall gleich | alle Follower dieselbe IP | ARP-Probe mit Konsolenmeldung (§2.3) |
| Serial-Word 0 trägt einen Lot-Code | zwei Boards mit identischer MAC, sieht wie ein Bridge-Fehler aus | `dump 0x008061FC 1` auf beiden Boards (§2.1) |
| mDNS-Modul erfordert MCC-Lauf | Mirror-Hook und `<itemPath>` weg | `python test_mirror.py` danach (D.6) |

**Der ungelöste Punkt: die Einbahnregel.** Weil die Follower nichts senden, erfährt der Master
keinen der vier Verweigerungsgründe aus C.6 — er sendet ins Blaue. Drei Auswege:

1. Blind feuern und in Kauf nehmen. Für ein Laborexperiment vertretbar.
2. **Statusanzeige lokal am Follower** — LED oder zweiter GPIO „geschärft und Zeitbasis gut".
   Kostet nichts, bricht die Einbahnregel nicht, mit Auge oder Scope sofort ablesbar.
3. Ack-Frame erlauben. Bricht die Regel — allerdings nur für *Steuerverkehr*, nicht für
   Zeitverkehr, die Frequenzargumente aus §11.4 bleiben unberührt.

**Empfehlung: 2, und die Entscheidung fällen, bevor Code entsteht.** Eine Statusleitung nachträglich
einzuführen ist billig; ein Ack-Frame nachträglich einzuführen heißt, die Grundsatzfestlegung des
Projekts wieder aufzumachen.

**Nebenbefund:** soll die Bridge selbst mitschalten, ist sie voraussichtlich der schlechteste
Teilnehmer — sie hat `t1` von Natur aus richtig, aber ihre Hauptschleife ist mit Bridging und Mirror
deutlich stärker belastet als die eines Followers.

---

## Fallstricke

- **`t1` in reine Nanosekunden normalisieren, bevor subtrahiert wird.** Ist das Feld als Sekunden +
  Nanosekunden gepackt, wrappt der ns-Teil bei 999 999 999, **nicht** bei `2^30`. Naive 64-Bit-
  Subtraktion über eine Sekundengrenze ist dann um **73 741 824** daneben. Immer
  `ns_total = sek · 1000000000ULL + ns`.
- **`sequenceId`-Paarung ist Pflicht.** `t1` steht im `Follow_Up`, `L` gehört zum `Sync`. Ohne
  Zuordnung paart man irgendwann über ein verlorenes Frame hinweg — Fehler von einem ganzen
  Intervall, und zwar lautlos.
- **`SYS_TIME` zählt in Hardware nur 16 Bit**, also 1,092 ms bis zum Überlauf; die oberen Bits führt
  Software nach. Ein blockierter Compare-Interrupt kostet eine ganze Periode. Der Min-Filter ist
  dagegen immun (der Fehler ist immer *positiv*), und im Histogramm erscheint er als **scharfe
  Spitze bei genau 1,092 ms** — verschmierter Jitter sieht anders aus.
- **In Ticks rechnen, nicht in µs.** `SYS_TIME_CountToUS()` rundet auf Mikrosekunden und wirft genau
  die Auflösung weg, um die es geht.
- **`SYS_TIME_FrequencyGet()` liefert 60000000 — den Nominalwert.** Der echte weicht um den
  Quarzfehler ab; ihn zu bestimmen *ist* die Aufgabe. Der Faktor `slope` ist am Ende die wahre
  Tickdauer, und die kennt nur die Messung.
- **Kein `unsigned __int128` auf 32-Bit-ARM.** Q24 statt Q32, Anker regelmäßig nachziehen.
- **Filter auf dem Residuum, nicht auf `L − t1`** — sonst wählt er driftabhängig immer den Rand des
  Blocks (B.2).
- **Das SOME/IP-Feld `Length` zählt nicht die ganze Nachricht.** Es beginnt bei der `Request ID`,
  ist also `8 + Nutzlast` — die ersten 8 Header-Bytes (Service ID, Method ID, Length selbst) zählen
  **nicht** mit. Ein Off-by-8 hier wird von Wireshark als „malformed" angezeigt, von einem
  selbstgeschriebenen Parser aber gern stillschweigend akzeptiert.
- **Kommandos nicht auf IPv4-Multicast legen.** Broadcast wird vom MAC-Filter immer angenommen,
  Multicast hängt am Stub-Filter und damit an einer einzigen Konfigurationszeile (§2.4).
- **Kein SPI, kein Frame-Versand, keine Konsolenausgabe im ISR-Callback.** Beides hängt an der
  Task-Ebene; ein Callback, der dort sendet oder druckt, zerstört genau die Zeitmessung, für die er
  da ist.
- **DWT ist hier nicht der richtige Zähler.** `DWT->CYCCNT` wäre feiner (8,3 ns), aber 32 Bit
  laufen bei 120 MHz alle **35,8 s** über, er hängt am Debug-Block (`TRCENA` muss selbst gesetzt
  werden, der Probe setzt es und verdeckt dadurch einen fehlenden Init), und er friert in
  Sleep-Modi ein. `SYS_TIME_Counter64Get()` hat 16,67 ns, 64 Bit und keine dieser Eigenschaften —
  und die Auflösung war ohnehin nie die Grenze.
