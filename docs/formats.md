# Dateiformate

> Alle hier beschriebenen Formate sind umgesetzt: Kommentardatei, Raw-Binary, S-Records, Amiga-Hunk (Lesen und Schreiben), Snapshot-Datei, Projektdatei.

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

- Der Lader schützt sich vor beschädigten Dateien: Die im Kopf angekündigten Größen dürfen zusammen den 16-MiB-Adressraum nicht überschreiten (geprüft, **bevor** Speicher belegt wird), Blöcke und Verschiebungen
  dürfen nicht über die Datei oder den Hunk hinausreichen, höchstens 4096 Hunks; jeder Verstoß ist ein `IoError`.
- unterstützte Blöcke: `HUNK_HEADER`, `HUNK_CODE`, `HUNK_DATA`, `HUNK_BSS`, `HUNK_RELOC32`, `HUNK_SYMBOL`,
  `HUNK_DEBUG` und `HUNK_NAME` (übersprungen), `HUNK_END`; die Speicherattribute (Chip/Fast) in den oberen
  Bits der Größen werden ignoriert
- die Hunks werden ab `--org` (Standard 0) **nacheinander** abgelegt, jeweils auf 4 Byte ausgerichtet; jede
  `RELOC32`-Stelle bekommt die Basisadresse ihres Ziel-Hunks addiert (BSS wird mit Nullen gefüllt)
- Einstiegspunkt ist der Anfang des ersten CODE-Hunks; `HUNK_SYMBOL`-Namen werden Labelnamen (ungültige Zeichen
  werden zu `_`, Namen wie `D0` bekommen ein `_` vorn, doppelte Namen eine Adresssuffix)
- alles andere (Overlays, Bibliotheken, andere Relokationsarten, falsche Längen, abgeschnittene Dateien) wird mit
  Fehlermeldung abgelehnt, nie außerhalb der Datei gelesen

Die Hunk-Verarbeitung ist mit synthetischen Dateien getestet (`tests/test_hunk.cpp`); ein echtes Amiga-Programm
war dafür nicht verfügbar.

## Amiga-Hunk (Ausgabe)

`m68ktool asm quelle.s --hunk programm [--hunk-symbols]` schreibt ein Programm mit `SECTION`-Zeilen ([assembler-syntax.md](assembler-syntax.md#sektionen-und-hunk-ausgabe-section))
als AmigaDOS-Ladedatei (`m68k::writeHunkFile`):

- `HUNK_HEADER` (keine residenten Bibliotheken, Hunk-Tabelle `0 … n-1`); die Größen in Langwörtern tragen die **Speicherart** in Bit 30 (`_C`, Chip) bzw. 31 (`_F`, Fast)
- je Sektion `HUNK_CODE`, `HUNK_DATA` oder `HUNK_BSS` (Länge in Langwörtern, Inhalt auf 4 Byte aufgefüllt), dann `HUNK_RELOC32` – **nach Ziel-Hunk gruppiert**, Offsets aufsteigend –,
  wenn die Sektion Relokationen hat, `HUNK_SYMBOL` mit den `XDEF`-Labels (bzw. allen mit `--hunk-symbols`), `HUNK_END`
- keine `HUNK_NAME`/`HUNK_DEBUG`-Blöcke, keine Overlays, keine Objektdateien (`HUNK_UNIT`), keine Bibliotheken

Die Ausgabe ist **nicht an einem echten Amiga erprobt**. Gegengeprüft wird sie auf drei Wegen: byteweise gegen den Aufbau aus der Hunk-Beschreibung (Hand-Erwartung), im Rundlauf durch
den eigenen Lader und den Emulator (das Programm rechnet mit den relokierten Zeigern) und mit 400 **Zufallsprogrammen**, die einmal als Hunk (geladen an `$20000`) und einmal absolut
mit denselben Adressen assembliert werden – die Speicherabbilder müssen übereinstimmen (`tests/test_hunk_write.cpp`).

## Amiga-Diskettenabbilder (ADF)

Gelesen, nicht geschrieben (`m68k::AdfImage`, `m68ktool adf`): 880 KiB (DD) und 1760 KiB (HD) mit 512-Byte-Blöcken. Bootblock `DOS\0`…`DOS\7` (Bit 0: FFS, Bit 1: internationale Namen, Bit 2: Verzeichnis-Cache), Wurzelblock bei 880 (HD: 1760) mit der Hash-Tabelle (72 Einträge, Hash über den Namen in Großbuchstaben),
Dateikopf-, Erweiterungs- und Datenblöcke (OFS: 24 Byte Kopf je Datenblock; FFS: reine Daten), Prüfsummen. Namen werden ohne Beachtung der Groß-/Kleinschreibung verglichen. Beschädigte Platten melden `problems()`, das Lesen läuft weiter, soweit es geht; harte Fehler sind `IoError`.
Disketten mit eigenem Lader (kein `DOS`-Bootblock) werden akzeptiert: `isDos()` ist dann falsch, Bootblock und Blöcke sind lesbar. Geprüft gegen `xdftool` (`tools/crosscheck_adf.py`, in der CI).

## Systemabbilder

Kickstart-ROM-Abbilder (256 KiB, vom Benutzer bereitgestellt) und andere ROM-Abbilder werden nur als Raw-Binary geladen (z. B. an eine
Basisadresse) und gehören nicht ins Repository. Snapshot-Dateien des Emulators bekommen ein eigenes,
dokumentiertes Format (siehe [emulator.md](emulator.md)).
