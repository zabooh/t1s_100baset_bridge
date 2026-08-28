# Sniffer/Mirror-Großframe-Bug: mögliche Ursachen

2026-08-27. Untersuchung des Befunds: Sobald ein über den Sniffer/Mirror-Pfad
(`eth0` T1S → `MIRROR_Eth0Rx()` → fester 8er-Pool → `DRV_GMAC_PacketTx()` →
`eth1`) laufender Frame eine bestimmte Größe überschreitet, unter **echter,
andauernder Last** (viele Frames hintereinander), bricht der Empfang beim PC
ab — Windows meldet den USB-Ethernet-Adapter (`Ethernet 8`, Realtek USB GbE)
kurz als "nicht mehr angeschlossen", dann sofort wieder da. Die Bridge selbst
bleibt die ganze Zeit gesund: `uptime` läuft durch (kein Reset), `stats` und
die Sniffer-Debug-Zähler (`rx_hook`/`passed_filter`/`tx_submitted`,
`pool_empty=0`) zeigen durchgehend Erfolg.

## Bisher gesicherte Fakten

1. **Exakte Bytegrenze gefunden** (Bisektion mit `iperf -u -l <size>` zwischen
   zwei Followern, Bridge nur als Sniffer dazwischen): `-l 1468` (→ 1514 Byte
   Frame ohne FCS) funktioniert, `-l 1469` (→ 1515 Byte) schlägt fehl.
   **1514 Byte ist die klassische Standard-Ethernet-Obergrenze** (14 Byte
   Header + 1500 Byte maximale Nutzlast, ohne FCS).
2. **Kein echtes Trennungsereignis im Windows-Ereignisprotokoll** während des
   Vorfalls — weder `Kernel-PnP` noch ein NIC-Link-State-Event. Die "no
   longer attached"-Meldung kommt ausschließlich von Npcaps eigener
   Capture-Sitzung, nicht von einem realen USB-/Treiber-Ereignis.
3. **`bigframe <len>`** (neues Diagnose-Kommando: baut EINEN frischen,
   eigenständig allozierten Frame und schickt ihn direkt über
   `DRV_GMAC_PacketTx()` auf `eth1` — kein T1S, kein Mirror-Pool, keine
   Wiederholung) **reproduziert den Fehler bei KEINER Größe.** Es gibt eine
   eigene, harmlose Obergrenze, ab der Frames einfach nicht mehr ankommen
   (vermutlich eine normale GMAC-Treiber-Grenze ohne Jumbo-Frame-Support) —
   aber ohne jedes Adapter-Glitch-Verhalten. Das schließt "irgendein großer
   Frame auf `eth1`" als alleinige Ursache aus.
4. **Der Fehler braucht also die volle Mirror-Pipeline UND echte
   Wiederholung**: `MIRROR_Eth0Rx()`, den festen `MIRROR_POOL_SIZE=8`-Pool,
   UND einen andauernden Strom (iperf), nicht einen Einzelframe.
5. `mirror_ethpkt_to_eth1()` hat bereits eine Längenprüfung
   (`flen > MIRROR_MAX_FRAME=1518` → verworfen) — 1515 Byte liegt darunter,
   ein simpler Pufferüberlauf durch Überschreiten von `MIRROR_MAX_FRAME`
   scheidet für den beobachteten Fehlerfall also aus.
6. `bigframe` hatte ursprünglich einen eigenen, unabhängigen Bug (fehlendes
   `ackFunc` → Speicherleck nach ein paar Aufrufen) — behoben, nicht
   Ursache des eigentlichen Sniffer-Bugs.

## Hypothesen

### H1 — Pool-Erschöpfung, die `pool_empty` nicht sieht
Der feste Pool (8 Puffer) läuft unter Last mit großen Frames schneller leer
als mit kleinen (mehr Bytes pro DMA-Zyklus, langsameres Nachrücken), aber der
`pool_empty`-Zähler selbst zählt das aus irgendeinem Grund nicht korrekt mit
(z. B. weil der Fehlerfall an anderer Stelle als der geprüften early-return
entsteht).

### H2 — GMAC-Deskriptor-Ring gerät durch den ungebremsten Direktpfad durcheinander
`mirror_ethpkt_to_eth1()` ruft `DRV_GMAC_PacketTx()` **direkt** auf, am
normalen `TCPIP_MAC_PacketTx()`-Stack vorbei — ohne dessen Backpressure/
Warteschlangen-Logik. Bei genügend großen, schnell aufeinanderfolgenden
Frames könnte der echte Hardware-Deskriptor-Ring des GMAC (nicht die
Software-Zähler) an eine Grenze stoßen, die die Software-Ebene nicht sieht,
weil sie nur "an den Treiber übergeben", nicht "vom Controller tatsächlich
sauber verarbeitet" zählt.

### H3 — Nebenläufigkeits-/Timing-Problem zwischen `eth0`-RX und `eth1`-TX
`MIRROR_Eth0Rx()` läuft synchron innerhalb der `eth0`-RX-Verarbeitung und
stößt von dort direkt GMAC-Hardwarezugriffe an. Größere Frames brauchen
länger für die DMA-Übergabe — das vergrößert ein mögliches Zeitfenster für
eine Race Condition mit einem echten, gleichzeitig laufenden GMAC-Interrupt
(TX-complete/RX), die das Locking (`_DRV_GMAC_TxLock`) nicht vollständig
abdeckt.

### H4 — PC-/USB-Adapter-/Npcap-seitige Eigenheit, unabhängig von der Bridge
Der USB-Ethernet-Adapter (Realtek, per USB) oder dessen Npcap-Anbindung
kommt mit einer bestimmten **Rate an nahezu-MTU-großen** Frames nicht klar
(z. B. weniger, dafür größere USB-Bulk-Transfers mit anderem Timing als bei
vielen kleinen Frames) — unabhängig davon, ob die Ursache dafür beim
Absender (Bridge) liegt. Der eigentliche „Fix" wäre dann kein
Firmware-Fix, sondern eine Begrenzung/Drosselung im Mirror-Pfad.

### H5 — SAME54-GMAC-Errata bei aufeinanderfolgenden großen Frames ohne Pause
Ein bekanntes Hardware-/Treiber-Problem des SAME54-GMAC-Peripheriegeräts bei
Rücken-an-Rücken-Übergabe großer Frames ohne Verschnaufpause zwischen den
Aufrufen (was der ungebremste Mirror-Pfad genau erzeugt).

## Nicht mehr weiter verfolgt

- **Reine Framegröße auf `eth1` allein** (durch `bigframe` widerlegt).
- **`MIRROR_MAX_FRAME`-Pufferüberlauf** (1515 Byte liegt unter der 1518-Byte-
  Grenze, die Prüfung dafür existiert bereits und greift nicht).
