# Monitor

> **Geplant** (Meilenstein 0.4). Die Befehle folgen dem Z80-Monitor, damit beide Projekte gleich
> bedient werden.

`m68ktool monitor` ist eine klassische Eingabeschleife mit **Hex als Standard** für Zahlen.

| Befehl | Wirkung                                                    |
|--------|------------------------------------------------------------|
| `d`    | disassemblieren                                            |
| `m`    | Speicher ansehen (Byte/Wort/Langwort)                      |
| `e`    | Speicher ändern                                            |
| `f` `t` `h` `c` | füllen, kopieren, suchen, vergleichen             |
| `a`    | Zeilen assemblieren                                        |
| `r`    | Register anzeigen/ändern (`D0`–`D7`, `A0`–`A7`, `PC`, `SR`, `USP`, `SSP`) |
| `n` `o` `u` `g` | Einzelschritt, Step-over, bis Rückkehr, laufen    |
| `b` `w`| Haltepunkte, Watchpoints                                   |
| `ck` `rb` | Checkpoint setzen / zurückrollen                        |
| `sym` `asm` `l` `sv` | Symbole, Datei assemblieren, laden, speichern |

`m68k::Monitor` hat – wie im Z80-Projekt – keine eigene Ein-/Ausgabe (`execute(zeile) → Text`) und
ist dadurch testbar und einbettbar.
