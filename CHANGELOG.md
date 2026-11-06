# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
- **Orchestrator** `agent/m68k_annotate.py` (Python, nur Standardbibliothek): lässt ein lokales Modell über Ollama ein Programm Routine für
  Routine analysieren, im Emulator ausprobieren und annotieren (nach dem Vorbild von `z80_annotate.py`; ohne Bilder, mit Amiga-Prompt);
  Mehrdurchgang-Verfahren, Vollständigkeitsprüfung, Projektdatei, Bericht, verifiziertes Listing. Tests `tests/agent/` gegen einen
  scripteten Mock-Ollama und den echten `m68ktool serve` (in CTest, wenn Python vorhanden ist). **Nicht** mit einem echten Modell erprobt.
- Protokoll: `routines` liefert `confidence`, `listing` liefert `instructions` und `user_comments`.
- **Meilenstein 0.4:** `m68ktool monitor` – klassischer Maschinensprache-Monitor (`m68k::Monitor`, Zeilenschnittstelle ohne eigene
  Ein-/Ausgabe) mit Hex als Standard, Assemble-Modus, Lauf-/Debug-Befehlen, Symbolen, Dateibefehlen (Quelltext, Raw, S-Record,
  Hunk), Checkpoints, Snapshots, Interrupts und Amiga-Speicherkarte. Der Ausdrucksauswerter kennt dafür einen Hex-Standard.
  Tests `monitor` (rund 160 Prüfungen, 5000 Zufallszeilen ohne Absturz), Beispielsitzung `examples/monitor_session.txt`.
- **Meilenstein 0.5 (Werkzeuge):** Reverse-Engineering-Befehle im Agenten-Protokoll: `analyze`, `routines`, `listing`, `ann_label`,
  `ann_routine`, `ann_variable`, `ann_data`, `ann_comment`, `ann_remove`, `ann_title`, `ann_list`, `ann_save`, `ann_load`.
  Neue Bausteine `m68k::Analysis` (Codefluss, Routinen, Aufrufgraph, Bibliotheksaufrufe), `m68k::Annotations` und `renderListing`
  (verifiziert: neu assembliert und verglichen). Tests `reverse` (rund 340 Prüfungen, davon 150 Zufallslistings).
- Behoben: `DC.B "a"` (ein Zeichen in doppelten Anführungszeichen) wurde nicht als Zeichenkette erkannt.
- **Meilenstein 0.3, zweiter Teil:** `m68ktool serve` – JSON-Zeilenprotokoll für LLM-Agenten (`m68k::Session`, nach dem Vorbild
  von `z80tool serve`), Snapshots (`m68k/snapshot.hpp`: Speicher und Datei, Speicher-Diff), `m68k::Amiga500` (Speicherkarte,
  Schreibprotokoll der Custom-/CIA-Register, ROM-Overlay), JSON-Baustein; Hunk-Programme per `load_file` laden und starten.
  `readOverride` gilt jetzt auch für Befehlsholen und Vektorlesen (Code im ROM). Tests `session` (rund 400 Prüfungen).
- **Emulator-Kern** (`m68k::Cpu`, Meilenstein 0.3 erster Teil): alle Befehle, Ausnahmen mit Stapelrahmen, Interrupts, `STOP`,
  Trace, Breakpoints/Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Speicher-Hooks. Funktional, ohne Timing.
- Differenztest gegen **Musashi** (`tools/diff_musashi.cpp`, `tools/run_diff_musashi.sh`): alle 65536 Opcodes, zufällige
  Zustände, Vergleich aller Register und jedes geschriebenen Bytes; 0 Abweichungen. CI-Job `emulator-diff`.
- Unit-Tests `cpu` (rund 52 000 Prüfungen), darunter ein Abgleich mit dem Dekoder für alle Opcodes.
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
