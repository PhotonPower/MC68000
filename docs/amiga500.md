# Zielplattform: Commodore Amiga 500

> **Umgesetzt** (`m68k::Amiga500`, `m68k::AmigaOs`). Entscheidung: Das Projekt zielt auf den **Amiga 500** mit MC68000 (nicht 68008/68010,
> nicht höhere Amiga-Modelle). Der Emulator-Kern bleibt maschinenunabhängig; alles Amiga-Spezifische
> liegt als Plattformprofil (Speicherkarte, Schreibprotokoll der Custom-/CIA-Register) und als OS-Emulation darüber. **Geprüft** (2026-11, von einem
> unabhängigen Agenten gegen das NDK 3.2 R4, die 1.3-Includes und `.fd`-Dateien der Amiga Developer CD 1.2 und das *Hardware Reference Manual*): Namen, Offsets und
> Registerbelegungen aller emulierten und benannten `exec`-/`dos`-Funktionen (alle 50 stimmen), die Konstanten und Strukturen in `amiga_os.cpp`, die Custom-Register-Namen
> und -Adressen und die CIA-Adressen. **Nicht geprüft** oder nur indirekt belegt: die Größe des ROM-Overlay-Fensters (512 KiB, keine Quelle), die Polarität des OVL-Bits
> (aus dem HRM erschlossen, nicht wörtlich angegeben) und das echte Verhalten auf einem Kickstart 1.3.

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
Für Disassembler und Reverse Engineering heißt das: `routines` meldet solche Aufrufe mit ihrem Offset (`library_calls`).
Die Funktionen selbst bildet die **OS-Emulation** nach (nächster Abschnitt); eine eingebaute Namenstabelle für alle Bibliotheken
gibt es nicht, weil falsche Namen in die Irre führen.

## Emulierte Bibliotheksaufrufe (`m68k::AmigaOs`)

Mit `machine {"type":"amiga500","os":true}` (Monitor: `machine amiga500 os`) bekommt das Programm gerade genug AmigaOS, um an seinen
Bibliotheksaufrufen **vorbeizulaufen**, damit man sich die Speicherwirkungen ansehen kann. Voraussetzung: mindestens 512 KiB Chip-RAM.

- **Aufbau:** Bei Adresse `4` steht der Zeiger auf eine `ExecBase` (mit `ThisTask`, einem minimalen Prozess-Knoten); jede geöffnete
  Bibliothek bekommt eine Basis mit **echter Sprungtabelle** (`JMP`-Befehle unter der Basis). Die Ziele liegen im Adressbereich
  `$F00000–$F7FFFF`, den der Prozessor nicht ausführt, sondern an den Emulator übergibt (`Cpu::executeHook`) – das Programm ruft
  `JSR -552(A6)` genau wie auf dem Amiga. Speicher: Programm unterhalb von Chip/2 (Hunk-Standard `$20000`), darüber der Heap für
  `AllocMem` bis zu einem 16-KiB-Stapelbereich, in den obersten 24 KiB die OS-Strukturen (`512 KiB`: Heap `$40000–$76000`, Stapel ab
  `$7A000−16`). Bis zu sieben Bibliotheken (exec + sechs); die achte `OpenLibrary` liefert 0.
- **Wirklich nachgebildet:**

  | Bibliothek | Funktion (Offset) | Verhalten |
  |---|---|---|
  | exec | `AllocMem` (−198), `FreeMem` (−210) | First-Fit im Heap, auf 8 Byte gerundet, `MEMF_CLEAR` (`$10000`) füllt mit 0; Größe 0 oder zu groß → 0; **`MEMF_FAST` (`4`) auf einer Maschine ohne Fast-RAM → 0** (wie im Autodoc); `FreeMem` eines unbekannten Blocks wird protokolliert. Mit Fast-RAM in der Maschine gelingt `MEMF_FAST`, der Heap liegt aber weiterhin im Chip-RAM |
  | exec | `AllocVec` (−684), `FreeVec` (−690) | wie oben, Größe steht 8 Byte vor dem Zeiger. **Erst ab V36 (Kickstart 2.0):** im Kickstart 1.2/1.3 eines Amiga 500 gibt es sie nicht (`libcalls` meldet `min_version: 36`). Die Bibliotheken werden mit `lib_Version` 34 angelegt, die Emulation führt diese beiden Funktionen trotzdem aus |
  | exec | `AvailMem` (−216) | freie Bytes im Heap (`MEMF_LARGEST` = `$20000`: größter freier Block); `MEMF_FAST` ohne Fast-RAM → 0 (Folgerung aus dem Autodoc, dort nicht ausdrücklich); sonst spielen die Flags für Chip/Fast keine Rolle (es gibt einen Heap) |
  | exec | `CopyMem` (−624), `CopyMemQuick` (−630) | `A0` Quelle, `A1` Ziel, `D0` Länge; überlappende Blöcke werden richtig kopiert |
  | exec | `AddHead` (−240), `AddTail` (−246), `Insert` (−234), `Remove` (−252), `RemHead` (−258), `RemTail` (−264) | echte `List`/`Node`-Strukturen im Speicher (`lh_Head` 0, `lh_Tail` 4, `lh_TailPred` 8; `ln_Succ` 0, `ln_Pred` 4); `RemHead`/`RemTail` liefern bei leerer Liste 0; `Insert` mit `A2` = 0 fügt am Anfang ein; `Remove` ist im Autodoc `void` und lässt `D0` unverändert |
  | exec | `Enqueue` (−270), `FindName` (−276) | `Enqueue` sortiert nach `ln_Pri` (vorzeichenbehaftet): hinter den Knoten gleicher Priorität, vor dem ersten niedrigerer; `FindName` vergleicht `ln_Name` mit Groß-/Kleinschreibung, beginnt hinter `A0` (Liste oder Knoten) |
  | exec | `OpenLibrary` (−552), `OldOpenLibrary` (−408), `CloseLibrary` (−414) | jede Bibliothek wird geöffnet (leere Sprungtabelle); gleicher Name → gleiche Basis; die Version wird ignoriert |
  | exec | `FindTask` (−294) | liefert den Prozess-Knoten (`ln_Type` = 13), auch für einen Namen |
  | exec | `Forbid`, `Permit`, `Disable`, `Enable` | tun nichts |
  | dos | `Output` (−60), `Input` (−54), `Write` (−48) | `Write` auf die Standardausgabe sammelt den Text (`console`); sonst −1 |
  | dos | `Read` (−42), `Close` (−36), `Delay` (−198), `UnLock`, `UnLoadSeg` | Dateiende bzw. tun nichts |
  | dos | `Open` (−30), `Lock` (−84), `LoadSeg` (−150) | es gibt **keine Dateien**: 0 und `IoErr` = 205 (Objekt nicht gefunden) |
  | dos | `IoErr` (−132), `Exit` (−144) | `Exit` beendet den Lauf (`stop: "exit"`) |

- **Alles andere wird nur protokolliert:** Jeder andere Aufruf – ob der Name bekannt ist (`Wait`, `GetMsg`, `OpenDevice`, …) oder nicht (alle
  `graphics`-/`intuition`-Funktionen) – landet in `libcalls` (Bibliothek, Offset, Name falls bekannt, Register, Rücksprungadresse,
  `emulated: false`) und liefert `D0 = 0`; sonst passiert nichts. Nichts blockiert, nichts wird geplant, es gibt keine Fenster, keine Grafik,
  keine Dateien, keine Signale/Nachrichten. Ein Programm, das auf ein Ergebnis wartet, nimmt dadurch meist seinen Fehlerpfad.
- **Programmstart:** Ein per `load_file` (Hunk) geladenes Programm – ebenso `asm` mit `entry`, im Monitor `prog adr` – startet wie unter
  AmigaDOS: `D0`=`A0`=0, Stapel unter den OS-Strukturen, **Rücksprungadresse auf einen Exit-Stub**: das abschließende `RTS` beendet den Lauf mit
  `stop: "exit"` und der Meldung `the program returned to the system (D0 = rc)`.
- **Zustand:** Belegungen, geöffnete Bibliotheken, Konsole und `IoErr` gehören zu den Checkpoints (Rollback stellt sie wieder her).
- **Geprüft:** Namen, Offsets und Registerbelegungen aller Einträge von `kExec`/`kDos` stimmen mit `exec_lib.fd`/`dos_lib.fd` aus NDK 3.2 R4 und aus den 1.3-Includes überein.
  Mindestversion: alle V33 oder älter (Kickstart 1.2), außer `AllocVec`/`FreeVec` (V36). Auf einem Amiga 500 mit Kickstart 1.2/1.3 sind das die Funktionen, die ein Programm
  dieser Zeit nicht benutzen darf; das Aufrufprotokoll (`libcalls`, `min_version`) und die Berichte des Orchestrators weisen darauf hin.

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
  `D0`=`A0`=0 und den Stapel an das Ende des Chip-RAM (`chip_kb*1024 - 16`). Bibliotheksaufrufe (`JSR -offset(A6)`) laufen ohne OS-Emulation
  (`os` nicht gesetzt) ins Leere; mit `os:true` werden einige nachgebildet, der Rest protokolliert (siehe oben).

- **Strahlzähler:** Jedes Lesen von `VPOSR`/`VHPOSR` rückt den Strahl um 32 Farbtakte weiter (228 je Zeile, 312 Zeilen, dann wechselt das Bild); Programme, die auf eine Rasterzeile warten, hängen also nicht. Der Zähler gehört zum Zustand (`rollback`).
- **Disketten:** Disketten laufen **nicht** als Laufwerk, aber ihre Abbilder (ADF) lassen sich lesen (`m68ktool adf …`, [unpacking.md](unpacking.md)).

Nicht vorgesehen (Entscheidung): Blitter, Copper, Paula/Ton, Laufwerke und Spurlader, Bildschirmdarstellung der Bitplanes, Kickstart-Boot.
Mögliche Erweiterungen der OS-Emulation (weitere `exec`/`dos`-Funktionen, Strukturen wie `FileHandle`/`Process`) stehen in der [Roadmap](roadmap.md).
