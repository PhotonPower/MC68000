# Reverse Engineering mit lokalem LLM

> **Geplant** (Meilenstein 0.5). Vorbild: [Z80-Projekt](https://github.com/PhotonPower/Z80/blob/main/docs/reverse-engineering.md).

Idee: Ein lokales Modell (über Ollama) analysiert ein Programm Unterprogramm für Unterprogramm, liest
das Disassembly, **führt Routinen im Emulator aus**, prüft das Ergebnis und vergibt Namen und
Kommentare. Das Ergebnis ist ein kommentiertes Listing, das neu assembliert und Byte für Byte
verifiziert wird – das Modell kann den Code nicht verändern.

Voraussetzungen, die erst geschaffen werden müssen: Disassembler mit Ablaufanalyse, Emulator mit
Snapshots, `serve`-Protokoll, Annotationen (Namen, Kommentare, Datenbereiche) und mindestens ein
Plattformprofil mit bekannten Systemaufrufen. Beim Amiga 500 sind das Bibliotheksaufrufe `JSR -offset(A6)`;
sie werden über eine LVO-Tabelle (Bibliothek, Offset, Funktionsname) benannt ([amiga500.md](amiga500.md)).
