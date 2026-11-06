# Assembler-Syntax

Motorola-Syntax, **Quelle zuerst, Ziel zuletzt**: `MOVE.L D0,(A0)+`. Die GNU-Syntax (`move.l %d0,%a0@+`)
wird nicht gelesen.

## Zeilenformat

```
[label[:]]  mnemonic[.größe]  [operand[,operand]]  [; kommentar]
```

- Ein Label steht in Spalte 1 (Doppelpunkt optional). Eingerückt ist ein Label nur mit Doppelpunkt
  erlaubt (`  loop: MOVE.W D0,D1`). Eine Zeile darf nur aus einem Label bestehen.
- Kommentare beginnen mit `;`; ein `*` in Spalte 1 macht die ganze Zeile zum Kommentar.
- Mnemonics, Register und Größen sind nicht groß-/kleinschreibungsabhängig, Labels schon.
- Lokale Labels beginnen mit `.` und gelten bis zum nächsten globalen Label (`start.next` in der Symboltabelle).
- `name EQU ausdruck` oder `name = ausdruck` definiert eine Konstante (Vorwärtsreferenzen erlaubt).

## Operanden

| Schreibweise                    | Bedeutung                                              |
|---------------------------------|--------------------------------------------------------|
| `D0`–`D7`, `A0`–`A7`, `SP`      | Register (`SP` = `A7`)                                 |
| `(An)`, `(An)+`, `-(An)`        | indirekt, Postinkrement, Prädekrement                  |
| `d(An)` oder `(d,An)`           | 16-Bit-Verschiebung; bleibt auch bei `d = 0` erhalten  |
| `d(An,Xn.W)` / `(d,An,Xn.L)`    | Index mit 8-Bit-Verschiebung; `.W` ist Standard         |
| `ziel(PC)`, `ziel(PC,Xn.W)`     | PC-relativ – **`ziel` ist die Zieladresse** (siehe unten) |
| `adresse`, `adresse.W`, `adresse.L`, `(adresse).W` | absolut                          |
| `#ausdruck`                     | unmittelbar                                            |
| `CCR`, `SR`, `USP`              | Sonderregister                                         |
| `D0-D3/A0/A2-A4`                | Registerliste (nur `MOVEM`)                            |

**PC-relative Adressen und Sprünge verwenden die Zieladresse**, nicht die Verschiebung:
`LEA message(PC),A0` und `BRA label` rechnen die Verschiebung selbst aus. Das gilt auch für Zahlen
(`$001012(PC)` = Ziel `$001012`). Der Disassembler schreibt dieselbe Form. GNU `as` behandelt
Zahlen vor `(PC)` dagegen als Verschiebung.

## Größen

- **Ohne Größenangabe gilt `.W`** (`MOVE D0,D1` = `MOVE.W D0,D1`).
- Befehle mit fester Größe (`MULU`, `LEA`, `SWAP`, `MOVEQ`, `Scc`, …) schreiben keine Größe; eine
  passende Angabe wird akzeptiert, eine andere ist ein Fehler.
- **Sprünge** `BRA`, `BSR`, `Bcc`: `.S` (8 Bit) oder `.W` (16 Bit). Ohne Angabe wählt der Assembler `.S`,
  wenn die Verschiebung hineinpasst (und nicht 0 ist), sonst `.W`. `DBcc` hat immer 16 Bit.
- **Absolute Adressen** ohne Angabe: `.W`, wenn der Wert in `-$8000…$7FFF` liegt, sonst `.L`.
  (`$8000.W` meint die vorzeichenerweiterte Adresse `$FFFF8000`.)
- Die gewählten Größen wachsen nur, nie schrumpfen sie; der Assembler wiederholt die Durchläufe, bis
  Symbole und Größen sich nicht mehr ändern (höchstens 40). Beim 68000 hängt die Befehlslänge vom
  Wert eines Symbols ab – anders als beim Z80.
- Wertebereiche unmittelbarer Operanden: `.B` −128…255, `.W` −32768…65535, `.L` −2³¹…2³²−1;
  `ADDQ`/`SUBQ`/Schiebezähler 1…8; `MOVEQ` −128…255; `TRAP` 0…15; Bitnummer 0…255.

## Wahl der Befehlsform

Der Name wird zuerst genau gesucht (`ADD`, `ADDA`, `ADDI`, … sind eigene Formen). Passt keine Form
mit genau diesem Namen, probiert der Assembler Verwandte:

| geschrieben | wird zu                                                         |
|-------------|-----------------------------------------------------------------|
| `MOVE …,An` | `MOVEA`                                                         |
| `ADD …,An`  | `ADDA`; `ADD #n,<ea>` (kein Dn als Ziel) → `ADDI`               |
| `SUB`, `CMP`| `SUBA`/`SUBI`, `CMPA`/`CMPI` entsprechend                       |
| `AND`, `OR`, `EOR` mit `#n` ohne Dn-Ziel | `ANDI`, `ORI`, `EORI`              |

Es wird **nichts optimiert**: `ADD.W #1,D0` bleibt `ADD` mit unmittelbarem Operanden (nicht `ADDQ`),
`MOVE.L #5,D0` wird nicht zu `MOVEQ`. Wer `ADDQ`/`MOVEQ` will, schreibt es. (GNU `as` wählt hier
andere Formen; `tools/crosscheck_gas.py` verwendet nur Schreibweisen, die beide gleich übersetzen.)

Aliase: `DBF` = `DBRA`, `BHS` = `BCC`, `BLO` = `BCS`, `SHS`/`SLO`, `DBHS`/`DBLO`.

## Zahlen und Ausdrücke

- Dezimal `123`, hexadezimal `$1F` oder `0x1F`, binär `%1010`, oktal `@17`, Zeichen `'A'` (bis vier Zeichen).
- Operatoren wie in C: `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`; unär `- + ~`; Klammern. Rechnung mit 64 Bit.
- `*` an einer Stelle, an der ein Wert erwartet wird, ist die Adresse des aktuellen Befehls
  (`DBRA D0,*`, `DC.W *-start`).

## Pseudobefehle

| Befehl                        | Wirkung                                                   |
|-------------------------------|-----------------------------------------------------------|
| `ORG adresse`                 | Startadresse setzen (mehrfach erlaubt, jeder `ORG` beginnt ein Segment) |
| `name EQU wert`, `name = wert`| Konstante definieren                                      |
| `DC.B/W/L a,b,"text"`         | Konstanten ablegen; Texte mit `"…"` oder `'…'` (mehr als ein Zeichen) bei `.B` |
| `DS.B/W/L n`                  | `n` Einheiten mit Nullen reservieren                      |
| `EVEN`                        | auf gerade Adresse ausrichten (Füllbyte 0)                |
| `END`                         | Ende des Quelltextes                                      |

Befehle müssen an geraden Adressen liegen (sonst Fehler); `DC.W`/`DC.L` dürfen an ungeraden Adressen stehen.

## Ausgabe und Fehler

Raw-Binary (von der niedrigsten Adresse, Lücken mit `00`), Motorola S-Record, Listing, Symboltabelle
([formats.md](formats.md)). Fehler tragen die Zeilennummer, alle Fehler eines Laufs werden gesammelt;
bei Fehlern entstehen keine Binär- oder S-Record-Dateien. Bei mehreren möglichen Befehlsformen meldet
der Assembler den aussagekräftigsten Fehler (z. B. „quick value must be 1..8“ statt „data register expected“).

## Nicht unterstützt

Makros, bedingte Assemblierung, `INCLUDE`, `SECTION`/`XDEF`/Hunk-Ausgabe, `DCB`, `REG`, Befehle des
MC68010 und höher, Zeichenketten mit `DC.W`/`DC.L`.
