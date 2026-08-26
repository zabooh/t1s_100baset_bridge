# Review und Korrekturanweisung für `lan8651_model.json`

## Ziel

Überarbeite die Datei `lan8651_model.json` anhand der folgenden Prüfergebnisse. Die Referenzdokumente sind:

- **Datenblatt:** `DS60001734F`, LAN8650/1 10BASE-T1S MAC-PHY Data Sheet
- **Errata:** `DS80001075F`, LAN8650/1 Silicon Errata and Data Sheet Clarifications

Ändere nur Inhalte, die durch die nachfolgenden Punkte abgedeckt sind. Bestehende, korrekte Registeradressen, Sections, Mnemonics und Bitfelder dürfen nicht unnötig verändert werden.

## Zusammenfassung der Prüfung

Folgender Umfang wurde geprüft:

- 183 Register
- 535 modellierte Bitfelder
- JSON-Syntax
- Registeradressen und MMS-Zuordnung
- doppelte Adressen
- überlappende Bitbereiche
- Register-Mnemonics und Sections
- ausgewählte Errata-Zuordnungen

### Positive Ergebnisse

- Die JSON-Datei ist syntaktisch gültig.
- Es wurden keine doppelten Registeradressen gefunden.
- Die MMS-Codierung der kombinierten Adressen ist konsistent.
- Es wurden keine überlappenden Bitbereiche gefunden.
- Registeradressen, Register-Mnemonics und Sections stimmen bis auf die unten beschriebenen Probleme weitgehend mit DS60001734F überein.
- `SLPCTL1` enthält die veralteten Felder `WAKEIND` und `CLRWKI` nicht. Das ist für Datenblattrevision F korrekt.
- `SLPCTL0.SLPCAL` ist mit Erratum `s6` verknüpft.
- `STS1.UNEXPB` ist mit Erratum `s5` verknüpft.
- `OA_PHYID` und `DEVID` berücksichtigen Erratum `s1`.

## Verbindlich zu korrigierende Registerfehler

In drei Registern fehlt das Bitfeld `PPSDONE`. Gleichzeitig ist `PAER` fälschlich Bit 30 statt Bit 29 zugeordnet.

Betroffen sind:

| Vollständige Adresse | Register | Section |
|---|---|---|
| `0x000A023A` | `SEVINTEN` | `11.6.49` |
| `0x000A023B` | `SEVINTDIS` | `11.6.50` |
| `0x000A023C` | `SEVIM` | `11.6.51` |

### Aktuell fehlerhafte Struktur

In jedem der drei Register steht derzeit sinngemäß:

```json
"31": {
  "name": "PADONE"
},
"30": {
  "name": "PAER"
}
```

### Korrekte Struktur

In allen drei Registern muss die obere Belegung wie folgt lauten:

```json
"31": {
  "name": "PADONE",
  "description": "Phase Adjust Done",
  "access": null,
  "reset": null
},
"30": {
  "name": "PPSDONE",
  "description": "One Pulse-per-Second Signal Generation Done",
  "access": null,
  "reset": null
},
"29": {
  "name": "PAER",
  "description": "Phase Adjust Error",
  "access": null,
  "reset": null
}
```

Dabei gilt:

- Bit 31: `PADONE`
- Bit 30: `PPSDONE`
- Bit 29: `PAER`

Die übrigen Bitfelder dieser drei Register bleiben unverändert.

Als zusätzliche interne Konsistenzreferenz kann das Register `SEVSTS` verwendet werden. Dort ist die Reihenfolge `PADONE`, `PPSDONE`, `PAER` bereits korrekt modelliert.

## Erratum `s7` neu zuordnen und inhaltlich korrigieren

### Problem

Erratum `s7` ist aktuell ausschließlich dem Register `SLPCTL1` zugeordnet. Diese Zuordnung ist irreführend, weil der dokumentierte Workaround nicht `SLPCTL1`, sondern `PLCA_CTRL0` und `SLPCTL0` verwendet.

### Erforderliche Änderung

1. Entferne die `s7`-Zuordnung von `SLPCTL1`.
2. Ordne `s7` mindestens diesen Registern zu:
   - `PLCA_CTRL0`, Adresse `0x0004CA01`
   - `SLPCTL0`, Adresse `0x00040080`
3. Falls das Schema globale oder funktionsbezogene Errata unterstützt, ergänze `s7` zusätzlich als globale Errata für das Zusammenspiel aus PLCA-Coordinator und Sleep Mode.

### Inhalt des Erratums

Das Problem ist nicht nur, dass Beacons nach Einleitung des Sleep Mode kurz weiterlaufen. Der wesentliche Effekt ist:

- Ein PLCA-Coordinator mit `NODE_ID = 0` sendet weiterhin Beacons, bis der Transmitter durch den Spannungsabfall deaktiviert wird.
- Bei einer durch Inaktivität ausgelösten Sleep-Sequenz kann der Coordinator seinen eigenen Beacon als Aktivität erkennen und wieder aufwachen.
- Die weiterlaufenden Beacons können außerdem andere Nodes auf dem Mixing Segment daran hindern, wegen Inaktivität einzuschlafen.

### Dokumentierter Workaround

Vor dem Sleep Mode des PLCA-Coordinators:

1. `PLCA_CTRL0.EN` auf `0` setzen, um PLCA und damit die Beacon-Erzeugung zu deaktivieren.
2. `SLPCTL0` so schreiben, dass:
   - `SLPEN = 1`
   - `WKINEN` und `MDIWKEN` passend zu den gewünschten Wake-Quellen eingestellt sind
   - `SLPCAL = 0`
   - `SLPINHDLY >= 1`
3. Einen PLCA-Coordinator nicht über den Inactivity Timer in den Sleep Mode schicken. Der Station Controller soll den Sleep Mode aktiv einleiten.

### Vorgeschlagene Errata-Darstellung für `SLPCTL0`

```json
{
  "doc": "DS80001075F",
  "item": "s7",
  "summary": "A PLCA coordinator does not stop transmitting BEACONs immediately when entering sleep mode.",
  "implication": "A coordinator using inactivity-triggered sleep can detect its own BEACON as activity and wake again. Continued BEACONs can also prevent other nodes from entering inactivity-triggered sleep.",
  "workaround": "Do not use inactivity-triggered sleep for a PLCA coordinator. Before entering sleep, clear PLCA_CTRL0.EN, set SLPCTL0.SLPEN, configure WKINEN and MDIWKEN as required, force SLPCAL to 0, and use SLPINHDLY of at least 1."
}
```

### Vorgeschlagene Errata-Darstellung für `PLCA_CTRL0`

```json
{
  "doc": "DS80001075F",
  "item": "s7",
  "summary": "PLCA must be disabled before a coordinator enters sleep mode.",
  "implication": "If PLCA_CTRL0.EN remains set, the coordinator may continue transmitting BEACONs during the sleep transition.",
  "workaround": "Clear PLCA_CTRL0.EN immediately before commanding the coordinator to enter sleep mode."
}
```

## Fehlendes Erratum `s8` ergänzen

### Zielregister

- Register: `PLCA_TOTMR`
- Adresse: `0x0004CA04`
- Section: `11.5.61`
- Bitfeld: `TOTMR[7:0]`

### Erforderlicher Inhalt

Erratum `s8` beschreibt den Zusammenhang zwischen Störfestigkeit und Carrier-Sense-Latenz.

Folgende Punkte müssen im Modell erkennbar sein:

- Der empfohlene Wert ist `TOTMR = 32`.
- `TOTMR = 32` entspricht 3,2 µs.
- `TOTMR` darf beim LAN8650/1 niemals kleiner als 29 gesetzt werden.
- Werte von 29 bis 31 erfordern eine gründliche Robustheitsprüfung des finalen Systems.
- Alle Nodes auf demselben PLCA Mixing Segment müssen denselben `TOTMR`-Wert verwenden.
- Bei Kollisionen mit Third-Party-Geräten kann ein Wert größer als 32 erforderlich sein.

### Vorgeschlagener JSON-Eintrag

```json
{
  "doc": "DS80001075F",
  "item": "s8",
  "summary": "Carrier-sense filtering creates a trade-off between noise immunity and PLCA carrier-sense latency.",
  "implication": "TOTMR should normally remain at 32. The LAN8650/1 must not use a value below 29, and values from 29 to 31 require thorough robustness testing.",
  "workaround": "Configure TOTMR to 32 on all nodes of the PLCA mixing segment. If collisions occur with third-party devices, a value greater than 32 may be required. All nodes must use the same value."
}
```

## Fehlendes Erratum `s9` ergänzen

### Problem

Erratum `s9` ist im Modell nicht enthalten.

### Inhalt

Wenn der lokale Wall Clock durch einen Clock-Servo-Algorithmus korrigiert wird, folgt ein Event Generator im Periodic Mode diesen Wall-Clock-Korrekturen nicht. Dadurch gilt:

- Der erste erzeugte Puls ist mit der Wall Clock synchron.
- Nachfolgende periodische Pulse folgen der lokalen Referenzclock.
- Die Pulse können daher gegenüber der synchronisierten Wall Clock driften.

### Workaround

Mehrere Events sollen einzeln im Single Mode ausgelöst werden. Im Single Mode erzeugte Events sind mit der Wall Clock synchron.

### Zuordnung

Ordne `s9` den Registern beziehungsweise Bitfeldern zu, welche den Periodic Mode der Event Generatoren konfigurieren. Falls das Schema dies nicht eindeutig erlaubt, füge `s9` als globale funktionsbezogene Errata für den Event-Generator-Block hinzu.

### Vorgeschlagener globaler Eintrag

```json
{
  "doc": "DS80001075F",
  "item": "s9",
  "scope": "Event Generator periodic mode",
  "summary": "Periodic Event Generator output does not track adjustments to a synchronized wall clock.",
  "implication": "After the first synchronized pulse, periodic output can drift relative to the synchronized wall clock.",
  "workaround": "Generate each event individually in single mode when synchronization to the wall clock must be maintained."
}
```

## Fehlendes Erratum `s4` als globale Errata ergänzen

### Problem

Erratum `s4` ist im Modell nicht enthalten. Es ist kein reines Registerproblem, sondern betrifft die Bildung von SPI Transmit Data Blocks.

### Inhalt

Der LAN8650/1 kann nach einem Excessive-Collision-Ereignis die Übertragung einstellen, wenn ein SPI Transmit Data Block gleichzeitig:

- das Ende eines Ethernet-Frames und
- den Anfang des nachfolgenden Ethernet-Frames

enthält.

### Workaround

- PLCA auf dem gesamten Segment korrekt konfigurieren, damit Excessive Collisions vermieden werden.
- Bei reinem CSMA/CD-Betrieb gegebenenfalls sicherstellen, dass ein Transmit Data Block Payload nur Daten eines einzelnen Ethernet-Frames enthält.

### Vorgeschlagener globaler Eintrag

```json
{
  "doc": "DS80001075F",
  "item": "s4",
  "scope": "SPI transmit data block handling",
  "summary": "Packet transmission can halt after excessive collisions when one SPI transmit data block contains the end of one frame followed by the beginning of the next frame.",
  "implication": "The device may stop transmitting Ethernet frames after an excessive-collision event.",
  "workaround": "Ensure valid PLCA configuration. For CSMA/CD operation, consider limiting each SPI transmit data block payload to one Ethernet frame."
}
```

## Access- und Resetwerte ergänzen

### Aktueller Zustand

Bei allen 183 Registern stehen `access` und `reset` auf `null`. Auch bei allen 535 modellierten Bitfeldern stehen diese Werte auf `null`.

### Bedeutung

Das Modell ist derzeit ein Register-Namens- und Bitfeldmodell, aber kein vollständiges Modell für sichere Lese- und Schreiboperationen. Es kann unter anderem nicht unterscheiden zwischen:

- `RO`
- `R/W`
- `RC`
- `W1C`
- `W1S`
- `SC`
- kombinierten Attributen wie `R/W SC`
- definierten, undefinierten oder bitweise unterschiedlichen Resetwerten

### Anweisung

Extrahiere für jedes modellierte Bitfeld aus DS60001734F:

1. Access-Typ
2. Resetwert

Berücksichtige dabei, dass ein mehrbitiges Feld unterschiedliche Access- oder Resetangaben pro Bit enthalten kann. Wenn sich ein Feld nicht verlustfrei mit einem einzelnen `access`- oder `reset`-Wert darstellen lässt, erweitere das Schema oder speichere die Werte bitweise. Erfinde keine Werte und leite keine nicht angegebenen Resetwerte ab.

### Sicherheitsrelevanz für die GUI

Die GUI darf ohne Access-Informationen insbesondere folgende Bits nicht so behandeln, als seien sie gewöhnliche Read/Write-Felder:

- Read-to-Clear
- Write-One-to-Clear
- Write-One-to-Set
- Self-Clearing
- Read-Only
- reservierte Bits

Read-Modify-Write-Operationen müssen die dokumentierten Seiteneffekte berücksichtigen. Besonders wichtig ist `SLPCTL0.SLPCAL`: Ein gelesener Wert darf gemäß Erratum `s6` nicht unverändert zurückgeschrieben werden. Bei jedem Schreiben muss `SLPCAL` explizit als `0` geschrieben werden.

## Empfohlene Schema-Erweiterung für globale Errata

Nicht alle Silicon Errata lassen sich sinnvoll einem einzelnen Register zuordnen. Ergänze deshalb, falls noch nicht vorhanden, eine Top-Level-Struktur wie:

```json
"global_errata": [
  {
    "doc": "DS80001075F",
    "item": "s4",
    "scope": "SPI transmit data block handling",
    "summary": "...",
    "implication": "...",
    "workaround": "..."
  },
  {
    "doc": "DS80001075F",
    "item": "s9",
    "scope": "Event Generator periodic mode",
    "summary": "...",
    "implication": "...",
    "workaround": "..."
  }
]
```

Bestehende registerbezogene Errata können weiterhin direkt am jeweiligen Register gespeichert werden.

## Abschlussprüfung nach der Korrektur

Führe nach der Überarbeitung mindestens folgende Prüfungen aus:

1. JSON lässt sich fehlerfrei parsen.
2. Es existieren weiterhin genau 183 Register, sofern durch die Korrektur keine neuen Register aus dem Datenblatt ergänzt werden.
3. Es gibt keine doppelten vollständigen Registeradressen.
4. Der MMS-Anteil jeder vollständigen Adresse passt zur Gruppe.
5. Kein Register enthält überlappende Bitbereiche.
6. In `SEVINTEN`, `SEVINTDIS` und `SEVIM` gilt jeweils:
   - Bit 31 = `PADONE`
   - Bit 30 = `PPSDONE`
   - Bit 29 = `PAER`
7. `SLPCTL1` enthält weiterhin weder `WAKEIND` noch `CLRWKI`.
8. `s7` ist nicht mehr ausschließlich oder irreführend `SLPCTL1` zugeordnet.
9. `s8` ist an `PLCA_TOTMR.TOTMR` dokumentiert.
10. `s9` ist am Event-Generator-Block oder global dokumentiert.
11. `s4` ist als globale beziehungsweise transaktionsbezogene Errata dokumentiert.
12. Access- und Resetwerte stimmen mit DS60001734F überein und wurden nicht geschätzt.
13. Reservierte Bits werden nicht als frei beschreibbare Felder dargestellt.

## Erwartetes Ergebnis

Liefere eine korrigierte Version von `lan8651_model.json`. Erhalte die bestehende Struktur und Formatierung soweit sinnvoll bei. Ergänze außerdem eine kurze Änderungsübersicht mit:

- korrigierten Registern
- hinzugefügten oder verschobenen Errata
- Status der Access- und Resetwert-Extraktion
- Ergebnissen der Abschlussprüfung
