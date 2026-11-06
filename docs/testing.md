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
tools/run_diff_musashi.sh 4 1        # Versuche je Opcode, Seed (holt und baut Musashi in einem Temp-Verzeichnis)
```

Details und Ergebnis siehe [emulator.md](emulator.md#wie-der-kern-geprüft-ist): alle 65536 Opcodes, zufällige Zustände,
Vergleich aller Register und jedes geschriebenen Bytes. Die CI führt einen Lauf mit 10 Versuchen je Opcode aus
(rund 600 000 Fälle). Noch nicht eingesetzt sind öffentliche Einzelbefehls-Testsätze.
