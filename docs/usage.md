# Benutzung von `m68ktool`

```
m68ktool help | version
```

Rückgabewerte: `0` Erfolg, `1` Assemblierfehler im Quelltext, `2` falsche Aufrufparameter,
Ein-/Ausgabefehler oder (noch) nicht umgesetzter Befehl.

## Heute verfügbar

| Befehl              | Wirkung                          |
|---------------------|----------------------------------|
| `m68ktool help`     | Aufrufhilfe                      |
| `m68ktool version`  | Version ausgeben                 |

## Geplant

Die folgenden Befehle sind in der Aufrufhilfe angekündigt und liefern bis zur Umsetzung den
Rückgabewert `2` mit einer Meldung.

```
m68ktool asm <source.s> [-o <bin>] [--srec <datei>] [-l <datei|->] [-s <datei|->] [--org <adresse>]
m68ktool dis <image> [--org <adresse>] [-c <kommentare>] [--asm] [--no-bytes]
m68ktool monitor [image] [--org <adresse>] [--no-files]
m68ktool serve [--allow-files]
```

Adressen und Zahlen werden in allen Schreibweisen des Assemblers angenommen
([assembler-syntax.md](assembler-syntax.md)). Die Optionen entsprechen bewusst denen von `z80tool`.
