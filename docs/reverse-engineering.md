# Reverse Engineering mit lokalem LLM

> **Geplant** (Meilenstein 0.5). Vorbild: [Z80-Projekt](https://github.com/PhotonPower/Z80/blob/main/docs/reverse-engineering.md).

Idee: Ein lokales Modell (über Ollama) analysiert ein Programm Unterprogramm für Unterprogramm, liest
das Disassembly, **führt Routinen im Emulator aus**, prüft das Ergebnis und vergibt Namen und
Kommentare. Das Ergebnis ist ein kommentiertes Listing, das neu assembliert und Byte für Byte
verifiziert wird – das Modell kann den Code nicht verändern.

Voraussetzungen, die erst geschaffen werden müssen: Disassembler mit Ablaufanalyse, Emulator mit
Snapshots, `serve`-Protokoll, Annotationen (Namen, Kommentare, Datenbereiche) und mindestens ein
Plattformprofil mit bekannten Systemaufrufen (bei 68000-Systemen häufig über `TRAP #n` oder
Line-A/Line-F-Ausnahmen, nicht über feste Sprungadressen wie beim CPC).
