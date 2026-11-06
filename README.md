# MC68000 Tools

[![CI](https://github.com/PhotonPower/MC68000/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/MC68000/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Assembler, Disassembler, **Emulator** und **Maschinensprache-Monitor** für den Motorola MC68000 in
modernem C++20 – das Schwesterprojekt von [PhotonPower/Z80](https://github.com/PhotonPower/Z80) mit
derselben Architektur: eine Bibliothek ohne Ein-/Ausgabe (`m68kcore`), ein Kommandozeilenwerkzeug
(`m68ktool`) und ein Emulator als Ausführungsumgebung für einen lokalen LLM-Agenten.

> **Zielplattform:** Commodore **Amiga 500** (MC68000, Motorola-Syntax) – siehe [docs/amiga500.md](docs/amiga500.md).

> **Status: Version 0.1 – Projektgerüst.** Build, Tests, CI und Dokumentation stehen; die eigentlichen
> Werkzeuge werden nach der [Roadmap](docs/roadmap.md) umgesetzt. Die Dokumente unter `docs/`
> beschreiben das **Zielbild** und sind am Anfang jeweils als „geplant“ gekennzeichnet, soweit noch
> nichts davon existiert.

## Was es heute gibt

- `m68k/types.hpp` – `Byte`/`Word`/`Long`, 24-Bit-Adressraum (16 MiB), `Size` (.B/.W/.L)
- `m68k/format.hpp` – Hexadezimalausgabe (`$1F`, `0x1F`, `1FH`)
- `m68k/memory.hpp` – dünn besetzter 16-MiB-Speicher, Big-Endian, Adressen auf 24 Bit maskiert
- `m68ktool help | version`
- Tests (`ctest`), CI für Linux/macOS/Windows plus AddressSanitizer/UBSan

## Geplante Funktionen

**Assembler** (Motorola-Syntax, [docs/assembler-syntax.md](docs/assembler-syntax.md))
- alle Befehle des MC68000 mit allen Adressierungsarten, Labels, `EQU`, `ORG`, `DC`/`DS`, `EVEN`
- Ausgabe: Raw-Binary, Motorola S-Record, Listing, Symboltabelle

**Disassembler** (Ablaufanalyse wie im Z80-Projekt: folgt `JSR`, `BSR`, `BRA`, `Bcc`, `DBcc`)
- Querverweise, Kommentardatei, wieder assemblierbarer Quelltext (`--asm`)

**Emulator** ([docs/emulator.md](docs/emulator.md))
- befehlsgenauer MC68000-Kern mit Ausnahmebehandlung, Supervisor-/Benutzermodus, Zyklen pro Befehl
- Einzelschritt, Breakpoints, Watchpoints, Checkpoints, Snapshots, Speicher-/E/A-Hooks
- Agenten-Schnittstelle `m68ktool serve` (JSON-Zeilenprotokoll, [docs/agent-protocol.md](docs/agent-protocol.md))

**Monitor** ([docs/monitor.md](docs/monitor.md)) – `m68ktool monitor`
- disassemblieren, Speicher ansehen/ändern, Zeilen assemblieren, Register, Ausführen im Emulator

**Reverse Engineering mit lokalem LLM** ([docs/reverse-engineering.md](docs/reverse-engineering.md))

## Schnellstart

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build

./build/m68ktool version
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
| [docs/emulator.md](docs/emulator.md)                     | Emulator: Umfang, Genauigkeit, Zustand speichern        |
| [docs/monitor.md](docs/monitor.md)                       | Monitor: Befehle und Beispiel                           |
| [docs/agent-protocol.md](docs/agent-protocol.md)         | JSON-Protokoll für LLM-Agenten                          |
| [docs/reverse-engineering.md](docs/reverse-engineering.md) | Programme mit einem lokalen Modell kommentieren       |
| [docs/testing.md](docs/testing.md)                       | Teststrategie und Kreuzprüfung                          |
| [docs/roadmap.md](docs/roadmap.md)                       | Meilensteine und offene Fragen                          |
| [CONTRIBUTING.md](CONTRIBUTING.md)                       | Bauen, Konventionen                                     |
| [CHANGELOG.md](CHANGELOG.md)                             | Änderungen                                              |

## Projektstruktur

```
include/m68k/  öffentliche Header der Bibliothek (m68kcore)
src/           Implementierung
app/main.cpp   Kommandozeilenwerkzeug m68ktool
agent/         (geplant) Orchestrator für Ollama
tests/         Unit-Tests (CTest)
tools/         Entwicklerwerkzeuge (derzeit CI-Annotationen; später Kreuzprüfung)
examples/      Beispielprogramm und Kommentardatei
docs/          Dokumentation
```

## Lizenz

[MIT](LICENSE)
