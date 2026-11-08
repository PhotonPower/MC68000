# Agenten-Protokoll (`m68ktool serve`)

Ein lokaler Agent (z. B. ein LLM mit Werkzeugaufruf) führt MC68000-Code aus, indem er JSON-Objekte an
`m68ktool serve` schickt. **Eine Zeile Anfrage → genau eine Zeile Antwort**, über stdin/stdout. Das Protokoll
folgt dem von [`z80tool serve`](https://github.com/PhotonPower/Z80/blob/main/docs/agent-protocol.md): gleiche
Grundregeln und, wo es passt, gleiche Befehlsnamen, damit ein Orchestrator beide Maschinen bedienen kann.

```sh
m68ktool serve                  # ohne Dateizugriff (Standard)
m68ktool serve --allow-files    # erlaubt load_file / load_rom / save_state / load_state
```

```console
$ echo '{"cmd":"asm","source":"  MOVEQ #5,D0\n  ADD.W D0,D0\n  STOP #$2700","entry":4096}' | m68ktool serve
{"ok":true,"size":8,"start":"0x001000","end":"0x001007","segments":[{"start":"0x001000","size":8}],"symbols":{},"entry":"0x001000"}
```

Eine vollständige Beispielsitzung liegt in `examples/agent_session.jsonl`: `m68ktool serve < examples/agent_session.jsonl`.

## Grundregeln

- Antwort: `{"ok":true, …}` oder `{"ok":false,"error":"…"}`. Der Server **stürzt bei Fehlern nicht ab**; ungültiges JSON
  ergibt ebenfalls eine Fehlerantwort. Ein fehlgeschlagenes `asm` ändert nichts (Speicher und Marken bleiben).
  Bei `set` und `call` können Register, die vor dem fehlerhaften Eintrag genannt wurden, bereits gesetzt sein.
- **Zahlen** dürfen überall als JSON-Zahl oder als String geschrieben werden: `"0x1000"`, `"$1000"`, `"4096"`.
  Strings sind **Ausdrücke**: `"start+4"`, `"A0"`, `"SP-2"`; Marken aus `asm` und `load_file` sowie Registernamen
  (`D0`–`D7`, `A0`–`A7`, `SP`, `PC`, `SR`, `USP`, `SSP`) sind verwendbar. Adressen werden auf 24 Bit beschränkt.
- **Ausgaben** sind Strings: Register `"0x0000002A"` (32 Bit), Adressen `"0x001000"` (24 Bit), `SR` `"0x2700"`.
- Läufe haben immer ein **Limit** (`max_instructions`, Standard 1 000 000, **höchstens 100 000 000**; ein größerer Wert – auch bei `step n` – ist ein Fehler, damit kein Aufruf den Server auf
  Dauer blockieren kann). Endlosschleifen halten also mit `"stop":"instruction_limit"` an; mit `run` geht es von dort weiter. Es gibt **kein Timing** (keine Zyklen) – siehe [emulator.md](emulator.md).
- Dateizugriff ist **aus**, solange der Betreiber `--allow-files` nicht angibt.
- Eine Sitzung ist eine Maschine (ein Prozessor, 16 MiB Adressraum). Parallele Maschinen = mehrere Prozesse.

## Maschinen

| `machine` | Speicher |
|-----------|----------|
| `generic` (Standard) | 16 MiB RAM überall; Stapel bei `$080000` |
| `amiga500` | Speicherkarte eines Amiga 500 ([amiga500.md](amiga500.md)): Chip-RAM, optional Fast-/Slow-RAM, Custom-Chip- und CIA-Register (nur Speicher, **keine Geräte**), ROM mit Overlay, sonst „unmapped“; Stapel am Ende des Chip-RAM |

`machine` setzt die Sitzung komplett zurück (Speicher gelöscht, Marken und Checkpoints verworfen).

## Antwort der Lauf-Befehle

`step`, `step_over`, `run`, `run_until_return`, `call` liefern:

```json
{"ok":true,"stop":"breakpoint","instructions":14,"pc":"0x00100E","exceptions":0,
 "regs":{"D0":"0x00000009", … ,"PC":"0x00100E","SR":"0x2704","USP":"…","SSP":"…","flags":".N...","S":true,"T":false,"IPL":7,
         "stopped":false,"halted":false,"instructions":123,"exceptions":0}}
```

`stop` ist einer von: `stepped`, `breakpoint`, `address`, `returned`, `stopped` (`STOP`-Befehl: wartet auf einen
Interrupt), `halted` (Doppelfehler), `instruction_limit`, `exit` (die OS-Emulation beendete den Lauf: das Programm kehrte zum System
zurück oder rief `Exit`; Text in `message`), `watchpoint` (dann zusätzlich `"watch":{"addr":"0x…"}`).
`exceptions` zählt die Ausnahmen und Interrupts, die während des Aufrufs auftraten; `last_exception` ist die Vektornummer
der letzten (nur wenn es welche gab). `flags` ist `XNZVC` mit `.` für gelöschte Bits. Die Zähler in `regs` sind
Gesamtzähler seit dem letzten `reset`.

## Befehle

| Befehl | Argumente | Wirkung / Antwort |
|---|---|---|
| `asm` | `source`, `origin?` (`0x1000`), `entry?`, `optimize?` (false: kürzere gleichwertige Formen, siehe assembler-syntax.md) | Quelltext ([assembler-syntax.md](assembler-syntax.md)) assemblieren und in den Speicher schreiben. Marken werden für spätere Argumente merkbar und können im Quelltext wiederverwendet werden. `entry` setzt den PC. Antwort: `size`, `start`, `end`, `segments`, `symbols`. Bei Fehlern `ok:false` und `errors:[{line,message,source}]`; nichts wird geändert. |
| `dis` | `addr?` (PC), `count?` (8) | disassemblieren → `lines:[{addr,label?,bytes,text,target?}]` (Marken statt Adressen; `target` bei Sprüngen) |
| `read` | `addr`, `len?` (16, höchstens 4096) | `hex`, `ascii` – so, wie der Prozessor den Speicher sieht (ROM, Overlay, Geräteregister) |
| `write` | `addr`, `hex` oder `bytes[]` | Speicher schreiben; ROM und „unmapped“ werden abgelehnt |
| `load_file` | `file`, `type?`, `addr?` | Datei laden (*braucht `--allow-files`*); `type`: `auto` (Standard: `.srec`/`.s19`/… → `srec`; Hunk-Kennung → `hunk`; sonst `raw`), `raw` (braucht `addr`), `srec` (setzt den PC aus dem Abschluss-Satz), `hunk` (Amiga-Programm, relokiert ab `addr`, Standard `0x20000`; setzt PC auf den Anfang des ersten Code-Hunks, `D0`=`A0`=0, Stapel wie bei `reset`; Symbole werden zu Marken; Antwort: `hunks`, `entry`, `end`, `symbols`) |
| `load_rom` | `file` | Kickstart-ROM-Abbild laden (*braucht `--allow-files`*, nur `amiga500`); setzt SSP/PC aus dem ROM zurück |
| `regs` | – | alle Register |
| `set` | `regs:{"D0":5,"A1":"buffer","PC":"start","SR":"0x2000"}` | Register setzen (`D0`–`D7`, `A0`–`A7`, `SP`, `PC`, `SR`, `CCR`, `USP`, `SSP`); `PC` weckt den Prozessor aus `STOP`/„halted“ |
| `step` | `n?` (1), `trace?` | `n` Befehle (Breakpoints werden ignoriert); mit `trace:true` zusätzlich `trace:[{addr,bytes,text}]` (höchstens 1000 Schritte) |
| `step_over` | – | ein Befehl; bei `BSR`/`JSR` bis zur Rückkehr |
| `run` | `until?`, `max_instructions?` | bis Breakpoint/Watchpoint/`STOP`/Limit; mit `until` bis der PC diese Adresse erreicht |
| `run_until_return` | `max_instructions?` | bis die aktuelle Routine zurückkehrt (`RTS`/`RTR`/`RTE`) |
| `call` | `addr`, `regs?`, `return_address?` (`0xFFFFF0`), `max_instructions?` | Routine wie eine Funktion aufrufen: Rücksprungadresse auf den Stapel, Register setzen, ausführen bis zur Rückkehr. Antwort enthält `returned`: **true nur, wenn die Routine wirklich zurückgekehrt ist**, also der PC die Rücksprungadresse erreicht hat **und** `A7` wieder den Wert vor dem Aufruf hat. Ein Sprung durch einen Zeiger, der noch 0 ist, läuft durch leeren Speicher und kann die Rücksprungadresse zufällig streifen; das ist keine Rückkehr (`returned:false`, `stop:"address"`, dazu ein `note`). Auf der Amiga-Maschine zählen `run`, `step`, `step_over`, `run_until_return` und `call` außerdem die Befehle, die aus **leerem Speicher** geholt wurden (nicht abgebildet, oder der ROM-Bereich ohne ROM): `fetched_from_empty_memory` und `first_empty_fetch` |
| `track_writes` | `on?` (true), `clear?` (true) | verfolgt, welche Bytes das Programm schreibt (ein Bit je Byte, 2 MiB); `on:false` schaltet aus |
| `written` | `min_length?` (16), `limit?` (20) | die größten zusammenhängenden geschriebenen Bereiche: `ranges:[{start,end,size}]`, `bytes` |
| `run_until_unpacked` | `min_written?` (4096), `continue?`, `ignore?:[[von,bis],…]`, `max_instructions?` (50 Mio.) | läuft, bis ein Befehl aus **geschriebenem Speicher** geholt werden soll, dessen zusammenhängender geschriebener Bereich mindestens `min_written` Byte groß ist: `stop:"unpacked"`, `pc`, `region:{start,end,size}`, `written_bytes`, `ranges`. `continue` überspringt den zuletzt gefundenen Bereich. Siehe [unpacking.md](unpacking.md) |
| `save_memory` | `addr`, `len` (bis 16 MiB), `file` | schreibt Speicher als Rohdatei (nur mit `--allow-files`) |
| `break` / `unbreak` | `addr` | Breakpoint setzen / entfernen |
| `watch` / `unwatch` | `addr`, `len?` (1), `kind?` (`write`, `read`, `access`) | Speicherüberwachung; `run` hält **nach** dem zugreifenden Befehl |
| `breaks` / `clear_breaks` | – | Breakpoints und Watchpoints auflisten / alles löschen |
| `checkpoint` / `rollback` / `drop` | `name` | kompletten Zustand (CPU, Speicher, bei `amiga500` auch Geräteregister) merken / zurückspielen / löschen |
| `checkpoints` | – | Namen auflisten |
| `diff` | `name` | geänderte Speicherbereiche seit dem Checkpoint: `changed_bytes`, `ranges:[{addr,len,before,after}]` (≤ 50 Bereiche, je ≤ 64 Byte angezeigt, nahe beieinander liegende Änderungen zusammengefasst). Geräteregister sind darin nicht enthalten (dafür `io`). |
| `state` / `set_state` | `state` | CPU-Zustand als JSON lesen / setzen (Register wie bei `regs`/`set`) |
| `save_state` / `load_state` | `file` | Snapshot-Datei (CPU + Speicher) schreiben / lesen (*braucht `--allow-files`*) |
| `interrupt` | `level` (1–7), `vector?` | Interrupt anfordern; wird vor dem nächsten Befehl angenommen, wenn der Pegel über der Maske im SR liegt (7 immer) |
| `io` | `clear?` (true) | `amiga500`: Protokoll aller Schreibzugriffe auf Custom-Chip-/CIA-Register, ROM und „unmapped“ (`output:[{addr,size,value,pc,kind,what?}]`, höchstens 4096; wird beim Lesen geleert); `what` ist der Registername (z. B. `COLOR00`, `DMACON`), `pc` die Adresse des schreibenden Befehls |
| `reset` | `memory?` | Register zurücksetzen (mit `memory:true` auch den Speicher löschen); Zähler auf 0 |
| `symbols` | – | alle Marken |
| `machine` | `type`, `chip_kb?` (512), `slow_kb?` (0), `fast_kb?` (0), `os?` (false) | Maschine wählen (siehe oben); `os:true` (nur `amiga500`, ≥ 512 KiB Chip-RAM) schaltet die **OS-Emulation** ein ([amiga500.md](amiga500.md#emulierte-bibliotheksaufrufe-m68kamigaos)) |
| `libcalls` | `clear?` (true) | `calls:[{library,offset,function?,min_version?,emulated,caller (Rücksprungadresse),call_site? (Adresse des JSR selbst),args:{D0..D3,A0,A1},result,note?}]` – alle Bibliotheksaufrufe seit dem letzten Lesen; `emulated:false` = nur protokolliert, `D0` = 0 |
| `console` | `clear?` (true) | `text`: was das Programm über `dos Write` auf die Standardausgabe geschrieben hat |
| `help` | – | Befehlsliste |

### Reverse Engineering ([reverse-engineering.md](reverse-engineering.md))

| Befehl | Argumente | Wirkung / Antwort |
|---|---|---|
| `analyze` | `entries[]`, `from?`, `to?`, `linear?{from,to}`, `reset?` | Codefluss ab den Einstiegspunkten analysieren (Standard: der PC; Bereich: zuletzt geladen/assembliert, sonst `from`/`to` angeben); `linear` dekodiert zusätzlich einen Bereich der Reihe nach; `reset` vergisst frühere Einstiegspunkte. Antwort: `code_bytes`, `instructions`, `routines`. Nötig für `routines` und `listing`. |
| `data_regions` | `apply?` (false) | `regions:[{addr,length,kind (text/pointers/words/longs/zeros),reason,preview?,applied?}]` – Typ der Bytes, die weder Code noch annotiert sind (Strings, Tabellen von String-Zeigern, Zahlentabellen, Nullbereiche); mit `apply:true` werden Text, Zeiger- und Zahlentabellen als Datenbereiche eingetragen (`applied`: Zahl) |
| `jump_tables` | `max_entries?` (64) | `tables:[{kind (branch/offset/pointers), jump?, table, bytes, base? (offset), followed, entry_count, entries:[{slot,target}], hint?}]` – Sprungtabellen der letzten Analyse; `followed:false` = nur vorgeschlagen (mit `ann_data kind=pointers` markieren und erneut `analyze`) |
| `routines` | `order?` (`bottom_up`, `address`), `undocumented_only?` | Unterprogramme: `addr`, `name` (oder `sub_XXXXXX`), `documented`, `confidence`, `start`/`end`, `instructions`, `bytes`, `depth`, `callers`, `callees`, `external_calls`, `library_calls:[{addr,offset}]`; bei dokumentierten Routinen auch `summary`, `inputs`, `outputs`, `clobbers`; `blocks:[[start,end],…]` (der eigene Code in zusammenhängenden Bereichen; `start`/`end` sind nur die Hülle), `via_table` (Adresse der Sprungtabelle, durch die ein Fall erreicht wird, wenn ihn niemand aufruft); ein Sprung zum Anfang einer anderen Routine gilt als Tail-Call (`callees`/`callers`, nicht Teil der Routine) |
| `references` | `from`, `to` | was die Befehle im Bereich ansprechen: `references:[{addr,name,kind (code/text/**text?**/words/longs/pointers/bytes/variable/system/unknown; `text?`: Bytes ohne Datenbereich, die wie ein kurzer String aussehen),length,preview?,used_at:[Befehlsadressen]}]`, sortiert nach Adresse. `preview`: bei Text der Inhalt, bei Tabellen die ersten Werte, sonst die ersten Bytes. Labels innerhalb des Bereichs fehlen (das Listing zeigt sie); Adresse 4 ist `ExecBase_pointer` |
| `listing` | `from?`, `to?`, `verify?` (true), `max_chars?`, `addresses?`, `equates?` (true: EQU-Zeilen für Namen, die nicht auf einer eigenen Zeile stehen können; ohne `verify` kann man sie für einen Ausschnitt zum Lesen weglassen) | kommentiertes, **neu assemblierbares** Listing; nach `load_file` (Hunk) mit Kommentaren `---- CODE hunk 0 (…) ----` an den Hunk-Grenzen, Nullbytes in einem BSS-Hunk ab 4 Byte als `DS.B`; `verified` = neu assembliert, identische Bytes; `unnamed` = Platzhalter ohne echten Namen; `instructions` / `user_comments` = Befehlszeilen bzw. Zeilen mit Kommentar; `truncated` bei `max_chars` |
| `ann_label` | `addr`, `name` | Namen vergeben; ungültige, reservierte, doppelte Namen werden mit Begründung abgewiesen |
| `ann_routine` | `addr`, `name?`, `summary?`, `inputs?`, `outputs?`, `clobbers?`, `confidence?` (`high`, `medium`, `low`) | Routine dokumentieren |
| `ann_variable` | `name`, `addr`, `size?` (1), `comment?` | Variable/Tabelle benennen |
| `ann_data` | `addr`, `length`, `kind?` (`bytes`, `words`, `longs`, `text`, `pointers`), `name?`, `comment?` | Bereich als Daten kennzeichnen; abgelehnt, wenn er analysierten Code enthält oder einen anderen Datenbereich überlappt |
| `ann_comment` | `addr`, `text`, `block?` | Kommentar zu einem Befehl (oder `block:true`: Block über der Zeile); Adressen **mitten in einem Befehl** werden mit Erklärung abgelehnt |
| `ann_constant` | `addr`, `remove?` | markiert den Befehl an der Adresse als einen, dessen Immediate eine **Zahl** ist, keine Adresse: das Listing erfindet dann kein Label für den Wert und schreibt kein `[#imm = …]` (Adressen **mitten in einem Befehl** und außerhalb des Codes werden abgelehnt); `remove:true` nimmt die Markierung zurück |
| `ann_remove` | `addr` | Namen, Dokumentation, Variable, Datenbereich, Kommentare und die Konstantenmarkierung an der Adresse entfernen |
| `ann_title` | `title` | Titel im Listing |
| `ann_list` | – | alle Annotationen als JSON |
| `ann_save` / `ann_load` | `file` | Annotationen als JSON-Datei speichern / laden (*braucht `--allow-files`*) |

Namen aus den Annotationen sind danach in allen Adress-Argumenten verwendbar (`"until":"copy_block"`) und erscheinen in `dis`.

## Typischer Ablauf

```json
{"cmd":"machine","type":"amiga500"}
{"cmd":"asm","source":"        ORG $1000\nstart:  MOVE.W #$0F00,$DFF180.L\n        MOVEQ #5,D0\nloop:   ADD.W D0,D1\n        DBRA D0,loop\n        STOP #$2700","entry":"start"}
{"cmd":"checkpoint","name":"fresh"}
{"cmd":"run"}
{"cmd":"io"}
{"cmd":"diff","name":"fresh"}
{"cmd":"rollback","name":"fresh"}
{"cmd":"call","addr":"loop","regs":{"D0":3,"D1":0}}
```

Hinweise zur Verwendung durch ein Modell:

- Nach `asm` bzw. `load_file` die Marken verwenden statt Adressen zu merken (`"until":"loop"`).
- `diff` zeigt, was ein Programm im Speicher verändert hat, ohne den ganzen Speicher zu lesen; bei `amiga500` zeigt `io`
  zusätzlich, was es in die Hardware-Register geschrieben hat.
- `checkpoint` vor riskantem Code, `rollback` danach; ein Rücksetzen kopiert nur die belegten 64-KiB-Seiten.
- Mit `trace:true` bekommt man bei kurzen Programmen den Ablauf Befehl für Befehl.
- `call` ist der einfachste Weg, eine einzelne Routine zu testen (Argumente in Registern, Ergebnis in `regs`).
- Ein `STOP` am Programmende zeigt sich als `"stop":"stopped"`; mit `set` des `PC` läuft man von dort weiter.

## Sicherheit

Der emulierte Code kann nur den eigenen 16-MiB-Speicher und die Geräte-Protokolle beeinflussen, nichts auf dem Host.
Dateizugriff gibt es nur mit `--allow-files`; dann kann der Agent beliebige Pfade lesen/schreiben, die der Serverprozess
erreicht – den Server deshalb mit eingeschränkten Rechten oder in einem Verzeichnis-Sandbox starten. Die Laufzeit ist über
`max_instructions` begrenzt (höchstens 100 000 000 Befehle je Aufruf, rund eine Sekunde bis wenige Sekunden); `read` liefert höchstens 4096 Byte, `write` nimmt höchstens 65 536 Byte, JSON-Eingaben dürfen höchstens 64 Ebenen tief verschachtelt sein.
