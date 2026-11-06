# Emulator

> **Geplant** (Meilenstein 0.3). Beschreibt das Zielbild.

## Umfang

- befehlsgenauer MC68000-Kern (`m68k::Cpu`): alle Befehle, alle Adressierungsarten, Flags `X N Z V C`
- Supervisor-/Benutzermodus mit getrennten Stapelzeigern (`SSP`/`USP`), Statusregister inkl. Trace und Interrupt-Maske
- Ausnahmen: Reset (Start-SSP/-PC aus den Vektoren 0 und 1), Adressfehler, unerlaubter Befehl,
  Division durch 0, `CHK`, `TRAPV`, Privilegverletzung, Trace, Line A/F, `TRAP #n`, Interrupts
  (Autovektor und Vektornummer vom Gerät) mit korrekten Stapelrahmen
- Einzelschritt, Lauf bis Breakpoint/Adresse/Rückkehr (`RTS`), Step-over, Watchpoints,
  Befehlslimits, Trace-Hook, Speicher- und E/A-Hooks (speicherabgebildet)
- **Zustand speichern/laden:** Checkpoints im Speicher, Snapshot-Datei, CPU-Zustand als JSON;
  Wiederaufnahme ist bit-identisch

## Genauigkeit

Entscheidung: **Nur die Speicherinhalte (und Register) nach der Ausführung zählen.** Der Emulator ist
befehlsgenau und funktional:

- richtige Ergebnisse, Flags und Ausnahmen, richtige Stapelrahmen und Speicherschreibzugriffe;
- **kein Timing:** keine Zyklenzählung, keine Prefetch-Queue, keine Buszyklen, keine DMA-Konkurrenz;
- als Zeitmaß dienen nur Befehlszähler (Limits für Läufe, Trace).

Folgen: Selbstmodifizierender Code, der vom Prefetch abhängt, und zeitkritische Programme verhalten
sich möglicherweise anders als auf echter Hardware. Das wird bewusst in Kauf genommen.

## Plattformen

Der Kern kennt keine bestimmte Maschine; Geräte kommen als Hook-Module hinzu. Zielplattform ist der
**Amiga 500** ([amiga500.md](amiga500.md)): Speicherkarte mit ROM-Overlay, Chip-RAM, Custom-Chip-Registern
und CIAs.
