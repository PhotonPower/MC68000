# Zielplattform: Commodore Amiga 500

> **Geplant.** Entscheidung: Das Projekt zielt auf den **Amiga 500** mit MC68000 (nicht 68008/68010,
> nicht höhere Amiga-Modelle). Der Emulator-Kern bleibt maschinenunabhängig; alles Amiga-Spezifische
> kommt als Plattformprofil und Gerätemodule dazu. Die Angaben sind Planungsgrundlage aus dem
> Gedächtnis und vor der Umsetzung gegen das *Amiga Hardware Reference Manual* zu prüfen.

## Speicherlayout (Grundausstattung)

| Bereich               | Inhalt                                                        |
|-----------------------|---------------------------------------------------------------|
| `$000000–$07FFFF`     | 512 KiB Chip-RAM (für Prozessor und Custom-Chips)             |
| `$BFD000`, `$BFE001`  | CIA-B (gerade Adressen) und CIA-A (ungerade Adressen)         |
| `$C00000–…`           | optionales „Slow“-/Trapdoor-RAM (Erweiterung A501)            |
| `$DFF000–$DFFFFF`     | Custom-Chip-Register (Agnus, Denise, Paula)                   |
| `$F80000–$FFFFFF`     | Kickstart-ROM (256 KiB)                                       |

- Nach dem Reset ist das ROM kurzzeitig auch bei Adresse 0 sichtbar (Overlay, gesteuert über CIA-A), damit der
  Prozessor Start-SSP und Start-PC aus den Vektoren 0 und 1 lesen kann. Der Emulator muss das nachbilden.
- Adresse `4` enthält den Zeiger auf die `ExecBase`.

## Systemaufrufe

Amiga-Programme rufen das Betriebssystem nicht über `TRAP`, sondern über **Bibliotheksvektoren** auf:
`JSR -offset(A6)` mit der Basisadresse der Bibliothek in `A6` (z. B. `exec.library` über Adresse 4).
Für Disassembler und Reverse Engineering heißt das: Sprünge mit negativer Verschiebung relativ zu `A6`
werden über eine Tabelle von Bibliotheken, Offsets und Funktionsnamen (*LVO-Tabelle*) benannt – das
Gegenstück zur Firmware-Tabelle des CPC im Z80-Projekt.

## Dateiformate

- **Amiga-Hunk-Programme** (ausführbare Dateien): Blöcke wie `HUNK_HEADER`, `HUNK_CODE`, `HUNK_DATA`,
  `HUNK_BSS`, `HUNK_RELOC32`, `HUNK_SYMBOL`, `HUNK_END`; Laden bedeutet Relozieren an eine Basisadresse.
- **Kickstart-ROM-Abbilder** werden vom Benutzer bereitgestellt und nie ins Repository gelegt.

## Geräte für den Emulator

Entscheidung: Es zählen nur **Speicherinhalte**. Deshalb gibt es kein Timing, keine Ton-, Disketten-
oder Bildschirmausgabe.

1. Speicherkarte mit ROM-Overlay, Chip-RAM und Custom-Register-Bereich als einfach les-/schreibbarer
   Block (Schreibzugriffe lassen sich protokollieren, Funktion wird nicht nachgebildet).
2. CIA-Register nur so weit, wie ein untersuchtes Programm sie braucht (z. B. ROM-Overlay).
3. Interrupts nur auf Anforderung des Anwenders (z. B. Auslösen per Kommando), nicht aus Geräten.

Nicht vorgesehen: Blitter, Copper, Paula/Ton, Disketten (ADF), Bildschirmdarstellung der Bitplanes.
Ein Kickstart-Boot ist damit kein Ziel; Programme und Routinen werden direkt im Speicher gestartet
(Hunk-Lader, Aufruf einer Adresse) und ihre Speicherergebnisse untersucht. Für Bibliotheksaufrufe
(`JSR -offset(A6)`) kommen später Stubs, die das Ergebnis im Speicher nachbilden.
