# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
- Entscheidungen festgehalten: Zielplattform Amiga 500, nur Motorola-Syntax, nur MC68000; neues Dokument
  `docs/amiga500.md`, Roadmap, Syntax-, Format- und Emulator-Doku angepasst.
- Projektgerüst nach dem Vorbild von [PhotonPower/Z80](https://github.com/PhotonPower/Z80):
  CMake-Build (`m68kcore`, `m68ktool`), CTest, GitHub-Actions-CI (Linux/macOS/Windows, ASan/UBSan),
  `.clang-format`, `.editorconfig`, `.gitattributes`, Mitarbeitsregeln, Pull-Request-Vorlage.
- Kernbausteine: `m68k/types.hpp` (24-Bit-Adressraum, `Byte`/`Word`/`Long`), `m68k/format.hpp`
  (Hexadezimalausgabe), `m68k/memory.hpp` (dünn besetzter 16-MiB-Speicher, Big-Endian) mit Tests.
- `m68ktool help|version`; `asm`, `dis`, `monitor`, `serve` sind angekündigt, aber noch nicht umgesetzt.
- Dokumentation unter `docs/` (Architektur, Befehlssatz-Überblick, Assembler-Syntax, Formate,
  Emulator, Monitor, Agenten-Protokoll, Reverse Engineering, Tests, Roadmap).
