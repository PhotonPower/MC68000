; a self-extracting program in two stages, like many real packers: the decruncher first copies itself to $50000 and runs there, and from there it unpacks to $30000
        SECTION code,CODE
start:  LEA     decrunch(PC),A0
        LEA     $50000,A1
        MOVE.W  #(decrunch_end-decrunch)/2-1,D0
.c:     MOVE.W  (A0)+,(A1)+
        DBRA    D0,.c
        LEA     packed(PC),A0
        LEA     $30000,A1
        JSR     $50000
        MOVE.W  #$0F00,$DFF180          ; the colour flash of the packer
        JMP     $30000

; decrunch: A0 = packed data, A1 = destination (position independent: it runs from the copy)
decrunch:
.next:  MOVEQ   #0,D0
        MOVE.B  (A0)+,D0
        CMP.B   #$FF,D0
        BEQ.S   .done
        TST.B   D0
        BMI.S   .match
.lit:   MOVE.B  (A0)+,(A1)+
        DBRA    D0,.lit
        BRA.S   .next
.match: AND.W   #$7F,D0
        ADDQ.W  #2,D0
        MOVEQ   #0,D1
        MOVE.B  (A0)+,D1
        MOVEA.L A1,A2
        SUBA.L  D1,A2
.copy:  MOVE.B  (A2)+,(A1)+
        DBRA    D0,.copy
        BRA.S   .next
.done:  RTS
decrunch_end:

packed:
