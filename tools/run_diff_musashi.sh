#!/bin/sh
# Builds Musashi (an independent MC68000 emulator, MIT-style license; fetched, never stored in this
# repository) and runs the differential test against the emulator core.
#
#   tools/run_diff_musashi.sh [trials-per-opcode] [seed]
#
# Needs: git, gcc/g++, network access to github.com. Works in a temporary directory.
set -e
TRIALS=${1:-4}
SEED=${2:-1}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${M68K_MUSASHI_WORK:-$(mktemp -d)}
if [ ! -d "$WORK/Musashi" ]; then
  git clone --quiet --depth 1 https://github.com/kstenerud/Musashi.git "$WORK/Musashi"
fi
cd "$WORK/Musashi"
[ -f m68kops.c ] || { gcc -O1 -o m68kmake m68kmake.c && ./m68kmake . m68k_in.c >/dev/null; }
[ -f m68kcpu.o ] || gcc -O2 -c m68kcpu.c m68kops.c softfloat/softfloat.c
g++ -std=c++20 -O2 -I "$ROOT/include" -I "$ROOT/src" -I "$WORK/Musashi" "$ROOT/tools/diff_musashi.cpp" \
    "$ROOT/src/cpu.cpp" "$ROOT/src/decoder.cpp" "$ROOT/src/forms.cpp" "$ROOT/src/format.cpp" \
    m68kcpu.o m68kops.o softfloat.o -o "$WORK/m68k_diff_musashi"
"$WORK/m68k_diff_musashi" "$TRIALS" "$SEED"
