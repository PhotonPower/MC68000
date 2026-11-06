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
- Einstiegspunkte müssen an geraden Adressen liegen (Befehle stehen nie an ungeraden); ungerade werden von der Ablaufanalyse ignoriert.
- Kommentare an Adressen mitten in einem Befehl werden nicht angezeigt.

## Raw-Binary

Bytes von der niedrigsten bis zur höchsten Adresse; Lücken zwischen `ORG`-Segmenten werden mit `00`
gefüllt. Beim Laden (`m68ktool dis`) wird die Ladeadresse mit `--org` angegeben.

## Motorola S-Record

Textformat, eine Zeile pro Satz: `S<typ><Anzahl><Adresse><Daten><Prüfsumme>` (alles hexadezimal; die
Anzahl zählt Adresse, Daten und Prüfsumme; die Prüfsumme ist das Einerkomplement der Summe aller Bytes
von Anzahl bis Daten).

- **Schreiben** (`--srec`): `S0`-Kopfzeile, `S2`-Datensätze (24-Bit-Adressen, höchstens 32 Datenbytes),
  `S8` als Abschluss mit der niedrigsten Adresse als Startadresse.
- **Lesen** (Dateiendung `.srec`, `.s19`, `.s28`, `.s37`, `.mot`): `S0` (ignoriert), `S1`/`S2`/`S3`
  (Daten), `S5`/`S6` (ignoriert), `S7`/`S8`/`S9` (Startadresse). Prüfsummen und Längen werden
  kontrolliert; Fehler nennen die Zeilennummer. Aneinandergrenzende Datensätze werden zu einem Segment
  zusammengefasst.

## Amiga-Hunk (Eingabe und Ausgabe, geplant)

Ausführbare Amiga-Dateien bestehen aus Blöcken (`HUNK_HEADER`, `HUNK_CODE`, `HUNK_DATA`, `HUNK_BSS`,
`HUNK_RELOC32`, `HUNK_SYMBOL`, `HUNK_END`). Der Lader legt die Segmente an eine Basisadresse und wendet die
Relokationen an; Symbole aus `HUNK_SYMBOL` können als Namen im Disassembler dienen. Details in
[amiga500.md](amiga500.md).

## Systemabbilder

Kickstart-ROM-Abbilder (256 KiB, vom Benutzer bereitgestellt) und andere ROM-Abbilder werden nur als Raw-Binary geladen (z. B. an eine
Basisadresse) und gehören nicht ins Repository. Snapshot-Dateien des Emulators bekommen ein eigenes,
dokumentiertes Format (siehe [emulator.md](emulator.md)).
