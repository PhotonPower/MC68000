# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
- **Amiga-Hunk-Lader** (`m68k/hunk.hpp`): `HUNK_HEADER/CODE/DATA/BSS/RELOC32/SYMBOL/DEBUG/NAME/END`, Relokation, Symbole als
  Labels; `m68ktool dis` erkennt Hunk-Dateien. Getestet mit synthetischen Dateien, inklusive aller Abkürzungen.
- **Meilenstein 0.2:** Dekoder (`decode()`), Encoder, Mehrpass-Assembler, Ausdrucksauswerter, Disassembler mit
  Ablaufanalyse, S-Record-Ein-/Ausgabe; `m68ktool asm` und `m68ktool dis`. Dekoder und Encoder teilen sich eine
  Befehlstabelle (164 Formen).
- Tests: exhaustiver Roundtrip (alle 65536 Opcode-Wörter, rund 248 000 gültige Befehle), 111 GNU-verifizierte
  Assembler-Zeilen, Zufalls- und Robustheitstests, Disassembler-Roundtrip, CLI-Tests mit dem Beispielprogramm.
- Kreuzprüfung gegen GNU binutils: `tools/crosscheck_objdump.py` (Dekoder, alle Opcodes inkl. Operanden) und
  `tools/crosscheck_gas.py` (Assembler, 125 Fälle); CI-Job `crosscheck`.
- Entscheidungen festgehalten: Zielplattform Amiga 500, nur Motorola-Syntax, nur MC68000, kein Timing
  (nur Speicherinhalte zählen), Monitor wie beim Z80, keine Ton-/Disketten-/Bildschirmausgabe; neues Dokument
  `docs/amiga500.md`, Roadmap, Syntax-, Format- und Emulator-Doku angepasst.
- Projektgerüst nach dem Vorbild von [PhotonPower/Z80](https://github.com/PhotonPower/Z80):
  CMake-Build (`m68kcore`, `m68ktool`), CTest, GitHub-Actions-CI (Linux/macOS/Windows, ASan/UBSan),
  `.clang-format`, `.editorconfig`, `.gitattributes`, Mitarbeitsregeln, Pull-Request-Vorlage.
- Kernbausteine: `m68k/types.hpp` (24-Bit-Adressraum, `Byte`/`Word`/`Long`), `m68k/format.hpp`
  (Hexadezimalausgabe), `m68k/memory.hpp` (dünn besetzter 16-MiB-Speicher, Big-Endian) mit Tests.
- `m68ktool help|version`; `monitor` und `serve` sind angekündigt, aber noch nicht umgesetzt.
- Dokumentation unter `docs/` (Architektur, Befehlssatz-Überblick, Assembler-Syntax, Formate,
  Emulator, Monitor, Agenten-Protokoll, Reverse Engineering, Tests, Roadmap).
