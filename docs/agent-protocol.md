# Agenten-Protokoll (`m68ktool serve`)

> **Geplant** (Meilenstein 0.3/0.5). Das Protokoll folgt dem von `z80tool serve`, damit derselbe
> Orchestrator beide Maschinen bedienen kann.

JSON-Zeilenprotokoll: pro Zeile auf stdin ein Objekt `{"cmd": "...", ...}`, pro Anfrage eine
Antwortzeile auf stdout mit `"ok": true|false`. Dateizugriff nur mit `--allow-files`.

Vorgesehene Befehle (Namen vorläufig): `info`, `asm`, `load`, `reg_get`/`reg_set`, `mem_read`/`mem_write`,
`step`, `run`, `call`, `break`, `watch`, `checkpoint`/`rollback`, `diff`.

Unterschiede zum Z80-Protokoll:

- Register sind `d0`–`d7`, `a0`–`a7`, `pc`, `sr`, `usp`, `ssp`; Werte sind 32 Bit (hexadezimal als Zeichenkette,
  damit Modelle keine nackten `0x…`-Zahlen erzeugen).
- Speicherzugriffe haben eine Größe (`b`/`w`/`l`) und folgen der Big-Endian-Byte-Reihenfolge.
- Ein Lauf meldet bei Ausnahmen Vektor, Adresse und Stapelrahmen.

Die genaue Spezifikation entsteht mit der Umsetzung der `Session`-Klasse.
