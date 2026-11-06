# Mitarbeit

## Bauen und testen

```sh
cmake -S . -B build -DM68K_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Voraussetzungen: CMake ≥ 3.16 und ein C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

Weitere CMake-Optionen:

| Option                   | Standard | Bedeutung                                               |
|--------------------------|----------|---------------------------------------------------------|
| `M68K_BUILD_TESTS`       | `ON`     | Unit-Tests bauen                                         |
| `M68K_WARNINGS_AS_ERRORS`| `OFF`    | Compilerwarnungen als Fehler (in der CI auf Linux `ON`)  |
| `M68K_SANITIZE`          | `OFF`    | AddressSanitizer + UBSan (GCC/Clang)                     |
| `M68K_BUILD_TOOLS`       | `OFF`    | Entwicklerwerkzeuge für die Kreuzprüfung (noch leer)     |

## Regeln

- Neue Funktionalität bekommt Tests in `tests/`. Fehlerkorrekturen bekommen zuerst einen Test,
  der den Fehler zeigt. Neue Test-Suites werden in `tests/CMakeLists.txt` eingetragen, neue
  Quelldateien in der Liste `m68kcore` in `CMakeLists.txt`.
- Änderungen an Dekoder oder Assembler müssen den (geplanten) exhaustiven Roundtrip-Test bestehen
  und, wo möglich, gegen unabhängige Werkzeuge geprüft werden (siehe [docs/testing.md](docs/testing.md)).
- Änderungen am Emulator müssen die (geplanten) Befehlstests bestehen; Details in
  [docs/testing.md](docs/testing.md).
- Die Kernbibliothek (`include/m68k`, `src`) hat keine Ein-/Ausgabe auf der Konsole und keine
  globalen Zustände; Benutzerinteraktion gehört nach `app/`.
- Quelltext, Bezeichner und Kommentare sind Englisch, die Dokumentation unter `docs/` Deutsch.
- ROM-Abbilder (Kickstart, TOS, …) gehören **nie** ins Repository (Urheberrecht); `/roms/` und
  `*.rom` stehen in der `.gitignore`.
- Formatierung: siehe `.clang-format` / `.editorconfig`.
