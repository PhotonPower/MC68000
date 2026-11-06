# Tests

```sh
ctest --test-dir build --output-on-failure
```

Die Tests brauchen kein externes Framework; `tests/test_util.hpp` stellt `CHECK`/`CHECK_EQ` bereit.

## Vorhandene Suites

| Suite        | Inhalt |
|--------------|--------|
| `format`     | Hexadezimalausgabe in allen Zahlenstilen, Parsen der Stilnamen |
| `memory`     | Big-Endian-Zugriffe, 24-Bit-Maskierung, Umlauf bei 16 MiB, seitenweise Belegung, `load`/`readBytes`, Kopieren, Ausrichtung |
| `cli_version`| `m68ktool version` gibt die Version aus |

## Geplante Suites

| Suite        | Inhalt |
|--------------|--------|
| `expression`, `assembler`, `decoder` | wie im Z80-Projekt, mit erwarteten Bytes aus dem Programmer's Reference Manual |
| `roundtrip`  | **exhaustiv**: alle 65536 ersten Opcode-Wörter (mit repräsentativen Erweiterungswörtern); was der Dekoder als Befehl erkennt, muss der Assembler zu denselben Bytes zurückübersetzen |
| `robustness` | feste Zufallsfolge: `assemble(disassemble(x)) == x`; zufällige Quelltexte dürfen den Assembler nicht abstürzen lassen |
| `cpu`        | Flags, BCD, Multiplikation/Division, Ausnahmen und Stapelrahmen, Privilegien, Adressfehler, Interrupts, Zufallscode mit Wiederholung aus Snapshot |

## Kreuzprüfung gegen unabhängige Werkzeuge (geplant)

Der Roundtrip-Test beweist nur die Konsistenz von Dekoder und Assembler *untereinander*. Gegen
gemeinsame Denkfehler helfen unabhängige Referenzen. Kandidaten (jeweils vor Verwendung zu
prüfen: Verfügbarkeit, Lizenz, Abdeckung):

- ein unabhängiger 68000-Assembler/Disassembler (z. B. GNU binutils für m68k, vasm) für `tools/crosscheck_*.py`;
- öffentlich verfügbare Einzelbefehls-Testsätze für den 68000 (Eingangs-/Ausgangszustand je Befehl);
- ein etablierter Software-Emulator als Gegenprobe für Flags und Ausnahmen (Zyklen werden nicht verglichen).

Welche davon tatsächlich eingesetzt werden, wird in dieser Datei festgehalten, sobald sie
ausprobiert wurden.
