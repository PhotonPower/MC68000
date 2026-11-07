# Anleitung: vom Programm zum kommentierten Listing

Diese Anleitung führt Schritt für Schritt durch die ganze Werkzeugkette – mit dem mitgelieferten Beispielprogramm `examples/amiga/hello_amiga.s`, danach mit einem eigenen Amiga-Programm.
Die Schritte 1–4 brauchen **kein Modell** und laufen vollständig lokal; ab Schritt 5 wird ein Ollama-Server gebraucht.
Der Orchestrator ist bisher nur gegen einen Mock getestet ([reverse-engineering.md](reverse-engineering.md)): Schritt 6 ist der erste Lauf mit einem echten Modell, und Prompt und Parameter brauchen danach vermutlich Feinschliff.

## 1. Bauen und prüfen

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # 28 Tests; dauert je nach Rechner 20 bis 60 Sekunden
```

Voraussetzungen: ein C++20-Compiler und CMake; für die Python-Teile Python ≥ 3.9 (nur Standardbibliothek). Der Orchestrator sucht `build/m68ktool` selbst, sonst `--m68ktool <Pfad>`.

## 2. Das Beispiel übersetzen und starten

```sh
build/m68ktool asm examples/amiga/hello_amiga.s --hunk hello_amiga                   # ohne Symbole: so sieht man ein fremdes Programm
build/m68ktool asm examples/amiga/hello_amiga.s --hunk hello_amiga_sym --hunk-symbols   # mit Namen zum Vergleichen
build/m68ktool monitor --amiga hello_amiga          # Monitor, Amiga-Speicherkarte, Programm geladen (Hunk, ab $20000)
```

Im Monitor (Eingaben hexadezimal, [monitor.md](monitor.md)):

```
machine amiga500 os          # emulierte exec/dos-Aufrufe einschalten (löscht den Speicher), danach das Programm erneut laden:
l hello_amiga
d 20000 8                    # disassemblieren
g                            # laufen lassen: "stopped: the program returned to the system (D0 = 0)"
console                      # Hello from the MC68000 example / sum=150 / words=5
libcalls                     # alle Bibliotheksaufrufe mit Register-Werten
```

Dieselbe Maschine steht für Agenten bereit (`m68ktool serve --allow-files`, JSON-Zeilen, [agent-protocol.md](agent-protocol.md)): `machine` (mit `"os":true`), `load_file` (`"type":"hunk"`), `run`, `console`, `libcalls`.

## 3. Die Analyse ansehen

```sh
python3 agent/m68k_annotate.py --hunk hello_amiga --dry-run
```

Der **Dry-Run** lädt das Programm, führt die Analyse aus und zeigt, was ein echter Lauf tun würde – **ohne ein Modell zu benutzen**:

- die Routinen in der Reihenfolge, in der sie bearbeitet werden (zuerst, was nichts Undokumentiertes aufruft), mit Größe, Befehlszahl, Aufrufern und Aufgerufenen und einer Schätzung der Prompt-Größe in Token
- Sprungtabellen (Branch-, Offset-Tabellen werden verfolgt; ihre Fälle sind eigene Routinen), erkannte Strings und Tabellen von String-Zeigern (werden als Datenbereiche markiert)
- die Bibliotheksaufrufe (`JSR -552(A6)` …) – welche Bibliothek hinter `A6` steckt, zeigt erst der Lauf im Emulator
- eine **Warnung**, wenn die größte Routine mehr als 40 % des Kontextfensters (`--num-ctx`, Standard 32768) beansprucht

Hier prüft man vor einem langen Lauf: Stimmt die Ladeadresse (`--hunk-base`, `--org`)? Sind alle Einstiegspunkte gefunden (`--entry`)? Passt der Kontext zum Modell?
Für ein Programm ohne Hunk-Struktur: `--bin datei --org 0x...` (Raw) oder `--srec datei`; Programme mit eigenem Speicherplan brauchen `--machine generic` oder `--rom`.

## 4. Das Listing ohne Modell

Als JSON-Zeilen an `m68ktool serve --allow-files` (Befehle in dieser Reihenfolge):

```
{"cmd":"machine","type":"amiga500","os":true}
{"cmd":"load_file","file":"hello_amiga","type":"hunk"}
{"cmd":"analyze","entries":["0x020000"]}
{"cmd":"data_regions","apply":true}
{"cmd":"analyze","entries":["0x020000"]}
{"cmd":"listing","verify":true}
```

Das Listing besteht aus den echten Bytes: Platzhalternamen (`sub_`, `loc_`, `data_`), Hardware-Register mit Namen, Strings als `DC.B "…",0`, Sprungtabellen mit ihren Zielen. Es wird neu assembliert und mit dem Original verglichen
(`; VERIFIED`). Auf diesem Stand fehlen nur noch die **Bedeutungen** – die liefert das Modell (Namen, Kommentare, Dokumentation).

## 5. Ollama vorbereiten

```sh
ollama list                       # welche Modelle sind installiert?
ollama pull qwen3-vl:8b-instruct  # der Standard dieses Orchestrators (wie im Z80-Projekt); das Modell braucht Tool-Aufrufe
```

Der Orchestrator ruft `/api/show` auf und lehnt ein Modell ohne Tool-Unterstützung ab; ist das Modell nicht installiert, nennt die Fehlermeldung die installierten Modelle und den passenden `ollama pull`-Befehl. Ein anderes Modell wählt man mit `--model`,
einen anderen Server mit `--host` oder `OLLAMA_HOST`. Ein Bildmodell ist in Ordnung (es bekommt nur Text).

## 6. Der erste Lauf

```sh
python3 agent/m68k_annotate.py --hunk hello_amiga --project hello.proj.json --out hello_kommentiert.s \
        --lang Deutsch --log hello.log.jsonl --report hello_bericht.md
```

- `--project`: die Annotationen werden **nach jeder Routine** gespeichert; der Lauf lässt sich abbrechen und mit demselben Befehl fortsetzen (bearbeitet werden nur noch offene Routinen).
- `--log`: jede Modellnachricht und jedes Werkzeugergebnis (JSON-Zeilen) – damit lässt sich nachvollziehen, was das Modell getan hat und wo es hängt.
- `--out`: das fertige Listing (verifiziert); `--report`: die verbleibenden Lücken je Routine.
- Zum Ausprobieren: `--only 0x02004A` bearbeitet nur eine Routine.
- Das Beispiel hat 7 Routinen.

Danach: `hello_kommentiert.s` lesen, den Bericht ansehen, bei Bedarf mit einem gründlicheren Modell nacharbeiten:

```sh
python3 agent/m68k_annotate.py --hunk hello_amiga --project hello.proj.json --out hello_kommentiert.s \
        --refine-model qwen3-vl:8b --refine-max-turns 25 --refine-max-tokens 12000 --report hello_bericht.md
```

## 6b. Ohne Ollama: ein Agent oder ein Mensch übernimmt die Rolle des Modells

Wo kein lokales Modell läuft, kann jemand mit einer Shell die Werkzeuge **von Hand** bedienen: `agent/m68k_drive.py` startet bei jedem Befehl den Emulator, baut den Zustand aus der
Projektdatei neu auf, führt **einen** Schritt aus und beendet sich. Es sind dieselben Werkzeuge, Berichte und Prüfungen wie im Orchestrator, das Ergebnis ist also dasselbe Listing.

```sh
W=~/m68k-test/arbeit
python3 agent/m68k_drive.py --work $W init --hunk hello_amiga --lang Deutsch   # einmal; weitere Optionen wie bei m68k_annotate.py
python3 agent/m68k_drive.py --work $W status                                   # Routinen in der Reihenfolge, was offen ist, was als Nächstes kommt
python3 agent/m68k_drive.py --work $W show 0x02004A                            # der Auftragstext, den ein Modell für diese Routine bekäme (--system: mit den Anweisungen)
python3 agent/m68k_drive.py --work $W tool run_routine '{"addr":"0x02004A","setup":[{"addr":"0x020000"}],"registers":{"A0":"0x030000"},"memory":[{"addr":"0x030000","hex":"48692074686572650A00"}]}'
python3 agent/m68k_drive.py --work $W tool document_routine '{"addr":"0x02004A","name":"print_string","summary":"...","confidence":"high"}'
python3 agent/m68k_drive.py --work $W tool name_label '{"addr":"0x020052","name":"find_end"}'
python3 agent/m68k_drive.py --work $W finish 0x02004A                          # prüft die Routine; die erste Prüfung kann zurückschicken, der zweite Aufruf gilt
python3 agent/m68k_drive.py --work $W listing --out kommentiert.s              # das verifizierte Listing
python3 agent/m68k_drive.py --work $W report                                   # was noch fehlt
```

`tool` nimmt jedes Werkzeug des Orchestrators (`get_listing`, `disassemble`, `read_memory`, `run_routine`, `reset_machine`, `name_label`, `document_routine`, `name_variable`, `mark_data`,
`add_comment`, `finish_routine`) mit den Argumenten als **einem JSON-Objekt**. Im Arbeitsverzeichnis liegen `config.json` (Optionen), `project.json` (die Annotationen, nach jedem Befehl gespeichert) und
`state.json` (welche Routinen abgeschlossen sind und bei welchen die erste Prüfung schon gelaufen ist). Eine Routine gilt als **fertig**, wenn sie dokumentiert ist **und** `finish` sie angenommen hat.
Ein starker Agent ersetzt dabei nicht den Test mit einem kleinen Modell (er prüft die Werkzeugkette, nicht Prompt und Parameter für ein 8-Milliarden-Modell), liefert aber ein brauchbares Listing.

## 7. Woran man ein Problem erkennt

| Beobachtung | Mögliche Ursache | Abhilfe |
|---|---|---|
| `error: model '…' not found` | Modell nicht installiert | `ollama pull …` oder `--model` |
| `answer cut off at the token limit` | das Modell „denkt“ endlos | `--think false` (Standard), ein `-instruct`-Modell, `--max-tokens` höher |
| `the model wrote an invalid tool call` | Adressen ohne Anführungszeichen (`0x1000` statt `"0x1000"`) | wird automatisch angemahnt; hilft das nicht, ein anderes Modell |
| `model stopped without finishing …` | das Modell ruft keine Werkzeuge mehr auf | `--log` ansehen; größeres Modell oder `--refine-model` |
| viele Routinen mit `confidence: low` im Bericht | Modell unsicher | `--refine-model`, danach `--refine-all` |
| der Lauf bricht nach drei Fehlern in Folge ab | der Server ist weg | Ollama prüfen; mit demselben `--project` weitermachen |
| `listing NOT verified` | ein Namens- oder Kommentarfehler, der das Listing ungültig macht | `--log` und den Hinweis `problem` ansehen; das sollte nicht vorkommen |

Alle Befehle des Protokolls und ihre Parameter: [agent-protocol.md](agent-protocol.md). Was das Modell kann und was nicht, wie die Mehrdurchgang-Verfahren arbeiten: [reverse-engineering.md](reverse-engineering.md).
