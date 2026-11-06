# Emulator

`m68k::Cpu` (`include/m68k/cpu.hpp`) ist ein **funktionaler** MC68000-Kern: Für jeden Befehl stimmen Register,
Flags und Speicherinhalt; Zeit wird nicht modelliert. Der Zustand (Register, Speicher) ist die Schnittstelle –
siehe die Entscheidungen in der [Roadmap](roadmap.md).

> **Stand:** Prozessorkern, Snapshots, die JSON-Sitzung (`m68ktool serve`, [agent-protocol.md](agent-protocol.md)) und die
> Amiga-500-Speicherkarte ([amiga500.md](amiga500.md)) sind fertig und geprüft.

## Umfang

- **Alle Befehle** des MC68000 mit allen Adressierungsarten und Flags (`X N Z V C`). Welche Opcodes gültig sind,
  entscheidet dieselbe Befehlstabelle wie beim Disassembler ([architecture.md](architecture.md)).
- **Supervisor-/Benutzermodus** mit getrennten Stapelzeigern (`A7` ist der jeweils aktive; `usp()`/`ssp()` liefern
  beide), Privilegprüfung (`MOVE to SR`, `MOVE USP`, `ANDI/ORI/EORI to SR`, `RTE`, `STOP`, `RESET`).
- **Ausnahmen** mit korrekten Stapelrahmen: Adressfehler (14-Byte-Rahmen), unerlaubter Befehl (gestapelter PC = Adresse
  des Befehls), Line A/Line F, Privilegverletzung, Division durch 0, `CHK`, `TRAPV`, `TRAP #n`, Trace (`T`-Bit).
  Ein Fehler beim Anlegen eines Rahmens (z. B. ungerader Stapelzeiger) hält den Prozessor an (`halted()`),
  `reset()` startet ihn neu.
- **Interrupts** auf Anforderung (`interrupt(level, vector)`): Pegel 1–7, Pegel 7 nicht maskierbar, Autovektoren
  oder ein vom Gerät gelieferter Vektor; `STOP` wartet auf einen Interrupt.
- **Adressfehler** bei Wort-/Langwortzugriffen und Befehlsholen an ungeraden Adressen; Adressen laufen bei 16 MiB um. Der 14-Byte-Rahmen enthält
  Statuswort (Lesen/Schreiben, **Funktionscode**: Daten- oder **Programmraum** – PC-relative Operanden und Befehlsholen sind Programmraum), Zugriffsadresse,
  Befehlswort, SR und PC. Einzelheiten, die sich beim Abgleich mit Musashi zeigten und nach dessen (hardwarenahem) Verhalten umgesetzt sind: `MOVE.L`/`MOVEM.L`
  nach `-(An)` schreiben das **niederwertige Wort zuerst**, ein ungerader Zugriff wird also bei `ea+2` gemeldet; Adressregister mit `(An)+` und der Stapelzeiger bei
  Pops (`UNLK`, `RTS`, `RTE`, `RTR`) sind beim Fehler **schon weitergezählt**; `CLR` und `MOVE` setzen ihre Flags **nach** dem Schreiben (ein Fehler lässt die alten
  Flags im Rahmen); bei `LINK` wird die Verschiebung nach dem Push geholt (gestapelter PC = Befehlsadresse + 2).

## Bedienung

| Aufruf | Wirkung |
|--------|---------|
| `reset()` | Supervisor-Modus, Maske 7, SSP und PC aus den Vektoren 0 und 1 |
| `step()` | ein Befehl (oder eine anstehende Interrupt-Annahme); meldet die Ausnahme-Nummer, falls eine auftrat |
| `run(n)` | höchstens `n` Befehle; stoppt an Breakpoints und Watchpoints; der Befehl am Startpunkt läuft auch dann, wenn dort ein Breakpoint sitzt |
| `runUntil(adresse, n)` | bis `pc == adresse` (vor dem Befehl geprüft) |
| `runUntilReturn(n)` | bis die laufende Unterroutine zurückkehrt (Aufruftiefe: `BSR`/`JSR`/Ausnahmen zählen, `RTS`/`RTR`/`RTE` zählen ab) |
| `stepOver(n)` | wie `step()`, aber `BSR`/`JSR` laufen bis zur Rückkehr |
| `call(einstieg, rücksprung, n)` | legt die Rücksprungadresse auf den Stapel und läuft bis dorthin |
| `addBreakpoint`, `addWatchpoint(start, länge, lesen, schreiben)` | Haltepunkte; ein Watchpoint hält **nach** dem Befehl, der zugreift |
| `traceHook`, `accessHook` | Rückrufe vor jedem Befehl bzw. bei jedem Datenzugriff |
| `readOverride`, `writeOverride` | Zugriffe abfangen (speicherabgebildete Geräte, ROM, Overlay); `readOverride` gilt auch für Befehlsholen und Vektorlesen |
| `resetHook` | der `RESET`-Befehl |
| `executeHook`, `hookLow`/`hookHigh` | statt eines Befehls aufgerufen, wenn der PC im Bereich liegt (Betriebssystemfunktionen, [amiga500.md](amiga500.md)); `returnFromSubroutine()` und `requestStop(text)` (→ `StopReason::Requested`) für den Haken |
| `registers()`, `setSr()`, `peek()` | Zugriff auf den Zustand ohne Seiteneffekte |
| `state()`, `setState()` | der ganze Prozessorzustand (Register, Zähler, anstehender Interrupt) – Grundlage der Snapshots |

Die Ergebnisse von `run…` nennen den Grund (`StopReason`): Limit, Breakpoint, Watchpoint, Reached, Returned, Stopped, Halted, Requested.

## Genauigkeit

**Nur Speicherinhalte und Register zählen – kein Timing.** Es gibt keine Zyklenzählung, keine Prefetch-Queue, keine
Buszyklen und keine DMA-Konkurrenz. Selbstmodifizierender Code, der vom Prefetch abhängt, und zeitkritische Programme
verhalten sich deshalb möglicherweise anders als auf echter Hardware.

### Wie der Kern geprüft ist

1. **Differenztest gegen Musashi** (`tools/diff_musashi.cpp`, `tools/run_diff_musashi.sh`): Musashi ist ein
   etablierter, unabhängiger MC68000-Emulator. Für **jeden der 65536 Opcodes** werden zufällige Zustände (Register mit
   Sonderwerten, Zeiger ins RAM, zufälliges SR, Supervisor/Benutzer) und zufällige Erweiterungswörter in beiden
   Emulatoren eingerichtet, ein Befehl ausgeführt und **alle Register, das SR und jedes geschriebene Byte** verglichen
   (auch der Ausnahme-Rahmen bei unerlaubten Befehlen, Privilegverletzung, `CHK`, Division durch 0). Ein Lauf vergleicht
   rund 2,5 Millionen Fälle (`m68k_diff_musashi 40`); Ergebnis über mehrere Seeds: **0 Abweichungen**. Auch unter
   AddressSanitizer/UBSan ohne Befund. Musashi wird zur Laufzeit geholt und nicht in dieses Repository aufgenommen.
2. **Differenztest für Befehlsfolgen** (`tools/diff_musashi_seq.cpp`, ebenfalls über `tools/run_diff_musashi.sh`): Zufällige **Programme** aus 2–10
   Befehlen (rund 15 % aus den Familien, deren Ergebnis von früheren Befehlen abhängt: `ADDX`/`SUBX`/`NEGX`, `ABCD`/`SBCD`/`NBCD`, `ROXL`/`ROXR`,
   `MOVE` von/nach SR/CCR/USP, `LINK`/`UNLK`, `MOVEM`, `RTE`/`RTR`, `TRAP`, `CHK`, …) laufen in beiden Emulatoren im Gleichschritt; **nach jedem Befehl**
   werden Register, SR, PC und jedes geschriebene Byte verglichen. Die Vektortabelle zeigt auf kleine Handler (`RTE`, bei unerlaubtem Befehl, Privilegverletzung
   und Line A/F `ADDQ.L #2,2(A7)` + `RTE`), sodass ein Programm nach einer Ausnahme weiterläuft; Ausnahmerahmen werden also von `RTE` wieder abgebaut, Stapelwechsel
   zwischen Benutzer- und Supervisor-Modus, Auto-Inkrement/-Dekrement und das X-Flag über mehrere Befehle hinweg sind Teil des Vergleichs. Musashi wird dafür mit
   **Trace-Emulation** gebaut, sodass auch die Trace-Ausnahme verglichen wird (ein vorheriges `MOVE …,SR` setzt das T-Bit). Ergebnis: **0 Abweichungen** über
   60 000 Programme (rund 300 000 Befehle, rund 60 000 Ausnahmen). Der Test hat Wirkung: Eine absichtlich falsche Trace-Entscheidung (nach statt vor dem Befehl) wird sofort
   gefunden, vom Einzelbefehlstest nicht. Seit Musashi mit **Adressfehler-Emulation** gebaut wird, sind auch Adressfehler Teil des Vergleichs (14-Byte-Rahmen, Folgen mit ungeraden Stapelzeigern; ein Handler baut den
   Rahmen wieder ab). Nicht verglichen werden Programme mit `STOP`, an einem 68040-`MOVE16`-Wort (Musashi führt es auch als 68000 aus) und solche, in denen der
   **PC Bits oberhalb von Bit 23** bekommt (ein `RTE`/`RTS`/`JMP` auf eine solche Adresse): Musashi behält das obere Byte des PC und legt es später wieder auf den Stapel, dieser
   Kern maskiert den PC auf 24 Bit; was der echte MC68000 dort tut, lässt sich mit diesem Test nicht entscheiden (praktisch kommt es in Amiga-Programmen nicht vor).
3. **Unit-Tests** (`tests/test_cpu.cpp`, rund 52 000 Prüfungen): von Hand gerechnete Ergebnisse und Flags,
   Adressierungsarten, `MOVEM`/`LINK`/`MOVEP`, Unterprogramme und Schleifen, alle Ausnahmen mit Rahmeninhalt,
   Stapelwechsel Benutzer/Supervisor, Interrupts, `STOP`, Trace, Ablaufsteuerung, Hooks – und ein Abgleich mit dem
   Dekoder für alle 65536 Opcodes (was der Disassembler als Befehl kennt, ist für den Prozessor nicht „illegal“).

**Nicht** an echter Hardware geprüft. Wo das Handbuch Flags oder Ergebnisse **undefiniert** lässt, folgt der Kern
dem Verhalten von Musashi (so wie die meisten Emulatoren):

| Fall | Verhalten |
|------|-----------|
| `CHK` | `Z` = (Dn.w == 0), `V` und `C` gelöscht; `N` nur bei Ausnahme gesetzt (Dn < 0) bzw. gelöscht (Dn > Grenze) |
| `DIVU`/`DIVS` mit Überlauf | nur `V` wird gesetzt, Zielregister bleibt, `N`/`Z`/`C` unverändert |
| `DIVS` mit `$80000000 / -1` | Ergebnis 0, `Z` gesetzt, `N`/`V`/`C` gelöscht |
| `ABCD`/`SBCD`/`NBCD` | `N` und `V` aus den Zwischenergebnissen, wie Musashi; bei `NBCD` mit Zwischenergebnis `$9A` (Operand `0`, oder ungültige BCD-Zahl mit `X`) bleibt das Ziel unverändert |
| `LINK A7,#d` | gestapelt wird der um 4 verminderte Stapelzeiger |

### Vereinfachungen

- Der Busfehler (Vektor 2) wird vom Kern nie ausgelöst. Im Rahmen eines Adressfehlers ist der gestapelte PC der PC
  nach den bis dahin geholten Wörtern (die echte Hardware stapelt einen Wert bis 10 Byte hinter dem Befehlsanfang).
- `MOVEM` liest nicht das zusätzliche Wort hinter der Registerliste, `CLR` liest vor dem Schreiben nicht
  (sichtbar nur über Lese-Watchpoints bzw. -Hooks); Befehlsholen und Vektorlesen laufen über `readOverride`
  (Code im ROM, Vektortabelle über das Overlay), aber nicht über `accessHook` und Watchpoints.
- Interrupts kommen nur über `interrupt()`, nicht von Geräten; der Differenztest deckt Adressfehler, Interrupts,
  `STOP` nicht ab – dafür gibt es die Unit-Tests. Adressfehler, Befehlsfolgen und Trace werden verglichen (siehe oben); im Statuswort eines Adressfehler-Rahmens
  nicht: die Bits 15–5 (undefiniert: dieser Kern füllt sie aus dem Befehlswort, Musashi mit 0) und Bit 3 (Musashi behält dort einen Rest früherer Ausnahmen).
  Der **gestapelte PC** bei Adressfehlern ist nur so genau wie Musashi (die echte Hardware stapelt je nach Befehl einen anderen Wert). Das obere Byte des PC wird nicht nachgebildet: der PC ist 24 Bit breit.

## Plattformen

Der Kern kennt keine bestimmte Maschine; Geräte hängen über `readOverride`/`writeOverride` an. Zielplattform ist der
**Amiga 500**: `m68k::Amiga500` ([amiga500.md](amiga500.md)) bildet die Speicherkarte nach (Chip-RAM, ROM-Overlay,
Custom-Chip- und CIA-Register als Speicher mit Schreibprotokoll).

## Snapshots

`capture(cpu)` / `restore(cpu, snapshot)` sichern und setzen Prozessorzustand und Speicher; `serialize()` /
`deserializeSnapshot()` schreiben/lesen die Datei (`M68KSNP1`, Register, Zähler, die belegten 64-KiB-Seiten; beschädigte
Dateien werden abgelehnt). Das Zurückspielen ist bit-identisch: Ein erneuter Lauf liefert dieselben Register und
denselben Speicher (getestet). Haltepunkte und Hooks gehören nicht zum Zustand.
