#!/bin/sh
# Builds Musashi (an independent MC68000 emulator, MIT-style license; fetched, never stored in this
# repository) and runs two differential tests against the emulator core:
#
#   1. tools/diff_musashi.cpp      every opcode word, random states, ONE instruction at a time
#   2. tools/diff_musashi_seq.cpp  random PROGRAMS of 2-10 instructions, compared after every instruction
#                                  (needs Musashi built with trace emulation, so a second build of it is made)
#
#   tools/run_diff_musashi.sh [trials-per-opcode] [seed] [programs]
#
# Needs: git, gcc/g++, network access to github.com. Works in a temporary directory.
set -e
TRIALS=${1:-4}
SEED=${2:-1}
PROGRAMS=${3:-20000}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${M68K_MUSASHI_WORK:-$(mktemp -d)}
if [ ! -d "$WORK/Musashi" ]; then
  git clone --quiet --depth 1 https://github.com/kstenerud/Musashi.git "$WORK/Musashi"
fi
cd "$WORK/Musashi"
[ -f m68kops.c ] || { gcc -O1 -o m68kmake m68kmake.c && ./m68kmake . m68k_in.c >/dev/null; }
[ -f m68kcpu.o ] || gcc -O2 -c m68kcpu.c m68kops.c softfloat/softfloat.c
[ -f m68kcpu_t.o ] || { gcc -O2 -DM68K_EMULATE_TRACE=1 -c m68kcpu.c -o m68kcpu_t.o && gcc -O2 -DM68K_EMULATE_TRACE=1 -c m68kops.c -o m68kops_t.o; }
SRC="$ROOT/src/cpu.cpp $ROOT/src/decoder.cpp $ROOT/src/forms.cpp $ROOT/src/format.cpp"
g++ -std=c++20 -O2 -I "$ROOT/include" -I "$ROOT/src" -I "$WORK/Musashi" "$ROOT/tools/diff_musashi.cpp" $SRC \
    m68kcpu.o m68kops.o softfloat.o -o "$WORK/m68k_diff_musashi"
g++ -std=c++20 -O2 -I "$ROOT/include" -I "$ROOT/src" -I "$WORK/Musashi" "$ROOT/tools/diff_musashi_seq.cpp" $SRC \
    m68kcpu_t.o m68kops_t.o softfloat.o -o "$WORK/m68k_diff_musashi_seq"
echo "== single instructions =="
"$WORK/m68k_diff_musashi" "$TRIALS" "$SEED"
echo "== instruction sequences =="
"$WORK/m68k_diff_musashi_seq" "$PROGRAMS" "$SEED"
