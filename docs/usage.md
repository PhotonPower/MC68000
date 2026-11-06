# Benutzung von `m68ktool`

```
m68ktool asm <source.s> [Optionen]
m68ktool dis <image>    [Optionen]
m68ktool help | version
```

Rückgabewerte: `0` Erfolg, `1` Fehler im Assembler-Quelltext, `2` falsche Aufrufparameter oder
Ein-/Ausgabefehler (auch für die noch nicht umgesetzten Befehle `monitor` und `serve`).

Adressen und Zahlen dürfen in allen Schreibweisen des Assemblers angegeben werden:
`4096`, `$1000`, `0x1000`, `%1000000000000`.

## `asm` – Assembler

| Option            | Bedeutung                                                                      |
|-------------------|--------------------------------------------------------------------------------|
| `-o <Datei>`      | Raw-Binary schreiben (ab der niedrigsten Adresse; Lücken zwischen `ORG`-Segmenten mit `00`) |
| `--srec <Datei>`  | Motorola S-Records schreiben                                                   |
| `-l <Datei\|->`   | Listing schreiben (`-` = Standardausgabe)                                      |
| `-s <Datei\|->`   | Symboltabelle schreiben                                                        |
| `--org <Adresse>` | Startadresse vor dem ersten `ORG` (Standard `0`)                               |

Ohne `-o` und `--srec` entsteht `<quelle>.bin`. Bei Fehlern im Quelltext werden **keine** Binär- oder
S-Record-Dateien geschrieben (Listing und Symboltabelle schon, das hilft bei der Fehlersuche). Alle
Fehler eines Laufs erscheinen auf `stderr`:

```
examples/hello.s:17: error: unknown instruction or directive 'MOVEE'
```

## `dis` – Disassembler

| Option            | Bedeutung                                                                      |
|-------------------|--------------------------------------------------------------------------------|
| `--org <Adresse>` | Ladeadresse einer Raw-Binärdatei (Standard `0`); S-Record-Dateien tragen ihre Adressen selbst |
| `--start <Adr.>`, `--end <Adr.>` | Bereich, der ausgegeben wird (Standard: das ganze Abbild; `--end` ist exklusiv) |
| `-e <Adresse>`    | zusätzlicher Einstiegspunkt der Ablaufanalyse (mehrfach möglich)               |
| `-c <Datei>`      | Kommentardatei ([formats.md](formats.md)); kommentierte Adressen sind Einstiegspunkte |
| `--linear`        | der Reihe nach dekodieren statt dem Programmfluss zu folgen                     |
| `--asm`           | wieder assemblierbaren Quelltext schreiben (`ORG`, Labels, keine Adressen/Bytes) |
| `--no-bytes`      | Listing ohne die Befehlswörter                                                 |
| `--style <s>`     | Zahlenschreibweise: `dollar` (`$1F`, Standard), `c` (`0x1F`), `h` (`1FH`)       |
| `-o <Datei>`      | in eine Datei statt auf die Standardausgabe schreiben                          |

**Ablaufanalyse (Standard):** Ab den Einstiegspunkten (Standard: Anfang des Abbilds bzw. Startadresse
des S-Record) folgt der Disassembler `BSR`, `JSR`, `BRA`, `Bcc`, `DBcc` und `JMP` mit bekanntem Ziel,
unterscheidet so Code von Daten und gibt Nicht-Code als `DC.B` aus. Sprungziele im Bereich bekommen
Labels (`L_XXXXXX`) und im Listing eine Liste der Aufrufer (`; <- …`). Mit `--linear` wird dagegen alles
von `--start` an dekodiert; unerlaubte Wörter erscheinen als `DC.W`.

Ein Beispiel:

```console
$ m68ktool asm examples/hello.s -o hello.bin
examples/hello.s: 34 bytes, $001000, 0 error(s)

$ m68ktool dis hello.bin --org 0x1000 -c examples/hello.comments.txt --no-bytes
$ m68ktool dis hello.bin --org 0x1000 --asm -o re.s && m68ktool asm re.s -o re.bin && cmp hello.bin re.bin
```
