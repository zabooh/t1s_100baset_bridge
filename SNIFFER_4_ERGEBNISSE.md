# Sniffer/Mirror-Großframe-Bug: Ergebnisse

2026-08-27. Ergebnisse der Tests aus `SNIFFER_2_TESTPLAN.md`, umgesetzt in
`scripts/sniffer_noip_investigation.py`.

## Wichtige Einschränkung zuerst: zwei verschiedene TC6-Sendepfade

`noip_send` (und `bigframe`) nutzen `TC6_SendRawEthernetPacket()` — einen
**einzelnen, unsegmentierten** Rohframe-Pfad. iperf/TCP/UDP laufen dagegen
über den normalen Stack (`TCPIP_MAC_PacketTx()` → `TC6_GetRawSegments()` /
`TC6_SendRawEthernetSegments()`), der **segmentiert** und von
`TC6_TX_ETH_MAX_SEGMENTS`/`TC6_CONCAT_THRESHOLD` gesteuert wird. Aus den
früheren iperf-UDP-Tests dieser Session (Nachtrag 3, `BANDWIDTH_7_MATRIX.md`)
ist bereits belegt: **der segmentierte Pfad überträgt große Payloads (bis
1468 Byte) zwischen zwei echten T1S-Knoten mit 0 % Verlust** — das
Mirror/`eth1`-Problem trat dort ausschließlich bei der Weiterleitung zum PC
auf, nie bei der reinen T1S-Übertragung selbst.

**Der mit `noip_send` gefundene Bug (unten) betrifft also nachweislich einen
anderen Codepfad als den, der im ursprünglichen Sniffer/Mirror-Vorfall
beteiligt war.** Er ist real und reproduzierbar, aber vermutlich **nicht**
dieselbe Ursache wie der ursprüngliche Aussetzer. Das relativiert die
Testmethode (T1–T4 wie geplant), nicht aber den Fund selbst.

## T2 — Sequenznummern-Abgleich (durchgeführt, mit Methodik-Korrektur)

**Erster Versuch** (`noip_send <n> <gap> <size>`, Firmware-eigene Schleife):
schlug bei `size=1515` **und** `size=60` gleichermaßen bei `seq=6` fehl,
unabhängig von `gap_ms` (0 und 5 getestet). Ursache: bereits im
Code-Kommentar zu `NOIP_SendOne()` dokumentiert — `TC6_SendRawEthernetPacket()`
reiht nur einen **Zeiger** auf den einzigen, wiederverwendeten `s_frame`-
Puffer ein, keine Kopie; ein Mehrfach-Aufruf überschreibt den Puffer, bevor
der vorherige Frame die SPI-Übertragung abgeschlossen hat. Kein neuer Bug,
sondern das bereits bekannte Limit dieser Schleife — als Testmethode damit
untauglich.

**Korrigierte Methode**: Wiederholung aus Python getrieben, ein
`noip_send 1 0 <size>`-Aufruf pro Frame (wie `NOIP_SendOne()` es für den
PTP-Trigger-Pfad schon vormacht). Ergebnisse, jeweils Bridge frisch
resettet, `sniffer` an, Follower B → Bridge:

| Größe | Anzahl | Ergebnis (Follower B `sent`) | Angekommen an der Bridge (`noip_stat RX`) | Angekommen beim PC |
|---|---|---|---|---|
| 60 Byte  | 30 | 30/30 `sent`, kein Fehler | **30/30** | **30/30**, lückenlos seq 1–30 |
| 1515 Byte | 30 | 30/30 `sent`, kein Fehler | **0/30** | **0/30** |

Follower B's **eigener** `eth0 TX`-Zähler (`stats`, nicht `noip_stat`)
steigt bei den 1515-Byte-Sendungen sauber mit (`ok` +6 für 3 Sendungen,
`err=0`, `qFull=0`, plus etwas Hintergrundverkehr) — der Sender registriert
auf Hardware-Ebene **keinen** Fehler. Die Bridge zeigt im selben Fenster
weder einen `eth0 RX`-Fehler noch einen `nobufs`-Anstieg — sie registriert
**gar nichts**, weder Erfolg noch Fehler, für diese Frames.

## Einordnung

Große (>~1514 Byte), über den **unsegmentierten** Rohpfad gesendete Frames
werden vom Sender ohne Fehlermeldung "verschickt", kommen aber **nirgendwo**
an — nicht beim Empfänger-Knoten (`noip_stat`), nicht beim PC. Das legt nahe,
dass die Übertragung auf der SPI/T1S-Ebene **unvollständig** bleibt: ein
einzelner `serviceData()`-Aufruf direkt beim Einreihen
(`TC6_SendRawEthernetPacket()`, `tc6.c:304`) überträgt nur einen begrenzten
Chunk-Batch (`maxTxLen`, gedeckelt auf `sizeof(entry->txBuff)`); ein
1515-Byte-Frame braucht deutlich mehr Chunks, als in einen Batch passen.
Ob der Rest zuverlässig über die periodische Treiber-Task nachgezogen wird,
ist die offene Frage — die Beobachtung (Sender zählt Erfolg, niemand
empfängt etwas, kein Fehler irgendwo) passt zu einem **abgeschnittenen,
unvollständigen Frame**, der von keinem Empfänger als gültig (oder
überhaupt) erkannt wird.

**Das ist ein eigenständiger, echter Bug im unsegmentierten Rohpfad
(`noip_send`/`bigframe` bei Größen > ~1514 Byte) — aber mangels Beteiligung
des segmentierten Pfads vermutlich nicht die Ursache des ursprünglichen
Mirror/`eth1`-Aussetzers.**

## T1, T3, T4 — nicht mehr wie ursprünglich geplant durchgeführt

Da T2 bereits zeigt, dass die Grundannahme (`noip_send` als sauberer Test
für den Mirror-Pfad) wegen des anderen Sendepfads nicht trägt, wurden T1/T3
(Timing-Varianten desselben Aufbaus) und T4 (bereits separat mit `bigframe`
durchgeführt, siehe `SNIFFER_1_HYPOTHESEN.md`, Punkt 3) nicht weiter mit
dieser Methode verfolgt.

## Nachtrag — Ground-Truth-Zähler und der endgültige Beweis (segmentierter Pfad)

`port_mirror.c` um echte, treiberbestätigte Sendezähler erweitert: `pkt->ackRes`
(von `TCPIP_MAC_PKT_ACK_TX_OK`, vom MAC-Treiber selbst beim TX-Abschluss
gesetzt, siehe `tcpip_mac.h`) wird jetzt in `mirror_pkt_ack()` ausgewertet und
als `ack_ok`/`ack_fail`/`max_len_submitted`/`max_len_ok` mitgeführt — ein
Beweis für "die Hardware hat die Übertragung wirklich abgeschlossen", nicht
nur "die Software hat die API aufgerufen" (`tx_submitted`).

**Test mit dem tatsächlich betroffenen, segmentierten Pfad** (`iperf -u -l 1469`,
Follower B → Follower A, Bridge nur als Sniffer dazwischen, alle drei Boards
frisch resettet, PC-Mitschnitt parallel):

- **Bridge-Zähler nach dem Lauf:** `rx_hook=985 passed_filter=985
  tx_submitted=985 ack_ok=985 ack_fail=0 max_len_submitted=1515
  max_len_ok=1515` — **jeder einzelne Frame, auch der größte, wurde vom
  GMAC-Treiber als erfolgreich abgeschlossen bestätigt.** `uptime` durchgehend
  (kein Reset), `stats` durchgehend `err=0`.
- **PC-Mitschnitt im selben Zeitfenster:** `udp.port==5001`-Frames im
  Mitschnitt: **0 von 985.** Bis t≈12,9 s nur der normale
  SOME/IP-SD-Hintergrundverkehr sichtbar, danach ausschließlich noch
  lokaler PC-eigener Verkehr — der T1S-gespiegelte Strom verschwindet
  komplett aus dem Mitschnitt, obwohl die Bridge durchgehend Erfolg meldet.

**Damit ist der Beweis erbracht: Der Verlust passiert nachweislich NACH der
hardwarebestätigten Übertragung durch den GMAC der Bridge** — auf der
Leitung, am LAN8740A-PHY, oder beim PC-seitigen USB-Ethernet-Adapter/Npcap.
Zusammen mit den bereits bekannten Befunden (kein echtes
Windows-Trennungsereignis im Ereignisprotokoll, Npcaps eigener Hinweis "kein
zu meldender Bug", `bigframe` löst das Problem bei Einzelframes nie aus)
spricht das stark für eine **PC-/Adapter-seitige Ursache, nicht für einen
Bridge-Firmware-Bug.**

## Root Cause außerhalb dieses Repos — Abmilderung statt Bugfix

Der ursprüngliche Mirror/`eth1`-Bug ist damit **eingegrenzt** (nicht die
Bridge), aber nicht auf Code-Ebene root-causebar, weil die Ursache
wahrscheinlich außerhalb dieses Repos liegt (USB-Adapter-Treiber/Npcap auf
diesem PC). Ein Firmware-seitiger Bugfix ist deshalb nicht möglich — die
Bridge kann nur verhindern, dass sie das PC-seitige Problem überhaupt
auslöst.

**Umgesetzt:** `mirror_ethpkt_to_eth1()` (`port_mirror.c`) kürzt jeden
gespiegelten Frame vor der Übergabe an `eth1` auf `MIRROR_SAFE_FRAME_LEN`
(1514 Byte, die bestätigte Grenze) — statt ihn zu verwerfen. Ein neuer
Zähler `truncated` zeigt, wie oft das greift. Die echte T1S-Verbindung
zwischen den tatsächlichen Endpunkten ist davon unberührt, nur was im
Mitschnitt landet wird gekappt (bei TCP entsprechend mit ungültiger
Prüfsumme jenseits der Kappgrenze — erwartet, da diese Kopie nie verarbeitet,
nur angezeigt wird).

**Verifiziert** (frisch resettete Boards, `sniffer` an, PC-Mitschnitt
parallel, zwei Läufe: `iperf -u -l 1469` — die vorher sicher fehlschlagende
Größe — und `-l 1470`, iperf-Standard):

- Bridge: `truncated=1840`, `max_len_submitted=1514` (vorher 1515),
  `ack_ok=1946 ack_fail=0`, `uptime` durchgehend (85 s, kein Reset).
- PC-Mitschnitt: **1840 von 1840** gekürzten Frames angekommen, alle exakt
  1514 Byte lang. `tshark` lief die vollen 60 s ohne Unterbrechung, **keine**
  „adapter no longer attached"-Meldung. Die einzige Lücke im Mitschnitt
  (9,5 s) liegt exakt zwischen den beiden nacheinander gestarteten
  Testläufen, nicht innerhalb eines Laufs.

**Damit ist bestätigt: die Kürzung verhindert das PC-seitige Symptom
zuverlässig**, ohne den eigentlichen (externen) Fehler zu beheben.
