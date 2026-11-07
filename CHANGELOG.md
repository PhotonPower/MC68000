# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

## [0.5.0] - 2026-11-07

Erste vollständige Fassung: Assembler (mit Hunk-Ausgabe), Disassembler, Emulator, Monitor, Agenten-Schnittstelle, Amiga-500-Speicherkarte mit OS-Emulation,
Reverse-Engineering-Werkzeuge und Orchestrator für ein lokales Modell. Der Orchestrator ist noch nicht mit einem echten Modell erprobt (siehe docs/roadmap.md).

### Behoben
- **`m68ktool dis`** (Befund eines Vergleichs mit vasm/vlink): Die Ablaufanalyse folgt jetzt **Sprungtabellen**; ihre Fälle (`cmd_hello`, `cmd_sum`, …) wurden vorher als `DC.B` angezeigt. Hunk-Dateien werden standardmäßig an `0x20000` geladen
  (wie bei `load_file` und im Monitor), nicht an 0: Dort stand `4.W` (ExecBase) als Label `L_000004` mitten im Code. Eine **Objektdatei** (`HUNK_UNIT`) wird in `dis`, `load_file` und im Monitor mit dem Hinweis „erst binden“ abgelehnt, statt still als Rohdaten gelesen zu werden.
  Ein vlink-gebundenes Programm gleicher Quelle läuft im Emulator mit identischem Ergebnis (379 Befehle, gleiche Ausgabe und Bibliotheksaufrufe) und unterscheidet sich von unserer Hunk-Ausgabe nur im Füllwort hinter dem Code (`0000` bei uns, `NOP` bei vasm).
- **OS-Emulation nach dem Abgleich mit dem NDK** (Auftrag an einen Agenten: alle 50 Funktionseinträge, Konstanten, Strukturen, Custom-/CIA-Adressen stimmen): `AllocMem`, `AllocVec` und `AvailMem` mit `MEMF_FAST`
  liefern auf einer Maschine ohne Fast-RAM jetzt 0 (laut Autodoc schlägt der Aufruf dort fehl); `Remove` lässt `D0` unverändert (im Autodoc `void`); `libcalls` meldet je Funktion `min_version`
  (33 = Kickstart 1.2; `AllocVec`/`FreeVec` sind V36, also **nicht** im Kickstart 1.2/1.3 eines Amiga 500), Monitor und Orchestrator-Bericht weisen darauf hin.
- **`call` meldete `returned: true`, obwohl die Routine nie zurückgekehrt war:** Ein Sprung durch einen Zeiger, der noch 0 war (z. B. `JSR -48(A6)` mit `A6 = 0`), lief durch leeren Speicher und kam zufällig an der Rücksprungadresse `$FFFFF0` vorbei. Jetzt zählt nur der PC auf der Rücksprungadresse **mit** dem Stapelzeiger von vor dem Aufruf; sonst `returned:false` mit einem `note`.
  Gefunden beim ersten Durchspielen einer Routine des Beispiels in der Rolle des Modells.
- Der Bericht von `run_routine` zählte den vom Aufrufer vorbereiteten Speicher als Änderung der Routine; gemessen wird jetzt ab dem Zustand nach der Vorbereitung.

### Hinzugefügt
- `run_routine` hat den Parameter **`setup`** (Routinen, die vorher auf derselben Maschine laufen, z. B. der Programmeinsprung): Routinen, die von Zustand abhängen, den anderer Code herstellt (Bibliotheksbasen, Handles, Variablen), waren sonst nicht testbar –
  `rollback` setzt auch die geöffneten Bibliotheken zurück, sodass sich der Zustand nicht von Hand schreiben lässt. Die Ausgaben und Aufrufe des `setup` zählen nicht zum Bericht.
- Auf der Amiga-Maschine melden `run`, `step`, `step_over`, `run_until_return` und `call` die Zahl der Befehle, die aus **leerem Speicher** geholt wurden (`fetched_from_empty_memory`, `first_empty_fetch`); der Bericht von `run_routine` erklärt, was das meist bedeutet.
- **Erkennung von Einzelvariablen** in `data_regions`: ein Speicheroperand, den der Code immer mit derselben Größe `.W` oder `.L` anspricht, wird als Wort- bzw. Langwort-Variable vorgeschlagen; das Listing zeigt `DC.W`/`DC.L` statt einzelner Bytes.
  Kein Vorschlag bei Bytes, gemischten Größen, `LEA`/`PEA`, ungerader Adresse. Bekannte Grenze: ein direkter Zugriff in die Mitte einer Tabelle schneidet sie ab (siehe docs/reverse-engineering.md).
- **Robustheitstest** (`tests/test_robustness.cpp`): zufällige Eingaben für JSON-Parser, Server (zufällige Befehle mit Argumenten aller JSON-Typen), Hunk-Lader, S-Records, Snapshot- und Projektdateien; läuft auch unter den Sanitizern.
  Er fand zwei Fehler, die behoben sind: **`max_instructions` und `step n` hatten keine Obergrenze** (ein Modell konnte den Server mit einer Endlosschleife und einem riesigen Limit praktisch für immer blockieren; jetzt höchstens
  100 000 000, darüber ein Fehler) und der **Hunk-Lader stürzte bei einem beschädigten Kopf mit `std::bad_alloc` ab** (Größen werden jetzt vor der Speicherbelegung gegen den 16-MiB-Adressraum geprüft). Außerdem schreibt der JSON-Writer `-0.0` als `0`.
- **Erkennung von Zahlentabellen** (`words`, `longs`) in `data_regions`: Der Code lädt die Adresse in ein Adressregister und läuft mit `(An)+` oder `(An,Dn)` in einer Operandengröße hindurch; Gemischtes, feste Verschiebungen, einzelne Einträge und Nullen
  ergeben keinen Vorschlag. Das Listing zeigt sie als `DC.W`/`DC.L`-Zeilen. Der Hinweis aus dem Code geht vor dem Inhalt (druckbar aussehende Zahlen werden nicht zu Text), ein eindeutiger String geht vor der Zahlentabelle.
- **Vorbereitung der Erprobung:** `--dry-run` im Orchestrator (Analyse ohne Modell, Bearbeitungsreihenfolge, Sprungtabellen, Datenbereiche, Bibliotheksaufrufe, Token-Schätzung mit Warnung);
  Beispielprogramm `examples/amiga/hello_amiga.s` (Bibliothek öffnen, Strings, Sprungtabelle, Zahlentabelle, Dezimalausgabe) mit Integrationstest über die ganze Kette; Anleitung `docs/walkthrough.md`.
- **Standard-Modell** des Orchestrators jetzt `qwen3-vl:8b-instruct` (wie im Z80-Projekt; das vorher eingetragene Tag war ungeprüft); bei einem fehlenden Modell nennt die Fehlermeldung die installierten Modelle.
- Die **Fälle einer Sprungtabelle** sind eigene Einstiegspunkte (Routinen), damit sie dokumentiert werden; Strings im Listing mit Steuerzeichen und Terminator in einer Zeile (`DC.B "Hello",10,0`).
- **OS-Emulation erweitert:** exec-Listenfunktionen (`AddHead`, `AddTail`, `Insert`, `Remove`, `RemHead`, `RemTail`, `Enqueue`, `FindName`) auf echten `List`/`Node`-Strukturen, `CopyMem`/`CopyMemQuick` (überlappend
  korrekt) und `AvailMem`. Test `os` jetzt mit 100 Prüfungen (Listenfolge vor- und rückwärts nachgeprüft). Funktionsnamen und Offsets wieder aus dem Gedächtnis: gegen das NDK prüfen.
- **Datentyp-Erkennung** (`Analysis::guessData`, Befehl `data_regions`): Strings (referenziert, NUL-terminiert oder lang), Tabellen von String-Zeigern (auch mit kurzen Strings) und Nullbereiche; `apply:true`
  trägt Text und Zeigertabellen als Datenbereiche ein. Das Listing schreibt Strings als `DC.B "…",0` (mit `EVEN` für das Füllbyte). Der Orchestrator übernimmt die Vorschläge selbst (`--no-auto-data`).
  Test `datatypes` (3101 Prüfungen, darunter 300 Zufallsprogramme und 400 Zufallsbilder), Orchestrator-Tests.
- **Erkennung von Sprungtabellen** in der Codefluss-Analyse: Branch-Tabellen (`JMP d(PC,Dn)` auf `BRA`-Folge) und Offset-Tabellen (`MOVE.W tab(PC,Dn),Dm` + `JMP base(PC,Dm)`) werden erkannt und
  ihre Ziele als Code verfolgt, Zeigertabellen werden vorgeschlagen (`followed:false`) und nach `ann_data kind=pointers` verfolgt. Neuer Befehl `jump_tables`, Feld `jump_tables` in `analyze`,
  Tabellenkommentare und `DC.W`-Zeilen mit Zielen im Listing, Orchestrator markiert Vorschläge selbst (`--no-auto-tables`). Test `jumptable` (5021 Prüfungen).
- **Assembler-Optimierung** `-O` (`AssemblerOptions::optimize`, Protokoll `optimize`): `ADD`/`SUB #1..8` → `ADDQ`/`SUBQ`, `MOVE.L #-128..127,Dn` → `MOVEQ`, `.L` → `.W` für 16-Bit-Werte
  zu `An`; nur gewöhnliche Zahlen, Entscheidungen werden nie hin und her gedreht. Standardmäßig aus. Test `optimize` (11 721 Prüfungen, darunter 6000 zufällige Befehle:
  optimiert und nicht optimiert führen zum selben Zustand) und 12 Fälle gegen GNU `as`.
- **Adressfehler im Differenztest:** Musashi wird jetzt mit Adressfehler-Emulation gebaut; Einzelbefehls- und Folgentest vergleichen den 14-Byte-Rahmen (auch bei ungeraden Stapelzeigern).
  Das führte zu Korrekturen im Emulator-Kern (jeweils nach Musashis hardwarenahem Verhalten): PC-relative Operanden liegen im Programmraum (Funktionscode im Rahmen);
  `MOVE.L`/`MOVEM.L` nach `-(An)` schreiben das niederwertige Wort zuerst (Zugriffsadresse `ea+2`); `CMPM`, `UNLK`, `RTS`, `RTE`, `RTR` zählen Zeiger vor dem Zugriff weiter;
  `CLR` setzt die Flags nach dem Schreiben; `LINK` holt die Verschiebung nach dem Push. Zusätzliche Unit-Tests. Nicht verglichen werden die undefinierten Bits des
  Statuswortes (siehe docs/emulator.md). Ergebnis: 0 Abweichungen (Einzelbefehle: rund 5 % der Fälle sind Adressfehler; 180 000 Folgen).
- **Differenztest für Befehlsfolgen gegen Musashi** (`tools/diff_musashi_seq.cpp`, in `tools/run_diff_musashi.sh` und der CI): zufällige Programme aus 2–10 Befehlen
  im Gleichschritt, Vergleich nach jedem Befehl (Register, SR, PC, geschriebene Bytes), Handler für Ausnahmen, Musashi mit Trace-Emulation. 0 Abweichungen über
  60 000 Programme; eine absichtlich falsche Trace-Entscheidung wird gefunden. Nicht verglichen: Adressfehler, `STOP`, PC über 24 Bit (Musashi behält das obere Byte,
  dieser Kern maskiert; siehe docs/emulator.md).
- **Hunk-Ausgabe des Assemblers:** `SECTION name[,CODE|DATA|BSS][_C|_F]` (Kurzformen `CODE`/`DATA`/`BSS`), `XDEF`, Relokationen für lange absolute Adressen,
  lange Direktwerte und `DC.L` (`HUNK_RELOC32`), `m68ktool asm --hunk [--hunk-symbols]`, `m68k::writeHunkFile`. Labels sind „Offset in Sektion“;
  PC-relative Verweise zwischen Sektionen und Differenzen/Summen von Adressen verschiedener Sektionen werden abgelehnt. Absolute Programme sind unverändert.
  Tests `hunk_write` (2108 Prüfungen, darunter 400 Zufallsprogramme: Hunk geladen = absolut übersetzt), Beispiel `examples/sections.s`.
- **Emulierte Amiga-Bibliotheksaufrufe** (`m68k::AmigaOs`, `machine … os:true`): ExecBase bei Adresse 4, Bibliotheksbasen mit echter
  `JMP`-Sprungtabelle, deren Ziele der Emulator abfängt (`Cpu::executeHook`); nachgebildet sind `AllocMem`/`FreeMem`/`AllocVec`/`FreeVec`,
  `OpenLibrary`/`CloseLibrary`, `FindTask`, `Forbid`/`Permit`/`Disable`/`Enable` sowie dos `Output`/`Input`/`Write`/`Read`/`Close`/`Open`/
  `Lock`/`IoErr`/`Delay`/`Exit`; **jeder andere Aufruf wird protokolliert und liefert 0**. Neue Protokollbefehle `libcalls` und `console`,
  Stop-Grund `exit`; Monitor: `machine amiga500 os`, `prog`, `libcalls`, `console`; der Orchestrator meldet Bibliotheksaufrufe im
  `run_routine`-Ergebnis (`--no-os` schaltet es ab). Tests `os` (76 Prüfungen), Monitor- und Orchestrator-Tests.
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
