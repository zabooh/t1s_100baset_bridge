# Sniffer/Mirror-Großframe-Bug: Implementierung der Tests

Umsetzung von `SNIFFER_2_TESTPLAN.md` in **`scripts/sniffer_noip_investigation.py`**.

## Firmware-Voraussetzungen (bereits umgesetzt und geflasht)

- **`noip_send <n> [gap_ms] [size]`** (Bridge + Follower, `noip_test.c`) —
  `size` (Gesamtframelänge, ohne FCS) neu, Default 60 (Ethernet-Minimum),
  Bereich 60..1518. Vorher fest auf 60 Byte. `NOIP_MAX_COUNT` von 100 auf
  1000 angehoben, damit auch längere Bursts (T2/T3) in einem Aufruf gehen.
- **`bigframe <total_len>`** (Bridge, `port_mirror.c`, neu) — ein einzelner,
  frisch allozierter Rohframe direkt auf `eth1`, EtherType `0xFEED`,
  Ziel-MAC Broadcast, hochzählendes Füllmuster. Eigenständiger Bug dabei
  gefunden und behoben: fehlendes `ackFunc` ließ jeden Aufruf einen Puffer
  lecken (`TCPIP_PKT_PacketAlloc()` setzt kein Default-`ackFunc`) — jetzt
  über `bigframe_pkt_ack()` sauber freigegeben.

## Testskript

`scripts/sniffer_noip_investigation.py t1|t2|t3|t4 [--size N] [--count N] [--gap-ms N] [--duration N]`

- **`t1`** — Reset Bridge+Follower B, `sniffer 1`, Burst per `noip_send`
  lostreten (ohne auf die Antwort zu warten), währenddessen jede Sekunde
  `sniffer`+`stats` auf der Bridge pollen und mitschreiben. Prüft H1
  (Pool-Erschöpfung, die der End-Snapshot verpasst).
- **`t2`** — Burst senden, danach den Mitschnitt (gefiltert auf
  `eth.type==0x88b5`) auswerten: Sequenznummern (erste 4 Byte der
  Nutzlast, `data.data`-Feld) extrahieren, mit den vom Follower als
  "sent" geloggten abgleichen, fehlende Nummern auflisten und einordnen
  (harter Abbruch an einer Stelle vs. verstreuter Verlust). Prüft H2.
- **`t3`** — derselbe Burst bei `gap_ms` = 0/2/5/10/20, um zu sehen, ob
  Pausen zwischen Frames den Fehler seltener machen oder beheben. Prüft H3/H5.
- **`t4`** — `bigframe <size>` viele Male hintereinander, **ohne** Follower/
  T1S überhaupt zu beteiligen — reine Wiederholung großer Frames auf `eth1`
  allein. Prüft H4 (PC-/Adapter-seitige Ursache, unabhängig vom
  Mirror-Pfad).

Auswertung überall über tatsächlich beim PC angekommene Frames
(`tshark`-Filter), nicht über die Firmware-eigenen Zähler — die haben sich
in der bisherigen Untersuchung als nicht aussagekräftig für diesen
spezifischen Fehler erwiesen (zeigen durchgehend Erfolg, auch wenn beim PC
nichts mehr ankommt).

## Ausführung

Läuft ab jetzt in `SNIFFER_4_ERGEBNISSE.md` protokolliert.
