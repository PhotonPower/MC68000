# Reverse Engineering mit lokalem LLM

Ziel: aus einem Amiga-Programm (Hunk-Datei oder Binary) ein **möglichst vollständig kommentiertes Listing** machen –
mit sinnvollen Namen für Unterprogramme, Variablen und Daten, Hardware-Zugriffen als benannte Register und Kommentaren –
mit Hilfe eines lokalen Modells (z. B. über Ollama). Das Vorbild ist das
[Z80-Projekt](https://github.com/PhotonPower/Z80/blob/main/docs/reverse-engineering.md).

```
   Modell (Ollama)  <---- Tool-Aufrufe ---->  Orchestrator (Python, geplant)  <---- JSON-Zeilen ---->  m68ktool serve
   liest, testet, benennt                                                                              Emulator, Analyse,
                                                                                                       Annotationen, Listing
```

> **Stand:** Die Seite von `m68ktool serve` ist fertig und getestet: Codefluss-Analyse, Routinen und Aufrufgraph,
> Annotationen, verifiziertes Listing, dazu der Emulator zum **Ausprobieren** von Routinen. Der **Orchestrator**
> (Python-Skript, das ein Modell über Ollama anbindet) ist noch **nicht** geschrieben; bis dahin lassen sich die Befehle
> von Hand oder aus einem eigenen Skript benutzen ([agent-protocol.md](agent-protocol.md)).

## Das Prinzip: das Modell kann den Code nicht verändern

Das Modell liefert nur **Namen und Texte**. Das Listing wird immer aus den echten Bytes erzeugt, danach **neu assembliert und
Byte für Byte verglichen** (`verified`). Auch halluzinierte Kommentare können das Programm deshalb nicht verfälschen; ungültige
Namen (Register, Mnemonics, Direktiven, Hardware-Registernamen, erzeugte Platzhalter, doppelte Namen) werden mit einer
Erklärung abgewiesen, aus der das Modell lernt. Das ist keine Behauptung, sondern getestet: 20 000 zufällige Programme mit
zufälligen Annotationen (auch mit Anführungszeichen, Semikolons und Zeilenumbrüchen in den Texten) ergaben jedes Mal ein
verifiziertes Listing (`tests/test_reverse.cpp`).

## Ablauf mit den Protokollbefehlen

```json
{"cmd":"machine","type":"amiga500"}
{"cmd":"load_file","file":"programm"}                       // Hunk: relokiert, Symbole werden Marken
{"cmd":"analyze","entries":["0x20000"]}                     // Codefluss ab den Einstiegspunkten
{"cmd":"routines"}                                          // von den Blättern zur Wurzel
{"cmd":"dis","addr":"sub_020040","count":30}                // lesen
{"cmd":"checkpoint","name":"x"}                             // ausprobieren ...
{"cmd":"call","addr":"0x20040","regs":{"A0":"0x30000","D0":16}}
{"cmd":"diff","name":"x"}                                   // ... was hat die Routine im Speicher geändert?
{"cmd":"io"}                                                // ... und in die Hardware-Register geschrieben?
{"cmd":"rollback","name":"x"}
{"cmd":"ann_routine","addr":"0x20040","name":"copy_block","summary":"…","inputs":"A0 = Quelle","confidence":"medium"}
{"cmd":"ann_variable","name":"frame_counter","addr":"0x30010","size":2}
{"cmd":"ann_comment","addr":"0x20050","text":"Schleife über die Zeilen"}
{"cmd":"listing"}                                           // verifiziert: "verified": true
```

`analyze` kann erneut aufgerufen werden, wenn Annotationen (z. B. Datenbereiche) die Analyse verändern; angegebene
Datenbereiche (`ann_data`) werden nie als Code dekodiert. Die Reihenfolge `routines` → je Routine lesen/ausprobieren/benennen
folgt dem Z80-Vorbild: zuerst, was nichts Undokumentiertes aufruft (`depth` 0), zuletzt die Wurzel.

## Was die Analyse liefert

`routines` nennt je Routine: Einsprung, Ausdehnung (alles, was die Sprünge erreichen, ohne die Aufgerufenen), Anzahl der
Befehle und Bytes, Aufrufer (Adressen der `BSR`/`JSR`), Aufgerufene (mit Namen), Aufrufe **außerhalb** des analysierten Codes,
**Bibliotheksaufrufe** (`JSR -552(A6)`: Adresse und Offset) und die Tiefe im Aufrufgraphen (Zyklen werden aufgebrochen).
Bibliotheksfunktionen werden **nicht** benannt: Es gibt keine eingebaute Tabelle (`exec`/`dos`-Offsets), weil falsche Namen
das Modell in die Irre führen würden; der Offset selbst ist ein guter Hinweis, den das Modell oder du per Kommentar auflöst.

## Was im Listing automatisch passiert

- **Platzhalter-Namen** für alles, was angesprungen oder angesprochen wird: `sub_XXXXXX` (Einsprünge, Aufrufziele),
  `loc_XXXXXX` (Sprungziele), `data_XXXXXX` (Daten, auf die Befehle zeigen). `unnamed` im Ergebnis zählt, wie viele noch auf
  einen echten Namen warten. Ein Name wird für alles verwendet, was an der Adresse dokumentiert ist (Label, Routine, Variable).
- **Zeiger**: Ein `.L`-Immediate, das einer Adresse im Bereich entspricht (`MOVE.L #table,A0`), bekommt dort ein Label und den
  Hinweis `[#imm = name]` (nur als Kommentar; ein Zufallstreffer kostet nichts, weil Labels keine Bytes ändern).
- **Hardware**: Auf der Amiga-Maschine erscheinen Custom-Register als Namen (`MOVE.W #$0F00,COLOR00.L` mit
  `COLOR00 EQU $DFF180`); die Namensliste steht in [amiga500.md](amiga500.md) (aus dem Gedächtnis, bitte prüfen).
- **Variablen und Tabellen**: `ann_variable` (1/2/4 Byte oder mehr) erscheint als `name: DC.W …`; Zugriffe in die Mitte werden
  `name+2`. `ann_data` mit `bytes`, `words`, `longs`, `text` oder `pointers` (Zeiger zeigen Namen: `DC.L entry,print_string`).
- **Daten ohne Annotation**: Texte (≥ 4 druckbare Zeichen, mit `0`-Ende), lange Nullfolgen (`DS.B n`), sonst `DC.B`.
- **Namen innerhalb eines Befehls**: Ein Label, das in der Mitte eines Befehls liegt, wird mit `EQU` definiert (so bleibt das
  Listing assemblierbar); ein Kommentar dort wird mit Erklärung abgelehnt, statt stillschweigend zu verschwinden.
- **Kopf einer Routine**: Name, Zusammenfassung, Ein-/Ausgaben, geänderte Register, `confidence` (`low` erscheint als
  „a guess, please verify“).
- `addresses:true` hängt jeder Zeile ihre Adresse als Kommentar an (um sie in `ann_comment` zu benutzen); das Listing bleibt
  verifizierbar. `max_chars` schneidet an einer Zeilengrenze ab (dann nicht verifiziert).

## Projektdateien

`ann_save`/`ann_load` (mit `--allow-files`) schreiben und lesen alle Annotationen als JSON (`labels`, `routines`, `variables`,
`data`, `comments`, `block_comments`, `title`). Eine fehlerhafte Datei (doppelte oder ungültige Namen, unbekannte Art) wird mit
Begründung abgelehnt. `machine` verwirft die Annotationen zusammen mit allem anderen.

## Was noch fehlt

- **Der Orchestrator** (`agent/m68k_annotate.py`, nur Standardbibliothek): Modell über Ollama, Werkzeuge `run_routine`
  (`checkpoint` + `call` + `diff` + `io` + `rollback`), Mehrdurchgang-Verfahren, Vollständigkeitsprüfung und Bericht wie im
  Z80-Projekt. Das Verfahren ist dort beschrieben und lässt sich übernehmen.
- **Amiga-Bibliotheksaufrufe**: ExecBase, `A6` und Stubs für `exec`/`dos`; ohne sie laufen Programme bis zum ersten
  Bibliotheksaufruf ([amiga500.md](amiga500.md)).
- **Bildschirm**: keine Darstellung der Bitplanes; das Modell sieht nur Speicher und Registerprotokolle (Entscheidung, siehe Roadmap).
- Erkennung von Sprungtabellen und Datentypen; derzeit hilft `ann_data` und der Hinweis auf Zeiger.
