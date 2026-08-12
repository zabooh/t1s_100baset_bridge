# PLCA-Verhalten am Bus ausmessen — Runbook

> **Wozu.** Der Bootstrap-Entwurf in
> [PTP_TIMEBASE_PLAN.md §2](PTP_TIMEBASE_PLAN.md#2-adressierung-und-identität) ruht auf Annahmen über
> das Verhalten des LAN8651 am Multidrop-Bus, die bisher **nicht gemessen** sind. Dieses Runbook
> misst sie mit den Mitteln, die die beiden Firmwares schon haben — kein Oszilloskop, kein
> Protokollanalysator, kein Löten.
>
> **Ergebnisse gehören nach [test_results.md](test_results.md)**, nicht in diese Datei. Hier steht,
> *wie* gemessen wird; dort, *was* herauskam und was es bedeutet.
>
> Zwei Boards mit LAN8651: die **Bridge** (Probe `ATML3264031800001049`, PLCA-Koordinator, Node 0)
> und der **Follower** (Probe `ATML3264031800001103`). Ein dritter Knoten — der T1S-Endpoint, der
> 1 Hz SOME/IP-SD sendet — ist für T4 nötig und für alles andere gleichgültig.

---

## Inhalt

- [Vorbereitung](#vorbereitung)
- [Drei Regeln, sonst misst du dich selbst](#drei-regeln-sonst-misst-du-dich-selbst)
- [Registerübersicht](#registerübersicht)
- [T0 — Grundlinie](#t0--grundlinie)
- [T1 — Beacon-Bit in PLCA_STATUS](#t1--beacon-bit-in-plca_status)
- [T2 — Sendet der Follower ohne Beacons?](#t2--sendet-der-follower-ohne-beacons)
- [T3 — Empfängt ein Knoten mit PLCA aus?](#t3--empfängt-ein-knoten-mit-plca-aus)
- [T4 — Doppelte Node-ID](#t4--doppelte-node-id)
- [T5 — Überlebt der Registerschreibzugriff einen Reinit?](#t5--überlebt-der-registerschreibzugriff-einen-reinit)
- [T6 — Kollisionserkennung](#t6--kollisionserkennung)
- [T7 — Der IP-Konflikt, der vermutlich gerade läuft](#t7--der-ip-konflikt-der-vermutlich-gerade-läuft)
- [Notausgang](#notausgang)
- [Reihenfolge und Zeitbedarf](#reihenfolge-und-zeitbedarf)

---

## Vorbereitung

**COM-Ports finden.** Jedes Board hat einen EDBG-COM-Port, 115200 8N1. Welcher zu welchem Board
gehört, sagt am schnellsten ein Kommando:

```
python cli.py --port COM8 --read 3 "stats"
```

Die Bridge nennt beide Interfaces (`eth0` *und* `eth1`), der Follower nur `eth0`. Alternativ zeigt
`python flash_same54.py --list` die angeschlossenen Probes; die Zuordnung Probe → Projekt steht in
[boards.json](boards.json).

Ab hier gilt die Schreibweise:

```
[B] <kommando>      auf der BRIDGE      -> python cli.py --port COM_B --read 1 "<kommando>"
[F] <kommando>      auf dem FOLLOWER    -> python cli.py --port COM_F --read 1 "<kommando>"
```

`--read 1` genügt für `lan_read`/`lan_write`. Für `showenv`, `stats`, `noip_stat` und `ptpf status`
mehr Zeilen anfordern (`--read 12`), sonst wird die Ausgabe abgeschnitten.

**Vor dem ersten Test ins Protokoll:** Datum, `git rev-parse --short HEAD`, welche Firmware auf
welchem Board läuft, COM-Zuordnung, und ob der dritte Knoten am Bus hängt.

---

## Drei Regeln, sonst misst du dich selbst

1. **Register setzen, dann SPI in Ruhe lassen, dann messen.** Zyklisches `lan_read` parallel zu
   Verkehr hat an baugleicher Firmware ~5 % Paketverlust erzeugt (`CLAUDE.md` Abschnitt 4). Wer
   während einer Verlustmessung pollt, misst seinen eigenen Registerzugriff. Zähler **vorher** und
   **nachher** lesen, nie dazwischen.
2. **Alles hier ist flüchtig.** `plca_node` und rohe Registerschreibzugriffe überleben keinen Reset —
   ein **Power-Cycle stellt jeden Zustand wieder her**. Die CLI hängt am UART, nicht am T1S: du kannst
   dich nicht aussperren.
3. **T0 zuerst, immer.** Ohne Grundlinie ist ein später gemessener Verlust nicht zuzuordnen.

**Nicht messen, während etwas anderes läuft.** Diese Tests unterbrechen den T1S-Link und das
Slotting. Eine parallel laufende Scope-Aufnahme oder PTP-Messung ist danach wertlos.

---

## Registerübersicht

| Register | Adresse | Inhalt |
|---|---|---|
| `PLCA_CTRL0` | `0x0004CA01` | **Bit 15 = Enable**, `0x8000` = an |
| `PLCA_CTRL1` | `0x0004CA02` | `NODE_CNT << 8 \| NODE_ID` |
| `PLCA_STATUS` | `0x0004CA03` | read-only, Bitbedeutung → T1 |
| `PLCA_BURST` | `0x0004CA05` | Burst-Modus |
| `COL_DET_CTRL0` | `0x00040087` | vom Treiber auf `0x83` gesetzt („Disable Collision Detection") |
| `T1SPMACTL` | `0x000308F9` | `RST`/`TXD`/`LPE`/`MDE`/`LBE`, siehe `CLAUDE.md` Abschnitt 4 |
| `T1STSTCTL` | `0x000308FB` | IEEE-Testmodi, Bits 15:13 |
| `OA_CONFIG0` | `0x00000004` | `FTSE`/`FTSS` in Bits 7:6 |
| Serial Word 0 | `0x008061FC` | über `dump`, nicht `lan_read` (MCU-Adresse, kein PHY-Register) |

---

## T0 — Grundlinie

**Ziel.** Ausgangszustand festhalten und die Messstrecke kalibrieren. Erledigt gleichzeitig die
Seriennummernfrage aus [§2.1](PTP_TIMEBASE_PLAN.md#21-was-heute-schon-stimmt-die-mac).

**Kommandos** — auf **beiden** Boards, Ausgaben vollständig ins Protokoll:

```
dump 0x008061FC 1
showenv
lan_read 0x0004CA01
lan_read 0x0004CA02
lan_read 0x0004CA03
lan_read 0x00040087
```

Dann die Verluststrecke kalibrieren:

```
[F] noip_send 5 200
    ... 2 s warten ...
[B] noip_stat
```

**Erwartung**

| Prüfung | Soll |
|---|---|
| Serial Word 0, untere 24 Bit | auf den beiden Boards **verschieden** |
| `PLCA_CTRL0` | `0x00008000` auf beiden |
| `PLCA_CTRL1` Bridge | `NODE_ID = 0` (Koordinator) |
| `PLCA_CTRL1` Follower | `NODE_ID ≠ 0` |
| `COL_DET_CTRL0` | `0x00000083` |
| `noip_stat` auf der Bridge | **5 von 5**, Sequenz lückenlos |

**Bedeutung.** Sind die unteren 24 Bit der Seriennummer gleich, ist die MAC-Ableitung unsicher und
§2.1 braucht den CRC32 über alle vier Worte. Kommen nicht 5/5 an, hat der Bus **ohne Zutun** ein
Problem — dann hier abbrechen und die Ursache suchen, alles Weitere wäre wertlos.

**Nicht mehr als 5 Frames anfordern.** `noip_send` bekommt pro Kommando nie mehr weg, die
Rohsende-Queue hat vier Plätze (`CLAUDE.md` Abschnitt 6).

---

## T1 — Beacon-Bit in `PLCA_STATUS`

**Ziel.** Herausfinden, welches Bit anzeigt, dass Beacons anliegen. Das ist das Handle, mit dem ein
Follower später selbst erkennt, ob das Segment geslottet fährt.

```
[B] lan_read  0x0004CA03          # Koordinator, Beacons an
[F] lan_read  0x0004CA03          # Follower,   Beacons an
[B] lan_write 0x0004CA01 0x0000   # Beacons aus
    ... 2 s warten ...
[F] lan_read  0x0004CA03          # Follower,   Beacons aus
[B] lan_write 0x0004CA01 0x8000   # zurück
[F] lan_read  0x0004CA03          # Kontrolle: wieder wie vorher?
```

**Erwartung.** Mindestens ein Bit im Follower-Wert ändert sich zwischen „Beacons an" und „Beacons
aus" und kehrt danach zurück.

**Bedeutung.** Ändert sich **nichts**, gibt es kein beobachtbares Beacon-Signal, und ein Follower
kann seinen Modus nicht selbst erkennen — dann muss das Bootstrap-Fenster ausschließlich über die
angekündigte Dauer laufen, ohne Rückversicherung am Register.

**Ins Protokoll:** die vier Rohwerte und die Bitmaske der Differenz.

---

## T2 — Sendet der Follower ohne Beacons?

**Die tragende Frage des ganzen Entwurfs.** Zwei Kommandos je Seite, binäres Ergebnis.

```
[B] noip_stat                     # Zählerstand VORHER notieren
[B] lan_write 0x0004CA01 0x0000   # keine Beacons mehr
[F] noip_send 5 200               # Follower unangetastet, PLCA bleibt an
    ... 3 s warten, kein weiterer SPI-Zugriff ...
[B] noip_stat                     # Zählerstand NACHHER
[B] lan_write 0x0004CA01 0x8000   # zurück
```

**Bedeutung**

| Ergebnis | Deutung | Folge für §2 |
|---|---|---|
| **5 von 5 angekommen** | PLCA fällt bei Beacon-Verlust auf CSMA zurück | Der Fallback ist ein echtes zweites Netz. Explizites PLCA-Aus bleibt der Hauptweg, hat aber eine Rückversicherung. |
| **0 von 5** | Kein Fallback — der Follower wartet auf einen Slot, der nie kommt | Explizites PLCA-Aus auf **jedem** Knoten ist **zwingend**. Ein Bootstrap ohne das würde lautlos verklemmen. |
| teilweise | unerwartet | genau protokollieren, welche Sequenznummern fehlen |

**Gegenprobe, falls 0/5:** dieselbe Sendung mit `[F] lan_write 0x0004CA01 0x0000` davor (Follower
explizit in CSMA). Kommen die Frames dann an, ist bewiesen, dass der Modus die Ursache war und nicht
etwas anderes. Danach `0x8000` zurück.

---

## T3 — Empfängt ein Knoten mit PLCA aus?

**Ziel.** Prüfen, ob Empfangen modusunabhängig ist. Darauf ruht die Annahme, dass ein Knoten die
Fensterankündigung **immer** hören kann.

```
[B] ptp status                    # Grandmaster muss laufen; sonst 'ptp start'
[F] ptpf status                   # Sync-/Follow_Up-Zähler VORHER notieren
[F] lan_write 0x0004CA01 0x0000   # nur der Follower geht in CSMA
    ... 10 s warten, kein SPI-Zugriff ...
[F] ptpf status                   # Zähler NACHHER
[F] lan_write 0x0004CA01 0x8000   # zurück
```

**Erwartung.** Bei 100 ms Intervall etwa **100 Sync** in 10 s.

**Bedeutung.** Zähler laufen weiter → Empfang ist modusunabhängig, der Bootstrap-Entwurf trägt.
Zähler stehen → die Ankündigung erreicht einen Knoten in CSMA nicht, und das Fenster muss anders
signalisiert werden (z. B. rein zeitgesteuert, ohne Ankündigung).

**Nebenertrag:** Läuft der Zähler weiter, ist gleichzeitig belegt, dass ein Knoten mit PLCA aus die
Zeit weiter mitbekommt — die Zeitbasis überlebt also ein Bootstrap-Fenster.

---

## T4 — Doppelte Node-ID

**Einschränkung vorweg.** Mit zwei Boards ist die *echte* Situation nicht herstellbar: einer muss
Koordinator (ID 0) sein, sonst gibt es keine Beacons. Zwei *Follower* mit gleicher ID gehen nur mit
einem dritten Knoten.

**Erst prüfen, ob der dritte Knoten da ist:**

```
[B] mirror on
    tshark -i <eth1-Adapter> -a duration:5
[B] mirror off
```

Sieht man 1-Hz-Verkehr von einer dritten MAC, ist die echte Variante möglich.

### T4a — echte Variante (dritter Knoten vorhanden)

Node-ID des Endpoints ermitteln (aus seiner Konfiguration oder durch Ausprobieren), dann:

```
[F] plca_node <id_des_endpoints>     # flüchtig, Reboot stellt env wieder her
    ... 30 s beobachten ...
[B] mirror on   + tshark             # kommt der 1-Hz-Verkehr des Endpoints noch?
[F] noip_send 5 200
[B] noip_stat                        # Verlust des Followers?
[F] plca_node <original_id>
```

### T4b — Ersatzvariante (nur zwei Boards)

```
[F] plca_node 0        # zwei Knoten im Beat 0, zwei Beacon-Quellen
[F] noip_send 5 200
[B] noip_stat
[F] ptpf status        # kommt vom Master noch etwas an?
[F] plca_node <original_id>
```

**Bedeutung.** Physikalisch derselbe Mechanismus — zwei Sender im gleichen Beat —, aber nicht
dieselbe Situation, weil zusätzlich zwei Beacon-Quellen im Spiel sind. **Nur mit dieser
Einschränkung dazugesagt protokollieren.** Interessant ist vor allem, *ob* der Ausfall total ist oder
teilweise: ein teilweiser Ausfall ist im Betrieb schwerer zu erkennen als ein totaler.

---

## T5 — Überlebt der Registerschreibzugriff einen Reinit?

**Ziel.** Klären, ob ein Bootstrap-Fenster als einmaliger Registerschreibzugriff haltbar ist.

```
[F] lan_write 0x0004CA01 0x0000
[F] lan_read  0x0004CA01          # Kontrolle: 0x00000000
[F] testmode 4                    # Transmitter High-Z -> Link bricht
    ... 3 s ...
[F] testmode 0                    # Link kommt zurück
    ... 5 s ...
[F] lan_read  0x0004CA01
```

**Bedeutung**

| Ergebnis | Folge |
|---|---|
| bleibt `0x00000000` | Ein einmaliger Schreibzugriff hält. |
| wieder `0x00008000` | Der Treiber schreibt PLCA bei Link-Ereignissen neu. Das Fenster muss **von Firmware gehalten und nachgezogen** werden, sonst springt ein Knoten mitten im Bootstrap zurück in PLCA und zerlegt es für alle anderen. |

**Zurückstellen:** `[F] lan_write 0x0004CA01 0x8000`, danach `lan_read` zur Kontrolle. `testmode`
ist für diesen Zweck bereits verifiziert (alle vier Modi, `test_lan8651.py`).

---

## T6 — Kollisionserkennung

**Vorbedingung: Bitbedeutung von `COL_DET_CTRL0` aus dem Datenblatt.** Ohne sie **nicht** an dem
Register drehen — `0x83` ist ein Wert, dessen Felder wir nicht kennen. Dieser Test bleibt bis dahin
offen.

Danach der empirische Teil: beide Boards in CSMA (`lan_write 0x0004CA01 0x0000` auf beiden), von
beiden möglichst gleichzeitig `noip_send 5 0` abschicken, Verlust zählen — einmal mit `0x83`, einmal
mit eingeschalteter Erkennung.

**Grenzen dieses Tests:** die Gleichzeitigkeit ist von Hand nicht herstellbar, die Aussage also
statistisch und grob. Ein deutlicher Unterschied wäre trotzdem sichtbar. Schwächster Test des
Runbooks, gehört zuletzt.

---

## T7 — Der IP-Konflikt, der vermutlich gerade läuft

**30 Sekunden, und er belegt die ganze Adressdiskussion aus §2.**

Der Follower-Default für `eth0` ist `192.168.0.200` — dieselbe Adresse wie `eth0` der Bridge
([follower/…/env.c:99](follower/firmware/src/env.c#L99) über
`TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0`). Wurde auf dem Follower nie ein `saveenv` mit anderer
Adresse ausgeführt, läuft der Konflikt jetzt.

```
[F] showenv                       # welche IP steht wirklich drin?
[B] showenv
[PC] ping 192.168.0.200
[PC] arp -a | findstr 192.168.0.200
```

**Bedeutung.** Wechselnde MAC im ARP-Cache oder uneinheitliche Ping-Antworten = Konflikt bestätigt.
Ist er da, ist er auch der Grund, warum spätere UDP-Kommandos unzuverlässig ankämen — und §2.2 ist
keine Vorsichtsmaßnahme, sondern eine Reparatur.

---

## Notausgang

| Symptom | Maßnahme |
|---|---|
| Bus tot, Zustand unklar | **Power-Cycle beider Boards.** Alle Registerschreibzugriffe und `plca_node` sind flüchtig. |
| PLCA-Zustand unklar | `lan_read 0x0004CA01` auf beiden; Koordinator muss `0x00008000` **und** `NODE_ID = 0` haben |
| Follower ist kein Koordinator mehr / Bus stumm | `[B] plca_node 0`, dauerhaft `setenv plca_id 0` + `saveenv` |
| Ein Testmodus wurde vergessen | `testmode` ohne Argument zeigt den Zustand, `testmode 0` stellt zurück |
| Nichts geht mehr, Verdacht auf `env` | `resetenv` (Defaults), danach `showenv` prüfen — **Achtung:** setzt auch `plca_id` zurück |

Der Link kommt nach jedem dieser Schritte von selbst wieder; die CLI ist nie betroffen.

---

## Reihenfolge und Zeitbedarf

| Test | Frage | Aufwand | Blockiert |
|---|---|---|---|
| **T0** | Grundlinie, Seriennummern | 10 min | alles |
| **T2** | Fällt PLCA ohne Beacons auf CSMA zurück? | 5 min | §2 Bootstrap-Mechanismus |
| **T3** | Empfängt ein CSMA-Knoten? | 5 min | §2 Fensterankündigung |
| **T7** | Läuft der IP-Konflikt schon? | 1 min | — |
| **T1** | Beacon-Bit | 5 min | Umsetzungsdetail |
| **T5** | Reinit-Festigkeit | 10 min | Umsetzungsdetail |
| **T4** | Doppelte Node-ID | 20 min | Diagnosewissen |
| **T6** | Kollisionserkennung | offen | braucht Datenblatt |

**T0 → T2 → T3** sind die Kette, die entscheidet, ob der Bootstrap-Entwurf so tragen kann wie
besprochen. Zusammen unter einer halben Stunde. Alles danach ist Verfeinerung.

**Ergebnisse und deren Beurteilung nach [test_results.md](test_results.md)** — je Test die
Rohausgabe, das Urteil, und was daraus für den Plan folgt.
