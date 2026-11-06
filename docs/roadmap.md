# Roadmap

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion. Vorbild ist der Weg des [Z80-Projekts](https://github.com/PhotonPower/Z80).

## Wo wir stehen

**Version 0.5:** Dekoder, Assembler, Disassembler, Emulator, Snapshots, Agenten-Schnittstelle (`m68ktool serve`), Amiga-500-Speicherkarte,
Monitor (`m68ktool monitor`) und die Reverse-Engineering-Werkzeuge (Analyse, Annotationen, verifiziertes Listing) sind fertig und gegen
GNU binutils bzw. Musashi kreuzgeprüft ([testing.md](testing.md)). Dazu kommt der Orchestrator für ein lokales LLM (nur gegen einen Mock getestet). Offen ist vor allem seine Erprobung mit einem echten
Modell.

## Meilenstein 0.2 – Dekoder, Assembler, Disassembler (erledigt)

- `Instruction`, `decode()` nach Tabelle (Opcode-Maske, Modus/Register-Felder, Erweiterungswörter)
- Ausdrucksauswerter, `encodeInstruction()`, Mehrpass-Assembler mit Verschiebungsgrößen-Bestimmung
- Disassembler mit Ablaufanalyse, Querverweisen, Kommentardatei, `--asm`
- Ausgabeformate: Raw-Binary, S-Record, Listing, Symboltabelle
- Tests: Einzelbefehle, exhaustiver Roundtrip, Robustheit, Kreuzprüfung gegen `objdump` und `as`

Amiga-Hunk (siehe [amiga500.md](amiga500.md)):
- **Hunk-Lader** (Relozieren, Symbole) – erledigt; Eingabeformat für den Disassembler, später für den Emulator
- **Hunk-Ausgabe** des Assemblers (`SECTION`/`CODE`/`DATA`/`BSS`, `XDEF`, `RELOC32`, Speicherart `_C`/`_F`) – erledigt ([assembler-syntax.md](assembler-syntax.md#sektionen-und-hunk-ausgabe-section))
- Optimierungen des Assemblers (`ADD #n` → `ADDQ`, `MOVE.L #n,Dn` → `MOVEQ`, `.L` → `.W` für `An`), per Schalter `-O` – erledigt; noch offen: absolut → PC-relativ

## Meilenstein 0.3 – Emulator (erledigt)

Erledigt ([emulator.md](emulator.md), [agent-protocol.md](agent-protocol.md), [amiga500.md](amiga500.md)):
- `Cpu` mit allen Befehlen, Flags, Supervisor/Benutzer, Ausnahmen und Stapelrahmen, Interrupts, `STOP`, Trace; Einzelschritt,
  Breakpoints, Watchpoints, `runUntil`/`runUntilReturn`/`stepOver`/`call`, Speicher-Hooks; Differenztest gegen Musashi
  (65536 Opcodes, 0 Abweichungen) und Unit-Tests
- Snapshots (Speicher und Datei), Speicher-Diff
- `Session` und `m68ktool serve` (JSON-Zeilenprotokoll nach dem Vorbild des Z80-Protokolls)
- Amiga-500-Speicherkarte (Chip-/Fast-/Slow-RAM, Custom-/CIA-Register als Speicher mit Schreibprotokoll, ROM-Overlay);
  Hunk-Programme laden und starten

- **OS-Emulation** (`m68k::AmigaOs`, [amiga500.md](amiga500.md#emulierte-bibliotheksaufrufe-m68kamigaos)): ExecBase, Bibliotheksbasen mit
  Sprungtabellen, `AllocMem`/`FreeMem`/`AllocVec`/`FreeVec`, `OpenLibrary`/`CloseLibrary`, `FindTask`, dos `Output`/`Write`/…/`Exit`;
  alles andere wird protokolliert (`libcalls`) und liefert 0

Offen (Ideen):
- weitere `exec`-/`dos`-Funktionen (Listen, Nachrichten/Ports, `Open` auf eine Dateiabbildung, `Printf`-Familie) – nur, wo die Semantik sicher bekannt ist
- Vergleich mit Daten von echter Hardware (öffentliche Einzelbefehls-Testsätze), vor allem für den gestapelten PC und die Statuswort-Bits eines Adressfehlers

## Meilenstein 0.4 – Monitor (erledigt)

`m68ktool monitor` ([monitor.md](monitor.md)) bietet die klassischen Befehle des Z80-Monitors: `d m e f c t h a r n o u g ru b bc w limit sym
symload symsave asm l sv ck rb savestate loadstate int io reset` und `machine`, `rom` für den Amiga. Hex als Standard, Assemble-Modus,
Raw/S-Record/Hunk laden.

Offen (Ideen):
- Zeileneditor mit Verlauf im Programm (statt `rlwrap`), Tab-Vervollständigung für Symbole
- Vorwärtsreferenzen beim zeilenweisen Assemblieren (Fixups, die bei der Definition nachgetragen werden)
- Textoberfläche (TUI) mit Registern, Disassembly und Speicher gleichzeitig

## Meilenstein 0.5 – Reverse Engineering (erledigt, Erprobung offen)

Erledigt ([reverse-engineering.md](reverse-engineering.md)): Annotationen (Namen, Dokumentation, Variablen, Datenbereiche, Kommentare,
Projektdatei), Codefluss-Analyse mit Routinen/Aufrufgraph/Bibliotheksaufrufen, **verifiziertes** Listing (neu assembliert und
verglichen), die Protokollbefehle `analyze`, `routines`, `listing`, `ann_*` und der **Orchestrator** `agent/m68k_annotate.py`
(Ollama, Routinen von unten nach oben, Mehrdurchgang-Verfahren, Vollständigkeitsprüfung, Bericht), getestet gegen einen scripteten
Mock-Ollama.

Sprungtabellen (Branch- und Offset-Tabellen automatisch, Zeigertabellen als Vorschlag) erkennt die Analyse seit 0.5 ([reverse-engineering.md](reverse-engineering.md#sprungtabellen)).

Offen:
- **Erprobung mit einem echten Modell** (bisher nur Mock) und Feinschliff von Prompt und Parametern
- Erkennung von Datentypen (Text, Strukturtabellen)

## Danach (Ideen)

- Makros, `INCLUDE`, bedingte Assemblierung
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
