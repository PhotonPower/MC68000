# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion. Vorbild ist der Weg des [Z80-Projekts](https://github.com/PhotonPower/Z80).

## Wo wir stehen

Projektgerüst (0.1): Build, Tests, CI, Dokumentation, `types`/`format`/`memory`.

## Meilenstein 0.2 – Dekoder, Assembler, Disassembler

- `Instruction`, `decode()` nach Schema (Opcode-Gruppen, Modus/Register-Felder, Erweiterungswörter)
- Ausdrucksauswerter, `encodeInstruction()`, Mehrpass-Assembler mit Verschiebungsgrößen-Bestimmung
- Disassembler mit Ablaufanalyse (`JSR`, `BSR`, `BRA`, `Bcc`, `DBcc`, `JMP`), Querverweisen, Kommentardatei, `--asm`
- Ausgabeformate: Raw-Binary, S-Record, Amiga-Hunk-Executable, Listing, Symboltabelle
- Hunk-Lader (Relozieren) als Eingabeformat für den Disassembler
- Tests: Einzelbefehle, exhaustiver Roundtrip, Robustheit

## Meilenstein 0.3 – Emulator

- `Cpu`: alle Befehle, Flags, Supervisor/Benutzer, Ausnahmen und Stapelrahmen, Interrupts, Zyklen
- Breakpoints, Watchpoints, Checkpoints, Snapshots, Speicher-/E/A-Hooks
- Amiga-500-Speicherkarte (ROM-Overlay, Chip-RAM, Custom-Register, CIA), Kickstart-Boot als Ziel
- `Session` und `m68ktool serve`
- Tests: Befehlstests, Zufallscode mit Snapshot-Wiederholung, Kreuzprüfung

## Meilenstein 0.4 – Monitor

- `m68k::Monitor` und `m68ktool monitor` mit den Befehlen aus [monitor.md](monitor.md)

## Meilenstein 0.5 – Reverse Engineering

- Annotationen, kommentiertes Listing mit Neu-Assemblieren-Prüfung
- Orchestrator (`agent/`, Python) für Ollama, Amiga-Plattformprofil mit LVO-Tabelle (Bibliotheksaufrufe über `A6`)

## Danach (Ideen)

- Makros, `INCLUDE`, bedingte Assemblierung
- Prefetch-/busgenaues Timing
- weitere Amiga-Gerätemodelle (Blitter, Copper, Ton, Disketten)
- Projektdatei (Kommentare, Symbole, Bereiche) statt vieler Kommandozeilenoptionen

## Entscheidungen

1. **Zielplattform:** Amiga 500 ([amiga500.md](amiga500.md)). Der Kern bleibt maschinenunabhängig.
2. **Syntax:** nur Motorola-Syntax; die GNU-Syntax (`move.l %d0,%a0@+`) ist kein Ziel.
3. **Prozessor:** nur MC68000 (kein 68008, 68010 oder höher).

## Offene Fragen

1. **Genauigkeit:** reicht befehls-/zyklengenau, oder soll der Emulator später busgenau werden
   (relevant für Amiga-Timing mit Blitter-/DMA-Konkurrenz)?
2. **Befehlsstil des Monitors:** wie beim Z80-Monitor, oder Anlehnung an einen historischen
   Amiga-Monitor (z. B. aus Devpac/AsmOne)?
3. **Amiga-Umfang:** reicht „Kickstart bootet und zeichnet Text/Bild“, oder sollen Disketten (ADF) und
   Ton mitgedacht werden?
