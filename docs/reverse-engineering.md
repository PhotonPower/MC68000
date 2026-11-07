# Reverse Engineering mit lokalem LLM

Ziel: aus einem Amiga-Programm (Hunk-Datei oder Binary) ein **möglichst vollständig kommentiertes Listing** machen –
mit sinnvollen Namen für Unterprogramme, Variablen und Daten, Hardware-Zugriffen als benannte Register und Kommentaren –
mit Hilfe eines lokalen Modells (z. B. über Ollama). Das Vorbild ist das
[Z80-Projekt](https://github.com/PhotonPower/Z80/blob/main/docs/reverse-engineering.md).

```
   Modell (Ollama)  <---- Tool-Aufrufe ---->  agent/m68k_annotate.py  <---- JSON-Zeilen ---->  m68ktool serve
   liest, testet, benennt                          (Python, nur stdlib)                          Emulator, Analyse,
                                                                                                 Annotationen, Listing
```

> **Stand:** Beide Seiten sind fertig: `m68ktool serve` (Codefluss-Analyse, Routinen und Aufrufgraph, Annotationen, verifiziertes
> Listing, Emulator zum **Ausprobieren** von Routinen) und der **Orchestrator** `agent/m68k_annotate.py`. Der Orchestrator ist gegen einen
> **scripteten Mock-Ollama** und den echten `m68ktool serve` getestet (`tests/agent/`), aber **noch nicht mit einem echten Modell
> ausprobiert**: Prompt und Parameter brauchen vermutlich Feinschliff (wie beim Z80-Vorbild).

## Was das Modell kann – und was nicht

| Werkzeug | Zweck |
|---|---|
| `get_listing`, `disassemble`, `read_memory` | ansehen |
| `run_routine` | Routine im Emulator wie eine Funktion aufrufen (Register/Speicher vorgeben); Ergebnis: Register, geänderter Speicher (Stapel ausgenommen), Schreibzugriffe auf Custom-/CIA-Register (getrennt nach „von der Routine selbst“ und „von aufgerufenem Code“), Ausnahmen (z. B. Adressfehler durch einen nicht gesetzten Zeiger), **Bibliotheksaufrufe** (Name, Register, ob emuliert) und über `dos Write` ausgegebener Text |
| `reset_machine` | Ausgangszustand wiederherstellen (sonst geschieht das nach jedem `run_routine` automatisch) |
| `name_label`, `document_routine`, `name_variable`, `mark_data`, `add_comment` | **Namen und Kommentare** festhalten |
| `finish_routine` | Routine abschließen |

Das Modell **kann den Code nicht verändern** (siehe unten). Jedes Experiment läuft auf einem Checkpoint (`baseline`) und wird danach
zurückgerollt; die geladene Datei bleibt unberührt. Es gibt **keine Bilder**: ein Modell sieht nur Text (Speicher, Register, Protokolle).

## Benutzung

```sh
python3 agent/m68k_annotate.py --hunk programm --project programm.m68kproj.json --out programm_kommentiert.s \
        --lang Deutsch --model qwen3-vl:8b-instruct --log programm.log.jsonl
```

Schritt für Schritt mit dem Beispielprogramm: [walkthrough.md](walkthrough.md). **Vorher** lohnt `--dry-run`: Er lädt das Programm und führt die Analyse aus, ohne ein Modell zu benutzen, und zeigt die Routinen in der Bearbeitungsreihenfolge, Sprungtabellen, Datenbereiche, Bibliotheksaufrufe und eine Schätzung der Prompt-Größe (mit Warnung, wenn die größte Routine das Kontextfenster zu sehr beansprucht).

Voraussetzungen: gebautes `m68ktool` (wird in `build/` gesucht, sonst `--m68ktool`), Python ≥ 3.9 (nur Standardbibliothek), ein Ollama-Server
mit einem Modell, das **Tool-Aufrufe** kann (Fähigkeiten erkennt das Skript über `/api/show`; ein Modell ohne Tools wird abgelehnt).

| Option | Bedeutung |
|---|---|
| `--hunk DATEI [--hunk-base ADR]` / `--bin DATEI --org ADR` / `--srec DATEI` | Eingabe (Hunk wird relokiert, Standard `0x20000`) |
| `--machine amiga500\|generic`, `--chip-kb`, `--no-os`, `--rom DATEI` | Speicherkarte ([amiga500.md](amiga500.md)); ein Kickstart-ROM ist optional (eigene Kopie) |
| `--entry ADR` (mehrfach) | Einsprungpunkte (Standard: Anfang des Programms) |
| `--project DATEI` | Annotationen; wird geladen, falls vorhanden, und nach **jeder Routine** gespeichert → Abbrechen und Fortsetzen ist möglich |
| `--out DATEI` | fertiges, verifiziertes Listing |
| `--only ADR` | nur diese Routine bearbeiten (zum Ausprobieren) |
| `--model`, `--host`, `--think true\|false`, `--num-ctx`, `--max-tokens`, `--max-turns`, `--temperature` | Modell und Grenzen |
| `--lang` | Sprache der Kommentare |
| `--log DATEI` | jede Modellnachricht und jedes Tool-Ergebnis (JSON-Zeilen), zum Nachvollziehen |

Rückgabewert: `0`, wenn das Listing verifiziert ist; `1`, wenn nicht; `2` bei Konfigurations- und Verbindungsproblemen (Modell ohne
Tools, Server nicht erreichbar, Modell nicht gefunden).

**Ablauf:** Analyse ab den Einsprungpunkten → Routinen **von den Blättern zur Wurzel** (zuerst, was nichts Undokumentiertes aufruft) →
je Routine eine eigene Unterhaltung mit dem Listing, den Namen der bereits dokumentierten Aufgerufenen und den Werkzeugen → Annotationen
speichern → am Ende das verifizierte Listing. Eine zweite Ausführung mit demselben `--project` fragt das Modell nur noch nach noch
offenen Routinen.

**Vollständigkeitsprüfung:** Beim ersten `finish_routine` prüft der Orchestrator die Routine: noch unbenannte Platzhalter (`loc_…`,
`sub_…`, `data_…`), ohne Namen benutzte absolute Speicherstellen (`$2200.W`; Hardware-Register zählen nicht) und zu wenige Kommentare
(etwa einer je 10 Befehle) werden dem Modell aufgelistet. Ein zweiter Aufruf gilt immer als endgültig, das Modell kann also nie
festhängen.

### Mehrere Durchgänge

Wie im Z80-Projekt lassen sich **mehrere Modelle nacheinander** einsetzen: ein schnelles Instruct-Modell für alles, danach ein
gründlicheres Modell (`--refine-model`, beliebig oft) nur für die Routinen mit **gemessenen** Lücken (nicht dokumentiert, Platzhalter,
unbenannte Speicherstellen, zu wenige Kommentare oder vom Modell selbst als `confidence: low` markiert). Die Vorgänger-Annotationen
gelten dabei als Hypothesen eines Kollegen, die geprüft und überschrieben werden. `--refine-all`, `--refine-medium`, `--refine-think`,
`--refine-max-tokens`, `--refine-max-turns` und `--report DATEI` (die verbleibenden Lücken je Routine) verhalten sich wie dort beschrieben.

**Was `gaps left: 0` bedeutet:** Der Bericht misst *Vollständigkeit*, nicht *Richtigkeit*. Kleine Modelle schreiben fast immer
`confidence: high`; wichtige Routinen kannst du mit `--refine-all` von einem größeren Modell gegenlesen lassen.

### Robustheit gegen Modelle

Getestet gegen Mock-Szenarien: Tool-Aufrufe als Text (`<tool_call>…`), Argumente als JSON-String, falsche Typen und unbekannte Argumente
(werden korrigiert bzw. gemeldet), von Ollama nicht lesbare Aufrufe (Hinweis „Adressen als Strings“), abgeschnittene Antworten (das
Limit wächst automatisch), Modelle ohne Thinking-Modus, ein Serverfehler bei einer Routine (sie wird übersprungen, der Lauf geht weiter;
erst drei in Folge brechen ab), Modelle, die ohne `finish_routine` aufhören („partial“).

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
Bibliotheksfunktionen werden im Listing **nicht** benannt: Es gibt keine eingebaute Tabelle für alle Bibliotheken, weil falsche Namen
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

## Sprungtabellen

Die Codefluss-Analyse erkennt Tabellen hinter indizierten Sprüngen, damit ihre Ziele als Code analysiert werden (sonst blieben sie unentdeckt, weil niemand sie direkt aufruft):

| Art | Muster | Behandlung |
|---|---|---|
| **Branch-Tabelle** (`branch`) | `JMP`/`JSR d(PC,Dn)` auf eine Folge von `BRA`- (oder `JMP abs.L`-)Befehlen | die Einträge sind Code, ihre Ziele werden verfolgt (mindestens zwei Einträge) |
| **Offset-Tabelle** (`offset`) | `MOVE.W tab(PC,Dn),Dm` unmittelbar (höchstens ein Befehl dazwischen) vor `JMP`/`JSR base(PC,Dm)`: Wörter, Ziel = `base` + Wort mit Vorzeichen | die Wörter sind **Daten**, die Ziele werden verfolgt; die Tabelle endet am ersten ungültigen Ziel oder dort, wo das erste Ziel beginnt (mindestens zwei Einträge) |
| **Zeigertabelle** (`pointers`) | Folge von Langwörtern (mindestens drei), die auf gültigen Code im Bereich zeigen, hinter einem Datenverweis (`LEA tab(PC),A0` …) | nur ein **Vorschlag** (`followed: false`): Zahlen, die wie Adressen aussehen, können auch Daten sein. Markiert man den Bereich mit `ann_data kind=pointers`, werden die Ziele beim nächsten `analyze` zu Einstiegspunkten (Routinen) |

Die beiden ersten Muster hängen am Sprungbefehl selbst und sind daher zuverlässig; sie werden automatisch verfolgt, und **jeder Fall der Tabelle ist ein eigener Einstiegspunkt (eine eigene Routine)** – sonst gäbe es für ihn nie einen Auftrag an das Modell. `jump_tables` zeigt alle Tabellen (Art, Sprungbefehl, Tabellenadresse, Basis, Einträge, `followed`).
Im **Listing** steht vor einer Branch-Tabelle ein Kommentar, eine Offset-Tabelle erscheint als `DC.W`-Zeilen mit dem Ziel als Kommentar (`; -> loc_001034`) und einem Hinweis auf Sprungbefehl und Basis; das Listing wird wie immer neu assembliert und
verglichen. Der **Orchestrator** markiert vorgeschlagene Zeigertabellen von selbst (`jump_table_<adresse>`) und analysiert neu (`--no-auto-tables` schaltet das ab); der Prompt weist das Modell an, die Fälle zu benennen.
Nicht erkannt werden Tabellen, die über ein Adressregister indiziert werden (`MOVE.W (A0,D0.W),D0`) und Muster mit mehr als einer Zwischenanweisung; sie lassen sich von Hand mit `ann_data` und `ann_label` festhalten.

## Datentypen

Bytes, die weder Code noch annotierte Daten sind, typisiert `Analysis::guessData` (Befehl `data_regions`):

| Art | Erkennung |
|---|---|
| **Text** (`text`) | druckbare Zeichen (ASCII, auch Tab/CR/LF), mindestens 4, zu mindestens 60 % Buchstaben oder Leerzeichen; und entweder von einem Befehl referenziert (`LEA msg(PC),A0`), mit NUL abgeschlossen oder mindestens 12 Zeichen lang. Der Abschluss-NUL und ein Füll-NUL bis zur geraden Länge gehören dazu. Eine Folge wie `NuNuNuNu` (aufeinanderfolgende `RTS`) ist ohne Referenz kein Text |
| **Tabelle von String-Zeigern** (`pointers`) | hinter einem Datenverweis Langwörter, die auf erkannte Strings zeigen (mindestens zwei, davon einer ein „echter“ String); ist die Tabelle erkannt, werden auch **kurze** Strings (1–3 Zeichen + NUL: „No“, „OK“) als Text erkannt, auf die sie zeigt (drei kurze Strings genügen ohne echten) |
| **Nullbereich** (`zeros`) | mindestens 16 Nullbytes (Füllung, leerer Puffer); braucht keine Annotation, das Listing schreibt `DS.B n` |

`data_regions` liefert die Vorschläge (`addr`, `length`, `kind`, `reason`, bei Text `preview`); mit `apply: true` werden Text und Zeigertabellen als Datenbereiche (`ann_data`) eingetragen – Bereiche, die schon annotiert sind, bleiben unberührt.
Danach erscheinen sie im Listing als `DC.B "Hello, Amiga world",0` (der Abschluss-NUL auf derselben Zeile, ein Füllbyte als `EVEN`) und `DC.L name1,name2,…`, und kein Analyseweg hält sie für Code. Der **Orchestrator** übernimmt die Vorschläge vor der
Arbeit von selbst (`--no-auto-data` schaltet das ab); das Modell soll ihnen mit `mark_data` Namen geben. Die Erkennung ist eine Schätzung (ein Text ohne Referenz, NUL und Länge wird nicht gefunden; Daten, die wie Text aussehen, können fälschlich als Text gelten) – sie ändert nur Namen und Darstellung, nie die Bytes.
**Nicht erkannt:** Tabellen von Zahlen (Wörter/Langwörter), Strukturen, Bitplanes und Grafikdaten, Musik/Samples; dafür bleibt `ann_data`.

## Was noch fehlt

- **Erprobung mit einem echten Modell** und der Feinschliff von Prompt und Parametern; bisher nur gegen einen Mock getestet.
- **Mehr emulierte Bibliotheksfunktionen**: Nur wenige `exec`-/`dos`-Funktionen sind nachgebildet, der Rest wird protokolliert und liefert 0
  ([amiga500.md](amiga500.md)); Programme, die auf Ergebnisse anderer Bibliotheken (Grafik, Fenster) warten, nehmen ihren Fehlerpfad.
- **Bildschirm**: keine Darstellung der Bitplanes; das Modell sieht nur Speicher und Registerprotokolle (Entscheidung, siehe Roadmap).
- Erkennung weiterer **Datentypen** (Zahlentabellen, Strukturen, Bitplanes, Samples); derzeit hilft `ann_data` und der Hinweis auf Zeiger.
