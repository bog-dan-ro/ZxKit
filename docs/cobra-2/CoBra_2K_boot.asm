; CoBra logo code disassembled and commented on 5 February 2022 by sorin@cismas.org
; about 36 years after it was written (the original source code was lost).
;
; The CoBra logo appearing on posters and documentation was designed by architect
; Alexandru Antal.  We wanted to have that logo on the CoBra start-up screen.
; The original Sinclair ZX Spectrum ROM had 1170 unused bytes, 0386eh - 03cffh.
; The CoBra logo generation and rotation takes 820 bytes, consisting of:
;   608 bytes table                         (address 00122h - 00381h)
;   129 bytes code for logo generation      (address 00026h - 000afh)
;    34 bytes code for full screen rotation (address 000cdh - 000efh)
;    49 bytes code for 4 pixel rotation     (address 000f1h - 00122h)
; The remaining 1170 - 820 = 350 bytes were sufficient for CoBra initialization.
;
; After support for CP/M was added, the boot code become larger and the 1170 bytes
; were no longer sufficient.  A decision was made to add a 2KB boot ROM.
; Initially, the boot ROM did not have the CoBra logo.  It was added at the last
; minute because it would fit in the 2KB boot ROM.
;
; To reduce the size, the CoBra logo is compressed.  The width of the letters
; and snake in the vertical sections is 8 pixels but is wider in the curved areas.
; There are many instances of 8 to 16 consecutive INK pixels in 2 consecutive bytes.
; Here are the 44 possible patterns:
;   01fe  01ff  03ff  07ff  0fff  1fff  3fff  7fff  ffff
;   03fc  03fe  07fe  0ffe  1ffe  3ffe  7ffe  fffe
;   07f8  07fc  0ffc  1ffc  3ffc  7ffc  fffc
;   0ff0  0ff8  1ff8  3ff8  7ff8  fff8
;   1fe0  1ff0  3ff0  7ff0  fff0
;   3fc0  3fe0  7fe0  ffe0
;   7f80  7fc0  ffc0
;   ff00  ff80
;
; CoBra_tbl has 3 sections.  The first (00122h - 0034dh) holds information
; about where these patterns are present, the second (0034e - 00379h) has
; final additions, and the third holds the 8 bytes for the color pattern.
;
; The structure of the first section is:
;     1 byte   Number of Word patterns (N)
;     2 bytes  Base Address (lowest) of the first instance
;   N-1 bytes  Adress Increments
; The Increments are only 8 bits.  In case the delta between two consecutive
; addresses is larger than 255, an intermediate write is done to a location that
; will be later overwritten.  The patterns are ordered as in the above table,
; vertical first, then horizontal, as the "Word_xxxx" table labels below show.
; There are 3 nested loops, the outer loop goes through the columns, the middle
; loop through the rows, and the inner loop through the pattern instances.
;
; The second section is a list of byte patterns that could not be generated with
; the 44 16-bit patterns.  There are 11 of them, the first 5 are in the 6KB
; screen area and remaining 6 are color attributes (blue crown, yellow eye,
; red tongue, leading edge of cyan C).  The structure of the second section is:
;     1 byte   Number of consecutive Byte patterns
;     2 bytes  Base Address (lowest) of the first instance
;     1 byte   Byte pattern
;
; CoBra logo generation goes through 4 stages:
;    1. Fill color attributes in the middle 3rd of the screen: first 12 colums
;       with magenta INK (snake) and the last 20 with cyan INK (COBRA).
;    2. Use section 1 of CoBra_tbl to unpack the 44 16-bit patterns.
;    3. Use section 2 of CoBra_tbl to unpack the finishing bytes.
;    4. Use section 3 of CoBra_tbl to write the color pattern.
;
; CoBra logo rotation is not synchronized with the frame interrupt, which causes
; tearing.  This was by design, not a mistake.  Rotation starts from the bottom
; line and propagates up.  Tearing will give the sensation of undulating motion.
; CoBra rotation is done in 3 steps and is repeated 32 times to bring it back in
; the same position:
;    1. Rotate left CoBra logo 4 pixels.
;    2. Rotate left color attributes.
;    3. Rotate left CoBra logo 4 pixels.

        org     00000h

start:
; initialize CoBra
        ld a,080h       ;0000  3e 80   ;| R[7]=1
        ld r,a          ;0002  ed 4f   ;| 
        ld hl,0069ch    ;0004  21 9c 06;| (0f000h)=0069ch
        ld (0f000h),hl  ;0007  22 00 f0;| 
        ld sp,0ffffh    ;000a  31 ff ff;  initialize SP=ffff
        ld h,0c0h       ;000d  26 c0   ;| 0c000h is video base address
        ld l,000h       ;000f  2e 00   ;| 
        ld (hl),000h    ;0011  36 00   ;  (0c000h)=0
        ld d,h          ;0013  54      ;|  0c001h
        ld e,001h       ;0014  1e 01   ;| 
        ld bc,01affh    ;0016  01 ff 1a;  01b00h is video memory size
        ldir            ;0019  ed b0   ;  clear video memory
        ld a,092h       ;001b  3e 92   ;| out(092dfh)=092h
        out (0dfh),a    ;001d  d3 df   ;| 
        ld a,040h       ;001f  3e 40   ;| out(040feh)=040h
        out (0feh),a    ;0021  d3 fe   ;| 

        ld bc,078feh    ;0023  01 fe 78;  NOT NEEDED
; The initial boot did not have the CoBra logo and the keyboard test
; that starts at 000adh was starting here. This is a leftover instruction
; after the CoBra logo was added to the CoBra boot code at the last minute

; generate CoBra logo
        ld h,0d9h       ;0026  26 d9   ;| 0d900h is start Address
        ld l,000h       ;0028  2e 00   ;| of middle 3rd of screen attributes
        ld b,008h       ;002a  06 08   ;  8 rows
l002ch:                                ;
        push bc         ;002c  c5      ;  save BC  <-- <-- <-- <-- <--,
        ld bc,00c05h    ;002d  01 05 0c;| fill first 12 bytes with 5  |
l0030h:                                ;| cyan INK on black PAPER     ^
        ld (hl),c       ;0030  71      ;|                             |
        inc hl          ;0031  23      ;|                             ^
        djnz l0030h     ;0032  10 fc   ;|                             |
        ld bc,01403h    ;0034  01 03 14;| fill next 20 bytes with 3   ^
l0037h:                                ;| magenta INK on black PAPER  |
        ld (hl),c       ;0037  71      ;|                             ^
        inc hl          ;0038  23      ;|                             |
        djnz l0037h     ;0039  10 fc   ;|                             ^
        pop bc          ;003b  c1      ;  restore BC                  |
        djnz l002ch     ;003c  10 ee   ;  loop 8 times --> --> --> -->'
        exx             ;003e  d9      ;  
        ld hl,CoBra_tbl ;003f  21 22 01;  load CoBra_tbl base address to H'L'
        exx             ;0042  d9      ;  
        ld h,0cbh       ;0043  26 cb   ;| load HL with first Base Address
        ld l,0b9h       ;0045  2e b9   ;| 
        ld de,001feh    ;0047  11 fe 01;  load DE with Word 001feh
        ld a,004h       ;004a  3e 04   ;  load A with Number of Words
        ld b,009h       ;004c  06 09   ;  9 outer loops
l004eh:                                ;
        push bc         ;004e  c5      ;  save BC  <-- <-- <-- <-- <-- <-- <-,
        ld b,a          ;004f  47      ;  Number of Words                    |
l0050h:                                ;                                     ^
        ld (hl),d       ;0050  72      ;  write upper byte <-- <--,<-- <--,  |
        inc hl          ;0051  23      ;| write lower byte        ^       ^  ^
        ld (hl),e       ;0052  73      ;|                         |       |  |
        exx             ;0053  d9      ;                          ^       ^  ^
        ld a,(hl)       ;0054  7e      ;  load A with Increment   |       |  |
        inc hl          ;0055  23      ;  increment H'L' pointer  ^       ^  ^
        exx             ;0056  d9      ;                          |       |  |
        ld c,a          ;0057  4f      ;  save byte read to C     ^       ^  ^
        add a,l         ;0058  85      ;| add hl,a                |       |  |
        ld l,a          ;0059  6f      ;|                         ^       ^  ^
        jr nc,l005dh    ;005a  30 01   ;|                         |       |  |
        inc h           ;005c  24      ;|                         ^       ^  ^
l005dh:                                ;                          |       |  |
        djnz l0050h     ;005d  10 f1   ;  close inner loop --> -->'       ^  ^
        ld b,c          ;005f  41      ;  byte read from H'L' pointer     |  |
        exx             ;0060  d9      ;                                  ^  ^
        ld e,(hl)       ;0061  5e      ;| load DE with next Base Address  |  |
        inc hl          ;0062  23      ;|                                 ^  ^
        ld d,(hl)       ;0063  56      ;|                                 |  |
        inc hl          ;0064  23      ;|                                 ^  ^
        push de         ;0065  d5      ;  save DE                         |  |
        exx             ;0066  d9      ;                                  ^  ^
        pop hl          ;0067  e1      ;  HL contains next Base Address   |  |
        sla e           ;0068  cb 23   ;| shift left arithmetic DE        ^  ^
        rl d            ;006a  cb 12   ;|                                 |  |
        jr nc,l0050h    ;006c  30 e2   ;  close middle loop -> --> --> -->'  ^
        rr d            ;006e  cb 1a   ;| unshift DE                         |
        rr e            ;0070  cb 1b   ;|                                    ^
        ld d,0ffh       ;0072  16 ff   ;  NOT NEEDED (redundant)             |
l0074h:                                ;                                     ^
        sla d           ;0074  cb 22   ;| determine next Word                |
        rl e            ;0076  cb 13   ;| with one extra 1 bit               ^
        jr c,l0074h     ;0078  38 fa   ;| right aligned                      |
        ld a,d          ;007a  7a      ;|                                    ^
        cpl             ;007b  2f      ;|                                    |
        ld d,a          ;007c  57      ;|                                    ^
        ld e,0ffh       ;007d  1e ff   ;|                                    |
        ld a,c          ;007f  79      ;  Number of Words/Bytes              ^
        pop bc          ;0080  c1      ;  restore BC                         |
        djnz l004eh     ;0081  10 cb   ;  close outer loop --> --> --> --> --'
        ld e,l          ;0083  5d      ;| save Address to DE
        ld d,h          ;0084  54      ;| 
        exx             ;0085  d9      ;| copy H'L' pointer to HL
        push hl         ;0086  e5      ;| 
        exx             ;0087  d9      ;| 
        pop hl          ;0088  e1      ;| 
        ld b,00bh       ;0089  06 0b   ;  11 finishing loops (should have been 12)
l008bh:                                ;
        push bc         ;008b  c5      ;  save BC <-- <-- <-- <-- <-- <,
        ld b,a          ;008c  47      ;  Number of Bytes              ^
        ld a,(hl)       ;008d  7e      ;| load A with Byte             |
        inc hl          ;008e  23      ;|                              ^
l008fh:                                ;                               |
        ld (de),a       ;008f  12      ;| write Byte B times           ^
        inc de          ;0090  13      ;|                              |
        djnz l008fh     ;0091  10 fc   ;|                              ^
        ld a,(hl)       ;0093  7e      ;| load A with Number of Bytes  |
        inc hl          ;0094  23      ;|                              ^
        ld e,(hl)       ;0095  5e      ;| load DE with next Address    |
        inc hl          ;0096  23      ;|                              ^
        ld d,(hl)       ;0097  56      ;|                              |
        inc hl          ;0098  23      ;|                              ^
        pop bc          ;0099  c1      ;  restore BC                   |
        djnz l008bh     ;009a  10 ef   ;  close finishing loop -> --> -'
        ld bc,00008h    ;009c  01 08 00;| add color pattern
        ld de,0da8ch    ;009f  11 8c da;| 
        ld hl,Colors    ;00a2  21 7a 03;| 
        ldir            ;00a5  ed b0   ;| 

; screen cycle
l00a7h:
        ld d,007h       ;00a7  16 07   ;  7 outer loops  <-- <-- <-- <-- <-- <-- <--,
        ld bc,00000h    ;00a9  01 00 00;  256 middle (C) and 256 inner (B) loops    ^
l00ach:                                ;  which introduces a delay of 19.4 seconds  | cycles
        push bc         ;00ac  c5      ;  save BC  <-- <-- <-- <-- <-- <-- <-,-,-,  ^   11
; keyboard test                        ;                                     | | |  |
        ld bc,078feh    ;00ad  01 fe 78;                                     ^ ^ ^  ^   10
        in a,(c)        ;00b0  ed 78   ;  A=in(078feh), rows KAf,KAa,KA9,KA8 | | |  |   12
        nop             ;00b2  00      ;| old instruction(s) nullified       ^ ^ ^  ^    4
        nop             ;00b3  00      ;| at the last minute                 | | |  |    4
        jp l0705h       ;00b4  c3 05 07;  jump to partial keyboard test      ^ ^ ^  ^   38
KB_return:                             ;  which returns here                 | | |  |
        rra             ;00b7  1f      ;  rotate right all, CY=or(col 2)     ^ ^ ^  ^    4
        jp nc,l03d8h    ;00b8  d2 d8 03;  jump if "M,"E","D","X" pressed     | | |  |   10
        rra             ;00bb  1f      ;  rotate right all, CY=or(col 3)     ^ ^ ^  ^    4
        jp nc,l06dfh    ;00bc  d2 df 06;  jump if "N,"R","F","C" pressed     | | |  |   10
        rra             ;00bf  1f      ;  rotate right all, CY=or(col 4)     ^ ^ ^  ^    4
        jp nc,l0719h    ;00c0  d2 19 07;  jump if "B,"T","G","V" pressed     | | |  |   10
        rra             ;00c3  1f      ;  rotate right all, CY=or(col 5)     ^ ^ ^  ^    4
        pop bc          ;00c4  c1      ;  restore BC                         | | |  |   10
        djnz l00ach     ;00c5  10 e5   ;  close inner loop --> --> --> --> --' ^ ^  ^   13,8: 148*256-5=37883
        dec c           ;00c7  0d      ;                                       | |  |    4
        jr nz,l00ach    ;00c8  20 e2   ;  close middle loop -> --> --> --> --> ' ^  ^   12,7: 37899*256-5=9702139
        dec d           ;00ca  15      ;                                         |  |    4
        jr nz,l00ach    ;00cb  20 df   ;  close outer loop --> --> --> --> --> --'  ^   12,7: 9702155*7-5=67915080
; rotate left CoBra logo, full screen  ;                                            ^
        ld b,020h       ;00cd  06 20   ;  32 character columns                      ^
l00cfh:                                ;                                            |
        push bc         ;00cf  c5      ;  save BC <-- <-- <-- <-- <-- <-- <-- <,    ^
        call Rotate     ;00d0  cd f1 00;  rotate left CoBra logo 4 pixels      |    |
; rotate left CoBra logo attributes    ;                                       ^    ^
        ld h,0d9h       ;00d3  26 d9   ;| 0d901h is start Address + 1          |    |
        ld l,001h       ;00d5  2e 01   ;| of middle 3rd of screen attributes   ^    ^
        ld b,008h       ;00d7  06 08   ;  rotate left 8 attribute rows         |    |
l00d9h:                                ;                                       ^    ^
        push bc         ;00d9  c5      ;  save BC <-- <-- <-- <-- <-- <,       |    |
        ld bc,0001fh    ;00da  01 1f 00;  32 bytes per attribute row   ^       ^    ^
        ld d,h          ;00dd  54      ;| load DE with HL Address - 1  |       |    |
        ld e,l          ;00de  5d      ;|                              ^       ^    ^
        dec e           ;00df  1d      ;|                              |       |    |
        ld a,(de)       ;00e0  1a      ;  save first byte to A         ^       ^    ^
        ldir            ;00e1  ed b0   ;  rotate left attribute row    |       |    |
        ld (de),a       ;00e3  12      ;  write last byte with A       ^       ^    ^
        inc l           ;00e4  2c      ;  next L                       |       |    |
        inc e           ;00e5  1c      ;  next E                       ^       ^    ^
        pop bc          ;00e6  c1      ;  restore BC                   |       |    |
        djnz l00d9h     ;00e7  10 f0   ;  close attribute row loop -> -'       ^    ^
        call Rotate     ;00e9  cd f1 00;  rotate left CoBra logo 4 pixels      |    |
        pop bc          ;00ec  c1      ;  restore BC                           ^    ^
        djnz l00cfh     ;00ed  10 e0   ;  close character rotate left loop -> -'    |
        jr l00a7h       ;00ef  18 b6   ;  close screen cycle --> --> --> --> --> -->'

Rotate:
        ld b,004h       ;00f1  06 04   ;  rotate left CoBra logo 4 pixels
l00f3h:                                ;
        push bc         ;00f3  c5      ;  save BC <-- <-- <-- <-- <-- <-- <-- <,
        ld h,0cfh       ;00f4  26 cf   ;| 0cfffh is end Address                ^
        ld l,0ffh       ;00f6  2e ff   ;| of middle 3rd of screen              |
        ld b,008h       ;00f8  06 08   ;  rotate left 8 character rows         ^
l00fah:                                ;                                       |
        push bc         ;00fa  c5      ;  save BC <-- <-- <-- <-- <-- <-- <-,  ^
        ld b,008h       ;00fb  06 08   ;  8 screen lines per character row  |  |
l00fdh:                                ;                                    ^  ^
        push bc         ;00fd  c5      ;  save BC <-- <-- <-- <-- <--,      |  |
        ld d,h          ;00fe  54      ;  save Address to DE         ^      ^  ^
        ld e,l          ;00ff  5d      ;                             |      |  |
        ld b,020h       ;0100  06 20   ;  32 bytes per screen line   ^      ^  ^
        scf             ;0102  37      ;  CY=1                       |      |  |
l0103h:                                ;                             ^      ^  ^
        rl (hl)         ;0103  cb 16   ;| rotate left                |      |  |
        dec hl          ;0105  2b      ;|                            ^      ^  ^
        djnz l0103h     ;0106  10 fb   ;|                            |      |  |
        jr c,l010dh     ;0108  38 03   ;  jump if 1 was rotated out  ^      ^  ^
        ld a,(de)       ;010a  1a      ;| otherwise                  |      |  |
        dec a           ;010b  3d      ;| clear bit on the right     ^      ^  ^
        ld (de),a       ;010c  12      ;|                            |      |  |
l010dh:                                ;                             ^      ^  ^
        pop bc          ;010d  c1      ;  restore BC                 |      |  |
        scf             ;010e  37      ;| CY=0                       ^      ^  ^
        ccf             ;010f  3f      ;|                            |      |  |
        ld de,000e0h    ;0110  11 e0 00;| load HL with end Address   ^      ^  ^
        sbc hl,de       ;0113  ed 52   ;| of previous screen line    |      |  |
        djnz l00fdh     ;0115  10 e6   ;  close screen line loop  -->'      ^  ^
        ld bc,007e0h    ;0117  01 e0 07;| load HL with end Address          |  |
        add hl,bc       ;011a  09      ;| of previous character row         ^  ^
        pop bc          ;011b  c1      ;  restore BC                        |  |
        djnz l00fah     ;011c  10 dc   ;  close character row loop -> --> --'  ^
        pop bc          ;011e  c1      ;  restore BC                           |
        djnz l00f3h     ;011f  10 d2   ;  close rotate left loop  --> --> --> -'
        ret             ;0121  c9      ;  

CoBra_tbl:
; outer loop 0 writes Words with 8 consecutive 1's (8 middle loops)
Word_01fe:
;       defb    004h   (initial value in A)            ; Number of Words
;       defw    0cbb9h (initial value in HL)           ; Base Address
        defb    0ffh,0e0h,0ffh
Word_03fc:
        defb    01fh                                   ; Number of Words
        defw    0c8b1h                                 ; Base Address
        defb    045h,0b9h,022h,002h,01fh,0a2h,016h,022h; Increments
        defb    002h,01fh,0a2h,016h,025h,015h,009h,075h
        defb    043h,025h,0bch,002h,019h,025h,0bch,002h
        defb    019h,007h,01dh,0d9h,007h,01dh
Word_07f8:
        defb    00ch                                   ; Number of Words
        defw    0c89ah                                 ; Base Address
        defb    007h,0a5h,090h,0ffh,088h,0ffh,0e5h,0ffh; Increments
        defb    0ffh,031h,007h
Word_0ff0:
        defb    00ch                                   ; Number of Words
        defw    0c882h                                 ; Base Address
        defb    0ffh,0ffh,0ffh,06dh,07fh,009h,007h,0f7h; Increments
        defb    007h,0dfh,0ffh
Word_1fe0:
        defb    009h                                   ; Number of Words
        defw    0c8cch                                 ; Base Address
        defb    07bh,092h,0ffh,0ffh,0ebh,0ffh,0ffh,0c4h; Increments
Word_3fc0:
        defb    010h                                   ; Number of Words
        defw    0c8afh                                 ; Base Address
        defb    0fch,002h,0fch,002h,0fch,002h,0fch,002h; Increments
        defb    0fch,002h,0fch,002h,00bh,0f3h,00bh
Word_7f80:
        defb    006h                                   ; Number of Words
        defw    0c887h                                 ; Base Address
        defb    0c0h,0ffh,0ffh,072h,0ffh               ; Increments
Word_ff00:
        defb    00bh                                   ; Number of Words
        defw    0c846h                                 ; Base Address
        defb    074h,06eh,03dh,052h,08eh,09fh,0feh,0ddh; Increments
        defb    0ffh,082h

; outer loop 1 writes Words with 9 consecutive 1's (8 middle loops)
Word_01ff:
        defb    011h                                   ; Number of Words
        defw    0c88ch                                 ; Base Address
        defb    062h,055h,020h,0e1h,070h,08eh,0dah,0a3h; Increments
        defb    07ah,05fh,09fh,05fh,0cch,02ah,028h,008h
Word_03fe:
        defb    00bh                                   ; Number of Words
        defw    0c862h                                 ; Base Address
        defb    0e5h,0ffh,0ffh,059h,0f7h,007h,0a5h,070h; Increments
        defb    0b6h,0d1h
Word_07fc:
        defb    013h                                   ; Number of Words
        defw    0c8d1h                                 ; Base Address
        defb    002h,002h,001h,088h,037h,007h,02eh,090h; Increments
        defb    03fh,0a5h,08bh,0bfh,002h,0f9h,0ffh,0ffh; Increments
        defb    002h,002h
Word_0ff8:
        defb    008h                                   ; Number of Words
        defw    0cad1h                                 ; Base Address
        defb    076h,090h,0edh,09ah,02eh,0e8h,007h     ; Increments
Word_1ff0:
        defb    009h                                   ; Number of Words
        defw    0c9cch                                 ; Base Address
        defb    07bh,086h,078h,092h,0ebh,09fh,027h,0fch; Increments
Word_3fe0:
        defb    00bh                                   ; Number of Words
        defw    0c8ach                                 ; Base Address
        defb    022h,00bh,0f3h,0fch,0ffh,0fah,0ffh,0c7h; Increments
        defb    0ffh,01ch
Word_7fc0:
        defb    003h                                   ; Number of Words
        defw    0cdbbh                                 ; Base Address
        defb    0b4h,0f6h                              ; Increments
Word_ff80:
        defb    010h                                   ; Number of Words
        defw    0c82ah                                 ; Base Address
        defb    0c6h,054h,003h,0fdh,072h,038h,0f5h,0dch; Increments
        defb    07bh,030h,002h,0f8h,032h,0a6h,026h

; outer loop 2 writes Words with 10 consecutive 1's (7 middle loops)
Word_03ff:
        defb    00ch                                   ; Number of Words
        defw    0c866h                                 ; Base Address
        defb    028h,0fch,0bbh,0ffh,0deh,0fbh,0e4h,0bah; Increments
        defb    008h,0fah,007h
Word_07fe:
        defb    001h                                   ; Number of Words
        defw    0cf46h                                 ; Base Address
Word_0ffc:
        defb    001h                                   ; Number of Words
        defw    0ce46h                                 ; Base Address
Word_1ff8:
        defb    005h                                   ; Number of Words
        defw    0cacch                                 ; Base Address
        defb    0ffh,0fah,07eh,045h
Word_3ff0:
        defb    001h                                   ; Number of Words
        defw    0cc46h                                 ; Base Address
Word_7fe0:
        defb    001h                                   ; Number of Words
        defw    0cb46h                                 ; Base Address
Word_ffc0:
        defb    00ch                                   ; Number of Words
        defw    0c891h                                 ; Base Address
        defb    0b6h,0fdh,0e1h,09bh,07eh,085h,0fdh,0ffh; Increments
        defb    061h,079h,02dh

; outer loop 3 writes Words with 11 consecutive 1's (6 middle loops)
Word_07ff:
        defb    00ah                                   ; Number of Words
        defw    0c924h                                 ; Base Address
        defb    06ah,0fch,057h,0ech,0f5h,0fbh,008h,056h; Increments
        defb    0abh
Word_0ffe:
        defb    002h                                   ; Number of Words
        defw    0cbcch                                 ; Base Address
        defb    0bfh                                   ; Increments
Word_1ffc:
        defb    004h                                   ; Number of Words
        defw    0caedh                                 ; Base Address
        defb    0e1h,0bfh,0ddh                         ; Increments
Word_3ff8:
        defb    003h                                   ; Number of Words
        defw    0cbd1h                                 ; Base Address
        defb    095h,029h                              ; Increments
Word_7ff0:
        defb    001h                                   ; Number of Words
        defw    0c8e6h                                 ; Base Address
Word_ffe0:
        defb    00dh                                   ; Number of Words
        defw    0c8c3h                                 ; Base Address
        defb    0cdh,062h,0f3h,05ah,0e3h,0ffh,0a1h,040h; Increments
        defb    01eh,04ah,002h,059h

; outer loop 4 writes Words with 12 consecutive 1's (5 middle loops)
Word_0fff:
        defb    00dh                                   ; Number of Words
        defw    0ca8fh                                 ; Base Address
        defb    0fch,002h,097h,09bh,008h,002h,009h,0f5h; Increments
        defb    009h,02ah,062h,0bbh
Word_1ffe:
        defb    001h                                   ; Number of Words
        defw    0c8e6h                                 ; Base Address
Word_3ffc:
        defb    004h                                   ; Number of Words
        defw    0caf0h                                 ; Base Address
        defb    0f8h,0ddh,0a8h                         ; Increments
Word_7ff8:
        defb    002h                                   ; Number of Words
        defw    0c9c3h                                 ; Base Address
        defb    07fh                                   ; Increments
Word_fff0:
        defb    014h                                   ; Number of Words
        defw    0c894h                                 ; Base Address
        defb    002h,05ch,0fbh,0a0h,0ffh,022h,002h,052h; Increments
        defb    0a9h,002h,019h,009h,04dh,0a7h,009h,0edh
        defb    0aah,002h,05ch

; outer loop 5 writes Words with 13 consecutive 1's (4 middle loops)
Word_1fff:
        defb    00ch                                   ; Number of Words
        defw    0c843h                                 ; Base Address
        defb    0b5h,0eah,03fh,0e3h,0bah,040h,0c2h,0ffh; Increments
        defb    0ffh,011h,0ffh
Word_3ffe:
        defb    007h                                   ; Number of Words
        defw    0c943h                                 ; Base Address
        defb    0e4h,09ah,084h,0aah,07fh,002h          ; Increments
Word_7ffc:
        defb    007h                                   ; Number of Words
        defw    0c9edh                                 ; Base Address
        defb    0fah,07eh,0bfh,0ffh,0ffh,045h          ; Increments
Word_fff8:
        defb    017h                                   ; Number of Words
        defw    0c826h                                 ; Base Address
        defb    0d4h,098h,002h,01ch,002h,0dch,002h,01ch; Increments
        defb    002h,052h,0fdh,020h,0fdh,08ch,002h,01ch
        defb    0dfh,002h,01ch,006h,0edh,011h

; outer loop 6 writes Words with 14 consecutive 1's (3 middle loops)
Word_3fff:
        defb    02ah                                   ; Number of Words
        defw    0c828h                                 ; Base Address
        defb    06ah,002h,01ch,002h,03ch,09fh,002h,01ch; Increments
        defb    002h,03ch,005h,099h,002h,01ch,002h,03ch
        defb    005h,00ah,08eh,002h,01ch,002h,042h,0b9h
        defb    002h,01ch,09fh,002h,03ch,002h,01ch,09fh
        defb    002h,03ch,002h,01ch,09fh,002h,03ch,002h
        defb    01ch
Word_7ffe:
        defb    001h                                   ; Number of Words
        defw    0c8e6h                                 ; Base Address
Word_fffc:
        defb    017h                                   ; Number of Words
        defw    0c88dh                                 ; Base Address
        defb    026h,002h,031h,003h,038h,066h,06dh,091h; Increments
        defb    06dh,02dh,063h,006h,002h,063h,0d1h,006h
        defb    0f8h,0ffh,09fh,046h,002h,015h

; outer loop 7 writes Words with 15 consecutive 1's (2 middle loops)
Word_7fff:
        defb    006h                                   ; Number of Words
        defw    0c825h                                 ; Base Address
        defb    0beh,043h,0ffh,0fbh,0c0h               ; Increments
Word_fffe:
        defb    008h                                   ; Number of Words
        defw    0c9e9h                                 ; Base Address
        defb    03ch,002h,03dh,0e1h,0ddh,09dh,083h     ; Increments

; outer loop 8 writes Words with 16 consecutive 1's (1 middle loops)
Word_ffff:
        defb    029h                                   ; Number of Words
        defw    0c8e6h                                 ; Base Address
        defb    001h,05fh,09dh,001h,007h,037h,01fh,09dh; Increments
        defb    001h,01dh,001h,01dh,001h,01fh,09eh,001h
        defb    01ch,001h,01ch,001h,09dh,001h,05dh,001h
        defb    020h,07ch,001h,03eh,001h,01ch,001h,020h
        defb    027h,054h,001h,05dh,001h,020h,07ch,001h

Bytes:
        defb    001h  ; Number of Bytes
        defw    0c845h; Address
        defb    0c3h  ; Byte
        defb    001h  ; Number of Bytes
        defw    0c887h; Address
        defb    003h  ; Byte
        defb    001h  ; Number of Bytes
        defw    0c987h; Address
        defb    001h  ; Byte
        defb    001h  ; Number of Bytes
        defw    0ce68h; Address
        defb    0e0h  ; Byte
        defb    001h  ; Number of Bytes
        defw    0cf67h; Address
        defb    007h  ; Byte
        defb    00ah  ; Number of Bytes
        defw    0d903h; Address
        defb    001h  ; Byte: blue    INK on black   PAPER
        defb    002h  ; Number of Bytes
        defw    0d927h; Address
        defb    035h  ; Byte: cyan    INK on yellow  PAPER
        defb    001h  ; Number of Bytes
        defw    0d947h; Address
        defb    015h  ; Byte: cyan    INK on red     PAPER
        defb    004h  ; Number of Bytes
        defw    0d948h; Address
        defb    002h  ; Byte: red     INK on black   PAPER
        defb    001h  ; Number of Bytes
        defw    0d98bh; Address
        defb    003h  ; Byte: magenta INK on black   PAPER
        defb    001h  ; Number of Bytes
        defw    0d9abh; Address
        defb    003h  ; Byte: magenta INK on black   PAPER
; missing loop
;       defb    001h  ; Number of Bytes
;       defw    0d9cbh; Address
;       defb    003h  ; Byte: magenta INK on black   PAPER

Colors:
        defb    007h  ;       white   INK on black   PAPER
        defb    00eh  ;       yellow  INK on blue    PAPER
        defb    015h  ;       cyan    INK on red     PAPER
        defb    01ch  ;       green   INK on magenta PAPER
        defb    023h  ;       magenta INK on green   PAPER
        defb    02ah  ;       red     INK on cyan    PAPER
        defb    031h  ;       blue    INK on yellow  PAPER
        defb    038h  ;       black   INK on white   PAPER

l0382h:
        defb              000h,000h,000h,000h,001h,000h,01ah,006h,080h,001h,007h,00dh,013h,019h;
        defb    005h,00bh,011h,017h,003h,009h,00fh,015h,002h,008h,00eh,014h,01ah,006h,00ch,012h;
        defb    018h,004h,00ah,010h,016h,001h,004h,007h,00ah,003h,006h,009h,002h,005h,008h;
l03afh:
        defb                                                                               000h;
        defb    053h,059h,053h,020h,020h,020h,020h,020h,043h,04fh,04dh,0ddh,021h,000h,000h,011h;
        defb    000h,020h,03eh,0feh,037h,0cdh,0c2h,004h,0ddh,021h,000h,020h,011h,000h,020h,03eh;
        defb    0feh,037h,0cdh,0c2h,004h,0c3h,000h,000h;

l03d8h:
        call sub_05feh  ;03d8  cd fe 05   ;
        ld bc,00009h    ;03db  01 09 00   ;
        ld hl,l0382h    ;03de  21 82 03   ;
        ld de,0f00ch    ;03e1  11 0c f0   ;
        ldir            ;03e4  ed b0      ;
l03e6h:
        call sub_05e9h  ;03e6  cd e9 05   ;
        and 020h        ;03e9  e6 20      ;
        jr nz,l03fbh    ;03eb  20 0e      ;
        ld a,(0f00dh)   ;03ed  3a 0d f0   ;
        inc a           ;03f0  3c         ;
        ld (0f00dh),a   ;03f1  32 0d f0   ;
        cp 004h         ;03f4  fe 04      ;
        jr nz,l03e6h    ;03f6  20 ee      ;
        jp 00000h       ;03f8  c3 00 00   ;
l03fbh:
        call sub_05a8h  ;03fb  cd a8 05   ;
        ld hl,08000h    ;03fe  21 00 80   ;
        ld (0f008h),hl  ;0401  22 08 f0   ;
        ld bc,00a01h    ;0404  01 01 0a   ;
        call sub_04b3h  ;0407  cd b3 04   ;
        ld (0f020h),a   ;040a  32 20 f0   ;
        xor a           ;040d  af         ;
        ld (0f00eh),a   ;040e  32 0e f0   ;
        inc a           ;0411  3c         ;
        ld (0f010h),a   ;0412  32 10 f0   ;
l0415h:
        call sub_0563h  ;0415  cd 63 05   ;
        ld c,008h       ;0418  0e 08      ;
        call sub_04eeh  ;041a  cd ee 04   ;
        jp nz,00000h    ;041d  c2 00 00   ;
        ld a,(08000h)   ;0420  3a 00 80   ;
        or a            ;0423  b7         ;
        jr z,l042eh     ;0424  28 08      ;
        cp 0e5h         ;0426  fe e5      ;
        jr z,l042eh     ;0428  28 04      ;
        ld a,002h       ;042a  3e 02      ;
        jr l042fh       ;042c  18 01      ;
l042eh:
        xor a           ;042e  af         ;
l042fh:
        ld (0f021h),a   ;042f  32 21 f0   ;
        call sub_0563h  ;0432  cd 63 05   ;
        call sub_04beh  ;0435  cd be 04   ;
        call sub_0509h  ;0438  cd 09 05   ;
        jp c,00000h     ;043b  da 00 00   ;
        ld de,08800h    ;043e  11 00 88   ;
        ld (0f008h),de  ;0441  ed 53 08 f0;
        ld de,00010h    ;0445  11 10 00   ;
        add hl,de       ;0448  19         ;
l0449h:
        ld a,(hl)       ;0449  7e         ;
        or a            ;044a  b7         ;
        jr z,l0490h     ;044b  28 43      ;
        push hl         ;044d  e5         ;
        ld h,000h       ;044e  26 00      ;
        ld l,a          ;0450  6f         ;
        call sub_0530h  ;0451  cd 30 05   ;
        ld a,(0f011h)   ;0454  3a 11 f0   ;
        or a            ;0457  b7         ;
        jr z,l045fh     ;0458  28 05      ;
        ld bc,00208h    ;045a  01 08 02   ;
        jr l0462h       ;045d  18 03      ;
l045fh:
        ld bc,00808h    ;045f  01 08 08   ;
l0462h:
        ld a,d          ;0462  7a         ;
        ld (0f00eh),a   ;0463  32 0e f0   ;
        push de         ;0466  d5         ;
        ld d,000h       ;0467  16 00      ;
        call sub_0559h  ;0469  cd 59 05   ;
        ld (0f010h),a   ;046c  32 10 f0   ;
        call sub_04eeh  ;046f  cd ee 04   ;
        jp nz,00000h    ;0472  c2 00 00   ;
        ld hl,(0f01eh)  ;0475  2a 1e f0   ;
        ld (0f008h),hl  ;0478  22 08 f0   ;
        ld hl,(0f01ch)  ;047b  2a 1c f0   ;
        inc hl          ;047e  23         ;
        pop de          ;047f  d1         ;
        inc e           ;0480  1c         ;
        ld a,e          ;0481  7b         ;
        cp l            ;0482  bd         ;
        jp c,l0489h     ;0483  da 89 04   ;
        inc d           ;0486  14         ;
        ld a,001h       ;0487  3e 01      ;
l0489h:
        ld e,a          ;0489  5f         ;
        djnz l0462h     ;048a  10 d6      ;
        pop hl          ;048c  e1         ;
        inc hl          ;048d  23         ;
        jr l0449h       ;048e  18 b9      ;
l0490h:
        ld hl,l04a5h    ;0490  21 a5 04   ;
        ld de,0e000h    ;0493  11 00 e0   ;
        ld bc,0000eh    ;0496  01 0e 00   ;
        ldir            ;0499  ed b0      ;
        ld hl,0e000h    ;049b  21 00 e0   ;
        ld a,040h       ;049e  3e 40      ;
        out (0feh),a    ;04a0  d3 fe      ;
        ld r,a          ;04a2  ed 4f      ;
        jp (hl)         ;04a4  e9         ;
l04a5h:
        ld hl,08800h    ;04a5  21 00 88   ;
        ld de,00100h    ;04a8  11 00 01   ;
        ld bc,05000h    ;04ab  01 00 50   ;
        ldir            ;04ae  ed b0      ;
        jp 00100h       ;04b0  c3 00 01   ;
sub_04b3h:
        ld a,b          ;04b3  78         ;
        ld (0f00ch),a   ;04b4  32 0c f0   ;
        ld hl,l06bah    ;04b7  21 ba 06   ;
        call sub_0647h  ;04ba  cd 47 06   ;
        ret             ;04bd  c9         ;
sub_04beh:
        or a            ;04be  b7         ;
        jr z,l04c6h     ;04bf  28 05      ;
        ld bc,00408h    ;04c1  01 08 04   ;
        jr l04c9h       ;04c4  18 03      ;
l04c6h:
        ld bc,01008h    ;04c6  01 08 10   ;
l04c9h:
        ld a,(0f021h)   ;04c9  3a 21 f0   ;
        ld (0f00eh),a   ;04cc  32 0e f0   ;
        ld a,001h       ;04cf  3e 01      ;
l04d1h:
        push af         ;04d1  f5         ;
        ld e,a          ;04d2  5f         ;
        ld d,000h       ;04d3  16 00      ;
        call sub_0559h  ;04d5  cd 59 05   ;
        ld (0f010h),a   ;04d8  32 10 f0   ;
        call sub_04eeh  ;04db  cd ee 04   ;
        jr nz,l04ebh    ;04de  20 0b      ;
        ld hl,(0f01eh)  ;04e0  2a 1e f0   ;
        ld (0f008h),hl  ;04e3  22 08 f0   ;
        pop af          ;04e6  f1         ;
        inc a           ;04e7  3c         ;
        djnz l04d1h     ;04e8  10 e7      ;
        ret             ;04ea  c9         ;
l04ebh:
        jp 00000h       ;04eb  c3 00 00   ;
sub_04eeh:
        push bc         ;04ee  c5         ;
        call sub_05b9h  ;04ef  cd b9 05   ;
        pop bc          ;04f2  c1         ;
        ld e,00ah       ;04f3  1e 0a      ;
l04f5h:
        push bc         ;04f5  c5         ;
        push de         ;04f6  d5         ;
        ld hl,l06bah    ;04f7  21 ba 06   ;
        call sub_0647h  ;04fa  cd 47 06   ;
        pop de          ;04fd  d1         ;
        pop bc          ;04fe  c1         ;
        ret z           ;04ff  c8         ;
        dec e           ;0500  1d         ;
        jr nz,l04f5h    ;0501  20 f2      ;
        ld a,(0f015h)   ;0503  3a 15 f0   ;
        and 0c0h        ;0506  e6 c0      ;
        ret             ;0508  c9         ;
sub_0509h:
        ld hl,08000h    ;0509  21 00 80   ;
        ld de,l03afh    ;050c  11 af 03   ;
        ld b,040h       ;050f  06 40      ;
l0511h:
        call sub_051fh  ;0511  cd 1f 05   ;
        ret z           ;0514  c8         ;
        push de         ;0515  d5         ;
        ld de,0001fh+1  ;0516  11 20 00   ;
        add hl,de       ;0519  19         ;
        pop de          ;051a  d1         ;
        djnz l0511h     ;051b  10 f4      ;
        scf             ;051d  37         ;
        ret             ;051e  c9         ;
sub_051fh:
        push hl         ;051f  e5         ;
        push de         ;0520  d5         ;
        push bc         ;0521  c5         ;
        ld b,00ch       ;0522  06 0c      ;
l0524h:
        ld a,(de)       ;0524  1a         ;
        cp (hl)         ;0525  be         ;
        jr nz,l052ch    ;0526  20 04      ;
        inc hl          ;0528  23         ;
        inc de          ;0529  13         ;
        djnz l0524h     ;052a  10 f8      ;
l052ch:
        pop bc          ;052c  c1         ;
        pop de          ;052d  d1         ;
        pop hl          ;052e  e1         ;
        ret             ;052f  c9         ;
sub_0530h:
        ld bc,003ffh    ;0530  01 ff 03   ;
l0533h:
        sla l           ;0533  cb 25      ;
        rl h            ;0535  cb 14      ;
        djnz l0533h     ;0537  10 fa      ;
        inc hl          ;0539  23         ;
        ld de,(0f01ch)  ;053a  ed 5b 1c f0;
l053eh:
        inc c           ;053e  0c         ;
        sbc hl,de       ;053f  ed 52      ;
        jr nc,l053eh    ;0541  30 fb      ;
        add hl,de       ;0543  19         ;
        ld a,(0f011h)   ;0544  3a 11 f0   ;
        or a            ;0547  b7         ;
        jr z,l0552h     ;0548  28 08      ;
        ld a,l          ;054a  7d         ;
        dec a           ;054b  3d         ;
        srl a           ;054c  cb 3f      ;
        srl a           ;054e  cb 3f      ;
        inc a           ;0550  3c         ;
        ld l,a          ;0551  6f         ;
l0552h:
        ld a,(0f021h)   ;0552  3a 21 f0   ;
        add a,c         ;0555  81         ;
        ld d,a          ;0556  57         ;
        ld e,l          ;0557  5d         ;
        ret             ;0558  c9         ;
sub_0559h:
        push ix         ;0559  dd e5      ;
        add ix,de       ;055b  dd 19      ;
        ld a,(ix+000h)  ;055d  dd 7e 00   ;
        pop ix          ;0560  dd e1      ;
        ret             ;0562  c9         ;
sub_0563h:
        xor a           ;0563  af         ;
        ld (0f01dh),a   ;0564  32 1d f0   ;
        ld a,(0f020h)   ;0567  3a 20 f0   ;
        or a            ;056a  b7         ;
        jr z,l058ch     ;056b  28 1f      ;
        ld a,046h       ;056d  3e 46      ;
        ld (0f00ch),a   ;056f  32 0c f0   ;
        ld a,00ah       ;0572  3e 0a      ;
        ld (0f012h),a   ;0574  32 12 f0   ;
        ld a,028h       ;0577  3e 28      ;
        ld (0f01ch),a   ;0579  32 1c f0   ;
        ld ix,003a4h    ;057c  dd 21 a4 03;
        ld hl,00002h    ;0580  21 02 00   ;
        ld (0f00ah),hl  ;0583  22 0a f0   ;
        ld a,002h       ;0586  3e 02      ;
        ld (0f011h),a   ;0588  32 11 f0   ;
        ret             ;058b  c9         ;
l058ch:
        ld a,006h       ;058c  3e 06      ;
        ld (0f00ch),a   ;058e  32 0c f0   ;
        ld a,01ah       ;0591  3e 1a      ;
        ld (0f012h),a   ;0593  32 12 f0   ;
        ld (0f01ch),a   ;0596  32 1c f0   ;
        ld ix,0038ah    ;0599  dd 21 8a 03;
        ld hl,08001h    ;059d  21 01 80   ;
        ld (0f00ah),hl  ;05a0  22 0a f0   ;
        xor a           ;05a3  af         ;
        ld (0f011h),a   ;05a4  32 11 f0   ;
        ret             ;05a7  c9         ;
sub_05a8h:
        call sub_05d2h  ;05a8  cd d2 05   ;
        ld a,007h       ;05ab  3e 07      ;
        out (0fdh),a    ;05ad  d3 fd      ;
        call sub_061ah  ;05af  cd 1a 06   ;
        ld a,(0f00dh)   ;05b2  3a 0d f0   ;
        out (0fdh),a    ;05b5  d3 fd      ;
        halt            ;05b7  76         ;
        ret             ;05b8  c9         ;
sub_05b9h:
        call sub_05d2h  ;05b9  cd d2 05   ;
        ld a,00fh       ;05bc  3e 0f      ;
        out (0fdh),a    ;05be  d3 fd      ;
        call sub_061ah  ;05c0  cd 1a 06   ;
        ld a,(0f00dh)   ;05c3  3a 0d f0   ;
        out (0fdh),a    ;05c6  d3 fd      ;
        call sub_061ah  ;05c8  cd 1a 06   ;
        ld a,(0f00eh)   ;05cb  3a 0e f0   ;
        out (0fdh),a    ;05ce  d3 fd      ;
        halt            ;05d0  76         ;
        ret             ;05d1  c9         ;
sub_05d2h:
        call sub_05e9h  ;05d2  cd e9 05   ;
        and 020h        ;05d5  e6 20      ;
        jr nz,l05dch    ;05d7  20 03      ;
        jp 00000h       ;05d9  c3 00 00   ;
l05dch:
        call sub_0620h  ;05dc  cd 20 06   ;
        ld hl,l069ch    ;05df  21 9c 06   ;
        ld (0f000h),hl  ;05e2  22 00 f0   ;
        call sub_0614h  ;05e5  cd 14 06   ;
        ret             ;05e8  c9         ;
sub_05e9h:
        call sub_0614h  ;05e9  cd 14 06   ;
        ld a,004h       ;05ec  3e 04      ;
        out (0fdh),a    ;05ee  d3 fd      ;
        call sub_061ah  ;05f0  cd 1a 06   ;
        ld a,(0f00dh)   ;05f3  3a 0d f0   ;
        out (0fdh),a    ;05f6  d3 fd      ;
        call sub_061ah  ;05f8  cd 1a 06   ;
        in a,(0fdh)     ;05fb  db fd      ;
        ret             ;05fd  c9         ;
sub_05feh:
        call sub_0614h  ;05fe  cd 14 06   ;
        ld a,003h       ;0601  3e 03      ;
        out (0fdh),a    ;0603  d3 fd      ;
        call sub_061ah  ;0605  cd 1a 06   ;
        ld a,03fh       ;0608  3e 3f      ;
        out (0fdh),a    ;060a  d3 fd      ;
        call sub_061ah  ;060c  cd 1a 06   ;
        ld a,017h       ;060f  3e 17      ;
        out (0fdh),a    ;0611  d3 fd      ;
        ret             ;0613  c9         ;
sub_0614h:
        in a,(0f5h)     ;0614  db f5      ;
        and 01fh        ;0616  e6 1f      ;
        jr nz,sub_0614h ;0618  20 fa      ;
sub_061ah:
        in a,(0f5h)     ;061a  db f5      ;
        rlca            ;061c  07         ;
        jr nc,sub_061ah ;061d  30 fb      ;
        ret             ;061f  c9         ;
sub_0620h:
        di              ;0620  f3         ;
        im 2            ;0621  ed 5e      ;
        ld hl,0f000h    ;0623  21 00 f0   ;
        ld a,h          ;0626  7c         ;
        ld i,a          ;0627  ed 47      ;
        ld a,l          ;0629  7d         ;
        out (0e3h),a    ;062a  d3 e3      ;
        ld a,0dfh       ;062c  3e df      ;
        out (0e3h),a    ;062e  d3 e3      ;
        ld a,001h       ;0630  3e 01      ;
        out (0e3h),a    ;0632  d3 e3      ;
        ld hl,(0f00ah)  ;0634  2a 0a f0   ;
        ld a,05fh       ;0637  3e 5f      ;
        out (0ebh),a    ;0639  d3 eb      ;
        ld a,h          ;063b  7c         ;
        out (0ebh),a    ;063c  d3 eb      ;
        ld a,05fh       ;063e  3e 5f      ;
        out (0f3h),a    ;0640  d3 f3      ;
        ld a,l          ;0642  7d         ;
        out (0f3h),a    ;0643  d3 f3      ;
        ei              ;0645  fb         ;
        ret             ;0646  c9         ;
sub_0647h:
        ld (0f000h),hl  ;0647  22 00 f0   ;
        call sub_0620h  ;064a  cd 20 06   ;
        ld hl,(0f008h)  ;064d  2a 08 f0   ;
        ld de,0f00ch    ;0650  11 0c f0   ;
        call sub_0614h  ;0653  cd 14 06   ;
l0656h:
        call sub_061ah  ;0656  cd 1a 06   ;
        ld a,(de)       ;0659  1a         ;
        out (0fdh),a    ;065a  d3 fd      ;
        inc de          ;065c  13         ;
        dec c           ;065d  0d         ;
        jr nz,l0656h    ;065e  20 f6      ;
        call sub_061ah  ;0660  cd 1a 06   ;
        ld a,(de)       ;0663  1a         ;
        ld b,020h       ;0664  06 20      ;
        ld c,0fdh       ;0666  0e fd      ;
        out (0fdh),a    ;0668  d3 fd      ;
        halt            ;066a  76         ;
l066bh:
        in a,(0f5h)     ;066b  db f5      ;
        and b           ;066d  a0         ;
        jp nz,l066bh    ;066e  c2 6b 06   ;
        ld b,000h       ;0671  06 00      ;
l0673h:
        djnz l0673h     ;0673  10 fe      ;
        dec hl          ;0675  2b         ;
        ld (0f01eh),hl  ;0676  22 1e f0   ;
        ld a,(hl)       ;0679  7e         ;
        ld hl,0f015h    ;067a  21 15 f0   ;
        ld (hl),a       ;067d  77         ;
        inc hl          ;067e  23         ;
l067fh:
        call sub_061ah  ;067f  cd 1a 06   ;
        in a,(0fdh)     ;0682  db fd      ;
        ld (hl),a       ;0684  77         ;
        inc hl          ;0685  23         ;
        ld b,000h       ;0686  06 00      ;
l0688h:
        djnz l0688h     ;0688  10 fe      ;
        in a,(0f5h)     ;068a  db f5      ;
        and 010h        ;068c  e6 10      ;
        jr nz,l067fh    ;068e  20 ef      ;
        ld hl,l069ch    ;0690  21 9c 06   ;
        ld (0f000h),hl  ;0693  22 00 f0   ;
        ld a,(0f015h)   ;0696  3a 15 f0   ;
        and 0c0h        ;0699  e6 c0      ;
        ret             ;069b  c9         ;
l069ch:
        push af         ;069c  f5         ;
        push hl         ;069d  e5         ;
        ld hl,0f015h    ;069e  21 15 f0   ;
        call sub_061ah  ;06a1  cd 1a 06   ;
        ld a,008h       ;06a4  3e 08      ;
        out (0fdh),a    ;06a6  d3 fd      ;
        call sub_061ah  ;06a8  cd 1a 06   ;
        in a,(0fdh)     ;06ab  db fd      ;
        ld (hl),a       ;06ad  77         ;
        inc hl          ;06ae  23         ;
        call sub_061ah  ;06af  cd 1a 06   ;
        in a,(0fdh)     ;06b2  db fd      ;
        ld (hl),a       ;06b4  77         ;
        pop hl          ;06b5  e1         ;
        pop af          ;06b6  f1         ;
        ei              ;06b7  fb         ;
        reti            ;06b8  ed 4d      ;
l06bah:
        ini             ;06ba  ed a2      ;
        inc b           ;06bc  04         ;
        ei              ;06bd  fb         ;
        reti            ;06be  ed 4d      ;
l06c0h:
        ld bc,02000h    ;06c0  01 00 20   ;
        ld de,08000h    ;06c3  11 00 80   ;
        ld hl,04000h    ;06c6  21 00 40   ;
        ldir            ;06c9  ed b0      ;
        ld bc,02000h    ;06cb  01 00 20   ;
        ld de,0e000h    ;06ce  11 00 e0   ;
        ld hl,06000h    ;06d1  21 00 60   ;
        ldir            ;06d4  ed b0      ;
l06d6h:
        ld hl,00000h    ;06d6  21 00 00   ;
        xor a           ;06d9  af         ;
        out (0feh),a    ;06da  d3 fe      ;
        ld r,a          ;06dc  ed 4f      ;
        jp (hl)         ;06de  e9         ;
l06dfh:
        ld sp,0c000h    ;06df  31 00 c0   ;
        scf             ;06e2  37         ;
        ld ix,08000h    ;06e3  dd 21 00 80;
        ld de,02000h    ;06e7  11 00 20   ;
        ld a,0feh       ;06ea  3e fe      ;
        call sub_073ah  ;06ec  cd 3a 07   ;
        jp nc,00000h    ;06ef  d2 00 00   ;
        ld ix,0e000h    ;06f2  dd 21 00 e0;
        ld de,02000h    ;06f6  11 00 20   ;
        scf             ;06f9  37         ;
        ld a,0feh       ;06fa  3e fe      ;
        call sub_073ah  ;06fc  cd 3a 07   ;
        jp nc,00000h    ;06ff  d2 00 00   ;
        jp l06d6h       ;0702  c3 d6 06   ;

; partial keyboard test                                                              ; cycles
l0705h:
        rra             ;0705  1f         ;  rotate right all, CY=or(col 0)          ;    4
        jp c,l0710h     ;0706  da 10 07   ;  jump if "SPACE,"Q","A","CS" not pressed ;   10
        ld a,002h       ;0709  3e 02      ;  
l070bh:
        out (c),a       ;070b  ed 79      ;  
        jp 006c0h       ;070d  c3 c0 06   ;  
l0710h:
        rra             ;0710  1f         ;  rotate right all, CY=or(col 1)          ;    4
        jp c,KB_return  ;0711  da b7 00   ;  jump if "SS,"W","S","Z" not pressed     ;   10

        ld a,005h       ;0714  3e 05      ;
        jp l070bh       ;0716  c3 0b 07   ;
l0719h:
        ld a,004h       ;0719  3e 04      ;
        out (c),a       ;071b  ed 79      ;
        jp l06c0h       ;071d  c3 c0 06   ;
        rlca            ;0720  07         ;
        jp c,l06c0h     ;0721  da c0 06   ;
        push ix         ;0724  dd e5      ;
        pop af          ;0726  f1         ;
        and 03fh        ;0727  e6 3f      ;
        srl a           ;0729  cb 3f      ;
        srl a           ;072b  cb 3f      ;
        srl a           ;072d  cb 3f      ;
        ld e,a          ;072f  5f         ;
        ld d,000h       ;0730  16 00      ;
        ld hl,0da8ch    ;0732  21 8c da   ;
        add hl,de       ;0735  19         ;
        set 7,(hl)      ;0736  cb fe      ;
l0738h:
        jr l0738h       ;0738  18 fe      ;
sub_073ah:
        inc d           ;073a  14         ;
        ex af,af'       ;073b  08         ;
        dec d           ;073c  15         ;
        di              ;073d  f3         ;
        ld a,00fh       ;073e  3e 0f      ;
        out (0feh),a    ;0740  d3 fe      ;
        in a,(0feh)     ;0742  db fe      ;
        rra             ;0744  1f         ;
        and 020h        ;0745  e6 20      ;
        or 002h         ;0747  f6 02      ;
        ld c,a          ;0749  4f         ;
        cp a            ;074a  bf         ;
l074bh:
        ret nz          ;074b  c0         ;
l074ch:
        call sub_07c7h  ;074c  cd c7 07   ;
        jr nc,l074bh    ;074f  30 fa      ;
        ld hl,l0415h    ;0751  21 15 04   ;
l0754h:
        djnz l0754h     ;0754  10 fe      ;
        dec hl          ;0756  2b         ;
        ld a,h          ;0757  7c         ;
        or l            ;0758  b5         ;
        jr nz,l0754h    ;0759  20 f9      ;
        call sub_07c3h  ;075b  cd c3 07   ;
        jr nc,l074bh    ;075e  30 eb      ;
l0760h:
        ld b,09ch       ;0760  06 9c      ;
        call sub_07c3h  ;0762  cd c3 07   ;
        jr nc,l074bh    ;0765  30 e4      ;
        ld a,0c6h       ;0767  3e c6      ;
        cp b            ;0769  b8         ;
        jr nc,l074ch    ;076a  30 e0      ;
        inc h           ;076c  24         ;
        jr nz,l0760h    ;076d  20 f1      ;
l076fh:
        ld b,0c9h       ;076f  06 c9      ;
        call sub_07c7h  ;0771  cd c7 07   ;
        jr nc,l074bh    ;0774  30 d5      ;
        ld a,b          ;0776  78         ;
        cp 0d4h         ;0777  fe d4      ;
        jr nc,l076fh    ;0779  30 f4      ;
        call sub_07c7h  ;077b  cd c7 07   ;
        ret nc          ;077e  d0         ;
        ld a,c          ;077f  79         ;
        xor 003h        ;0780  ee 03      ;
        ld c,a          ;0782  4f         ;
        ld h,000h       ;0783  26 00      ;
        ld b,0b0h       ;0785  06 b0      ;
        jr l07a8h       ;0787  18 1f      ;
l0789h:
        ex af,af'       ;0789  08         ;
        jr nz,l0793h    ;078a  20 07      ;
        jr nc,l079dh    ;078c  30 0f      ;
        ld (ix+000h),l  ;078e  dd 75 00   ;
        jr l07a2h       ;0791  18 0f      ;
l0793h:
        rl c            ;0793  cb 11      ;
        xor l           ;0795  ad         ;
        ret nz          ;0796  c0         ;
        ld a,c          ;0797  79         ;
        rra             ;0798  1f         ;
        ld c,a          ;0799  4f         ;
        inc de          ;079a  13         ;
        jr l07a4h       ;079b  18 07      ;
l079dh:
        ld a,(ix+000h)  ;079d  dd 7e 00   ;
        xor l           ;07a0  ad         ;
        ret nz          ;07a1  c0         ;
l07a2h:
        inc ix          ;07a2  dd 23      ;
l07a4h:
        dec de          ;07a4  1b         ;
        ex af,af'       ;07a5  08         ;
        ld b,0b2h       ;07a6  06 b2      ;
l07a8h:
        ld l,001h       ;07a8  2e 01      ;
l07aah:
        call sub_07c3h  ;07aa  cd c3 07   ;
        ret nc          ;07ad  d0         ;
        ld a,0cbh       ;07ae  3e cb      ;
        cp b            ;07b0  b8         ;
        rl l            ;07b1  cb 15      ;
        ld b,0b0h       ;07b3  06 b0      ;
        jp nc,l07aah    ;07b5  d2 aa 07   ;
        ld a,h          ;07b8  7c         ;
        xor l           ;07b9  ad         ;
        ld h,a          ;07ba  67         ;
        ld a,d          ;07bb  7a         ;
        or e            ;07bc  b3         ;
        jr nz,l0789h    ;07bd  20 ca      ;
        ld a,h          ;07bf  7c         ;
        cp 001h         ;07c0  fe 01      ;
        ret             ;07c2  c9         ;
sub_07c3h:
        call sub_07c7h  ;07c3  cd c7 07   ;
        ret nc          ;07c6  d0         ;
sub_07c7h:
        ld a,016h       ;07c7  3e 16      ;
l07c9h:
        dec a           ;07c9  3d         ;
        jr nz,l07c9h    ;07ca  20 fd      ;
l07cch:
        and a           ;07cc  a7         ;
l07cdh:
        inc b           ;07cd  04         ;
        ret z           ;07ce  c8         ;
        ld a,07fh       ;07cf  3e 7f      ;
        in a,(0feh)     ;07d1  db fe      ;
        rra             ;07d3  1f         ;
        ret nc          ;07d4  d0         ;
        xor c           ;07d5  a9         ;
        and 020h        ;07d6  e6 20      ;
        jr z,l07cdh     ;07d8  28 f3      ;
        ld a,c          ;07da  79         ;
        cpl             ;07db  2f         ;
        ld c,a          ;07dc  4f         ;
        and 007h        ;07dd  e6 07      ;
        or 008h         ;07df  f6 08      ;
        out (0feh),a    ;07e1  d3 fe      ;
        scf             ;07e3  37         ;
        ret             ;07e4  c9         ;
        jr nc,$-26      ;07e5  30 e4      ;
        ld a,0c6h       ;07e7  3e c6      ;
        cp b            ;07e9  b8         ;
        jr nc,l07cch    ;07ea  30 e0      ;
        inc h           ;07ec  24         ;
        jr nz,$-13      ;07ed  20 f1      ;
l07efh:
        ld b,0c9h       ;07ef  06 c9      ;
        call sub_07c7h  ;07f1  cd c7 07   ;
        jr nc,$-41      ;07f4  30 d5      ;
        ld a,b          ;07f6  78      
        cp 0d4h         ;07f7  fe d4      ;
        jr nc,l07efh    ;07f9  30 f4      ;
        call sub_07c7h  ;07fb  cd c7 07   ;
        ret nc          ;07fe  d0         ;
        ld a,c          ;07ff  79         ;

        end

