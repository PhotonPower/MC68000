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

## Amiga-Hunk (Eingabe)

`m68ktool dis` erkennt AmigaDOS-Programme am Kennwort `$000003F3` und lädt sie (`m68k/hunk.hpp`):

- unterstützte Blöcke: `HUNK_HEADER`, `HUNK_CODE`, `HUNK_DATA`, `HUNK_BSS`, `HUNK_RELOC32`, `HUNK_SYMBOL`,
  `HUNK_DEBUG` und `HUNK_NAME` (übersprungen), `HUNK_END`; die Speicherattribute (Chip/Fast) in den oberen
  Bits der Größen werden ignoriert
- die Hunks werden ab `--org` (Standard 0) **nacheinander** abgelegt, jeweils auf 4 Byte ausgerichtet; jede
  `RELOC32`-Stelle bekommt die Basisadresse ihres Ziel-Hunks addiert (BSS wird mit Nullen gefüllt)
- Einstiegspunkt ist der Anfang des ersten CODE-Hunks; `HUNK_SYMBOL`-Namen werden Labelnamen (ungültige Zeichen
  werden zu `_`, Namen wie `D0` bekommen ein `_` vorn, doppelte Namen eine Adresssuffix)
- alles andere (Overlays, Bibliotheken, andere Relokationsarten, falsche Längen, abgeschnittene Dateien) wird mit
  Fehlermeldung abgelehnt, nie außerhalb der Datei gelesen
- **nicht umgesetzt:** Hunk-*Ausgabe* des Assemblers (`SECTION`, `XDEF`, Relokationen)

Die Hunk-Verarbeitung ist mit synthetischen Dateien getestet (`tests/test_hunk.cpp`); ein echtes Amiga-Programm
war dafür nicht verfügbar.

## Systemabbilder

Kickstart-ROM-Abbilder (256 KiB, vom Benutzer bereitgestellt) und andere ROM-Abbilder werden nur als Raw-Binary geladen (z. B. an eine
Basisadresse) und gehören nicht ins Repository. Snapshot-Dateien des Emulators bekommen ein eigenes,
dokumentiertes Format (siehe [emulator.md](emulator.md)).
