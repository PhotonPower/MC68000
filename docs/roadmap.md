# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion. Vorbild ist der Weg des [Z80-Projekts](https://github.com/PhotonPower/Z80).

## Wo wir stehen

Projektgerüst (0.1): Build, Tests, CI, Dokumentation, `types`/`format`/`memory`.

## Meilenstein 0.2 – Dekoder, Assembler, Disassembler

- `Instruction`, `decode()` nach Schema (Opcode-Gruppen, Modus/Register-Felder, Erweiterungswörter)
- Ausdrucksauswerter, `encodeInstruction()`, Mehrpass-Assembler mit Verschiebungsgrößen-Bestimmung
- Disassembler mit Ablaufanalyse (`JSR`, `BSR`, `BRA`, `Bcc`, `DBcc`, `JMP`), Querverweisen, Kommentardatei, `--asm`
- Ausgabeformate: Raw-Binary, S-Record, Listing, Symboltabelle
- Tests: Einzelbefehle, exhaustiver Roundtrip, Robustheit

## Meilenstein 0.3 – Emulator

- `Cpu`: alle Befehle, Flags, Supervisor/Benutzer, Ausnahmen und Stapelrahmen, Interrupts, Zyklen
- Breakpoints, Watchpoints, Checkpoints, Snapshots, Speicher-/E/A-Hooks
- `Session` und `m68ktool serve`
- Tests: Befehlstests, Zufallscode mit Snapshot-Wiederholung, Kreuzprüfung

## Meilenstein 0.4 – Monitor

- `m68k::Monitor` und `m68ktool monitor` mit den Befehlen aus [monitor.md](monitor.md)

## Meilenstein 0.5 – Reverse Engineering

- Annotationen, kommentiertes Listing mit Neu-Assemblieren-Prüfung
- Orchestrator (`agent/`, Python) für Ollama, Plattformprofil mit Systemaufrufen

## Danach (Ideen)

- MC68010-Befehle und -Verhalten (optional per Schalter), MC68008
- Makros, `INCLUDE`, bedingte Assemblierung
- Prefetch-/busgenaues Timing
- Gerätemodelle für eine Zielplattform
- Projektdatei (Kommentare, Symbole, Bereiche) statt vieler Kommandozeilenoptionen

## Offene Fragen

1. **Zielplattform:** nur generischer 68000, oder gezielt Amiga, Atari ST, Mega Drive, Macintosh,
   Sinclair QL (dort steckt ein MC68008), …? Das bestimmt Geräte, Systemaufrufe und Dateiformate
   (z. B. Amiga-Hunk, Atari-`.PRG`).
2. **Syntax:** Motorola-Syntax ist als Standard festgelegt; soll zusätzlich die Syntax des GNU-Assemblers
   (`move.l %d0,%a0@+`) gelesen werden?
3. **Prozessorvarianten:** nur MC68000, oder auch MC68008/MC68010?
4. **Genauigkeit:** reicht befehls-/zyklengenau, oder soll es später busgenau werden?
5. **Befehlsstil des Monitors:** wie beim Z80-Monitor, oder Anlehnung an einen historischen 68000-Monitor?
