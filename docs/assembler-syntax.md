# Assembler-Syntax

> **Geplant.** Dieses Dokument legt die Syntax fest, die der Assembler (Meilenstein 0.2) umsetzen soll.
> Es ist der Vertrag für Encoder, Tests und `examples/hello.s`.

## Grundformat

Motorola-Syntax, **Quelle zuerst, Ziel zuletzt**: `MOVE.L D0,(A0)+`.

```
[label[:]]  mnemonic[.größe]  [operand[,operand]]  [; kommentar]
```

- Kommentare beginnen mit `;`, ein `*` in Spalte 1 macht die ganze Zeile zum Kommentar.
- Mnemonics, Register und Größen sind nicht groß-/kleinschreibungsabhängig, Labels schon.
- Größen: `.B`, `.W`, `.L`; bei Verzweigungen zusätzlich `.S` (kurz, 8 Bit) – ohne Angabe wählt der
  Assembler die kürzeste passende Form.
- Lokale Labels beginnen mit `.` und gelten bis zum nächsten globalen Label.

## Adressierungsarten

`Dn`, `An`, `(An)`, `(An)+`, `-(An)`, `d(An)`, `d(An,Xn[.W|.L])`, `xxx.W`, `xxx.L`, `d(PC)`,
`d(PC,Xn[.W|.L])`, `#imm` – siehe [instruction-set.md](instruction-set.md). `SP` ist ein Alias für `A7`.
Ohne Größenangabe ist eine absolute Adresse `.W`, wenn sie sich vorzeichenerweitert in 16 Bit
darstellen lässt, sonst `.L`.

## Zahlen und Ausdrücke

- Dezimal `123`, hexadezimal `$1F` oder `0x1F`, binär `%1010`, Zeichen `'A'`.
- Operatoren: `+ - * / % << >> & | ^ ~`, Klammern, `*` bzw. `$` als aktuelle Adresse (Festlegung steht
  noch aus, da `*` auch Multiplikation ist), Symbole aus Labels und `EQU`.

## Pseudobefehle

| Befehl                   | Wirkung                                                   |
|--------------------------|-----------------------------------------------------------|
| `ORG adresse`            | Startadresse setzen (mehrfach erlaubt)                    |
| `name EQU wert`          | Konstante definieren                                      |
| `DC.B/W/L a,b,"text"`    | Konstanten ablegen                                        |
| `DS.B/W/L n`             | Platz reservieren                                         |
| `EVEN`                   | auf gerade Adresse ausrichten (Füllbyte 0)                |
| `END`                    | Ende des Quelltextes                                      |

## Ausgabe und Fehler

Raw-Binary, Motorola S-Record, Listing und Symboltabelle ([formats.md](formats.md)). Fehler tragen
die Zeilennummer; alle Fehler eines Laufs werden gesammelt. Ein Befehl an einer ungeraden Adresse
ist ein Fehler (der 68000 würde einen Adressfehler auslösen).

## Nicht vorgesehen in 0.2

Makros, bedingte Assemblierung, `INCLUDE`, Befehle des MC68010 und höher.
