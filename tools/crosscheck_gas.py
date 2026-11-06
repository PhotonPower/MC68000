#!/usr/bin/env python3
"""Cross-checks the assembler against GNU as (an independent MC68000 assembler).

    cmake -S . -B build && cmake --build build
    python3 tools/crosscheck_gas.py build/m68ktool

Every case is a pair: our Motorola-syntax source and the same program in GNU syntax. Both are
assembled and the bytes must be identical. Needs binutils for m68k
(Debian/Ubuntu: apt install binutils-m68k-linux-gnu).

Only spellings that both assemblers encode the same way are listed; e.g. `ADD.W #1,D0` is left out
because GNU as silently turns it into ADDI/ADDQ while this assembler keeps the ADD form
(see docs/assembler-syntax.md).
"""
import os
import subprocess
import sys
import tempfile

CASES = [
    # (our source, GNU source)
    ("MOVE.L D0,D1", "move.l %d0,%d1"),
    ("MOVE.W (A0)+,-(A7)", "move.w %a0@+,%sp@-"),
    ("MOVE.B #$FF,D3", "move.b #255,%d3"),
    ("MOVE.L #$12345678,D0", "move.l #0x12345678,%d0"),
    ("MOVE.L A0,A1", "movea.l %a0,%a1"),
    ("MOVEA.W #5,A2", "movea.w #5,%a2"),
    ("MOVEQ #-1,D5", "moveq #-1,%d5"),
    ("MOVEQ #127,D0", "moveq #127,%d0"),
    ("MOVE.W D0,$10(A1)", "move.w %d0,%a1@(16)"),
    ("MOVE.W -2(A6),D0", "move.w %a6@(-2),%d0"),
    ("MOVE.L 4(A0,D1.W),D2", "move.l %a0@(4,%d1:w),%d2"),
    ("MOVE.L (A0,D1.L),D2", "move.l %a0@(0,%d1:l),%d2"),
    ("MOVE.L (4,A0,A1.L),D2", "move.l %a0@(4,%a1:l),%d2"),
    ("MOVE.W $1234.W,D1", "move.w 0x1234:w,%d1"),
    ("MOVE.W $12345678.L,D1", "move.w 0x12345678:l,%d1"),
    ("MOVE.L D0,$00FF0000.L", "move.l %d0,0xff0000:l"),
    ("MOVE.W SR,D0", "move.w %sr,%d0"),
    ("MOVE.W D0,SR", "move.w %d0,%sr"),
    ("MOVE.W D0,CCR", "move.w %d0,%ccr"),
    ("MOVE.L A0,USP", "move.l %a0,%usp"),
    ("MOVE.L USP,A3", "move.l %usp,%a3"),
    ("ADD.W D1,D2", "add.w %d1,%d2"),
    ("ADD.L D1,(A0)", "add.l %d1,%a0@"),
    ("ADD.B (A1)+,D0", "add.b %a1@+,%d0"),
    ("ADD.W D0,A1", "adda.w %d0,%a1"),
    ("ADDA.L #8,A7", "adda.l #8,%sp"),
    ("ADDI.W #100,D0", "addi.w #100,%d0"),
    ("ADDQ.L #8,A0", "addq.l #8,%a0"),
    ("ADDQ.W #1,(A0)", "addq.w #1,%a0@"),
    ("ADDX.L D1,D2", "addx.l %d1,%d2"),
    ("ADDX.B -(A1),-(A2)", "addx.b %a1@-,%a2@-"),
    ("SUBQ.B #3,D1", "subq.b #3,%d1"),
    ("SUB.L (A0),D1", "sub.l %a0@,%d1"),
    ("SUB.W D1,(A0)", "sub.w %d1,%a0@"),
    ("SUBI.L #$10000,$20(A1)", "subi.l #0x10000,%a1@(32)"),
    ("SUBA.W D0,A5", "suba.w %d0,%a5"),
    ("AND.W D1,D2", "and.w %d1,%d2"),
    ("AND.L D1,(A2)", "and.l %d1,%a2@"),
    ("ANDI.W #$00FF,D0", "andi.w #255,%d0"),
    ("ANDI.B #$FE,CCR", "andi.b #254,%ccr"),
    ("ORI.W #$0700,SR", "ori.w #0x700,%sr"),
    ("OR.B D1,D2", "or.b %d1,%d2"),
    ("EOR.L D1,D2", "eor.l %d1,%d2"),
    ("EORI.B #1,(A0)", "eori.b #1,%a0@"),
    ("CMP.W D1,D2", "cmp.w %d1,%d2"),
    ("CMP.L A1,D2", "cmp.l %a1,%d2"),
    ("CMPA.L D0,A1", "cmpa.l %d0,%a1"),
    ("CMPI.B #10,(A0)+", "cmpi.b #10,%a0@+"),
    ("CMPM.W (A0)+,(A1)+", "cmpm.w %a0@+,%a1@+"),
    ("CLR.L D0", "clr.l %d0"),
    ("NEG.W (A0)", "neg.w %a0@"),
    ("NEGX.B D1", "negx.b %d1"),
    ("NOT.L D3", "not.l %d3"),
    ("TST.W D0", "tst.w %d0"),
    ("TST.L (A0)+", "tst.l %a0@+"),
    ("EXT.W D0", "ext.w %d0"),
    ("EXT.L D0", "ext.l %d0"),
    ("SWAP D3", "swap %d3"),
    ("EXG D0,D1", "exg %d0,%d1"),
    ("EXG A0,A1", "exg %a0,%a1"),
    ("EXG D0,A1", "exg %d0,%a1"),
    ("LEA (A0),A1", "lea %a0@,%a1"),
    ("LEA 8(A0),A1", "lea %a0@(8),%a1"),
    ("LEA $1000.L,A0", "lea 0x1000:l,%a0"),
    ("PEA (A0)", "pea %a0@"),
    ("LINK A6,#-4", "link %a6,#-4"),
    ("UNLK A6", "unlk %a6"),
    ("MULU D1,D2", "mulu.w %d1,%d2"),
    ("MULS.W (A0),D2", "muls.w %a0@,%d2"),
    ("DIVU D1,D2", "divu.w %d1,%d2"),
    ("DIVS #7,D2", "divs.w #7,%d2"),
    ("CHK D1,D2", "chk.w %d1,%d2"),
    ("ABCD D1,D2", "abcd %d1,%d2"),
    ("SBCD -(A1),-(A2)", "sbcd %a1@-,%a2@-"),
    ("NBCD D0", "nbcd %d0"),
    ("TAS (A0)", "tas %a0@"),
    ("ASL.W #1,D0", "asl.w #1,%d0"),
    ("ASR.L D1,D2", "asr.l %d1,%d2"),
    ("LSL.B #8,D0", "lsl.b #8,%d0"),
    ("LSR.W (A0)", "lsr.w %a0@"),
    ("ROL.L #4,D0", "rol.l #4,%d0"),
    ("ROR.W D1,D0", "ror.w %d1,%d0"),
    ("ROXL.W #1,D0", "roxl.w #1,%d0"),
    ("ROXR.B D1,D0", "roxr.b %d1,%d0"),
    ("BTST D0,D1", "btst %d0,%d1"),
    ("BTST #7,(A0)", "btst #7,%a0@"),
    ("BSET #31,D1", "bset #31,%d1"),
    ("BCLR D2,(A0)", "bclr %d2,%a0@"),
    ("BCHG #0,D0", "bchg #0,%d0"),
    ("MOVEM.L D0-D7/A0-A6,-(A7)", "moveml %d0-%d7/%a0-%a6,%sp@-"),
    ("MOVEM.L (A7)+,D0-D7/A0-A6", "moveml %sp@+,%d0-%d7/%a0-%a6"),
    ("MOVEM.W D0/D2/A1,(A0)", "movemw %d0/%d2/%a1,%a0@"),
    ("MOVEM.L 4(A0),D0-D3", "moveml %a0@(4),%d0-%d3"),
    ("MOVEP.W D0,4(A1)", "movep.w %d0,%a1@(4)"),
    ("MOVEP.L 2(A1),D3", "movep.l %a1@(2),%d3"),
    ("JMP (A0)", "jmp %a0@"),
    ("JSR $1000.L", "jsr 0x1000:l"),
    ("RTS", "rts"), ("RTE", "rte"), ("RTR", "rtr"), ("NOP", "nop"), ("RESET", "reset"),
    ("ILLEGAL", "illegal"), ("TRAPV", "trapv"), ("TRAP #15", "trap #15"), ("STOP #$2700", "stop #0x2700"),
    ("SEQ D0", "seq %d0"), ("SNE (A0)", "sne %a0@"), ("ST D1", "st %d1"), ("SF D1", "sf %d1"),
    # branches and PC-relative addressing with labels
    ("BRA.S next\nNOP\nnext: NOP", "bra.s next\nnop\nnext: nop"),
    ("BRA.W next\nNOP\nnext: NOP", "bra.w next\nnop\nnext: nop"),
    ("BRA next\nNOP\nnext: NOP", "bras next\nnop\nnext: nop"),
    ("start: NOP\nBRA start", "start: nop\nbras start"),
    ("BEQ.S l\nNOP\nl: RTS", "beq.s l\nnop\nl: rts"),
    ("BSR sub\nRTS\nsub: RTS", "bsr.s sub\nrts\nsub: rts"),
    ("BNE.W far\nDS.W 100\nfar: RTS", "bne.w far\n.skip 200\nfar: rts"),
    ("BGT far\nDS.B 200\nfar: RTS", "bgt.w far\n.skip 200\nfar: rts"),
    ("loop: DBRA D0,loop", "loop: dbra %d0,loop"),
    ("DBEQ D1,l\nNOP\nl: NOP", "dbeq %d1,l\nnop\nl: nop"),
    ("LEA data(PC),A0\nRTS\ndata: DC.W 1", "lea data(%pc),%a0\nrts\ndata: .word 1"),
    ("MOVE.W data(PC),D0\nRTS\ndata: DC.W 1", "move.w data(%pc),%d0\nrts\ndata: .word 1"),
    ("MOVE.W data(PC,D1.W),D0\nRTS\ndata: DC.W 1", "move.w %pc@(data,%d1:w),%d0\nrts\ndata: .word 1"),
    ("DC.B 1,2,3\nDC.W $1234\nDC.L $DEADBEEF", ".byte 1,2,3\n.word 0x1234\n.long 0xdeadbeef"),
    ("DC.B \"AB\",0\nEVEN\nDS.W 2", ".ascii \"AB\"\n.byte 0\n.align 2\n.skip 4"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ours(tool, src, tmp):
    path = os.path.join(tmp, "a.s")
    out = os.path.join(tmp, "a.bin")
    # labels must start in column 1, instructions are indented
    lines = []
    for ln in src.split("\n"):
        lines.append(ln if ":" in ln.split(" ")[0] else "  " + ln)
    open(path, "w").write("\n".join(lines) + "\n")
    r = run([tool, "asm", path, "-o", out])
    if r.returncode != 0:
        return None, r.stderr.strip()
    return open(out, "rb").read(), ""


def gas(src, tmp):
    path = os.path.join(tmp, "g.s")
    obj = os.path.join(tmp, "g.o")
    out = os.path.join(tmp, "g.bin")
    open(path, "w").write(".text\n" + src + "\n")
    r = run(["m68k-linux-gnu-as", "-m68000", path, "-o", obj])
    if r.returncode != 0:
        return None, r.stderr.strip()
    run(["m68k-linux-gnu-objcopy", "-O", "binary", "-j", ".text", obj, out])
    return open(out, "rb").read(), ""


def main():
    tool = sys.argv[1]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for mine, theirs in CASES:
            a, ea = ours(tool, mine, tmp)
            b, eb = gas(theirs, tmp)
            if a is None or b is None or a != b:
                bad += 1
                print(f"MISMATCH: {mine!r}")
                print(f"   ours: {a.hex() if a is not None else 'ERROR ' + ea}")
                print(f"   gas:  {b.hex() if b is not None else 'ERROR ' + eb}")
    print(f"{len(CASES)} cases, {bad} mismatches")
    sys.exit(1 if bad else 0)


main()
