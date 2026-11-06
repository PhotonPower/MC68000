; A relocatable program with sections: assemble it to an AmigaDOS load file with
;     m68ktool asm examples/sections.s --hunk sections.hunk --hunk-symbols
; and load it with `load_file` (type hunk) in `m68ktool serve`, or with `l sections.hunk` in `m68ktool monitor`.
; It adds up the words of a table (through a relocated pointer) and stores the sum in a BSS variable.
        SECTION code,CODE
        XDEF    start
start:  MOVE.L  #table,A0             ; a long immediate holding an address: relocated by the loader
        LEA     total,A1              ; long absolute address into the BSS hunk: relocated as well
        MOVEQ   #3,D1                 ; four words
        MOVEQ   #0,D0
loop:   ADD.W   (A0)+,D0
        DBRA    D1,loop
        MOVE.W  D0,(A1)               ; total = 10+20+30+40
        MOVE.L  #ptrs,D2
        RTS

        SECTION data,DATA
table:  DC.W    10,20,30,40
ptrs:   DC.L    table,start           ; pointers inside the data hunk (RELOC32 entries)

        SECTION bss,BSS
total:  DS.W    1
