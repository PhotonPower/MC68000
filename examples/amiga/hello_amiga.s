; A small AmigaDOS program for trying out the whole tool chain (assembler -> hunk file -> emulator -> reverse engineering).
; It opens dos.library, prints a banner, runs three "commands" through a jump table and prints a sum as a decimal number.
;
;   m68ktool asm examples/amiga/hello_amiga.s --hunk hello_amiga          (no symbols in the file: the reverse engineering starts from nothing)
;   m68ktool asm examples/amiga/hello_amiga.s --hunk hello_amiga --hunk-symbols   (with the names, to compare with)
;
; Expected output of the program:
;   Hello from the MC68000 example
;   sum=150
;   words=5

DOSBASE_OFFSET = 0

        SECTION code,CODE

start:  MOVEM.L D2-D7/A2-A6,-(A7)
        MOVEA.L 4.W,A6                  ; ExecBase
        LEA     dosname(PC),A1
        MOVEQ   #0,D0
        JSR     -552(A6)                ; OpenLibrary("dos.library", 0)
        MOVE.L  D0,dosbase
        BEQ.S   .nodos
        MOVEA.L D0,A6
        JSR     -60(A6)                 ; Output()
        MOVE.L  D0,stdout
        LEA     banner(PC),A0
        BSR     print
        MOVEQ   #1,D0                   ; command 1: sum
        BSR     dispatch
        MOVEQ   #2,D0                   ; command 2: count
        BSR     dispatch
        MOVEA.L 4.W,A6
        MOVEA.L dosbase,A1
        JSR     -414(A6)                ; CloseLibrary
.nodos: MOVEQ   #0,D0
        MOVEM.L (A7)+,D2-D7/A2-A6
        RTS

; print: A0 = zero terminated string; writes it to the standard output
print:  MOVEM.L D0-D3/A0-A1/A6,-(A7)
        MOVE.L  A0,D2                   ; buffer
        MOVEQ   #0,D3
.len:   TST.B   (A0)+
        BEQ.S   .got
        ADDQ.L  #1,D3
        BRA.S   .len
.got:   MOVE.L  stdout,D1
        MOVEA.L dosbase,A6
        JSR     -48(A6)                 ; Write(stdout, buffer, length)
        MOVEM.L (A7)+,D0-D3/A0-A1/A6
        RTS

; dispatch: D0 = command number 0..2
dispatch:
        ADD.W   D0,D0
        MOVE.W  cmdtab(PC,D0.W),D0
        JMP     cmdbase(PC,D0.W)
cmdbase:
cmdtab: DC.W    cmd_hello-cmdbase
        DC.W    cmd_sum-cmdbase
        DC.W    cmd_count-cmdbase

cmd_hello:
        LEA     banner(PC),A0
        BRA     print

; cmd_sum: adds up the numbers of the table and prints "sum=<n>"
cmd_sum:
        LEA     numbers,A2
        MOVEQ   #0,D4
        MOVEQ   #4,D5                   ; five entries
.add:   ADD.W   (A2)+,D4
        DBRA    D5,.add
        MOVE.W  D4,total
        LEA     sumtext(PC),A0
        BSR     print
        MOVEQ   #0,D0
        MOVE.W  total,D0
        BSR     print_dec
        BRA     newline

; cmd_count: prints "words=<n>" (the number of table entries)
cmd_count:
        LEA     counttext(PC),A0
        BSR     print
        MOVEQ   #5,D0
        BSR     print_dec
newline:
        LEA     nltext(PC),A0
        BRA     print

; print_dec: D0 = unsigned number; prints it in decimal
print_dec:
        MOVEM.L D0-D2/A0,-(A7)
        LEA     numbuf+8,A0
        CLR.B   -(A0)                   ; terminator
.digit: DIVU    #10,D0
        SWAP    D0                      ; remainder in D0.w, quotient in the upper word
        ADD.B   #'0',D0
        MOVE.B  D0,-(A0)
        CLR.W   D0
        SWAP    D0                      ; quotient
        TST.W   D0
        BNE.S   .digit
        BSR     print
        MOVEM.L (A7)+,D0-D2/A0
        RTS

dosname:   DC.B "dos.library",0
banner:    DC.B "Hello from the MC68000 example",10,0
sumtext:   DC.B "sum=",0
counttext: DC.B "words=",0
nltext:    DC.B 10,0
        EVEN

        SECTION data,DATA
numbers:   DC.W 10,20,30,40,50

        SECTION bss,BSS
dosbase:   DS.L 1
stdout:    DS.L 1
total:     DS.W 1
numbuf:    DS.B 12
