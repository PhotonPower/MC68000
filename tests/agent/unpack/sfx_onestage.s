; a self-extracting program: unpacks to $30000 and jumps there
        SECTION code,CODE
start:  LEA     packed(PC),A0
        LEA     $30000,A1
        BSR     decrunch
        MOVE.W  #$0F00,$DFF180          ; the colour flash of the packer
        JMP     $30000

; decrunch: A0 = packed data, A1 = destination
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

packed:
