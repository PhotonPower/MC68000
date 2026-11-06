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

## Umsetzung: `m68k::Amiga500`

`Amiga500` (`include/m68k/amiga.hpp`) hängt über `readOverride`/`writeOverride` an den Prozessor. Die Speicherkarte
(Adressen aus dem *Amiga Hardware Reference Manual* aus dem Gedächtnis – vor dem Verlassen darauf bitte prüfen):

| Bereich | Inhalt |
|---------|--------|
| `$000000–` | Chip-RAM (`chip_kb`, Standard 512); nach dem Reset blendet das Kickstart-ROM sich hier ein |
| `$200000–` | Fast-RAM (`fast_kb`, Standard 0) |
| `$BF0000–$BFFFFF` | CIA-Bereich: **nur Bytes im Speicher** (CIA-A auf ungeraden, CIA-B auf geraden Adressen) |
| `$C00000–` | „Slow“-RAM (`slow_kb`, Standard 0) |
| `$DFF000–$DFFFFF` | Custom-Chip-Register: **nur Speicher**, Schreibzugriffe werden protokolliert |
| `$F80000–$FFFFFF` | Kickstart-ROM (256/512 KiB, gespiegelt), schreibgeschützt |
| sonst | „unmapped“: Lesen liefert 0, Schreiben wird verworfen und protokolliert |

- **Es werden keine Geräte nachgebildet** (kein DMA, Copper, Blitter, Ton, Disk, Video; kein Timing). Custom-Register und
  CIAs merken sich nur, was geschrieben wurde. Das Protokoll (`io` in [agent-protocol.md](agent-protocol.md)) nennt zu
  jedem Zugriff Adresse, Größe, Wert, den schreibenden Befehl und – für bekannte Custom-Register (`DMACON`, `INTENA`,
  `INTREQ`, `ADKCON`, `BPLCON0–2`, `DIWSTRT/-STOP`, `DDFSTRT/-STOP`, `COP1LC`, `COPJMP1`, `BLTSIZE`, `COLOR00–31`, …) – den Namen.
- **ROM-Overlay:** Mit geladenem ROM ist das Overlay nach `reset()` an: Lesen von Adresse 0 bis 512 KiB liefert das ROM, SSP und PC
  kommen aus dessen ersten acht Bytes. Es geht aus, wenn das Programm Bit 0 von CIA-A Port A (`$BFE001`) mit gesetztem
  Richtungsbit (`$BFE201`) auf 0 schreibt – so schaltet das Kickstart das RAM nach Adresse 0. Ohne ROM gibt es kein Overlay.
  Befehlsholen und Vektorlesen gehen durch dieselbe Abbildung; Code im ROM läuft also.
- **Hunk-Programme:** `load_file` lädt sie ab `$20000` (anpassbar), setzt den PC auf den Anfang des ersten Code-Hunks,
  `D0`=`A0`=0 und den Stapel an das Ende des Chip-RAM (`chip_kb*1024 - 16`). Bibliotheksaufrufe (`JSR -offset(A6)`) sind
  noch **nicht** nachgebildet: `A6` und die ExecBase fehlen; solche Programme laufen bis zum ersten Bibliotheksaufruf.

Nicht vorgesehen (Entscheidung): Blitter, Copper, Paula/Ton, Disketten (ADF), Bildschirmdarstellung der Bitplanes, Kickstart-Boot.
Ein späterer Ausbau (Stubs für `exec`/`dos`-Funktionen, die ihr Ergebnis im Speicher nachbilden) steht in der [Roadmap](roadmap.md).
