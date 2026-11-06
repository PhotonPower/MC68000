; hello.s - minimal MC68000 example (Motorola syntax)
;
;   m68ktool asm examples/hello.s -l -                 assemble, listing on the screen
;   m68ktool asm examples/hello.s -o hello.bin
;   m68ktool dis hello.bin --org 0x1000 -c examples/hello.comments.txt
;
; It copies a zero-terminated string to a memory-mapped output port. The port address is an
; assumption of this example, not a real machine.

OUTPORT     EQU     $00FF0000
CR          EQU     13
LF          EQU     10

            ORG     $1000

start:      LEA     message(PC),A0      ; A0 -> text
.next:      MOVE.B  (A0)+,D0            ; fetch the next character
            BEQ.S   .done               ; zero byte ends the text
            MOVE.B  D0,OUTPORT          ; write it to the port
            BRA.S   .next
.done:      RTS

message:    DC.B    "Hello, 68000!",CR,LF,0
            EVEN
