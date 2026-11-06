# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion. Vorbild ist der Weg des [Z80-Projekts](https://github.com/PhotonPower/Z80).

## Wo wir stehen

**Version 0.2:** Dekoder, Assembler und Disassembler (Meilenstein 0.2) sind fertig und gegen GNU binutils
kreuzgeprüft ([testing.md](testing.md)). `m68ktool asm` und `m68ktool dis` arbeiten mit Raw-Binary und
Motorola S-Records. Emulator, Monitor und Agenten-Schnittstelle fehlen noch.

## Meilenstein 0.2 – Dekoder, Assembler, Disassembler (erledigt)

- `Instruction`, `decode()` nach Tabelle (Opcode-Maske, Modus/Register-Felder, Erweiterungswörter)
- Ausdrucksauswerter, `encodeInstruction()`, Mehrpass-Assembler mit Verschiebungsgrößen-Bestimmung
- Disassembler mit Ablaufanalyse, Querverweisen, Kommentardatei, `--asm`
- Ausgabeformate: Raw-Binary, S-Record, Listing, Symboltabelle
- Tests: Einzelbefehle, exhaustiver Roundtrip, Robustheit, Kreuzprüfung gegen `objdump` und `as`

Amiga-Hunk (siehe [amiga500.md](amiga500.md)):
- **Hunk-Lader** (Relozieren, Symbole) – erledigt; Eingabeformat für den Disassembler, später für den Emulator
- noch offen: **Hunk-Ausgabe** des Assemblers (`SECTION`, `XDEF`, Relokationen)
- optional: Optimierungen des Assemblers (`ADD #n` → `ADDQ`, `MOVE #n` → `MOVEQ`, absolut → PC-relativ), per Schalter

## Meilenstein 0.3 – Emulator

- `Cpu`: alle Befehle, Flags, Supervisor/Benutzer, Ausnahmen und Stapelrahmen, Interrupts (auf Anforderung); kein Timing
- Breakpoints, Watchpoints, Checkpoints, Snapshots, Speicher-/E/A-Hooks
- Amiga-500-Speicherkarte (ROM-Overlay, Chip-RAM, Custom-Register als einfacher Block, CIA nach Bedarf)
- `Session` und `m68ktool serve`
- Tests: Befehlstests, Zufallscode mit Snapshot-Wiederholung, Kreuzprüfung

## Meilenstein 0.4 – Monitor

- `m68k::Monitor` und `m68ktool monitor` mit den Befehlen aus [monitor.md](monitor.md)

## Meilenstein 0.5 – Reverse Engineering

- Annotationen, kommentiertes Listing mit Neu-Assemblieren-Prüfung
- Orchestrator (`agent/`, Python) für Ollama, Amiga-Plattformprofil mit LVO-Tabelle (Bibliotheksaufrufe über `A6`)

## Danach (Ideen)

- Makros, `INCLUDE`, bedingte Assemblierung
- Stubs für häufige Amiga-Bibliotheksfunktionen (`exec`, `dos`), die ihr Ergebnis im Speicher nachbilden
- Projektdatei (Kommentare, Symbole, Bereiche) statt vieler Kommandozeilenoptionen

## Entscheidungen

1. **Zielplattform:** Amiga 500 ([amiga500.md](amiga500.md)). Der Kern bleibt maschinenunabhängig.
2. **Syntax:** nur Motorola-Syntax; die GNU-Syntax (`move.l %d0,%a0@+`) ist kein Ziel.
3. **Prozessor:** nur MC68000 (kein 68008, 68010 oder höher).
4. **Genauigkeit:** nur Speicherinhalte und Register zählen; kein Timing (keine Zyklen, kein Prefetch,
   keine Bus-/DMA-Modelle).
5. **Monitor:** Bedienung wie beim Z80-Monitor, keine Anlehnung an historische Monitore.
6. **Amiga-Umfang:** keine Ton-, Disketten- oder Bildschirmausgabe, kein Kickstart-Boot als Ziel.

## Offene Fragen

Derzeit keine. Neue Fragen entstehen mit Meilenstein 0.2 (z. B. Namensgebung der Bibliotheks-LVO-Tabelle).
