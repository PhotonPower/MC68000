# Selbstentpackende Programme untersuchen

Viele Amiga-Programme sind **gepackt**: Der Anfang ist ein kleines Entpackprogramm (der *Decruncher*), dahinter liegen die gepackten Daten. Beim Start entpackt es das eigentliche Programm in den Speicher und springt hinein.
Untersucht werden soll das **entpackte** Programm; dessen Listing ist das Ziel. Dieses Dokument beschreibt den Weg von einer Diskette dahin. Der Entpacker selbst wird dabei **nicht verstanden oder nachgebaut**: Er läuft einfach im Emulator.

## 1. Das Programm von der Diskette holen

Eine Diskette (`.adf`, 880 KiB) enthält entweder ein **AmigaDOS-Dateisystem** (OFS oder FFS) oder einen eigenen Lader (Spiele und Demos, die ihre Spuren selbst lesen).

```sh
m68ktool adf info  test.adf              # Bootblock, Dateisystem, Name der Diskette
m68ktool adf ls    test.adf -R           # alle Dateien (mit Unterverzeichnissen)
m68ktool adf extract test.adf 1 -o prog  # die Datei "1" herauskopieren (Groß-/Kleinschreibung egal)
m68ktool adf boot  test.adf -o boot.bin  # die beiden Bootblöcke (1024 Byte)
m68ktool adf block test.adf 880          # ein beliebiger Block (Hexdump)
```

Der Leser ist gegen `xdftool` aus *amitools* geprüft (`tools/crosscheck_adf.py`: Zufallsdisketten, OFS und FFS, Dateien mit Erweiterungsblöcken, Unterverzeichnisse).
Eine Diskette **ohne** Dateisystem (`adf info` sagt „no AmigaDOS file system“) hat einen eigenen Lader im Bootblock: Dann steht das Programm nicht in einer Datei, sondern auf bestimmten Spuren, und der Bootblock-Code sagt, welche. Dafür gibt es `adf boot` und `adf block`; Spuren lesen kann der Emulator nicht.

## 2. Bis zum entpackten Programm laufen lassen

```sh
m68ktool serve --allow-files
{"cmd":"machine","type":"amiga500","os":true}
{"cmd":"load_file","file":"prog","type":"hunk"}
{"cmd":"run_until_unpacked"}
```

`run_until_unpacked` verfolgt alle Speicherstellen, die das **Programm schreibt**, und hält an, sobald ein Befehl aus **geschriebenem Speicher** geholt werden soll, sofern der zusammenhängende geschriebene Bereich um ihn mindestens `min_written` Byte groß ist (Standard 4096).
Das ist das Merkmal eines Entpackers: Er schreibt das Programm und springt hinein. Die Antwort enthält `stop: "unpacked"`, den PC (meist der **Einsprung des entpackten Programms**) und `region` (der geschriebene Bereich, in dem der PC liegt).

- **Mehrstufige Packer** kopieren zuerst ihren Entpacker an eine andere Stelle und laufen von dort. Dieser kleine Bereich zählt wegen der Schwelle nicht; ist das entpackte Programm kleiner als 4096 Byte, stellt man `min_written` kleiner ein (z. B. 256) und ruft
  `{"cmd":"run_until_unpacked","min_written":256,"continue":true}` auf, um den nächsten Fund zu bekommen: `continue` überspringt den zuletzt gefundenen Bereich.
- `ignore:[[von,bis],…]` nimmt weitere Bereiche aus. Das Gerüst des emulierten Systems (Bibliotheksbasen, Sprungtabellen) zählt nie.
- `max_instructions` (Standard 50 Millionen, höchstens 100 Millionen je Aufruf): Ein Entpacker braucht grob 30 bis 100 Befehle je entpacktes Byte.
- `written` zeigt die größten geschriebenen Bereiche jederzeit, `track_writes` schaltet die Verfolgung ein oder aus.

## 3. Den entpackten Speicher sichern und analysieren

```sh
{"cmd":"save_memory","addr":"0x30000","len":4234,"file":"unpacked.bin"}
```

Adresse und Länge stehen in der Antwort von `run_until_unpacked` (`region`). Dann wird der Speicherauszug **an seiner Adresse** als Rohabbild geladen, mit dem Einsprung als Einstiegspunkt:

```sh
python3 agent/m68k_drive.py --work arbeit init --bin unpacked.bin --org 0x30000 --entry 0x30000
python3 agent/m68k_drive.py --work arbeit status
```

Ab hier ist es ein gewöhnliches Programm (siehe [walkthrough.md](walkthrough.md) und [reverse-engineering.md](reverse-engineering.md)): Routinen, Datenbereiche, Sprungtabellen, Läufe im Emulator. Das Ergebnis ist ein neu assemblierbares Listing mit Kommentaren. Weil das entpackte Programm
keine Hunk-Struktur mehr hat, fehlen Relokationen: Es muss an der Adresse laufen, an der es entpackt wurde (bei Programmen, die sich selbst verschieben, ist das die Adresse, an der es steht, wenn der Einsprung erreicht ist).

## Grenzen

- **Kein Kopierschutz, keine Spurlader:** Ein Programm, das beim Start Spuren der Diskette liest (Trackloader), läuft hier nicht: Der Emulator kennt keine Laufwerke.
- **Hardware:** Es gibt keinen Blitter, Copper und keinen Ton. Entpacker, die auf Zeilen des Elektronenstrahls warten (`VHPOSR`), kommen durch: Jedes Lesen von `VPOSR`/`VHPOSR` rückt den Strahl weiter (alle 8 Lesezugriffe eine Zeile, 312 Zeilen je Bild).
  Wer auf Unterbrechungen oder CIA-Zeitgeber wartet, hängt.
- **Packer, die sich selbst prüfen** (Prüfsumme über den Entpacker, Prüfung des Prozessors, `Supervisor`-Aufrufe) können scheitern; dann zeigt `run_until_unpacked` das mit `stop` (`limit`, `halted`, Ausnahmen), und der Befehl `libcalls` zeigt, welche Systemaufrufe nicht nachgebildet sind.
- **Der Fund ist eine Heuristik:** „Code aus geschriebenem Speicher“ trifft auch Programme, die Code zur Laufzeit erzeugen. Der Einsprung des entpackten Programms ist meist der erste Fund mit einem großen Bereich; im Zweifel `written` ansehen und den Bereich mit dem größten Umfang nehmen.
