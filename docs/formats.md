# Dateiformate

> **Geplant**, soweit nicht anders vermerkt.

## Kommentardatei (Disassembler, `-c`)

Textdatei, eine Zeile pro Kommentar (Beispiel: `examples/hello.comments.txt`):

```
; Zeilen mit Semikolon und Leerzeilen werden ignoriert
$1000,"Einsprung: Hauptprogramm"
0x1004,Kommentar ohne Anführungszeichen geht auch
```

- Adresse dezimal, `$hex` oder `0xhex`. Eine Adresse wie `1B32` ohne Präfix ist *dezimal*.
- Der Text steht in Anführungszeichen (`""` ergibt ein Anführungszeichen) oder bis Zeilenende.
- Mehrere Kommentare zur selben Adresse werden mit ` / ` zusammengefügt.
- Kommentaradressen sind zusätzliche Einstiegspunkte der Ablaufanalyse.
- Adressen müssen gerade sein (Befehle liegen immer an geraden Adressen).

## Raw-Binary

Bytes von der niedrigsten bis zur höchsten Adresse; Lücken zwischen `ORG`-Segmenten werden mit `00`
gefüllt. Beim Laden wird die Ladeadresse (`--org`) angegeben.

## Motorola S-Record

Textformat mit Zeilen `S0`/`S1`/`S2`/`S3`/`S7`/`S8`/`S9`; für den 24-Bit-Adressraum des 68000
genügen `S2` (24-Bit-Adressen) mit Abschluss `S8`. Jede Zeile hat eine Prüfsumme (Einerkomplement
der Summe aller Bytes nach `S<typ>`).

## Systemabbilder

Kickstart-, TOS- und andere ROM-Abbilder werden nur als Raw-Binary geladen (z. B. an eine
Basisadresse) und gehören nicht ins Repository. Snapshot-Dateien des Emulators bekommen ein eigenes,
dokumentiertes Format (siehe [emulator.md](emulator.md)).
