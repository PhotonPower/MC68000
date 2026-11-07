# MC68000 Tools

[![CI](https://github.com/PhotonPower/MC68000/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/MC68000/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Assembler, Disassembler, **Emulator** und **Maschinensprache-Monitor** für den Motorola MC68000 in
modernem C++20 – das Schwesterprojekt von [PhotonPower/Z80](https://github.com/PhotonPower/Z80) mit
derselben Architektur: eine Bibliothek ohne Ein-/Ausgabe (`m68kcore`), ein Kommandozeilenwerkzeug
(`m68ktool`) und ein Emulator als Ausführungsumgebung für einen lokalen LLM-Agenten.

> **Zielplattform:** Commodore **Amiga 500** (MC68000, Motorola-Syntax) – siehe [docs/amiga500.md](docs/amiga500.md).

> **Status: Version 0.5.** Assembler, Disassembler, Emulator, Monitor, Agenten-Schnittstelle, Amiga-500-Speicherkarte,
> Reverse-Engineering-Werkzeuge, ein Orchestrator für ein lokales LLM und die Emulation einiger Amiga-Bibliotheksaufrufe sind
> fertig und geprüft (GNU `objdump`/`as`, Musashi, Mock-Ollama). Der Orchestrator wurde noch nicht mit einem echten Modell
> ausprobiert ([Roadmap](docs/roadmap.md)).

## Was es heute gibt

**Assembler** (`m68ktool asm`, Motorola-Syntax, [docs/assembler-syntax.md](docs/assembler-syntax.md))
- alle Befehle des MC68000 mit allen Adressierungsarten; Labels (auch lokale), `EQU`, `ORG` (mehrfach), `DC`/`DS`, `EVEN`
- Ausdrücke: `+ - * / % << >> & | ^ ~`, Klammern, `*` (aktuelle Adresse), Zahlen als `123`, `$1F`, `0x1F`, `%1010`, `'A'`
- Vorwärtsreferenzen; Sprünge und absolute Adressen wählen die kürzeste passende Größe (Mehrpass)
- Ausgabe: Raw-Binary, Motorola S-Record, Listing, Symboltabelle; Fehlermeldungen mit Zeilennummer, alle Fehler eines Laufs

**Disassembler** (`m68ktool dis`, [docs/usage.md](docs/usage.md))
- Ablaufanalyse: folgt `BSR`, `JSR`, `BRA`, `Bcc`, `DBcc`, `JMP` ab den Einstiegspunkten und trennt so Code von Daten;
  alternativ linearer Durchlauf
- Labels und Querverweise, Kommentardatei (`Adresse,"Text"`), Zahlenstile
- Ausgabe als lesbares Listing **oder** als wieder assemblierbarer Quelltext (`--asm`, byte-genau)
- Eingabe: Raw-Binary, S-Record oder **Amiga-Hunk-Programm** (wird relokiert, Symbole werden Labels)

**Bibliothek `m68kcore`:** `Memory` (16 MiB, Big-Endian), `decode()`, `encodeInstruction()`, `assemble()`,
`Disassembler`, `Cpu`, S-Record- und Hunk-Ein-/Ausgabe, Ausdrucksauswerter.

**Getestet** ([docs/testing.md](docs/testing.md)): ein exhaustiver Roundtrip über alle 65536 Opcode-Wörter
(rund 248 000 Befehle), 111 Zeilen mit Bytes aus GNU `as`, Zufallstests und – als unabhängige
Kreuzprüfung – der Dekoder gegen GNU `objdump` (Gültigkeit, Länge, Mnemonic, Operanden aller 65536 Opcodes)
und der Assembler gegen GNU `as` (125 Fälle); AddressSanitizer/UBSan in der CI.

**Emulator** (`m68k::Cpu`, [docs/emulator.md](docs/emulator.md))
- alle MC68000-Befehle und -Flags; Supervisor-/Benutzermodus; Ausnahmen mit Stapelrahmen (Adressfehler, unerlaubter Befehl,
  Privilegverletzung, `CHK`, `TRAP`, Division durch 0, Trace, Line A/F); Interrupts, `STOP`
- Einzelschritt, Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Speicher-Hooks für Geräte
- nur Register und Speicher zählen, **kein Timing**
- **Differenztest gegen Musashi:** alle 65536 Opcodes mit zufälligen Zuständen, jeweils alle Register, SR und jedes
  geschriebene Byte verglichen (rund 2,5 Millionen Fälle pro Lauf, 0 Abweichungen); dazu **Befehlsfolgen** (zufällige Programme, Vergleich nach
  jedem Befehl, auch Ausnahmerahmen, `RTE`, Stapelwechsel, X-Flag und Trace; 0 Abweichungen) und Unit-Tests

**Agenten-Schnittstelle** (`m68ktool serve`, [docs/agent-protocol.md](docs/agent-protocol.md))
- JSON-Zeilenprotokoll nach dem Vorbild von `z80tool serve`: assemblieren, disassemblieren, Speicher lesen/schreiben, Register,
  Einzelschritt/Lauf/`call` mit Breakpoints und Watchpoints, Checkpoints mit Rollback und Speicher-Diff, Snapshots,
  Interrupts; Dateizugriff nur mit `--allow-files`

**Amiga 500** ([docs/amiga500.md](docs/amiga500.md)): Speicherkarte (Chip-/Fast-/Slow-RAM, Custom-Chip- und CIA-Register als
Speicher mit Schreibprotokoll, ROM mit Overlay); Amiga-Hunk-Programme laden und starten. Keine Geräte (kein Ton/Video/Disk).
Optional **emulierte Bibliotheksaufrufe** (`os`): ExecBase und Bibliotheksbasen mit echter Sprungtabelle, `AllocMem`/`FreeMem`, `OpenLibrary`,
`FindTask`, dos `Output`/`Write`/`Exit` u. a.; **alles andere wird protokolliert (`libcalls`) und liefert 0** – kein Blockieren, keine Dateien,
keine Grafik.

**Reverse Engineering** ([docs/reverse-engineering.md](docs/reverse-engineering.md)): Codefluss-Analyse mit Routinen und
Aufrufgraph (`analyze`, `routines`), Annotationen (Namen, Dokumentation, Datenbereiche, Kommentare, Projektdatei) und ein
**verifiziertes Listing** – aus den echten Bytes erzeugt, neu assembliert und Byte für Byte verglichen, sodass kein Name und
kein Kommentar das Programm verändern kann (an 20 000 Zufallsprogrammen getestet).

**Monitor** (`m68ktool monitor`, [docs/monitor.md](docs/monitor.md))
- klassische Eingabeschleife wie beim Z80-Monitor, Zahlen hexadezimal: disassemblieren, Speicher ansehen/ändern/füllen/vergleichen/
  kopieren/suchen, **Zeilen assemblieren**, Register, Einzelschritt/Step over/bis Rückkehr/laufen, Haltepunkte und Watchpoints,
  Symbole, Dateien (Quelltext, Raw, S-Record, Hunk), Checkpoints, Snapshots, Interrupts, Amiga-Speicherkarte

**Orchestrator** (`agent/m68k_annotate.py`, [docs/reverse-engineering.md](docs/reverse-engineering.md))
- lässt ein lokales Modell über **Ollama** ein Programm Routine für Routine (von den Blättern zur Wurzel) analysieren: lesen, im Emulator
  **ausprobieren**, benennen und kommentieren; mehrere Modelle nacheinander (schnell zuerst, gründlich für die Lücken), Vollständigkeits-
  prüfung, Projektdatei zum Abbrechen/Fortsetzen, Bericht; Ergebnis ist das **verifizierte** Listing. Nur Python-Standardbibliothek.

## Geplant

- Erprobung des Orchestrators mit einem echten Modell und Feinschliff von Prompt und Parametern (vorbereitet: `--dry-run`, [Anleitung](docs/walkthrough.md))
- weitere Datentypen in der Analyse (Strukturen, Bitplanes, Samples)
- weitere `exec`-/`dos`-Funktionen in der OS-Emulation (Ports und Nachrichten, Dateien)
- Vergleich mit Daten von echter Hardware (gestapelter PC und Statuswort bei Adressfehlern)

## Schnellstart

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build

./build/m68ktool asm examples/hello.s -l - -o hello.bin      # assemblieren, Listing auf den Bildschirm
./build/m68ktool dis hello.bin --org 0x1000                  # disassemblieren (Ablaufanalyse)
./build/m68ktool dis hello.bin --org 0x1000 --asm            # als wieder assemblierbarer Quelltext
./build/m68ktool monitor < examples/monitor_session.txt      # interaktiver Monitor (hier als Skript)
./build/m68ktool serve < examples/agent_session.jsonl        # Emulator per JSON-Protokoll
```

Voraussetzungen: CMake ≥ 3.16, C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

## Dokumentation

| Dokument                                                 | Inhalt                                                  |
|----------------------------------------------------------|---------------------------------------------------------|
| [docs/architecture.md](docs/architecture.md)             | Aufbau des Codes, Komponenten, Besonderheiten des 68000 |
| [docs/amiga500.md](docs/amiga500.md)                     | Zielplattform: Speicherlayout, Systemaufrufe, Hunk/ADF  |
| [docs/instruction-set.md](docs/instruction-set.md)       | Befehlssatz-Überblick: Register, Adressierung, Opcode-Gruppen |
| [docs/assembler-syntax.md](docs/assembler-syntax.md)     | Quelltextformat, Ausdrücke, Pseudobefehle               |
| [docs/formats.md](docs/formats.md)                       | Kommentardatei, S-Record, Ausgabeformate                |
| [docs/usage.md](docs/usage.md)                           | Kommandozeile                                           |
| [docs/emulator.md](docs/emulator.md)                     | Emulator: Umfang, Zustand speichern                |
| [docs/monitor.md](docs/monitor.md)                       | Monitor: Befehle und Beispiel                           |
| [docs/agent-protocol.md](docs/agent-protocol.md)         | JSON-Protokoll für LLM-Agenten                          |
| [docs/walkthrough.md](docs/walkthrough.md)               | Anleitung: vom Programm zum kommentierten Listing     |
| [docs/reverse-engineering.md](docs/reverse-engineering.md) | Programme mit einem lokalen Modell kommentieren       |
| [docs/testing.md](docs/testing.md)                       | Teststrategie und Kreuzprüfung                          |
| [docs/roadmap.md](docs/roadmap.md)                       | Meilensteine und offene Fragen                          |
| [CONTRIBUTING.md](CONTRIBUTING.md)                       | Bauen, Konventionen                                     |
| [CHANGELOG.md](CHANGELOG.md)                             | Änderungen                                              |

## Beispiel

```console
$ m68ktool asm examples/hello.s -o hello.bin
examples/hello.s: 34 bytes, $001000, 0 error(s)

$ m68ktool dis hello.bin --org 0x1000 -c examples/hello.comments.txt --no-bytes
; MC68000 disassembly  $001000 - $001021
L_001000:
001000  LEA      L_001012(PC),A0  ; entry: print the message below
L_001004:               ; <- $00100E
001004  MOVE.B   (A0)+,D0  ; fetch the next character
001006  BEQ.S    L_001010
001008  MOVE.B   D0,$00FF0000.L
00100E  BRA.S    L_001004
L_001010:               ; <- $001006
001010  RTS
L_001012:               ; <- $001000
001012  DC.B     $48,$65,$6C,$6C,$6F,$2C,$20,$36
00101A  DC.B     $38,$30,$30,$30,$21,$0D,$0A,$00
```

## Projektstruktur

```
include/m68k/  öffentliche Header der Bibliothek (m68kcore)
src/           Implementierung
app/main.cpp   Kommandozeilenwerkzeug m68ktool
agent/         Orchestrator für Ollama (Python, m68k_annotate.py)
tests/         Unit-Tests (CTest)
tools/         Entwicklerwerkzeuge: Kreuzprüfung gegen GNU binutils, Differenztests gegen Musashi, Formentabelle, CI-Annotationen
examples/      Beispiele: kleines Programm mit Kommentardatei, Sektionsprogramm, Amiga-Beispiel, Monitor- und Agenten-Sitzung
docs/          Dokumentation
```

## Lizenz

[MIT](LICENSE)
