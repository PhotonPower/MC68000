# Emulator

> **Geplant** (Meilenstein 0.3). Beschreibt das Zielbild.

## Umfang

- befehlsgenauer MC68000-Kern (`m68k::Cpu`): alle Befehle, alle Adressierungsarten, Flags `X N Z V C`
- Supervisor-/Benutzermodus mit getrennten Stapelzeigern (`SSP`/`USP`), Statusregister inkl. Trace und Interrupt-Maske
- Ausnahmen: Reset (Start-SSP/-PC aus den Vektoren 0 und 1), Adressfehler, unerlaubter Befehl,
  Division durch 0, `CHK`, `TRAPV`, Privilegverletzung, Trace, Line A/F, `TRAP #n`, Interrupts
  (Autovektor und Vektornummer vom Gerät) mit korrekten Stapelrahmen
- Zyklenzählung pro Befehl; Takte als Vielfache von vier (Buszyklen)
- Einzelschritt, Lauf bis Breakpoint/Adresse/Rückkehr (`RTS`), Step-over, Watchpoints,
  Befehls-/Zyklenlimits, Trace-Hook, Speicher- und E/A-Hooks (speicherabgebildet)
- **Zustand speichern/laden:** Checkpoints im Speicher, Snapshot-Datei, CPU-Zustand als JSON;
  Wiederaufnahme ist bit-identisch

## Genauigkeit

Stufenweise Ziele:

1. **Befehlsgenau:** richtige Ergebnisse, Flags, Ausnahmen – genügt für die meisten Programme und
   für das Reverse Engineering.
2. **Zyklengenau:** richtige Taktzahl pro Befehl.
3. **Busgenau (optional):** Prefetch-Queue und einzelne Buszyklen. Wird nur wichtig bei selbstmodifizierendem
   Code, exaktem Timing und Busfehlern; das Ziel steht noch nicht fest.

Unbekannt bzw. offen: Verhalten der Befehle mit unerlaubten Bitmustern auf echter Hardware wird nur
dort nachgebildet, wo es durch Tests belegt ist.

## Plattformen

Der Kern kennt keine bestimmte Maschine; Geräte (z. B. Amiga-Custom-Chips, Atari-ST-Hardware,
Mega-Drive-VDP) kommen als Hook-Module hinzu. Die Zielplattform ist offen
([roadmap.md](roadmap.md#offene-fragen)).
