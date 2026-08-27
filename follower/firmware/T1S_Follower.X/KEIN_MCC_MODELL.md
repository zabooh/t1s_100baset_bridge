# Dieses Projekt hat absichtlich kein MCC-Modell

**Entfernt am 2026-08-21** (Entscheidung vom 2026-08-12). Vorher lagen hier
`T1S_Follower_default/mcc-config.mc4` und 47 `components/*.yml`, dazu die zwei
`mcc-manifest-*.yml` — 48 getrackte Dateien, 272 KB.

## Warum sie weg sind

**Das Modell war nicht das dieses Projekts, sondern das der Bridge.** Die Hash-Maps
beider Projekte waren byteidentisch, und die Modulliste enthielt weiter `drvGmac`,
`drvMiim`, `drvExtPhyLan8740`, `sercom0` und `tcpipNetConfig_1` — alles Dinge, die
der Follower nicht hat. Die Herleitung steht in
[CONFIG_BASELINE.md](../../../docs/wissen/CONFIG_BASELINE.md) §0.2.

Daraus folgte eine Falle, und sie war der Grund für das Entfernen: **„Generate Code"
in diesem Projekt hätte keinen älteren Follower wiederhergestellt, sondern aus dem
Projekt eine Bridge gemacht** — zwei Schnittstellen, GMAC, MAC-Bridge. Der Schaden
sieht danach aus wie ein zerstörtes Projekt, nicht wie ein regeneriertes, und kostet
entsprechend Zeit bei der Suche.

Nichts ging dabei verloren: es gab nie ein Modell, das diesen Follower beschrieb.

## Was das für den Build heißt: nichts

Gebaut wird aus `nbproject/configurations.xml`, und die Makefile-Fragmente erzeugt
`genmk.bat` daraus mit dem Werkzeug, das MPLAB X selbst mitbringt. **Belegt am
2026-08-21:** Fragmente gelöscht, aus der bearbeiteten `configurations.xml` neu
erzeugt, voller Rebuild — `BUILD SUCCESSFUL`, Hex **gleich groß** (649 676 B) und
nur **zwei abweichende Zeilen**, und die sind der Build-Stempel `__DATE__`/`__TIME__`
in `app.c`.

## Wenn du das Modell zurückhaben willst

Dann ist die richtige Antwort **nicht** `git revert`, sondern ein **neues** Modell,
das diesen Follower beschreibt: eine Schnittstelle, kein GMAC, kein MAC-Bridge, der
LAN865x auf SPI. Die Werte dafür stehen vollständig in
[CONFIG_BASELINE.md](../../../docs/wissen/CONFIG_BASELINE.md) — das Dokument existiert genau für
diesen Fall. Was ein Agent dabei einhalten muss, steht in
[MCC_IN_THE_AGENT_AGE.md](../../../docs/strategie/MCC_IN_THE_AGENT_AGE.md) Anhang A.

**Die Bridge hat ihr Modell am 2026-08-22 ebenfalls abgegeben** — dort war der Anlass
aber ein anderer: nicht ein Modell, das das Projekt falsch beschrieb, sondern der
Taktumbau, der einen Patch in generiertem Gebiet brauchte. Begründung und die
Kennzahlen des Modells in `CONFIG_BASELINE.md` §5.3, der Merker dort in
[KEIN_MCC_MODELL.md](../../../firmware/T1S_100BaseT_Bridge.X/KEIN_MCC_MODELL.md).
