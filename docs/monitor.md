# Monitor

`m68ktool monitor` ist ein klassischer Maschinensprache-Monitor mit Eingabeschleife: Speicher ansehen und ändern, disassemblieren,
Zeilen assemblieren, Programme im Emulator einzeln oder bis zu einem Haltepunkt ausführen. Befehle und Bedienung folgen dem
[Z80-Monitor](https://github.com/PhotonPower/Z80/blob/main/docs/monitor.md); alles basiert auf den Bausteinen der Bibliothek
(`m68k::Cpu`, Dekoder, Assembler). Die Klasse `m68k::Monitor` hat keine eigene Ein-/Ausgabe (`execute(zeile) → Text`), sodass sie
sich auch direkt testen oder in andere Oberflächen einbauen lässt.

```sh
m68ktool monitor                          # leerer Speicher (generische Maschine, 16 MiB RAM)
m68ktool monitor programm --org 20000     # Raw-Binary (hex), S-Record oder Amiga-Hunk-Programm laden
m68ktool monitor --amiga                  # Speicherkarte eines Amiga 500
m68ktool monitor --no-files               # Dateibefehle sperren
m68ktool monitor < befehle.txt            # Skript: ohne Banner und Prompt
```

Ohne Zeileneditor (kein `readline`-Verlauf); mit `rlwrap m68ktool monitor` bekommt man Pfeiltasten und Verlauf.

## Zahlen

**Argumente sind hexadezimal**, wie bei klassischen Monitoren: `d 1000`, `e 2000 4E 75`.

| Schreibweise | Bedeutung |
|---|---|
| `1000`, `FF`, `BEEF` | hexadezimal (auch wenn mit einem Buchstaben begonnen wird) |
| `100.` | **dezimal** (Punkt dahinter) |
| `$FF`, `0xFF`, `%1010`, `@17` (oktal im Assembler; im Monitor steht `@` für Register) | mit Präfix wie im Assembler |
| `start`, `loop+3` | Symbole und Ausdrücke (`+ - * / % & \| ^ << >>`, Klammern) |
| `@A0`, `@PC`, `@SP`, `@SR`, `@USP`, `@D3` | Registerinhalt |

Ein bekanntes Symbol hat immer Vorrang: gibt es ein Symbol `DEAD`, ist `DEAD` dieses Symbol und nicht die Zahl `$DEAD`
(`sym -dead` entfernt es wieder). Adressen werden auf 24 Bit beschränkt.

**Im Assemble-Modus** (Befehl `a`) gilt die Syntax des Assemblers, nicht die des Monitors: `MOVE.W #10,D0` ist dezimal, hex
schreibt man `#$0A` ([assembler-syntax.md](assembler-syntax.md)).

## Befehle

| Befehl | Wirkung |
|---|---|
| `d [adr [n]]` | disassemblieren (`n` Befehle, Standard 12; ohne Adresse: Fortsetzung). `>` markiert den PC, Marken stehen als `NAME:` davor |
| `m [adr [n]]` | Speicher als Hex + ASCII (`n` Bytes, Standard 128; ohne Adresse: Fortsetzung); `m.w` / `m.l` gruppieren zu Wörtern / Langwörtern |
| `e adr b…` | Bytes schreiben; auch Text: `e 2000 "Hallo" 0`; `e.w` / `e.l` schreiben Wörter / Langwörter (Big-Endian, gerade Adresse) |
| `f von bis b…` | füllen (Muster wird wiederholt); `bis` ist **einschließlich** |
| `c von bis ziel` | vergleichen (zeigt bis zu 32 Unterschiede) |
| `t von bis ziel` | kopieren (überlappend korrekt) |
| `h von bis b…` | suchen: Bytes oder `"Text"` (bis zu 64 Treffer) |
| `a [adr]` | **Zeilen assemblieren**; leere Zeile beendet. Labels (`loop:`) und frühere Symbole sind verwendbar |
| `r [name=wert …]` | Register anzeigen / setzen: `D0`–`D7`, `A0`–`A7`, `SP`, `PC`, `SR`, `CCR`, `USP`, `SSP` (`r pc=1000 d0=@a1 sr=2000`); PC setzen weckt aus `STOP` |
| `n [anzahl]` | Einzelschritt(e); bei 2–20 Schritten wird jeder Befehl gezeigt |
| `o` | Step over: ein `BSR`/`JSR` läuft bis zur Rückkehr |
| `u` | läuft, bis die aktuelle Routine zurückkehrt (`RTS`/`RTR`/`RTE`) |
| `g [adr]` | ab `adr` (oder PC) laufen lassen bis Haltepunkt / Watchpoint / `STOP` / Limit |
| `ru adr` | laufen, bis `PC == adr` |
| `b [adr…]` | Haltepunkte setzen; ohne Argument: Haltepunkte und Watchpoints auflisten |
| `w adr [länge] [r\|w\|rw]` | Watchpoint (Standard: 1 Byte, Schreiben); `g` hält nach dem zugreifenden Befehl |
| `bc [adr…]` | Haltepunkte (und Watchpoints) löschen; ohne Argument alle |
| `limit n` | höchstens `n` Befehle pro Lauf (Standard 10 000 000 = `989680` hex) – schützt vor Endlosschleifen |
| `sym [name wert \| -name]` | Symbole anzeigen, setzen, löschen |
| `symload datei` / `symsave datei` | Symboldateien (Format der Assembler-Ausgabe `-s`: `name = $XXXXXXXX`) |
| `asm datei [org]` | Quelltext-Datei assemblieren und in den Speicher laden; Symbole werden übernommen |
| `l datei [adr]` | Datei laden: **Raw** (braucht `adr`), **S-Record** (setzt den PC), **Amiga-Hunk** (relokiert ab `adr`, Standard `20000`; PC, Stapel, Symbole) |
| `sv datei von bis` | Speicherbereich speichern (einschließlich `bis`) |
| `ck name` / `rb name` | Checkpoint (CPU + Speicher) anlegen / zurückspielen |
| `savestate datei` / `loadstate datei` | Snapshot-Datei (`M68KSNP1`) |
| `int pegel [vektor]` | Interrupt anfordern (1–7); wird vor dem nächsten Befehl angenommen, wenn der Pegel über der Maske liegt |
| `machine [generic\|amiga500 [chip_kb]]` | Maschine anzeigen / wählen (löscht Speicher und Symbole); `chip_kb` hexadezimal |
| `rom datei` | Kickstart-ROM laden (nur `amiga500`; SSP/PC kommen aus dem ROM) |
| `io` | `amiga500`: Schreibzugriffe auf Custom-/CIA-Register, ROM und „unmapped“ (wird beim Lesen geleert) |
| `reset [mem]` | CPU zurücksetzen (mit `mem` auch Speicher löschen) |
| `?`, `q` | Hilfe, Beenden |

Fehler erscheinen als `? Meldung` und ändern den Zustand nicht (bei mehreren Argumenten wie `r d0=1 zz=2` kann der erste Teil
schon gesetzt sein). Bereichsbefehle (`f`, `c`, `t`, `h`, `sv`) arbeiten auf höchstens `100000` (hex) = 1 MiB je Aufruf.

Nach jedem Lauf (`n`, `o`, `u`, `g`, `ru`) zeigt der Monitor den Grund (`stopped: breakpoint at 001014  [19 instructions]`), die
Register und den nächsten Befehl; Ausnahmen und Interrupts, die dabei auftraten, werden gezählt
(`exceptions: 1 (last: vector 4)`).

## Beispiel

Eingabe (als Skript, `examples/monitor_session.txt`):

```
sym twice 1014
a 1000
start: MOVEQ #5,D0
MOVEQ #0,D1
loop: ADD.W D0,D1
SUBQ.W #1,D0
BNE.S loop
MOVE.W D1,$2000.W
BSR.S twice
STOP #$2700
twice: ADD.W D1,D1
RTS

d start 6
b twice
g start
u
m 2000 2
q
```

Ausgabe:

```
ok
assemble mode: type assembler lines (decimal numbers, $ for hex), empty line to finish
  001000  7005                  MOVEQ #5,D0
  001002  7200                  MOVEQ #0,D1
  001004  D240                  ADD.W D0,D1
  001006  5340                  SUBQ.W #1,D0
  001008  66FA                  BNE.S loop
  00100A  31C1 2000             MOVE.W D1,$2000.W
  00100E  6104                  BSR.S twice
  001010  4E72 2700             STOP #$2700
  001014  D241                  ADD.W D1,D1
  001016  4E75                  RTS
ok
start:
  001000  7005                  MOVEQ #$05,D0
  001002  7200                  MOVEQ #$00,D1
loop:
  001004  D240                  ADD.W D0,D1
  001006  5340                  SUBQ.W #1,D0
  001008  66FA                  BNE.S loop
  00100A  31C1 2000             MOVE.W D1,$2000.W
ok
stopped: breakpoint at 001014  [19 instructions]
 PC=001014 SR=2700  .....  S IPL7  USP=00000000 SSP=0007FFFC
 D0=00000000 D1=0000000F D2=00000000 D3=00000000
 D4=00000000 D5=00000000 D6=00000000 D7=00000000
 A0=00000000 A1=00000000 A2=00000000 A3=00000000
 A4=00000000 A5=00000000 A6=00000000 A7=0007FFFC
twice:
> 001014  D241                  ADD.W D1,D1
stopped: returned  [2 instructions]
 PC=001010 SR=2700  .....  S IPL7  USP=00000000 SSP=00080000
 D0=00000000 D1=0000001E D2=00000000 D3=00000000
 D4=00000000 D5=00000000 D6=00000000 D7=00000000
 A0=00000000 A1=00000000 A2=00000000 A3=00000000
 A4=00000000 A5=00000000 A6=00000000 A7=00080000
> 001010  4E72 2700             STOP #$2700
002000  00 0F                                            |..|
```

Der Ablauf: Programm eintippen (`twice` vorher festlegen, siehe Grenzen), Haltepunkt auf die Routine, laufen lassen (`D1` = 15
nach der Schleife), mit `u` bis zur Rückkehr (`D1` = 30), das Ergebnis im Speicher ansehen.

## Grenzen

- Beim zeilenweisen Assemblieren sind **Vorwärtsreferenzen nicht möglich** (eine Zeile kennt nur Symbole, die schon definiert
  sind). Der Monitor weist darauf hin; mit `sym name adr` legt man die Adresse vorher fest, oder man assembliert eine ganze
  Datei mit `asm`.
- Eine Zeile mit nur einem Wort ohne `:` im Assemble-Modus, das kein Befehl ist, wird abgelehnt (Tippfehlerschutz).
- Es gibt keine Befehlsverlauf-/Zeilenbearbeitung im Programm selbst (siehe `rlwrap`).
- Keine Zyklen- oder Zeitanzeige: Der Emulator kennt kein Timing ([emulator.md](emulator.md#genauigkeit)). Auf der Amiga-Maschine gibt es
  keine Geräte, nur Speicher und das Schreibprotokoll (`io`, [amiga500.md](amiga500.md)).
- Die Analyse- und Annotationsbefehle des Agenten-Protokolls (`analyze`, `listing`, `ann_*`) gibt es im Monitor nicht;
  sie gehören zu `m68ktool serve` ([reverse-engineering.md](reverse-engineering.md)).
