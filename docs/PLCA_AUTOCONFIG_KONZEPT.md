# PLCA-Autokonfiguration — Konzept (unimplementiert)

Entstanden am 2026-08-29 aus einer Diskussion im Anschluss an die Einführung von `plca_stat`
(`firmware/src/lan865x_diag.c`). Reines Design-Papier, **kein Code geschrieben**. Anlass war die
händische Fehleranfälligkeit von `setenv plca_id`/`setenv plca_cnt` pro Board: in derselben Session
entstanden dabei zweimal echte ID-Kollisionen (Doppel-Coordinator, siehe `FALLSTRICKE.md`
2026-08-29), einmal absichtlich zu Testzwecken reproduziert. Ziel des Konzepts: Knoten vergeben ihre
PLCA-Node-ID selbstständig, ohne dass jemand pro Board manuell `plca_id`/`plca_cnt` setzt.

## Ausgangslage — was ein Knoten aus den PLCA-Registern lernen kann

Empirisch an diesem Bench ermittelt (Details: `FALLSTRICKE.md`, Einträge 2026-08-29):

- **Empfang hängt nicht an der eigenen PLCA-Teilnahme.** `sniffer` beweist das bereits: ein Knoten
  mit abgeschaltetem Sender (`T1SPMACTL.TXD=1`) hört trotzdem jeden Frame auf dem Bus mit.
- **`PLCA_CTRL1`-Reset-Default ist `NODE_ID=0xFF`** ("PLCA disabled" laut Registermodell) — ein
  Knoten ab Werk sitzt von selbst still am Bus, ohne zu senden oder zu kollidieren.
- **`PRSSTS.MAXID` liest sich auch passiv** (im Sniffer-Modus, ohne eigene Teilnahme) und zeigt die
  auf dem Bus tatsächlich aktive Segmentgröße — vermutlich vom Coordinator per BEACON verteilt.
- **Es gibt kein Register, das belegte/freie IDs einzeln auflistet.** `PRSSTS.MAXID` ist nur die
  Segmentgröße, kein Belegungs-Bitmap. Eine ID-Kollision wird nur indirekt sichtbar, und nur für die
  eigene ID: `STS1.UNEXPB` bei Duplikat auf der Coordinator-ID (0), `STS1.RXINTO` bei Duplikat auf
  einer regulären ID, `STS1.TXCOL` als eigener Sendeversuch, der kollidiert.

Daraus folgt: Segmentgröße lässt sich passiv erfragen, Belegung einzelner IDs nicht — dafür braucht
es einen zusätzlichen Mechanismus oberhalb der reinen PLCA-Register.

## Protokoll-Idee

**Coordinator (oder ein designierter Knoten) sendet einen periodischen Info-Frame:**
- Enthält eine Tabelle belegter IDs mit der jeweiligen MAC-Adresse (ID ↔ MAC).
- Läuft normalerweise **zyklisch** (Heartbeat).
- Nach einem erfolgreichen Claim wird der Info-Frame **sofort** (nicht erst beim nächsten Zyklus)
  neu verschickt — verkürzt die Zeit, in der andere Knoten mit einer veralteten Liste arbeiten,
  besonders wichtig, wenn ein ganzes Segment gleichzeitig hochfährt.

**Ein frischer Knoten:**
1. Bootet mit `NODE_ID=0xFF` (Werksdefault) — PLCA aus, stört nichts.
2. Hört passiv mit: liest `PRSSTS.MAXID` (Segmentgröße) und wertet den Info-Frame aus
   (welche IDs sind belegt, mit welcher MAC).
3. Wählt eine als frei gemeldete ID.
4. Berechnet aus der eigenen MAC-Adresse einen Start-Delay (siehe unten) und wartet ab.
5. **Claim:** setzt `PLCA_CTRL1` auf die Kandidaten-ID, aktiviert PLCA, sendet einen Claim-Frame
   mit der eigenen MAC.
6. Prüft unmittelbar danach `STS1.TXCOL` (eigener Sendeversuch kollidiert). Bei Kollision war die
   ID entgegen der (evtl. veralteten) Liste bereits belegt → Claim verwerfen, neue ID wählen bzw.
   auf aktualisierten Info-Frame warten und erneut versuchen. Ohne Kollision: ID ist erfolgreich
   belegt.

**Coordinator-Seite:** baut seine Belegungstabelle aus eingehenden Claim-Frames auf (jeder Claim
liefert ID + MAC), sendet die aktualisierte Tabelle wie oben beschrieben.

## Warum das trägt

- Passives Lernen von Segmentgröße und Belegungsliste stört den laufenden Bus nicht — das folgt
  direkt aus der Sniffer-Beobachtung oben.
- Der **Collision-Check nach dem Claim ist die eigentliche Korrektheitsgarantie**, nicht die
  Aktualität des Info-Frames. Beide heute an diesem Bench erzeugten Kollisionsarten (Doppel-
  Coordinator → `UNEXPB`, Doppel-regulärer-Knoten → `RXINTO`) wurden zuverlässig erkannt; ein
  eigener Sendeversuch auf eine schon belegte ID zeigt sich zusätzlich über `TXCOL`.
- Der MAC-abgeleitete Start-Delay reduziert nur die *Wahrscheinlichkeit* gleichzeitiger Claims
  („Thundering Herd" beim Segment-weiten Power-up) — er ersetzt den Collision-Check nicht, sondern
  ergänzt ihn um schnellere Konvergenz bei vielen gleichzeitig bootenden Knoten.

## Offene Punkte (nicht ausgearbeitet)

- **Frame-Format** für Claim- und Info-Frame (EtherType, Payload-Layout) ist nicht definiert. Als
  Vorbild könnte der rohe EtherType-Ansatz aus `noip_test.c` (`0x88B5`, umgeht den TCP/IP-Stack)
  dienen — ein eigener EtherType ohne IP-Overhead wäre naheliegend.
- **MAC → Delay sollte gehasht werden, nicht die unteren Bytes direkt verwenden.** An diesem Bench
  beobachtet: `eth0`/`eth1` derselben Bridge unterscheiden sich nur im letzten Byte
  (`...CE:D9` / `...CE:DA`) — sequenziell vergebene Hersteller-MACs (z. B. aus derselben
  Fertigungscharge) könnten bei einer naiven „letztes Byte = ms Delay"-Regel eng clustern statt sich
  gleichmäßig zu verteilen.
- **Wer ist "der Coordinator"** im Sinne dieses Protokolls — zwingend Node-ID 0, oder ein beliebiger
  Knoten, der die Rolle übernimmt? Nicht festgelegt.
- Kein Code, keine Firmware-Änderung, kein Zeitplan — reine Konzeptnotiz für eine mögliche
  spätere Umsetzung.
