# Befehlssatz-Überblick MC68000

Kurzreferenz als Grundlage für Dekoder, Assembler und Emulator. Maßgeblich bleibt das offizielle
*M68000 Family Programmer's Reference Manual*; Abweichungen davon sind Fehler dieses Dokuments.

## Programmiermodell

- 8 Datenregister `D0`–`D7`, 8 Adressregister `A0`–`A7`, jeweils 32 Bit. `A7` ist der Stapelzeiger:
  im Supervisor-Modus der Supervisor-Stapelzeiger (SSP), im Benutzermodus der Benutzer-Stapelzeiger (USP).
- Programmzähler `PC` (32 Bit, auf dem 68000 nur 24 Bit herausgeführt).
- Statusregister `SR` (16 Bit): Benutzerbyte = **CCR** mit den Flags `X N Z V C`; Systembyte mit
  Trace-Bit `T`, Supervisor-Bit `S` und Interrupt-Maske `I2–I0`.
- Operandengrößen `.B`, `.W`, `.L`. Schreiben auf ein Datenregister mit `.B`/`.W` ändert nur den
  unteren Teil; Operationen auf Adressregistern sind `.W` (vorzeichenerweitert) oder `.L`.

## Speicher

- Adressbus 24 Bit (16 MiB), **Big-Endian**.
- Wort- und Langwortzugriffe sowie Befehle müssen an **geraden Adressen** liegen; sonst folgt ein
  Adressfehler (Ausnahme 3). `Memory` selbst prüft das nicht, der Emulator tut es.
- Ausnahmevektoren: 256 × 4 Byte ab Adresse 0. Vektor 0 = Start-SSP, Vektor 1 = Start-PC.

## Adressierungsarten

| Schreibweise     | Bedeutung                               |
|------------------|-----------------------------------------|
| `Dn`             | Datenregister direkt                    |
| `An`             | Adressregister direkt                   |
| `(An)`           | Adressregister indirekt                 |
| `(An)+`          | indirekt mit Postinkrement              |
| `-(An)`          | indirekt mit Prädekrement               |
| `d16(An)`        | indirekt mit 16-Bit-Verschiebung        |
| `d8(An,Xn)`      | indirekt mit Index und 8-Bit-Verschiebung |
| `xxx.W`, `xxx.L` | absolut kurz / lang                     |
| `d16(PC)`        | PC-relativ                              |
| `d8(PC,Xn)`      | PC-relativ mit Index                    |
| `#imm`           | unmittelbar                             |

Ein Befehl besteht aus einem 16-Bit-Opcodewort und bis zu vier Erweiterungswörtern.

## Opcode-Gruppen (obere vier Bit des ersten Wortes)

| Bits  | Inhalt                                                  |
|-------|---------------------------------------------------------|
| `0x0` | Bitoperationen, `MOVEP`, unmittelbare Operationen (`ORI`, `ANDI`, `SUBI`, `ADDI`, `EORI`, `CMPI`) |
| `0x1` | `MOVE.B`                                                |
| `0x2` | `MOVE.L` / `MOVEA.L`                                    |
| `0x3` | `MOVE.W` / `MOVEA.W`                                    |
| `0x4` | Verschiedenes (`LEA`, `PEA`, `CLR`, `NEG`, `NOT`, `TST`, `JMP`, `JSR`, `RTS`, `RTE`, `TRAP`, `MOVEM`, `EXT`, `SWAP`, `LINK`, `UNLK`, …) |
| `0x5` | `ADDQ`, `SUBQ`, `Scc`, `DBcc`                           |
| `0x6` | `Bcc`, `BRA`, `BSR`                                     |
| `0x7` | `MOVEQ`                                                 |
| `0x8` | `OR`, `DIVU`, `DIVS`, `SBCD`                            |
| `0x9` | `SUB`, `SUBA`, `SUBX`                                   |
| `0xA` | „Line A“ – keine Befehle; löst eine Ausnahme aus (von Systemen als Emulator-/Systemaufruf genutzt) |
| `0xB` | `CMP`, `CMPA`, `CMPM`, `EOR`                            |
| `0xC` | `AND`, `MULU`, `MULS`, `ABCD`, `EXG`                    |
| `0xD` | `ADD`, `ADDA`, `ADDX`                                   |
| `0xE` | Schiebe- und Rotationsbefehle                           |
| `0xF` | „Line F“ – keine Befehle; löst eine Ausnahme aus        |

## Ausnahmen (Auswahl)

| Vektor | Ursache                      | Vektor | Ursache                  |
|--------|------------------------------|--------|--------------------------|
| 2      | Busfehler                    | 8      | Privilegverletzung       |
| 3      | Adressfehler                 | 9      | Trace                    |
| 4      | unerlaubter Befehl           | 10     | Line A                   |
| 5      | Division durch 0             | 11     | Line F                   |
| 6      | `CHK`                        | 25–31  | Autovektor-Interrupts 1–7 |
| 7      | `TRAPV`                      | 32–47  | `TRAP #0`–`TRAP #15`     |

Bus- und Adressfehler legen einen größeren Stapelrahmen an als die übrigen Ausnahmen; dieses
Detail ist für den Emulator wichtig (siehe [emulator.md](emulator.md)).

## Varianten

Dieses Projekt unterstützt **nur den MC68000** (Entscheidung, siehe [roadmap.md](roadmap.md)). MC68008,
MC68010 und höher (u. a. `MOVEC`, `RTD`, Vektorbasisregister, erweiterte Adressierung) sind kein Ziel;
ihre Opcodes gelten als unerlaubte Befehle.
