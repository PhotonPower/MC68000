; the program that gets packed: it lives at $30000 and prints a message
        ORG     $30000
entry:  MOVEA.L 4.W,A6
        LEA     dosname(PC),A1
        MOVEQ   #0,D0
        JSR     -552(A6)                ; OpenLibrary
        MOVE.L  D0,dosbase
        BEQ.S   .exit
        MOVEA.L D0,A6
        JSR     -60(A6)                 ; Output
        MOVE.L  D0,outh
        LEA     msg(PC),A0
        BSR     print
        MOVEA.L 4.W,A6
        MOVEA.L dosbase,A1
        JSR     -414(A6)                ; CloseLibrary
.exit:  MOVEQ   #0,D0
        RTS

print:  MOVEM.L D0-D3/A0-A1/A6,-(A7)
        MOVE.L  A0,D2
        MOVEQ   #0,D3
.len:   TST.B   (A0)+
        BEQ.S   .got
        ADDQ.L  #1,D3
        BRA.S   .len
.got:   MOVE.L  outh,D1
        MOVEA.L dosbase,A6
        JSR     -48(A6)                 ; Write
        MOVEM.L (A7)+,D0-D3/A0-A1/A6
        RTS

dosname: DC.B   "dos.library",0
msg:     DC.B   "Unpacked and running",10,0
        EVEN
dosbase: DC.L   0
outh:    DC.L    0
buffer:  DS.B   4096
