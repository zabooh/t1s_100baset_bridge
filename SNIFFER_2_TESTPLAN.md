# Sniffer/Mirror-Großframe-Bug: Testplan

Für jede Hypothese aus `SNIFFER_1_HYPOTHESEN.md` ein konkreter, ausführbarer
Test. Alle Tests nutzen das erweiterte `noip_send <n> [gap_ms] [size]`
(Follower B → Bridge, EtherType `0x88B5`, T1S) statt iperf — kein TCP/UDP-
Overhead, volle Kontrolle über Größe/Anzahl/Abstand, und jeder Frame trägt
schon eine fortlaufende Sequenznummer (`NOIP_SeqFromFrame()`), die sich auf
dem PC direkt mitschneiden lässt.

## T1 — Feinkörnige Zählerbeobachtung während des Laufs (H1)

Statt nur vorher/nachher `sniffer`/`stats` zu lesen: während ein
Großframe-Strom läuft, alle ~1 s pollen (`sniffer`, `stats`) und die
Zeitreihe aufzeichnen. Zeigt `pool_empty` oder `eth1 TX qFull`/`eth0 RX
nobufs` irgendwann während des Laufs eine Bewegung, auch wenn der
End-Snapshot wieder "sauber" aussieht?

## T2 — Sequenznummern-Abgleich: wo genau bricht der Strom ab? (H2)

Follower B sendet `noip_send <n> 0 <size>` (Größe über der 1514-Grenze,
z. B. 1600, Rücken an Rücken). Der PC schneidet parallel auf `eth.type ==
0x88b5` mit. Abgleich: letzte beim PC angekommene Sequenznummer gegen die
letzte vom Follower als "sent" geloggte. Bricht der Strom klar an EINER
Stelle ab (nie wieder ein späterer Frame), oder gehen einzelne Frames
verstreut verloren? Ersteres spricht für einen harten Zustandsfehler
(GMAC/Pool hängt), Letzteres für reinen Paketverlust.

## T3 — Wiederholrate variieren: verschwindet der Fehler mit Pause zwischen Frames? (H3, H5)

Derselbe Test wie T2, aber mit variierendem `gap_ms` (0, 2, 5, 10, 20 ms) bei
fester, bekannt fehlschlagender Größe (z. B. 1600 Byte). Wird der Fehler bei
größerem `gap_ms` seltener oder verschwindet er, spricht das stark für ein
zeit-/lastabhängiges Problem (Race Condition oder Hardware-Errata bei
Rücken-an-Rücken-Übergabe) statt für einen reinen Größen-Effekt.

## T4 — `bigframe` in einer engen Schleife, ohne T1S (H4)

Viele `bigframe <size>`-Aufrufe direkt hintereinander (Größe über 1514,
kein Follower, kein T1S, kein Mirror-Pool beteiligt) — reproduziert reine
Wiederholung großer Frames auf `eth1` allein (ohne die Mirror-Pipeline) den
Adapter-Aussetzer? Wenn ja: Ursache liegt eher beim PC/USB-Adapter/Npcap
selbst und ist unabhängig vom Mirror-Mechanismus. Wenn nein (wie bisher bei
Einzelaufrufen beobachtet): bestätigt, dass die Mirror-Pipeline selbst
(Pool, `MIRROR_Eth0Rx`, Kombination mit `eth0`-RX-Last) Teil der Ursache
ist, nicht nur "großer Frame auf eth1".

## T5 — GMAC-Errata-Hinweise (H5, informativ)

Kurzer Check der SAME54-Dokumentation/Errata auf bekannte GMAC-Probleme bei
Rücken-an-Rücken-Übertragung großer Frames. Kein eigener Testlauf, nur
Recherche zur Einordnung von H3/H5.

## Erfolgskriterium je Test

Wie bei der bisherigen automatisierten Suche: **nicht** die
Firmware-eigenen Zähler als Beweis nehmen (die zeigen durchgehend "Erfolg"),
sondern die tatsächlich beim PC angekommenen Frames (Sequenznummern bzw.
Zählung per `tshark`-Filter auf `eth.type==0x88b5`).
