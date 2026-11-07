# Tests

```sh
ctest --test-dir build --output-on-failure
```

Die Tests brauchen kein externes Framework; `tests/test_util.hpp` stellt `CHECK`/`CHECK_EQ` bereit.

## Suites

| Suite        | Inhalt |
|--------------|--------|
| `format`     | Hexadezimalausgabe in allen Zahlenstilen, Parsen der Stilnamen |
| `memory`     | Big-Endian-Zugriffe, 24-Bit-Maskierung, Umlauf bei 16 MiB, seitenweise Belegung, `load`/`readBytes`, Kopieren, Ausrichtung |
| `expression` | alle Zahlenschreibweisen, Operatoren und Vorrang, `*`, Symbole, undefinierte Symbole, Fehlerfälle |
| `decoder`    | über 50 gültige Befehle mit Bytes → Text/Länge, Sprungziele und Flussart, Symbolizer, Zahlenstile, unerlaubte Wörter (Line A/F, 68010+, ungerade Sprungziele, unerlaubte Adressierungsarten, nicht-kanonische Erweiterungswörter), Umlauf bei 16 MiB |
| `assembler`  | **111 Zeilen mit Bytes, die GNU `as` erzeugt hat** (`tests/gnu_verified.inc`), Konventionen (Standardgröße, `SP`, `MOVE`→`MOVEA` usw.), Labels, Vorwärtsreferenzen, Größenwahl und Wachstum von Sprüngen, lokale Labels, `ORG`/`EVEN`/`END`/`DC`/`DS`, Listing, Symboltabelle, über 30 Fehlerfälle, Robustheit gegen 7000 zufällige Quelltexte |
| `roundtrip`  | **exhaustiv:** alle 65536 ersten Opcode-Wörter mit 6 Sätzen von Erweiterungswörtern (rund 248 000 gültige Befehle); was der Dekoder als Befehl erkennt, muss der vollständige Assembler zu denselben Bytes zurückübersetzen |
| `image_io`   | S-Record-Lesen/-Schreiben, Prüfsummen und Fehler mit Zeilennummer, Dateizugriff |
| `hunk`       | Amiga-Hunk-Lader: Platzierung und Ausrichtung, Relokation, BSS, Symbole, Speicherflags in Größen, Namen für den Assembler, **jede Abkürzung der Datei** und weitere beschädigte Dateien werden abgelehnt |
| `disassembler` | Ablaufanalyse (Code/Daten), zweiter Einstiegspunkt, überlappender Code, Symbole, Kommentare, Listing-Optionen, linearer Modus, Befehle über das Bereichsende, **Byte-genauer Roundtrip `assemble(--asm) == Speicher`** für 300 zufällige Speicher (linear) und 150 (Ablaufanalyse mit zufälligen Einstiegspunkten) |
| `cpu`        | Emulator (rund 52 000 Prüfungen): Arithmetik und Flags mit von Hand gerechneten Werten, Schiebebefehle (Zähler über der Operandengröße, 0 und 64), BCD, Mul/Div, Bitbefehle, Adressierungsarten, `MOVEM`/`LINK`/`MOVEP`, Sprünge und Schleifen, alle Ausnahmen mit Rahmeninhalt (TRAP, Privileg, illegal, Line A, CHK, TRAPV, Division durch 0, Adressfehler, Doppelfehler), Stapelwechsel, Interrupts, `STOP`, Trace, Breakpoints/Watchpoints/`runUntil`/`stepOver`/`call`, Hooks, **Abgleich mit dem Dekoder für alle 65536 Opcodes** |
| `session`    | JSON-Baustein; alle Befehle des Agenten-Protokolls (asm/dis/read/write, Ausdrücke und Marken als Argumente, Fehler ohne Absturz, Breakpoints/Watchpoints/`step`/`call`/`run_until_return`, Ausnahmen-Zähler); **Checkpoint → Lauf → Rollback → Lauf liefert identische Antworten**; Dateizugriff nur mit `--allow-files` (Raw, S-Record, Hunk-Programm mit Relokation und Symbolen, Snapshot-Datei); Amiga-500-Maschine (Geräteprotokoll mit Registernamen, ROM/unmapped nicht schreibbar, Konfigurationen); **ROM-Overlay**, von einem ROM-Programm wie vom Kickstart ausgeschaltet; Snapshot-Serialisierung (jede Abkürzung wird abgelehnt) und Speicher-Diff |
| `reverse`    | Annotationen (Namensregeln, Umbenennen, Entfernen, Projektdatei-Roundtrip, fehlerhafte Dateien); Codefluss-Analyse (Routinen, Aufrufer/Aufgerufene, Tiefe, Zyklen, Bibliotheksaufrufe, externe Aufrufe, Datenbarriere, linear); Listing (Namen, Köpfe, Variablen, alle Datenarten, EQU für Namen mitten im Befehl, Abschneiden); **150 zufällige Programme mit zufälligen Annotationen: das Listing ist jedes Mal verifiziert** (bei der Entwicklung 20 000); die Protokollbefehle `analyze`/`routines`/`listing`/`ann_*` inklusive Ablehnungen und Projektdateien |
| `monitor`    | Zahlenschreibweisen (hex, `100.`, Präfixe, Ausdrücke, Symbole mit Vorrang, `@Register`), Dump/Füllen/Vergleichen/Kopieren (überlappend)/Suchen, Assemble-Modus (Labels, Vorwärtsreferenz-Hinweis, ungerade Adresse, Fehler), Register, `n`/`o`/`u`/`g`/`ru`, Haltepunkte, Watchpoints, Limit, Ausnahmen und Interrupts im Lauf, Symbole und Symboldateien, Dateibefehle (Quelltext, Raw, S-Record, Hunk, Speicherbereich, Snapshots), `--no-files`, Amiga-Speicherkarte mit `io`, Bereichsgrenzen, **5000 zufällige Befehlszeilen ohne Absturz** |
| `agent` (Python) | Orchestrator `agent/m68k_annotate.py` gegen einen **scripteten Mock-Ollama** und den echten `m68ktool serve`: kompletter Lauf (Routinen von unten nach oben, Aufgerufene mit echten Namen im Auftrag, abgewiesener Name als lesbare Fehlermeldung, `run_routine`-Bericht mit Speicher/Hardware-Schreibzugriffen, Projektdatei, **verifiziertes** Listing), Fortsetzen, Tool-Aufrufe als Text/JSON-String/mit falschen Typen, Vollständigkeitsprüfung, `finish_routine` vor `document_routine`, von Ollama nicht lesbare Aufrufe, abgeschnittene Antworten, Modell ohne Thinking, ausfallende Routine, Konfigurationsfehler (kein Tool-Support, Modell nicht gefunden, Server nicht erreichbar), Mehrdurchgang-Verfahren mit Bericht, Hilfsfunktionen |
| `os`         | OS-Emulation: ein Programm, das `dos.library` öffnet, „Hello“ schreibt, Speicher belegt/freigibt und zurückkehrt (ExecBase bei Adresse 4, `JMP`-Sprungtabelle, Konsole, `libcalls`, Exit-Stub); First-Fit-Heap (Lücken, Rundung, AllocVec/FreeVec, Speicher voll, Größe 0, unbekannter Block); nicht emulierte Aufrufe (unbekannte Funktion, bekannter Name ohne Emulation) werden protokolliert und liefern 0; `Open` → `IoErr` 205; `Exit`; gleiche Bibliothek → gleiche Basis, höchstens sieben; Checkpoints und Rollback; Konfiguration; **Listenfunktionen** (aufbauen, `RemHead`/`Remove`/`RemTail`/`Insert`, `Enqueue` nach Priorität, `FindName`, leere Liste, Vorwärts-/Rückwärtsverkettung nachgeprüft), `CopyMem` mit überlappenden Blöcken, `AvailMem` |
| `hunk_write` | Sektionsprogramme: Sektionen/Relokationen/Symbole, **exakte Bytes** einer kleinen Hunk-Datei, Rundlauf Assembler → Hunk → Lader → Emulator an drei Basisadressen, Ausdrücke (Differenz innerhalb einer Sektion, `label+n`), Fortsetzen von Sektionen, BSS, Speicherarten, **alle abgelehnten Konstrukte** mit ihrer Meldung, und 400 **Zufallsprogramme**, die als Hunk (geladen) und absolut mit denselben Adressen übersetzt werden und dasselbe Speicherabbild ergeben (ein absichtlich falscher Relokationsoffset wird davon erkannt) |
| `optimize`   | Assembler-Optimierungen (`-O`): alle Formen mit ihren Grenzen (1…8, −128…127, 16 Bit), ausdrücklich geschriebene Formen bleiben, Vorwärtsverweise und Labels konvergieren, **6000 zufällige Befehle** mit und ohne Optimierung assembliert und aus zufälligen Zuständen ausgeführt (Register, SR und Speicher identisch, nie länger), ungültige Befehle bleiben ungültig; dazu 12 Fälle gegen GNU `as` in `tools/crosscheck_gas.py` |
| `jumptable`  | Sprungtabellen: Branch-, Offset- und Zeigertabellen mit Einsprung, Zielen, Tabellengröße und Datenbytes; markierte Zeigerbereiche werden verfolgt; Listing (verifiziert, Kommentare, `DC.W`-Zeilen mit Zielen); Session (`analyze`, `jump_tables`, `ann_data`, erneut analysieren); **300 zufällige Programme** (richtige Art, richtige Zahl und Ziele der Einträge, Listing verifiziert); Gegenbeispiele (ein Eintrag, kein Muster, Ziele außerhalb, Nullwörter); **400 Zufallsbilder** mit eingestreuten indizierten Sprüngen (kein Absturz, Listing immer verifiziert) |
| `datatypes`  | Datentyp-Erkennung: Strings (referenziert, NUL-terminiert, lang), Füll-NUL, Tabelle von String-Zeigern mit kurzen Strings, Nullbereiche; Gegenbeispiele (`NuNuNuNu`, Satzzeichen, zu kurz, Code, zwei kurze Strings); Session (`data_regions`, `apply`, Listing mit `DC.B "…",0`/`EVEN`/`DC.L`); **300 zufällige Programme** mit Strings (alle gefunden, Listing verifiziert); **Einzelvariablen** (Größe aus den Zugriffen; Gegenfälle: Bytes, gemischte Größen, `LEA`, Verweis in die Mitte, außerhalb des Programms; Zugriff in die Mitte einer Tabelle schneidet sie ab; **200 Zufallsprogramme** mit exakter Größe je Variable und verifiziertem Listing), **Zahlentabellen** (Wörter über `(An)+`, Langwörter über `(An,Dn)`, Adresse als Direktwert; Gegenbeispiele: gemischte Größen, nur feste Verschiebungen, ein Eintrag, nur Nullen, Bytes, nie durchlaufen, Register neu geladen, Aufruf dazwischen; Ende vor der nächsten Referenz; Textkopie mit `.L` bleibt Text; **200 Zufallstabellen** mit exakter Art und Länge), **400 Zufallsbilder** mit ASCII-Anteil (keine Überlappung, Listing verifiziert) und Zufallsbytes (höchstens 8 falsche Texte in 400 Bildern) |
| `agent` (Beispiel) | `tests/agent/test_example.py`: das Beispielprogramm `examples/amiga/hello_amiga.s` durch die ganze Kette: Assembler → Hunk (mit und ohne Symbole) → Lader → Emulator mit OS-Aufrufen (Ausgabe `Hello…/sum=150/words=5`, Aufrufe in der richtigen Reihenfolge, alle emuliert) → Analyse (7 Routinen, Offset-Sprungtabelle, 4 Strings, verifiziertes Listing) → **Dry-Run** des Orchestrators (kein Modellaufruf, Ausgabe, Warnung bei kleinem Kontext) |
| `robustness` | Eingaben von außen: **JSON-Parser** (40 000 Token-Mischungen, Rundlauf Parser → Writer → Parser, 100 000-fach verschachtelt wird abgelehnt statt den Stapel zu sprengen); **Server** (12 000 zufällige Befehle mit zufälligen Argumenten **jedes JSON-Typs**, in einer langlebigen Session, Antwort immer ein JSON-Objekt mit `ok`, bei Fehlern ein `error`-Text, jeder Aufruf unter einer Zeitgrenze; kein Argument kann den Server hängen: `max_instructions`/`n` über 100 Mio. sind Fehler); **Hunk-Lader**, **S-Records**, **Snapshot-Dateien** und **Projektdateien** mit je 6000–12 000 Mutationen gültiger Dateien (Bitkipper, Abschneiden, Extremwerte, Einfügen, Anhängen): nur die dokumentierten Fehler, nie ein Absturz oder `bad_alloc`. Unter den Sanitizern läuft ein Zehntel der Durchläufe (`M68K_FUZZ_SCALE`); `M68K_FUZZ_SEED=<n>` wählt eine andere Zufallsfolge |
| `cli_*`      | `m68ktool version`; Beispielprogramm assemblieren → disassemblieren (`--asm`) → erneut assemblieren → Dateien identisch |

Der Roundtrip-Test beweist die Konsistenz von Dekoder und Assembler **untereinander**. Gegen
gemeinsame Denkfehler schützen die Kreuzprüfungen mit unabhängigen Werkzeugen.

## Kreuzprüfung gegen GNU binutils

```sh
sudo apt install binutils-m68k-linux-gnu
cmake -S . -B build -DM68K_BUILD_TOOLS=ON && cmake --build build
python3 tools/crosscheck_objdump.py build/m68k_dump_forms 0 3
python3 tools/crosscheck_gas.py build/m68ktool
```

**Dekoder gegen `objdump`** (`crosscheck_objdump.py`): Für alle 65536 Opcode-Wörter, jeweils mit festen
Erweiterungswörtern, werden Gültigkeit, Länge, Mnemonic **und Operanden** (Register, Verschiebungen,
absolute und PC-relative Ziele, Unmittelbares, Registerlisten – beide Schreibweisen in eine gemeinsame
Form gebracht) verglichen. Ergebnis: keine ungewollten Abweichungen. Bewusste Unterschiede, die das Skript
getrennt zählt:

| Unterschied | Grund |
|-------------|-------|
| Line F (`$Fxxx`) | `objdump` dekodiert Koprozessor-/MMU-Befehle (68020+); für den 68000 sind es unerlaubte Befehle |
| Sprünge auf ungerade Adressen | `objdump` zeigt sie an; hier werden sie als `DC.W` ausgegeben (der 68000 löste einen Adressfehler aus) |
| `ADDQ.B`/`SUBQ.B` auf `An` | `objdump` akzeptiert es; auf dem 68000 ist es unerlaubt |
| `$4AFD` | Pseudobefehl `swbeg` von `objdump` |

**Assembler gegen GNU `as`** (`crosscheck_gas.py`): 125 Fälle (Befehle und kleine Programme mit Labels,
Sprüngen, PC-relativer Adressierung, Daten) werden in Motorola-Syntax mit `m68ktool` und in GNU-Syntax mit
`as` übersetzt; die Bytes müssen gleich sein. Aufgeführt sind nur Schreibweisen, die beide gleich
übersetzen (siehe [assembler-syntax.md](assembler-syntax.md#wahl-der-befehlsform)).

Die CI führt beide Prüfungen aus.

## Emulator gegen Musashi

```sh
tools/run_diff_musashi.sh 4 1 20000  # Versuche je Opcode, Seed, Zahl der Folgen-Programme (holt und baut Musashi in einem Temp-Verzeichnis)
```

Das Skript führt zwei Tests aus: **Einzelbefehle** (alle 65536 Opcodes) und **Befehlsfolgen** (zufällige Programme aus 2–10 Befehlen, Vergleich nach jedem Befehl); Musashi wird mit
**Trace- und Adressfehler-Emulation** gebaut, sodass auch Trace-Ausnahmen und der 14-Byte-Rahmen eines Adressfehlers verglichen werden; einzelne Folgen lassen sich mit `m68k_diff_musashi_seq <programme> <seed> <nummer>` Schritt für Schritt ausgeben.

Details und Ergebnis siehe [emulator.md](emulator.md#wie-der-kern-geprüft-ist): alle 65536 Opcodes, zufällige Zustände,
Vergleich aller Register und jedes geschriebenen Bytes. Die CI führt einen Lauf mit 10 Versuchen je Opcode aus
(rund 600 000 Fälle) und 20 000 Folgen-Programme. Noch nicht eingesetzt sind öffentliche Einzelbefehls-Testsätze.
