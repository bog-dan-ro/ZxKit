; Copyright 2022 The Cismas Foundation, under the terms of the MIT License.
; 
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
; 
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
; 
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.
; 
; Turbo Spectrum boot code  v1.0  09/30/2022  sorin@cismas.org
; multi-line comments start with ";b", continue with ";|" and end with ";e"

        org     00000h

; After reset, Turbo Spectrum (TS) boot code starts executing in EEPROM Mode.
; The 16Kx8 EEPROM boot code contains the Turbo Spectrum logo generation and
; a compacted Sinclair ZX Spectrum ROM.  After some initializations are done,
; including setting Stack Pointer to 0e800h, clearing the screen, and setting
; the color bar at the bottom of the screen, 7b4h bytes are copied from EEPROM
; addresses 0004ch to 007ffh to DRAM addresses 0dfffh to 0e7b2h, then a jump
; is executed at 0e004h, where a transition is made to the 64K DRAM Mode.

; The Turbo Spectrum logo is stored compressed to reduce code and data size.
; The logo can be generated from 9 special characters and their rotations by
; a multiple of 90 degrees.  The total size of code and data for logo
; generation and rotation is 1231 bytes.  The original Sinclair ZX Spectrum
; ROM had 1170 unused bytes, 0386eh - 03cffh, which are not sufficient for
; the logo code and data. To make room in the 16KB EEPROM, the characters in
; the Sinclair ZX Spectrum ROM are stored using a light compression.

start:
        di              ;0000  f3         ;  disable interrupts
        ld sp,0e800h    ;0001  31 00 e8   ;  initialize SP=0e800h
        ld a,020h       ;0004  3e 20      ;b out(020feh)=020h
        out (0feh),a    ;0006  d3 fe      ;e set black border and O5 (input to !CE flop)
        xor a           ;0008  af         ;b clear (09900h)-(0daffh)
        ld hl,0daffh    ;0009  21 ff da   ;| 
        ld (hl),a       ;000c  77         ;| 
        ld de,0dafeh    ;000d  11 fe da   ;| 
        ld bc,041ffh    ;0010  01 ff 41   ;| 
        lddr            ;0013  ed b8      ;e 
        ld hl,0dac7h    ;0015  21 c7 da   ;b add color bar in the center
        ld c,002h       ;0018  0e 02      ;e of the two bottom screen lines
l001ah:
        ld b,008h       ;001a  06 08      ;  8*2 characters <-- <--,
l001ch:                                   ;                        ^
        ld (hl),a       ;001c  77         ;  1st character INK <,  |
        inc hl          ;001d  23         ;                     ^  ^
        xor 040h        ;001e  ee 40      ;  set BRIGHT         |  |
        ld (hl),a       ;0020  77         ;  2nd character INK  ^  ^
        inc hl          ;0021  23         ;                     |  |
        xor 040h        ;0022  ee 40      ;  clear BRIGHT       ^  ^
        add a,008h      ;0024  c6 08      ;  next color         |  |
        djnz l001ch     ;0026  10 f4      ;  loop 8 times --> --'  ^
        ld l,0e7h       ;0028  2e e7      ;  character row below   |
        xor a           ;002a  af         ;  clear A               ^
        dec c           ;002b  0d         ;                        |
        jr nz,l001ah    ;002c  20 ec      ;  loop 2 times --> --> -'
        ld hl,0d800h    ;002e  21 00 d8   ;  color attributes base address
        ld a,043h       ;0031  3e 43      ;  magenta INK on black PAPER
        ld c,002h       ;0033  0e 02      ;  2 thirds of screen
l0035h:
        ld (hl),a       ;0035  77         ;  store color attribute <-,--,
        inc hl          ;0036  23         ;                          |  |
        djnz l0035h     ;0037  10 fc      ;  loop 1st 3rd of screen -'  ^
        ld a,045h       ;0039  3e 45      ;  cyan INK on black PAPER    |
        dec c           ;003b  0d         ;                             ^
        jr nz,l0035h    ;003c  20 f7      ;  loop 2nd 3rd of screen  -->'
        ld hl,0004ch    ;003e  21 4c 00   ;b copy from (0004ch)-(007ffh)
        ld de,0dfffh    ;0041  11 ff df   ;| to (0dfffh)-(0e7b2h)
        ld bc,007b3h+1  ;0044  01 b4 07   ;| 007b4h bytes
        ldir            ;0047  ed b0      ;e 
        jp 0e004h       ;0049  c3 04 e0   ;  jump to (0e004h)

        org     09900h
T_buffer:

        org     0b100h
S_buffer:

        org     0c000h

        org     0db00h
C_buffer0:

        org     0db08h
C_buffer1:

        org     0db16h
HL_save:

        org     0dfffh

        defw    IntHandler      ;dfff   3a e1      ;  

le001h:
        inc d           ;e001   14         ;b for TURBO only
        jr le035h       ;e002   18 31      ;e 

; enter 64K DRAM Mode and generate TURBO SPECTRUM logo with 060h byte lines
        in a,(0feh)     ;e004   db fe      ;  !CE=1; A=in(045feh), rows KAf,KAd,KAc,KAb,KA9
        ld hl,C_struct  ;e006   21 35 e3   ;  Char structure Base address
        ld bc,00109h    ;e009   01 09 01   ;  1 rotation and 9 special characters
        exx             ;e00c   d9         ;  
        ld hl,C_base    ;e00d   21 ed e2   ;  load HL with Special Character Base Address
le010h:
        ld de,C_buffer0 ;e010   11 00 db   ;b copy special character to (C_buffer0) <-- <,
        ld bc,00008h    ;e013   01 08 00   ;|                                            ^
        ldir            ;e016   ed b0      ;e                                            |
        exx             ;e018   d9         ;                                             ^
le019h:                                    ;                                             |
        push bc         ;e019   c5         ;  save BC <-- <-- <-- <-- <-- <-- <-- <-- <, ^
        ld e,(hl)       ;e01a   5e         ;b load DE with char start address          | |
        inc hl          ;e01b   23         ;|                                          ^ ^
        ld d,(hl)       ;e01c   56         ;|                                          | |
        inc hl          ;e01d   23         ;e                                          ^ ^
        ld c,(hl)       ;e01e   4e         ;  load C with number of char increments    | |
le01fh:                                    ;                                           ^ ^
        push hl         ;e01f   e5         ;  save HL <-- <-- <-- <-- <-- <-- <-- <--, | |
        ld hl,C_buffer0 ;e020   21 00 db   ;  load HL with C_buffer0                 ^ ^ ^
        ld b,008h       ;e023   06 08      ;  8 bytes per character                  | | |
le025h:                                    ;                                         ^ ^ ^
        ld a,(de)       ;e025   1a         ;b xor char at (DE) with (C_buffer0) <--, | | |
        xor (hl)        ;e026   ae         ;|                                      ^ ^ ^ ^
        ld (de),a       ;e027   12         ;|                                      | | | |
        inc l           ;e028   2c         ;|                                      ^ ^ ^ ^
        inc d           ;e029   14         ;e                                      | | | |
        ld a,d          ;e02a   7a         ;b for TURBO, lines are 0300h apart     ^ ^ ^ ^
        cp 0b2h         ;e02b   fe b2      ;| and D is incremented 3 times         | | | |
        jr c,$-44       ;e02d   38 d2      ;e jump if TURBO                        ^ ^ ^ ^
        ld a,e          ;e02f   7b         ;b for SPECTRUM, lines are 01e0h apart  | | | |
        add a,0e0h      ;e030   c6 e0      ;| and D is incremented once or twice   ^ ^ ^ ^
        ld e,a          ;e032   5f         ;|                                      | | | |
        jr nc,le036h    ;e033   30 01      ;e                                      ^ ^ ^ ^
le035h:                                    ;                                       | | | |
        inc d           ;e035   14         ;                                       ^ ^ ^ ^
le036h:                                    ;                                       | | | |
        djnz le025h     ;e036   10 ed      ;  close byte loop --> --> --> --> --> -' ^ ^ ^
        ld a,d          ;e038   7a         ;b TURBO char start address < 09c00h,     | | |
        sub 018h        ;e039   d6 18      ;| and it takes 8 * 0300h = 01800h bytes  ^ ^ ^
        cp 09ch         ;e03b   fe 9c      ;|                                        | | |
        jr c,le041h     ;e03d   38 02      ;e                                        ^ ^ ^
        add a,009h      ;e03f   c6 09      ;  SPECTRUM takes 8*01e0h = 00f00h bytes  | | |
le041h:                 ;                                         ^ ^ ^
        ld d,a          ;e041   57         ;  restore DE with char start address     | | |
        pop hl          ;e042   e1         ;  restore HL                             ^ ^ ^
        inc hl          ;e043   23         ;                                         | | |
        ld a,(hl)       ;e044   7e         ;  load A with next char increment        ^ ^ ^
        add a,e         ;e045   83         ;b next char start address                | | |
        ld e,a          ;e046   5f         ;e                                        ^ ^ ^
        jr nc,le052h    ;e047   30 09      ;  jump if E does not wrap around         | | |
        inc d           ;e049   14         ;  need to increment D                    ^ ^ ^
        ld a,d          ;e04a   7a         ;                                         | | |
        cp 09ch         ;e04b   fe 9c      ;                                         ^ ^ ^
        jr nz,le052h    ;e04d   20 03      ;  jump if TURBO                          | | |
        add a,015h      ;e04f   c6 15      ;b transition to SPECTRUM                 ^ ^ ^
        ld d,a          ;e051   57         ;e                                        | | |
le052h:                                    ;                                         ^ ^ ^
        dec c           ;e052   0d         ;                                         | | |
        jr nz,le01fh    ;e053   20 ca      ;  close char loop --> --> --> --> --> -->' ^ ^
        push hl         ;e055   e5         ;  save HL                                  | |
        ld hl,C_buffer0 ;e056   21 00 db   ;b copy (C_buffer0) to (C_buffer1)          ^ ^
        ld de,C_buffer1 ;e059   11 08 db   ;| then rotate left char (C_buffer0)        | |
        ld bc,00008h    ;e05c   01 08 00   ;|                                          ^ ^
        ldir            ;e05f   ed b0      ;e HL holds C_buffer1                       | |
        ld e,000h       ;e061   1e 00      ;  DE holds C_buffer0                       ^ ^
        ld c,008h       ;e063   0e 08      ;  8 bytes per character                    | |
le065h:                                    ;                                           ^ ^
        ld b,008h       ;e065   06 08      ;  8 bits per byte <-- <-- <-- <-,          | |
        ld l,b          ;e067   68         ;                                ^          ^ ^
le068h:                                    ;                                |          | |
        rr (hl)         ;e068   cb 1e      ;  shift bit 0 of (HL) to CY <,  ^          ^ ^
        ex de,hl        ;e06a   eb         ;                             |  |          | |
        rl (hl)         ;e06b   cb 16      ;  shift CY to bit 0 of (DE)  ^  ^          ^ ^
        ex de,hl        ;e06d   eb         ;                             |  |          | |
        inc l           ;e06e   2c         ;                             ^  ^          ^ ^
        djnz le068h     ;e06f   10 f7      ;  close bit loop  --> --> -->'  |          | |
        inc e           ;e071   1c         ;                                ^          ^ ^
        dec c           ;e072   0d         ;                                |          | |
        jr nz,le065h    ;e073   20 f0      ;  close byte loop --> --> --> --'          ^ ^
        pop hl          ;e075   e1         ;  restore HL                               | |
        pop bc          ;e076   c1         ;  restore BC                               ^ ^
        djnz le019h     ;e077   10 a0      ;  close special char rotation loop  --> -->' |
        ld b,004h       ;e079   06 04      ;  4 character rotations                      ^
        dec c           ;e07b   0d         ;                                             |
        exx             ;e07c   d9         ;                                             ^
        jr nz,le010h    ;e07d   20 91      ;  close special char loop --> --> --> --> -->'

; copy left 3rd to the right third
        ld de,T_buffer  ;e07f   11 00 99   ;  load DE with T_buffer
        ld b,068h       ;e082   06 68      ;  (8 TURBO + 5 SPECTRUM) * 8 lines/char = 104 lines
le084h:
        push bc         ;e084   c5         ;  save BC <-- <-- <-- <,
        ld h,d          ;e085   62         ;b copy first 32 bytes  |
        ld l,e          ;e086   6b         ;| in the 96 byte line  ^
        ld bc,00040h    ;e087   01 40 00   ;| to the last 32       |
        add hl,bc       ;e08a   09         ;|                      ^
        ex de,hl        ;e08b   eb         ;|                      |
        ld c,020h       ;e08c   0e 20      ;|                      ^
        ldir            ;e08e   ed b0      ;e                      |
        pop bc          ;e090   c1         ;  restore BC           ^
        djnz le084h     ;e091   10 f1      ;  close line loop --> -'

; generate 4 shifted versions of TURBO and 8 of SPECTRUM
        ld hl,T_buffer  ;e093   21 00 99   ;b copy TURBO from 09900h to 03000h
        ld de,03000h    ;e096   11 00 30   ;| 
        ld bc,01800h    ;e099   01 00 18   ;| 
        ldir            ;e09c   ed b0      ;e 
        ld de,0b0ffh    ;e09e   11 ff b0   ;b rotate left SPECTRUM
        ld hl,00f00h    ;e0a1   21 00 0f   ;| by 1 bit, 7 times
        ld b,023h       ;e0a4   06 23      ;| 7 x 5 = 35 character rows
        call Rotate     ;e0a6   cd d0 e0   ;e 
        ld de,02fffh    ;e0a9   11 ff 2f   ;  
        ld c,003h       ;e0ac   0e 03      ;  3 times
le0aeh:
        ld hl,01800h    ;e0ae   21 00 18   ;b rotate left TURBO by 1 bit <,
        ld b,008h       ;e0b1   06 08      ;| 8 character rows            ^
        call Rotate     ;e0b3   cd d0 e0   ;e                             |
        add hl,de       ;e0b6   19         ;b rotate left TURBO by 1 bit  ^
        ex de,hl        ;e0b7   eb         ;|                             |
        ld hl,00000h    ;e0b8   21 00 00   ;| in place                    ^
        ld b,008h       ;e0bb   06 08      ;| 8 character rows            |
        call Rotate     ;e0bd   cd d0 e0   ;e                             ^
        dec c           ;e0c0   0d         ;                              |
        jr nz,le0aeh    ;e0c1   20 eb      ;  close rotate TURBO loop  -->'

; set interrupt mode 2 with interrupt handler address at 0dfffh, i.e. 0e13ah
        ld bc,00800h    ;e0c3   01 00 08   ;  8 bit rotate, starting from horizontal char 0
        ld a,0dfh       ;e0c6   3e df      ;  
        ld i,a          ;e0c8   ed 47      ;  
        im 2            ;e0ca   ed 5e      ;  
le0cch:
        ei              ;e0cc   fb         ;  enable interrupts <-- <-- <-,
        halt            ;e0cd   76         ;  halt                        |
        jr le0cch       ;e0ce   18 fc      ;  loop until a key pressed -->'

Rotate: ; subroutine to Rotate left B 060h byte characters lines ending at DE+HL to DE
        push bc         ;e0d0   c5         ;  save BC  <-- <-- <-- <-- <--,
        ld c,008h       ;e0d1   0e 08      ;  8 lines per character       ^
le0d3h:                                    ;                              |
        push hl         ;e0d3   e5         ;  save HL <-- <-- <-- <-- <,  ^
        add hl,de       ;e0d4   19         ;  sdd DE to HL             |  |
        ld b,060h       ;e0d5   06 60      ;  060h bytes per line      ^  ^
le0d7h:                                    ;                           |  |
        ld a,(hl)       ;e0d7   7e         ;b rotate left 1 bit <,     ^  ^
        rla             ;e0d8   17         ;|                    |     |  |
        ld (de),a       ;e0d9   12         ;|                    ^     ^  ^
        dec hl          ;e0da   2b         ;|                    |     |  |
        dec de          ;e0db   1b         ;e                    ^     ^  ^
        djnz le0d7h     ;e0dc   10 f9      ;  close byte loop -->'     |  |
        jr nc,le0e5h    ;e0de   30 05      ;                           ^  ^
        ld hl,00060h    ;e0e0   21 60 00   ;b increment the last byte  |  |
        add hl,de       ;e0e3   19         ;| in case a 1 bit was      ^  ^
        inc (hl)        ;e0e4   34         ;e shifted out on the left  |  |
le0e5h:                                    ;                           ^  ^
        pop hl          ;e0e5   e1         ;  restore HL               |  |
        dec c           ;e0e6   0d         ;                           ^  ^
        jr nz,le0d3h    ;e0e7   20 ea      ;  close line loop --> --> -'  |
        pop bc          ;e0e9   c1         ;  restore BC                  ^
        djnz Rotate     ;e0ea   10 e4      ;  close character row loop -->'
        ret             ;e0ec   c9         ;  return

Copy:   ; subroutine to copy 1 line of L character rows starting from IX to IY-010h
        ld (HL_save),hl ;e0ed   22 16 db   ;  save HL <-- <-- <-- <-- <-- <-- <-,
        ld sp,ix        ;e0f0   dd f9      ;b read 16 bytes from (IX)           |
        pop af          ;e0f2   f1         ;|                                   ^
        pop bc          ;e0f3   c1         ;|                                   |
        pop de          ;e0f4   d1         ;|                                   ^
        pop hl          ;e0f5   e1         ;|                                   |
        ex af,af'       ;e0f6   08         ;|                                   ^
        exx             ;e0f7   d9         ;|                                   |
        pop af          ;e0f8   f1         ;|                                   ^
        pop bc          ;e0f9   c1         ;|                                   |
        pop de          ;e0fa   d1         ;|                                   ^
        pop hl          ;e0fb   e1         ;e                                   |
        ld sp,iy        ;e0fc   fd f9      ;b write them to (IY-010h)           ^
        push hl         ;e0fe   e5         ;|                                   |
        push de         ;e0ff   d5         ;|                                   ^
        push bc         ;e100   c5         ;|                                   |
        push af         ;e101   f5         ;|                                   ^
        ex af,af'       ;e102   08         ;|                                   |
        exx             ;e103   d9         ;|                                   ^
        push hl         ;e104   e5         ;|                                   |
        push de         ;e105   d5         ;|                                   ^
        push bc         ;e106   c5         ;|                                   |
        push af         ;e107   f5         ;e                                   ^
        ld de,00010h    ;e108   11 10 00   ;b next 16 bytes                     |
        add ix,de       ;e10b   dd 19      ;|                                   ^
        add iy,de       ;e10d   fd 19      ;e                                   |
        ld sp,ix        ;e10f   dd f9      ;b read 16 bytes from (IX)           ^
        pop af          ;e111   f1         ;|                                   |
        pop bc          ;e112   c1         ;|                                   ^
        pop de          ;e113   d1         ;|                                   |
        pop hl          ;e114   e1         ;|                                   ^
        ex af,af'       ;e115   08         ;|                                   |
        exx             ;e116   d9         ;|                                   ^
        pop af          ;e117   f1         ;|                                   |
        pop bc          ;e118   c1         ;|                                   ^
        pop de          ;e119   d1         ;|                                   |
        pop hl          ;e11a   e1         ;e                                   ^
        ld sp,iy        ;e11b   fd f9      ;b write them to (IY-010h)           |
        push hl         ;e11d   e5         ;|                                   ^
        push de         ;e11e   d5         ;|                                   |
        push bc         ;e11f   c5         ;|                                   ^
        push af         ;e120   f5         ;|                                   |
        ex af,af'       ;e121   08         ;|                                   ^
        exx             ;e122   d9         ;|                                   |
        push hl         ;e123   e5         ;|                                   ^
        push de         ;e124   d5         ;|                                   |
        push bc         ;e125   c5         ;|                                   ^
        push af         ;e126   f5         ;e                                   |
        ld de,00050h    ;e127   11 50 00   ;b move IX to next source line       ^
        add ix,de       ;e12a   dd 19      ;e                                   |
        ld e,010h       ;e12c   1e 10      ;b move IY to next destination line  ^
        add iy,de       ;e12e   fd 19      ;e                                   |
        ld hl,(HL_save) ;e130   2a 16 db   ;  restore HL                        ^
        dec l           ;e133   2d         ;                                    |
        jr nz,Copy      ;e134   20 b7      ;  close line loop --> --> --> --> --'
        ld sp,0e7fah    ;e136   31 fa e7   ;  restore SP
        ret             ;e139   c9         ;  return

IntHandler:                                ;  (07ffeh) holds the 0e0ceh Return Address
; There are 8 * 64 rotation phases, 8 bits per character and 64 characters,
; which take 512 frame interrupts, or 17 seconds
; The bit phase is stored to B and the character phase to C.
; TURBO rotates left 2 pixels per frame interrupt and SPECTRUM 1 pixel.
; TURBO completes a rotation cycle 2 times faster than SPECTRUM.
; Both TURBO and SPECTRUM completely exit the screen on the left before they reappear on the right.
        push bc         ;e13a   c5         ;  save BC to (0e7fch)

; copy SPECTRUM starting at 0c860h, the 4th character line of the 2nd third of the screen
        ld a,039h       ;e13b   3e 39      ;b determine high address of SPECTRUM source
le13dh:                                    ;| 039h + 00fh * B
        add a,00fh      ;e13d   c6 0f      ;| B=8 ,B=7 ,B=6 ,B=5 ,B=4 ,B=3 ,B=2 ,B=1 
        djnz le13dh     ;e13f   10 fc      ;e 0b1h,0a2h,093h,084h,075h,066h,057h,048h
        ld ixh,a        ;e141   dd 67      ;b source address
        ld ixl,c        ;e143   dd 69      ;e 
        ld iy,0c870h    ;e145   fd 21 70 c8;  destination address + 010h
        ld hl,00805h    ;e149   21 05 08   ;  8 lines of 5 character rows
le14ch:
        call Copy       ;e14c   cd ed e0   ;  (0e7fah) holds 0e14fh <-- <-- <--,
        ld de,00060h    ;e14f   11 60 00   ;b set IY to next destination line  ^
        add iy,de       ;e152   fd 19      ;e                                  |
        ld l,005h       ;e154   2e 05      ;  5 character rows                 ^
        dec h           ;e156   25         ;                                   |
        jr nz,le14ch    ;e157   20 f3      ;  close SPECTRUM loop --> --> --> -'

; keyboard test
        xor a           ;e159   af         ;  
        ld b,064h       ;e15a   06 64      ;  100 loops
le15ch:                                    ;  delay = 4495/3500 = 1.28 ms  cycles
        in a,(0feh)     ;e15c   db fe      ;  A=in(000feh) <-- <-- <-- <-,   11
        and 01fh        ;e15e   e6 1f      ;                             |    7
        cp 01fh         ;e160   fe 1f      ;                             ^    7
        jr nz,Key       ;e162   20 36      ;  jump if a key was pressed  |    7
        djnz le15ch     ;e164   10 f6      ;  close keyboard test --> -->'   13,8: 45*100-5=4495

; copy TURBO to the 1st third of the screen
le166h:
        pop bc          ;e166   c1         ;  restore BC
        push bc         ;e167   c5         ;  save BC to (0e7fch)
        ld a,b          ;e168   78         ;b TURBO is rotated twice as fast (2 bits at a time)
        dec a           ;e169   3d         ;| C *= 2;
        and 003h        ;e16a   e6 03      ;| if (B>4) B-=4;
        inc a           ;e16c   3c         ;| else C++;
        cp b            ;e16d   b8         ;| 
        ld b,a          ;e16e   47         ;| 
        ld a,c          ;e16f   79         ;| 
        ccf             ;e170   3f         ;| 
        rla             ;e171   17         ;| 
        and 03fh        ;e172   e6 3f      ;| 
        ld c,a          ;e174   4f         ;e 
        ld a,0d0h       ;e175   3e d0      ;b determine high address of TURBO source
le177h:                                    ;| 0d0h + 018h * B
        add a,018h      ;e177   c6 18      ;| B=4 ,B=3 ,B=2 ,B=1 
        djnz le177h     ;e179   10 fc      ;e 030h,018h,000h,0e8h
        ld ixh,a        ;e17b   dd 67      ;b source address
        ld ixl,c        ;e17d   dd 69      ;e 
        ld iy,0c010h    ;e17f   fd 21 10 c0;  destination address + 010h
        ld hl,00808h    ;e183   21 08 08   ;  8 lines of 8 character rows
le186h:
        call Copy       ;e186   cd ed e0   ;  (0e7fah) holds 0e189h <-,
        ld l,008h       ;e189   2e 08      ;  8 character rows        ^
        dec h           ;e18b   25         ;                          |
        jr nz,le186h    ;e18c   20 f8      ;  close TURBO loop --> -->'
        pop bc          ;e18e   c1         ;  restore BC
        djnz le198h     ;e18f   10 07      ;  jump if not done with bits
        ld b,008h       ;e191   06 08      ;  8 bits
        inc c           ;e193   0c         ;b increment C modulo 64
        ld a,c          ;e194   79         ;| 
        and 03fh        ;e195   e6 3f      ;| 
        ld c,a          ;e197   4f         ;e 
le198h:
        reti            ;e198   ed 4d      ;  return from interrupt

; a key was pressed
Key:
        ld a,0fbh       ;e19a   3e fb      ;  
        in a,(0feh)     ;e19c   db fe      ;  A=in(0fbfeh), row KAa
        and 010h        ;e19e   e6 10      ;  
        jr z,T_key      ;e1a0   28 69      ;  jump if T was pressed
        ld a,0fdh       ;e1a2   3e fd      ;  
        in a,(0feh)     ;e1a4   db fe      ;  A=in(0fdfeh), row KA9
        and 002h        ;e1a6   e6 02      ;  
        jr z,S_key      ;e1a8   28 60      ;  jump if S was pressed

; go to Spectrum mode
        xor a           ;e1aa   af         ;b out(000feh)=000h
        out (0feh),a    ;e1ab   d3 fe      ;e set black border and clear O5 (input to !CE flop)
        in a,(0feh)     ;e1ad   db fe      ;  !CE=0; A=in(000feh), row KA9
        ld hl,00558h    ;e1af   21 58 05   ;b copy Spectrum code
        ld de,08000h    ;e1b2   11 00 80   ;| from (00558h)-(03dc5h)
        ld bc,0386eh    ;e1b5   01 6e 38   ;| to   (08000h)-(0b86dh)
        ldir            ;e1b8   ed b0      ;e 
        ld a,0ffh       ;e1ba   3e ff      ;b fill (0b86eh)-(0bcffh)
        ld bc,09205h    ;e1bc   01 05 92   ;| with 0ffh
le1bfh:                                    ;| 
        ld (de),a       ;e1bf   12         ;| 
        inc de          ;e1c0   13         ;| 
        djnz le1bfh     ;e1c1   10 fc      ;| 
        dec c           ;e1c3   0d         ;| 
        jr nz,le1bfh    ;e1c4   20 f9      ;e 
        inc a           ;e1c6   3c         ;b fill (0bd00h)-(0bd07h)
        ld b,008h       ;e1c7   06 08      ;| with 000h
le1c9h:                                    ;| (blank character)
        ld (de),a       ;e1c9   12         ;| 
        inc de          ;e1ca   13         ;| 
        djnz le1c9h     ;e1cb   10 fc      ;e 
        ld b,05fh       ;e1cd   06 5f      ;  unpack remaining 05fh characters
le1cfh:                                    ;  
        push bc         ;e1cf   c5         ;  save BC <-- <-- <-- <,
        ld bc,00006h    ;e1d0   01 06 00   ;                       |
        ld (de),a       ;e1d3   12         ;  first byte is 000h   ^
        inc de          ;e1d4   13         ;                       |
        ldir            ;e1d5   ed b0      ;  copy middle 6 bytes  ^
        ld (de),a       ;e1d7   12         ;   last byte is 000h   |
        inc de          ;e1d8   13         ;                       ^
        pop bc          ;e1d9   c1         ;  restore BC           |
        djnz le1cfh     ;e1da   10 f3      ;  close character loop '
        ld bc,0000ch    ;e1dc   01 0c 00   ;  12 bytes
        ld hl,Bytes     ;e1df   21 f3 e1   ;  Bytes structure
        ld de,0bd27h    ;e1e2   11 27 bd   ;  start address
le1e5h:
        ldi             ;e1e5   ed a0      ;  copy (HL) to (DE) <,
        jp po,Done      ;e1e7   e2 df e2   ;  jump if done       |
        ld a,(hl)       ;e1ea   7e         ;b add increment      ^
        inc hl          ;e1eb   23         ;|                    |
        add a,e         ;e1ec   83         ;|                    ^
        ld e,a          ;e1ed   5f         ;e                    |
        jr nc,le1e5h    ;e1ee   30 f5      ;b close              ^
        inc d           ;e1f0   14         ;| byte               |
        jr le1e5h       ;e1f1   18 f2      ;e loop --> --> --> --'

Bytes:                                     ; Bytes structure
        defb         008h                  ;            byte c
        defb    03fh,010h                  ; increment, byte b
        defb    077h,020h                  ; increment, byte a
        defb    08fh,000h                  ; increment, byte 9
        defb    08fh,0ffh                  ; increment, byte 8
        defb    03fh,038h                  ; increment, byte 7
        defb    017h,018h                  ; increment, byte 6
        defb    02fh,040h                  ; increment, byte 5
        defb    007h,006h                  ; increment, byte 4
        defb    03fh,038h                  ; increment, byte 3
        defb    028h,03ch                  ; increment, byte 2
        defb    006h,03ch                  ; increment, byte 1

S_key:                                     ;  E=060h
        add a,e         ;e20a   83         ;  A=060h
T_key:                                     ;  A=000h
        push af         ;e20b   f5         ;  save AF
        ld ix,08000h    ;e20c   dd 21 00 80;  
        ld a,0ffh       ;e210   3e ff      ;  
        ld de,04000h    ;e212   11 00 40   ;  
        jr nz,le21ah    ;e215   20 03      ;  jump if S was pressed
        ld ixh,000h     ;e217   dd 26 00   ;  
le21ah:                                    ;  IX = T_was_pressed ? 00000h : 08000h
        and a           ;e21a   a7         ;  
        scf             ;e21b   37         ;  
        ex af,af'       ;e21c   08         ;  
        ld a,02fh       ;e21d   3e 2f      ;b out(002feh)=02fh
        out (0feh),a    ;e21f   d3 fe      ;e set white border, TO, and O5 (input to !CE flop)
        ld hl,le2c8h    ;e221   21 c8 e2   ;  
        push hl         ;e224   e5         ;  push 0e2c8h
        in a,(0feh)     ;e225   db fe      ;  !CE=1; A=in(002feh)
        rra             ;e227   1f         ;  
        and 020h        ;e228   e6 20      ;  
        or 002h         ;e22a   f6 02      ;  
        ld c,a          ;e22c   4f         ;  C is 022h 0r 002h
        cp a            ;e22d   bf         ;  Z=0
le22eh:
        ret nz          ;e22e   c0         ;  jump if NZ (Space was pressed) to le2c8h
le22fh:
        call sub_e2aah  ;e22f   cd aa e2   ;  check is Space was pressed
        jr nc,le22eh    ;e232   30 fa      ;  jump if Space was pressed
        ld hl,00415h    ;e234   21 15 04   ;  1045 loops
le237h:                                    ;  delay = 3499700/3500 = 1000 ms
        djnz le237h     ;e237   10 fe      ;  13*256-5=3323 <-, cycles
        dec hl          ;e239   2b         ;                  |    6
        ld a,h          ;e23a   7c         ;                  ^    4
        or l            ;e23b   b5         ;                  |    4
        jr nz,le237h    ;e23c   20 f9      ;  close loop --> -'   12,7: 3349*1045-5=3499700
        call sub_e2a6h  ;e23e   cd a6 e2   ;  check is Space was pressed
        jr nc,le22eh    ;e241   30 eb      ;  jump if Space was pressed
le243h:
        ld b,09ch       ;e243   06 9c      ;  
        call sub_e2a6h  ;e245   cd a6 e2   ;  
        jr nc,le22eh    ;e248   30 e4      ;  
        ld a,0c6h       ;e24a   3e c6      ;  
        cp b            ;e24c   b8         ;  
        jr nc,le22fh    ;e24d   30 e0      ;  
        inc h           ;e24f   24         ;  
        jr nz,le243h    ;e250   20 f1      ;  
le252h:
        ld b,0c9h       ;e252   06 c9      ;  
        call sub_e2aah  ;e254   cd aa e2   ;  
        jr nc,le22eh    ;e257   30 d5      ;  
        ld a,b          ;e259   78         ;  
        cp 0d4h         ;e25a   fe d4      ;  
        jr nc,le252h    ;e25c   30 f4      ;  
        call sub_e2aah  ;e25e   cd aa e2   ;  
        ret nc          ;e261   d0         ;  
        ld a,c          ;e262   79         ;  
        xor 003h        ;e263   ee 03      ;  
        ld c,a          ;e265   4f         ;  
        ld h,000h       ;e266   26 00      ;  
        ld b,0b0h       ;e268   06 b0      ;  
        jr le28bh       ;e26a   18 1f      ;  
le26ch:
        ex af,af'       ;e26c   08         ;  
        jr nz,le276h    ;e26d   20 07      ;  
        jr nc,le280h    ;e26f   30 0f      ;  
        ld (ix+000h),l  ;e271   dd 75 00   ;  
        jr le285h       ;e274   18 0f      ;  
le276h:
        rl c            ;e276   cb 11      ;  
        xor l           ;e278   ad         ;  
        ret nz          ;e279   c0         ;  
        ld a,c          ;e27a   79         ;  
        rra             ;e27b   1f         ;  
        ld c,a          ;e27c   4f         ;  
        inc de          ;e27d   13         ;  
        jr le287h       ;e27e   18 07      ;  
le280h:
        ld a,(ix+000h)  ;e280   dd 7e 00   ;  
        xor l           ;e283   ad         ;  
        ret nz          ;e284   c0         ;  
le285h:
        inc ix          ;e285   dd 23      ;  
le287h:
        dec de          ;e287   1b         ;  
        ex af,af'       ;e288   08         ;  
        ld b,0b2h       ;e289   06 b2      ;  
le28bh:
        ld l,001h       ;e28b   2e 01      ;  
le28dh:
        call sub_e2a6h  ;e28d   cd a6 e2   ;  
        ret nc          ;e290   d0         ;  
        ld a,0cbh       ;e291   3e cb      ;  
        cp b            ;e293   b8         ;  
        rl l            ;e294   cb 15      ;  
        ld b,0b0h       ;e296   06 b0      ;  
        jp nc,le28dh    ;e298   d2 8d e2   ;  
        ld a,h          ;e29b   7c         ;  
        xor l           ;e29c   ad         ;  
        ld h,a          ;e29d   67         ;  
        ld a,d          ;e29e   7a         ;  
        or e            ;e29f   b3         ;  
        jr nz,le26ch    ;e2a0   20 ca      ;  
        ld a,h          ;e2a2   7c         ;  
        cp 001h         ;e2a3   fe 01      ;  
        ret             ;e2a5   c9         ;  
sub_e2a6h:
        call sub_e2aah  ;e2a6   cd aa e2   ;  
        ret nc          ;e2a9   d0         ;  

sub_e2aah:                                 ;  B is 064h to 001h
        ld a,016h       ;e2aa   3e 16      ;  22 loops
le2ach:                                    ;  delay = 347/3500 = 0.099 ms
        dec a           ;e2ac   3d         ;  decrement <--,   4    cycles
        jr nz,le2ach    ;e2ad   20 fd      ;  close loop ->'  12,7: 16*22-5=347
        and a           ;e2af   a7         ;  CY=0
le2b0h:
        inc b           ;e2b0   04         ;  
        ret z           ;e2b1   c8         ;  return if B=0 and redo the loop
        ld a,07fh       ;e2b2   3e 7f      ;  
        in a,(0feh)     ;e2b4   db fe      ;  A=in(07ffeh), row KAf
        rra             ;e2b6   1f         ;  
        ret nc          ;e2b7   d0         ;  return if Space was pressed
        xor c           ;e2b8   a9         ;  
        and 020h        ;e2b9   e6 20      ;  A is 020h if TI changed state, 000h if not
        jr z,le2b0h     ;e2bb   28 f3      ;  wait for TI to change state
        ld a,c          ;e2bd   79         ;  
        cpl             ;e2be   2f         ;  
        ld c,a          ;e2bf   4f         ;  C=0dfh
        and 007h        ;e2c0   e6 07      ;  
        or 028h         ;e2c2   f6 28      ;b out(002feh)=02fh
        out (0feh),a    ;e2c4   d3 fe      ;e set white border, TO, and O5 (input to !CE flop)
        scf             ;e2c6   37         ;  
        ret             ;e2c7   c9         ;  
le2c8h:
        jr c,le2dbh     ;e2c8   38 11      ;  jump if CY to le2dbh
        pop af          ;e2ca   f1         ;  
        ld h,003h       ;e2cb   26 03      ;  768 to 1024 loops
le2cdh:                                    ;  delay = 2572027/3500 = 735 to 980 ms
        djnz le2cdh     ;e2cd   10 fe      ;  13*256-5=3323 <-, cycles
        dec hl          ;e2cf   2b         ;                  |    6
        ld a,h          ;e2d0   7c         ;                  ^    4
        or l            ;e2d1   b5         ;                  |    4
        jr nz,le2cdh    ;e2d2   20 f9      ;  close loop --> -'   12,7: 3349*768-5=2572027
        ld a,020h       ;e2d4   3e 20      ;b out(020feh)=020h
        out (0feh),a    ;e2d6   d3 fe      ;e set black border and O5 (input to !CE flop)
        jp le166h       ;e2d8   c3 66 e1   ;  continue TURBO rotation
le2dbh:
        pop af          ;e2db   f1         ;  
        jp z,00000h     ;e2dc   ca 00 00   ;  jump if Z to 00000h

Done:
        ld sp,00000h    ;e2df   31 00 00   ;  initialize SP=00000h
        ld a,060h       ;e2e2   3e 60      ;b out(060feh)=060h
        out (0feh),a    ;e2e4   d3 fe      ;e set black border, O5 and O6 (inputs to !CE and SM flops)
        ld hl,0fedbh    ;e2e6   21 db fe   ;  
        push hl         ;e2e9   e5         ;  write 0dbfeh at (0fffeh) - this is: in a,(0feh)
        jp 0fffeh       ;e2ea   c3 fe ff   ;  jump and execute it;  A=in(060feh)
; then start executing in Spectrum mode from address 00000h

C_base:                                                ; Character Base Address
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; char 0
        defb    0f0h,0f0h,0f0h,0f0h,0f0h,0f0h,0f0h,0f0h; char 1
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0fch,0f0h,080h; char 2
        defb    0ffh,0feh,0feh,0feh,0fch,0fch,0f8h,0f8h; char 3
        defb    0feh,0fch,0fch,0f8h,0f0h,0e0h,080h,000h; char 4
        defb    0f8h,0f0h,0e0h,0c0h,080h,000h,000h,000h; char 5
        defb    0f0h,0e0h,0c0h,080h,000h,000h,000h,000h; char 6
        defb    0c0h,080h,000h,000h,000h,000h,000h,000h; char 7
        defb    080h,000h,000h,000h,000h,000h,000h,000h; char 8

C_struct:                                              ; Char structure Base address
        defw    09901h                                 ; Character 0 start address
        defb    0b7h                                   ; number of char increments
        defb    001h,001h,001h,001h,002h,001h,002h,001h; TURBO char increments
        defb    002h,001h,001h,004h,001h,001h,006h,046h;
        defb    001h,001h,001h,001h,002h,001h,002h,001h;
        defb    002h,001h,001h,001h,003h,001h,001h,001h;
        defb    004h,001h,001h,047h,004h,001h,002h,001h;
        defb    005h,001h,005h,001h,002h,001h,002h,001h;
        defb    046h,004h,001h,002h,001h,002h,001h,001h;
        defb    001h,003h,001h,001h,001h,003h,001h,002h;
        defb    001h,046h,004h,001h,002h,001h,002h,001h;
        defb    001h,001h,003h,001h,001h,001h,003h,001h;
        defb    002h,001h,046h,004h,001h,002h,001h,005h;
        defb    001h,005h,001h,002h,001h,002h,001h,046h;
        defb    005h,001h,001h,006h,001h,002h,001h,001h;
        defb    001h,004h,001h,001h,047h,006h,007h,001h;
        defb    002h,001h,001h,006h;
        defb                        046h,003h,001h,003h; SPECTRUM char increments
        defb    001h,001h,003h,003h,001h,001h,002h,001h;
        defb    003h,002h,002h,003h,041h,004h,002h,002h;
        defb    004h,005h,003h,002h,002h,002h,002h,001h;
        defb    001h,001h,042h,003h,001h,003h,001h,003h;
        defb    005h,003h,001h,003h,002h,002h,003h,043h;
        defb    002h,004h,004h,005h,003h,001h,003h,002h;
        defb    002h,003h,042h,003h,004h,001h,001h,003h;
        defb    004h,003h,002h,003h,003h,003h

        defw    099c4h                                 ; Character 1, rotation 0 start address
        defb    006h                                   ; number of char increments
        defb    060h,060h,060h,060h,060h               ; increments
        defw    0b220h                                 ; Character 1, rotation 1 start address
        defb    002h                                   ; number of char increments
        defb    00eh                                   ; increments
        defw    099c2h                                 ; Character 1, rotation 2 start address
        defb    006h                                   ; number of char increments
        defb    060h,060h,060h,060h,060h               ; increments
        defw    0b162h                                 ; Character 1, rotation 3 start address
        defb    002h                                   ; number of char increments
        defb    00ch                                   ; increments

        defw    09baah                                 ; Character 2, rotation 0 start address
        defb    003h                                   ; number of char increments
        defb    00ch,006h                              ; increments
        defw    09971h                                 ; Character 2, rotation 1 start address
        defb    007h                                   ; number of char increments
        defb    006h,006h,0c0h,000h,054h,006h          ; increments
        defw    0991ah                                 ; Character 2, rotation 2 start address
        defb    001h                                   ; number of char increments
        defw    09b47h                                 ; Character 2, rotation 3 start address
        defb    002h                                   ; number of char increments
        defb    012h                                   ; increments

        defw    09a31h                                 ; Character 3, rotation 0 start address
        defb    007h                                   ; number of char increments
        defb    006h,0c0h,000h,054h,00ch,006h          ; increments
        defw    09910h                                 ; Character 3, rotation 1 start address
        defb    003h                                   ; number of char increments
        defb    006h,006h                              ; increments
        defw    09979h                                 ; Character 3, rotation 2 start address
        defb    001h                                   ; number of char increments
        defw    09ba8h                                 ; Character 3, rotation 3 start address
        defb    002h                                   ; number of char increments
        defb    012h                                   ; increments

        defw    0b1c6h                                 ; Character 4, rotation 0 start address
        defb    005h                                   ; number of char increments
        defb    010h,0ach,00ch,00ch                    ; increments
        defw    0b102h                                 ; Character 4, rotation 1 start address
        defb    005h                                   ; number of char increments
        defb    004h,008h,008h,0ach                    ; increments
        defw    0b100h                                 ; Character 4, rotation 2 start address
        defb    002h                                   ; number of char increments
        defb    00ch                                   ; increments
        defw    0b1c0h                                 ; Character 4, rotation 3 start address
        defb    004h                                   ; number of char increments
        defb    0c0h,00ch,00ch                         ; increments

        defw    0b1deh                                 ; Character 5, rotation 0 start address
        defb    001h                                   ; number of char increments
        defw    0b11dh                                 ; Character 5, rotation 1 start address
        defb    004h                                   ; number of char increments
        defb    0c0h,000h,059h                         ; increments
        defw    0b11eh                                 ; Character 5, rotation 2 start address
        defb    001h                                   ; number of char increments
        defw    0b1ddh                                 ; Character 5, rotation 3 start address
        defb    002h                                   ; number of char increments
        defb    0b8h                                   ; increments

        defw    09babh                                 ; Character 6, rotation 0 start address
        defb    003h                                   ; number of char increments
        defb    00ch,006h                              ; increments
        defw    09911h                                 ; Character 6, rotation 1 start address
        defb    003h                                   ; number of char increments
        defb    006h,006h                              ; increments
        defw    09919h                                 ; Character 6, rotation 2 start address
        defb    001h                                   ; number of char increments
        defw    09ba7h                                 ; Character 6, rotation 3 start address
        defb    002h                                   ; number of char increments
        defb    012h                                   ; increments

        defw    099dbh                                 ; Character 7, rotation 0 start address
        defb    009h                                   ; number of char increments
        defb    0c0h,000h,0c0h,000h,0c4h,042h,00ch,011h; increments
        defw    09ae9h                                 ; Character 7, rotation 1 start address
        defb    00bh                                   ; number of char increments
        defb    012h,0c0h,000h,0a6h,01ch,0a4h,00ch,008h; increments
        defb    004h,05dh                              ; 
        defw    099cfh                                 ; Character 7, rotation 2 start address
        defb    00fh                                   ; number of char increments
        defb    006h,0c0h,000h,054h,00ch,006h,0c0h,000h; increments
        defb    0aah,010h,009h,0a3h,00ch,00ch          ; 
        defw    099cfh                                 ; Character 7, rotation 3 start address
        defb    012h                                   ; number of char increments
        defb    006h,006h,0c0h,000h,054h,006h,0c0h,000h; increments
        defb    067h,045h,004h,008h,008h,008h,0a4h,014h; 
        defb    061h                                   ; 

        defw    0b101h                                 ; Character 8, rotation 0 start address
        defb    004h                                   ; number of char increments
        defb    00ch,053h,00ch                         ; increments
        defw    0b160h                                 ; Character 8, rotation 1 start address
        defb    008h                                   ; number of char increments
        defb    061h,05fh,00ch,00ch,049h,00ch,00ch     ; increments
        defw    0b166h                                 ; Character 8, rotation 2 start address
        defb    00ah                                   ; number of char increments
        defb    010h,04fh,010h,04dh,00ch,00ch,047h,00ch; increments
        defb    00ch                                   ; 
        defw    0b101h                                 ; Character 8, rotation 3 start address
        defb    00ah                                   ; number of char increments
        defb    004h,008h,008h,04dh,004h,008h,008h,04bh; increments
        defb    061h                                   ; 

Spare:                                                 ; spare bytes
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh; 
        defb    0ffh,0ffh,0ffh,0ffh,0ffh               ; 

Spectrum:
        defb                                            0f3h,0afh,011h,0ffh,0ffh,0c3h,0cbh,011h;
        defb    02ah,05dh,05ch,022h,05fh,05ch,018h,043h,0c3h,0f2h,015h,0ffh,0ffh,0ffh,0ffh,0ffh;
        defb    02ah,05dh,05ch,07eh,0cdh,07dh,000h,0d0h,0cdh,074h,000h,018h,0f7h,0ffh,0ffh,0ffh;
        defb    0c3h,05bh,033h,0ffh,0ffh,0ffh,0ffh,0ffh,0c5h,02ah,061h,05ch,0e5h,0c3h,09eh,016h;
        defb    0f5h,0e5h,02ah,078h,05ch,023h,022h,078h,05ch,07ch,0b5h,020h,003h,0fdh,034h,040h;
        defb    0c5h,0d5h,0cdh,0bfh,002h,0d1h,0c1h,0e1h,0f1h,0fbh,0c9h,0e1h,06eh,0fdh,075h,000h;
        defb    0edh,07bh,03dh,05ch,0c3h,0c5h,016h,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0ffh,0f5h,0e5h;
        defb    02ah,0b0h,05ch,07ch,0b5h,020h,001h,0e9h,0e1h,0f1h,0edh,045h,02ah,05dh,05ch,023h;
        defb    022h,05dh,05ch,07eh,0c9h,0feh,021h,0d0h,0feh,00dh,0c8h,0feh,010h,0d8h,0feh,018h;
        defb    03fh,0d8h,023h,0feh,016h,038h,001h,023h,037h,022h,05dh,05ch,0c9h,0bfh,052h,04eh;
        defb    0c4h,049h,04eh,04bh,045h,059h,0a4h,050h,0c9h,046h,0ceh,050h,04fh,049h,04eh,0d4h;
        defb    053h,043h,052h,045h,045h,04eh,0a4h,041h,054h,054h,0d2h,041h,0d4h,054h,041h,0c2h;
        defb    056h,041h,04ch,0a4h,043h,04fh,044h,0c5h,056h,041h,0cch,04ch,045h,0ceh,053h,049h;
        defb    0ceh,043h,04fh,0d3h,054h,041h,0ceh,041h,053h,0ceh,041h,043h,0d3h,041h,054h,0ceh;
        defb    04ch,0ceh,045h,058h,0d0h,049h,04eh,0d4h,053h,051h,0d2h,053h,047h,0ceh,041h,042h;
        defb    0d3h,050h,045h,045h,0cbh,049h,0ceh,055h,053h,0d2h,053h,054h,052h,0a4h,043h,048h;
        defb    052h,0a4h,04eh,04fh,0d4h,042h,049h,0ceh,04fh,0d2h,041h,04eh,0c4h,03ch,0bdh,03eh;
        defb    0bdh,03ch,0beh,04ch,049h,04eh,0c5h,054h,048h,045h,0ceh,054h,0cfh,053h,054h,045h;
        defb    0d0h,044h,045h,046h,020h,046h,0ceh,043h,041h,0d4h,046h,04fh,052h,04dh,041h,0d4h;
        defb    04dh,04fh,056h,0c5h,045h,052h,041h,053h,0c5h,04fh,050h,045h,04eh,020h,0a3h,043h;
        defb    04ch,04fh,053h,045h,020h,0a3h,04dh,045h,052h,047h,0c5h,056h,045h,052h,049h,046h;
        defb    0d9h,042h,045h,045h,0d0h,043h,049h,052h,043h,04ch,0c5h,049h,04eh,0cbh,050h,041h;
        defb    050h,045h,0d2h,046h,04ch,041h,053h,0c8h,042h,052h,049h,047h,048h,0d4h,049h,04eh;
        defb    056h,045h,052h,053h,0c5h,04fh,056h,045h,0d2h,04fh,055h,0d4h,04ch,050h,052h,049h;
        defb    04eh,0d4h,04ch,04ch,049h,053h,0d4h,053h,054h,04fh,0d0h,052h,045h,041h,0c4h,044h;
        defb    041h,054h,0c1h,052h,045h,053h,054h,04fh,052h,0c5h,04eh,045h,0d7h,042h,04fh,052h;
        defb    044h,045h,0d2h,043h,04fh,04eh,054h,049h,04eh,055h,0c5h,044h,049h,0cdh,052h,045h;
        defb    0cdh,046h,04fh,0d2h,047h,04fh,020h,054h,0cfh,047h,04fh,020h,053h,055h,0c2h,049h;
        defb    04eh,050h,055h,0d4h,04ch,04fh,041h,0c4h,04ch,049h,053h,0d4h,04ch,045h,0d4h,050h;
        defb    041h,055h,053h,0c5h,04eh,045h,058h,0d4h,050h,04fh,04bh,0c5h,050h,052h,049h,04eh;
        defb    0d4h,050h,04ch,04fh,0d4h,052h,055h,0ceh,053h,041h,056h,0c5h,052h,041h,04eh,044h;
        defb    04fh,04dh,049h,05ah,0c5h,049h,0c6h,043h,04ch,0d3h,044h,052h,041h,0d7h,043h,04ch;
        defb    045h,041h,0d2h,052h,045h,054h,055h,052h,0ceh,043h,04fh,050h,0d9h,042h,048h,059h;
        defb    036h,035h,054h,047h,056h,04eh,04ah,055h,037h,034h,052h,046h,043h,04dh,04bh,049h;
        defb    038h,033h,045h,044h,058h,00eh,04ch,04fh,039h,032h,057h,053h,05ah,020h,00dh,050h;
        defb    030h,031h,051h,041h,0e3h,0c4h,0e0h,0e4h,0b4h,0bch,0bdh,0bbh,0afh,0b0h,0b1h,0c0h;
        defb    0a7h,0a6h,0beh,0adh,0b2h,0bah,0e5h,0a5h,0c2h,0e1h,0b3h,0b9h,0c1h,0b8h,07eh,0dch;
        defb    0dah,05ch,0b7h,07bh,07dh,0d8h,0bfh,0aeh,0aah,0abh,0ddh,0deh,0dfh,07fh,0b5h,0d6h;
        defb    07ch,0d5h,05dh,0dbh,0b6h,0d9h,05bh,0d7h,00ch,007h,006h,004h,005h,008h,00ah,00bh;
        defb    009h,00fh,0e2h,02ah,03fh,0cdh,0c8h,0cch,0cbh,05eh,0ach,02dh,02bh,03dh,02eh,02ch;
        defb    03bh,022h,0c7h,03ch,0c3h,03eh,0c5h,02fh,0c9h,060h,0c6h,03ah,0d0h,0ceh,0a8h,0cah;
        defb    0d3h,0d4h,0d1h,0d2h,0a9h,0cfh,02eh,02fh,011h,0ffh,0ffh,001h,0feh,0feh,0edh,078h;
        defb    02fh,0e6h,01fh,028h,00eh,067h,07dh,014h,0c0h,0d6h,008h,0cbh,03ch,030h,0fah,053h;
        defb    05fh,020h,0f4h,02dh,0cbh,000h,038h,0e6h,07ah,03ch,0c8h,0feh,028h,0c8h,0feh,019h;
        defb    0c8h,07bh,05ah,057h,0feh,018h,0c9h,0cdh,08eh,002h,0c0h,021h,000h,05ch,0cbh,07eh;
        defb    020h,007h,023h,035h,02bh,020h,002h,036h,0ffh,07dh,021h,004h,05ch,0bdh,020h,0eeh;
        defb    0cdh,01eh,003h,0d0h,021h,000h,05ch,0beh,028h,02eh,0ebh,021h,004h,05ch,0beh,028h;
        defb    027h,0cbh,07eh,020h,004h,0ebh,0cbh,07eh,0c8h,05fh,077h,023h,036h,005h,023h,03ah;
        defb    009h,05ch,077h,023h,0fdh,04eh,007h,0fdh,056h,001h,0e5h,0cdh,033h,003h,0e1h,077h;
        defb    032h,008h,05ch,0fdh,0cbh,001h,0eeh,0c9h,023h,036h,005h,023h,035h,0c0h,03ah,00ah;
        defb    05ch,077h,023h,07eh,018h,0eah,042h,016h,000h,07bh,0feh,027h,0d0h,0feh,018h,020h;
        defb    003h,0cbh,078h,0c0h,021h,005h,002h,019h,07eh,037h,0c9h,07bh,0feh,03ah,038h,02fh;
        defb    00dh,0fah,04fh,003h,028h,003h,0c6h,04fh,0c9h,021h,0ebh,001h,004h,028h,003h,021h;
        defb    005h,002h,016h,000h,019h,07eh,0c9h,021h,029h,002h,0cbh,040h,028h,0f4h,0cbh,05ah;
        defb    028h,00ah,0fdh,0cbh,030h,05eh,0c0h,004h,0c0h,0c6h,020h,0c9h,0c6h,0a5h,0c9h,0feh;
        defb    030h,0d8h,00dh,0fah,09dh,003h,020h,019h,021h,054h,002h,0cbh,068h,028h,0d3h,0feh;
        defb    038h,030h,007h,0d6h,020h,004h,0c8h,0c6h,008h,0c9h,0d6h,036h,004h,0c8h,0c6h,0feh;
        defb    0c9h,021h,030h,002h,0feh,039h,028h,0bah,0feh,030h,028h,0b6h,0e6h,007h,0c6h,080h;
        defb    004h,0c8h,0eeh,00fh,0c9h,004h,0c8h,0cbh,068h,021h,030h,002h,020h,0a4h,0d6h,010h;
        defb    0feh,022h,028h,006h,0feh,020h,0c0h,03eh,05fh,0c9h,03eh,040h,0c9h,0f3h,07dh,0cbh;
        defb    03dh,0cbh,03dh,02fh,0e6h,003h,04fh,006h,000h,0ddh,021h,0d1h,003h,0ddh,009h,03ah;
        defb    048h,05ch,0e6h,038h,00fh,00fh,00fh,0f6h,008h,000h,000h,000h,004h,00ch,00dh,020h;
        defb    0fdh,00eh,03fh,005h,0c2h,0d6h,003h,0eeh,010h,0d3h,0feh,044h,04fh,0cbh,067h,020h;
        defb    009h,07ah,0b3h,028h,009h,079h,04dh,01bh,0ddh,0e9h,04dh,00ch,0ddh,0e9h,0fbh,0c9h;
        defb    0efh,031h,027h,0c0h,003h,034h,0ech,06ch,098h,01fh,0f5h,004h,0a1h,00fh,038h,021h;
        defb    092h,05ch,07eh,0a7h,020h,05eh,023h,04eh,023h,046h,078h,017h,09fh,0b9h,020h,054h;
        defb    023h,0beh,020h,050h,078h,0c6h,03ch,0f2h,025h,004h,0e2h,06ch,004h,006h,0fah,004h;
        defb    0d6h,00ch,030h,0fbh,0c6h,00ch,0c5h,021h,06eh,004h,0cdh,006h,034h,0cdh,0b4h,033h;
        defb    0efh,004h,038h,0f1h,086h,077h,0efh,0c0h,002h,031h,038h,0cdh,094h,01eh,0feh,00bh;
        defb    030h,022h,0efh,0e0h,004h,0e0h,034h,080h,043h,055h,09fh,080h,001h,005h,034h,035h;
        defb    071h,003h,038h,0cdh,099h,01eh,0c5h,0cdh,099h,01eh,0e1h,050h,059h,07ah,0b3h,0c8h;
        defb    01bh,0c3h,0b5h,003h,0cfh,00ah,089h,002h,0d0h,012h,086h,089h,00ah,097h,060h,075h;
        defb    089h,012h,0d5h,017h,01fh,089h,01bh,090h,041h,002h,089h,024h,0d0h,053h,0cah,089h;
        defb    02eh,09dh,036h,0b1h,089h,038h,0ffh,049h,03eh,089h,043h,0ffh,06ah,073h,089h,04fh;
        defb    0a7h,000h,054h,089h,05ch,000h,000h,000h,089h,069h,014h,0f6h,024h,089h,076h,0f1h;
        defb    010h,005h,0cdh,0fbh,024h,03ah,03bh,05ch,087h,0fah,08ah,01ch,0e1h,0d0h,0e5h,0cdh;
        defb    0f1h,02bh,062h,06bh,00dh,0f8h,009h,0cbh,0feh,0c9h,021h,03fh,005h,0e5h,021h,080h;
        defb    01fh,0cbh,07fh,028h,003h,021h,098h,00ch,008h,013h,0ddh,02bh,0f3h,03eh,002h,047h;
        defb    010h,0feh,0d3h,0feh,0eeh,00fh,006h,0a4h,02dh,020h,0f5h,005h,025h,0f2h,0d8h,004h;
        defb    006h,02fh,010h,0feh,0d3h,0feh,03eh,00dh,006h,037h,010h,0feh,0d3h,0feh,001h,00eh;
        defb    03bh,008h,06fh,0c3h,007h,005h,07ah,0b3h,028h,00ch,0ddh,06eh,000h,07ch,0adh,067h;
        defb    03eh,001h,037h,0c3h,025h,005h,06ch,018h,0f4h,079h,0cbh,078h,010h,0feh,030h,004h;
        defb    006h,042h,010h,0feh,0d3h,0feh,006h,03eh,020h,0efh,005h,0afh,03ch,0cbh,015h,0c2h;
        defb    014h,005h,01bh,0ddh,023h,006h,031h,03eh,07fh,0dbh,0feh,01fh,0d0h,07ah,03ch,0c2h;
        defb    0feh,004h,006h,03bh,010h,0feh,0c9h,0f5h,03ah,048h,05ch,0e6h,038h,00fh,00fh,00fh;
        defb    0d3h,0feh,03eh,07fh,0dbh,0feh,01fh,0fbh,038h,002h,0cfh,00ch,0f1h,0c9h,014h,008h;
        defb    015h,0f3h,03eh,00fh,0d3h,0feh,021h,03fh,005h,0e5h,0dbh,0feh,01fh,0e6h,020h,0f6h;
        defb    002h,04fh,0bfh,0c0h,0cdh,0e7h,005h,030h,0fah,021h,015h,004h,010h,0feh,02bh,07ch;
        defb    0b5h,020h,0f9h,0cdh,0e3h,005h,030h,0ebh,006h,09ch,0cdh,0e3h,005h,030h,0e4h,03eh;
        defb    0c6h,0b8h,030h,0e0h,024h,020h,0f1h,006h,0c9h,0cdh,0e7h,005h,030h,0d5h,078h,0feh;
        defb    0d4h,030h,0f4h,0cdh,0e7h,005h,0d0h,079h,0eeh,003h,04fh,026h,000h,006h,0b0h,018h;
        defb    01fh,008h,020h,007h,030h,00fh,0ddh,075h,000h,018h,00fh,0cbh,011h,0adh,0c0h,079h;
        defb    01fh,04fh,013h,018h,007h,0ddh,07eh,000h,0adh,0c0h,0ddh,023h,01bh,008h,006h,0b2h;
        defb    02eh,001h,0cdh,0e3h,005h,0d0h,03eh,0cbh,0b8h,0cbh,015h,006h,0b0h,0d2h,0cah,005h;
        defb    07ch,0adh,067h,07ah,0b3h,020h,0cah,07ch,0feh,001h,0c9h,0cdh,0e7h,005h,0d0h,03eh;
        defb    016h,03dh,020h,0fdh,0a7h,004h,0c8h,03eh,07fh,0dbh,0feh,01fh,0d0h,0a9h,0e6h,020h;
        defb    028h,0f3h,079h,02fh,04fh,0e6h,007h,0f6h,008h,0d3h,0feh,037h,0c9h,0f1h,03ah,074h;
        defb    05ch,0d6h,0e0h,032h,074h,05ch,0cdh,08ch,01ch,0cdh,030h,025h,028h,03ch,001h,011h;
        defb    000h,03ah,074h,05ch,0a7h,028h,002h,00eh,022h,0f7h,0d5h,0ddh,0e1h,006h,00bh,03eh;
        defb    020h,012h,013h,010h,0fch,0ddh,036h,001h,0ffh,0cdh,0f1h,02bh,021h,0f6h,0ffh,00bh;
        defb    009h,003h,030h,00fh,03ah,074h,05ch,0a7h,020h,002h,0cfh,00eh,078h,0b1h,028h,00ah;
        defb    001h,00ah,000h,0ddh,0e5h,0e1h,023h,0ebh,0edh,0b0h,0dfh,0feh,0e4h,020h,049h,03ah;
        defb    074h,05ch,0feh,003h,0cah,08ah,01ch,0e7h,0cdh,0b2h,028h,0cbh,0f9h,030h,00bh,021h;
        defb    000h,000h,03ah,074h,05ch,03dh,028h,015h,0cfh,001h,0c2h,08ah,01ch,0cdh,030h,025h;
        defb    028h,018h,023h,07eh,0ddh,077h,00bh,023h,07eh,0ddh,077h,00ch,023h,0ddh,071h,00eh;
        defb    03eh,001h,0cbh,071h,028h,001h,03ch,0ddh,077h,000h,0ebh,0e7h,0feh,029h,020h,0dah;
        defb    0e7h,0cdh,0eeh,01bh,0ebh,0c3h,05ah,007h,0feh,0aah,020h,01fh,03ah,074h,05ch,0feh;
        defb    003h,0cah,08ah,01ch,0e7h,0cdh,0eeh,01bh,0ddh,036h,00bh,000h,0ddh,036h,00ch,01bh;
        defb    021h,000h,040h,0ddh,075h,00dh,0ddh,074h,00eh,018h,04dh,0feh,0afh,020h,04fh,03ah;
        defb    074h,05ch,0feh,003h,0cah,08ah,01ch,0e7h,0cdh,048h,020h,020h,00ch,03ah,074h,05ch;
        defb    0a7h,0cah,08ah,01ch,0cdh,0e6h,01ch,018h,00fh,0cdh,082h,01ch,0dfh,0feh,02ch,028h;
        defb    00ch,03ah,074h,05ch,0a7h,0cah,08ah,01ch,0cdh,0e6h,01ch,018h,004h,0e7h,0cdh,082h;
        defb    01ch,0cdh,0eeh,01bh,0cdh,099h,01eh,0ddh,071h,00bh,0ddh,070h,00ch,0cdh,099h,01eh;
        defb    0ddh,071h,00dh,0ddh,070h,00eh,060h,069h,0ddh,036h,000h,003h,018h,044h,0feh,0cah;
        defb    028h,009h,0cdh,0eeh,01bh,0ddh,036h,00eh,080h,018h,017h,03ah,074h,05ch,0a7h,0c2h;
        defb    08ah,01ch,0e7h,0cdh,082h,01ch,0cdh,0eeh,01bh,0cdh,099h,01eh,0ddh,071h,00dh,0ddh;
        defb    070h,00eh,0ddh,036h,000h,000h,02ah,059h,05ch,0edh,05bh,053h,05ch,037h,0edh,052h;
        defb    0ddh,075h,00bh,0ddh,074h,00ch,02ah,04bh,05ch,0edh,052h,0ddh,075h,00fh,0ddh,074h;
        defb    010h,0ebh,03ah,074h,05ch,0a7h,0cah,070h,009h,0e5h,001h,011h,000h,0ddh,009h,0ddh;
        defb    0e5h,011h,011h,000h,0afh,037h,0cdh,056h,005h,0ddh,0e1h,030h,0f2h,03eh,0feh,0cdh;
        defb    001h,016h,0fdh,036h,052h,003h,00eh,080h,0ddh,07eh,000h,0ddh,0beh,0efh,020h,002h;
        defb    00eh,0f6h,0feh,004h,030h,0d9h,011h,0c0h,009h,0c5h,0cdh,00ah,00ch,0c1h,0ddh,0e5h;
        defb    0d1h,021h,0f0h,0ffh,019h,006h,00ah,07eh,03ch,020h,003h,079h,080h,04fh,013h,01ah;
        defb    0beh,023h,020h,001h,00ch,0d7h,010h,0f6h,0cbh,079h,020h,0b3h,03eh,00dh,0d7h,0e1h;
        defb    0ddh,07eh,000h,0feh,003h,028h,00ch,03ah,074h,05ch,03dh,0cah,008h,008h,0feh,002h;
        defb    0cah,0b6h,008h,0e5h,0ddh,06eh,0fah,0ddh,066h,0fbh,0ddh,05eh,00bh,0ddh,056h,00ch;
        defb    07ch,0b5h,028h,00dh,0edh,052h,038h,026h,028h,007h,0ddh,07eh,000h,0feh,003h,020h;
        defb    01dh,0e1h,07ch,0b5h,020h,006h,0ddh,06eh,00dh,0ddh,066h,00eh,0e5h,0ddh,0e1h,03ah;
        defb    074h,05ch,0feh,002h,037h,020h,001h,0a7h,03eh,0ffh,0cdh,056h,005h,0d8h,0cfh,01ah;
        defb    0ddh,05eh,00bh,0ddh,056h,00ch,0e5h,07ch,0b5h,020h,006h,013h,013h,013h,0ebh,018h;
        defb    00ch,0ddh,06eh,0fah,0ddh,066h,0fbh,0ebh,037h,0edh,052h,038h,009h,011h,005h,000h;
        defb    019h,044h,04dh,0cdh,005h,01fh,0e1h,0ddh,07eh,000h,0a7h,028h,03eh,07ch,0b5h,028h;
        defb    013h,02bh,046h,02bh,04eh,02bh,003h,003h,003h,0ddh,022h,05fh,05ch,0cdh,0e8h,019h;
        defb    0ddh,02ah,05fh,05ch,02ah,059h,05ch,02bh,0ddh,04eh,00bh,0ddh,046h,00ch,0c5h,003h;
        defb    003h,003h,0ddh,07eh,0fdh,0f5h,0cdh,055h,016h,023h,0f1h,077h,0d1h,023h,073h,023h;
        defb    072h,023h,0e5h,0ddh,0e1h,037h,03eh,0ffh,0c3h,002h,008h,0ebh,02ah,059h,05ch,02bh;
        defb    0ddh,022h,05fh,05ch,0ddh,04eh,00bh,0ddh,046h,00ch,0c5h,0cdh,0e5h,019h,0c1h,0e5h;
        defb    0c5h,0cdh,055h,016h,0ddh,02ah,05fh,05ch,023h,0ddh,04eh,00fh,0ddh,046h,010h,009h;
        defb    022h,04bh,05ch,0ddh,066h,00eh,07ch,0e6h,0c0h,020h,00ah,0ddh,06eh,00dh,022h,042h;
        defb    05ch,0fdh,036h,00ah,000h,0d1h,0ddh,0e1h,037h,03eh,0ffh,0c3h,002h,008h,0ddh,04eh;
        defb    00bh,0ddh,046h,00ch,0c5h,003h,0f7h,036h,080h,0ebh,0d1h,0e5h,0e5h,0ddh,0e1h,037h;
        defb    03eh,0ffh,0cdh,002h,008h,0e1h,0edh,05bh,053h,05ch,07eh,0e6h,0c0h,020h,019h,01ah;
        defb    013h,0beh,023h,020h,002h,01ah,0beh,01bh,02bh,030h,008h,0e5h,0ebh,0cdh,0b8h,019h;
        defb    0e1h,018h,0ech,0cdh,02ch,009h,018h,0e2h,07eh,04fh,0feh,080h,0c8h,0e5h,02ah,04bh;
        defb    05ch,07eh,0feh,080h,028h,025h,0b9h,028h,008h,0c5h,0cdh,0b8h,019h,0c1h,0ebh,018h;
        defb    0f0h,0e6h,0e0h,0feh,0a0h,020h,012h,0d1h,0d5h,0e5h,023h,013h,01ah,0beh,020h,006h;
        defb    017h,030h,0f7h,0e1h,018h,003h,0e1h,018h,0e0h,03eh,0ffh,0d1h,0ebh,03ch,037h,0cdh;
        defb    02ch,009h,018h,0c4h,020h,010h,008h,022h,05fh,05ch,0ebh,0cdh,0b8h,019h,0cdh,0e8h;
        defb    019h,0ebh,02ah,05fh,05ch,008h,008h,0d5h,0cdh,0b8h,019h,022h,05fh,05ch,02ah,053h;
        defb    05ch,0e3h,0c5h,008h,038h,007h,02bh,0cdh,055h,016h,023h,018h,003h,0cdh,055h,016h;
        defb    023h,0c1h,0d1h,0edh,053h,053h,05ch,0edh,05bh,05fh,05ch,0c5h,0d5h,0ebh,0edh,0b0h;
        defb    0e1h,0c1h,0d5h,0cdh,0e8h,019h,0d1h,0c9h,0e5h,03eh,0fdh,0cdh,001h,016h,0afh,011h;
        defb    0a1h,009h,0cdh,00ah,00ch,0fdh,0cbh,002h,0eeh,0cdh,0d4h,015h,0ddh,0e5h,011h,011h;
        defb    000h,0afh,0cdh,0c2h,004h,0ddh,0e1h,006h,032h,076h,010h,0fdh,0ddh,05eh,00bh,0ddh;
        defb    056h,00ch,03eh,0ffh,0ddh,0e1h,0c3h,0c2h,004h,080h,053h,074h,061h,072h,074h,020h;
        defb    074h,061h,070h,065h,02ch,020h,074h,068h,065h,06eh,020h,070h,072h,065h,073h,073h;
        defb    020h,061h,06eh,079h,020h,06bh,065h,079h,0aeh,00dh,050h,072h,06fh,067h,072h,061h;
        defb    06dh,03ah,0a0h,00dh,04eh,075h,06dh,062h,065h,072h,020h,061h,072h,072h,061h,079h;
        defb    03ah,0a0h,00dh,043h,068h,061h,072h,061h,063h,074h,065h,072h,020h,061h,072h,072h;
        defb    061h,079h,03ah,0a0h,00dh,042h,079h,074h,065h,073h,03ah,0a0h,0cdh,003h,00bh,0feh;
        defb    020h,0d2h,0d9h,00ah,0feh,006h,038h,069h,0feh,018h,030h,065h,021h,00bh,00ah,05fh;
        defb    016h,000h,019h,05eh,019h,0e5h,0c3h,003h,00bh,04eh,057h,010h,029h,054h,053h,052h;
        defb    037h,050h,04fh,05fh,05eh,05dh,05ch,05bh,05ah,054h,053h,00ch,03eh,022h,0b9h,020h;
        defb    011h,0fdh,0cbh,001h,04eh,020h,009h,004h,00eh,002h,03eh,018h,0b8h,020h,003h,005h;
        defb    00eh,021h,0c3h,0d9h,00dh,03ah,091h,05ch,0f5h,0fdh,036h,057h,001h,03eh,020h,0cdh;
        defb    065h,00bh,0f1h,032h,091h,05ch,0c9h,0fdh,0cbh,001h,04eh,0c2h,0cdh,00eh,00eh,021h;
        defb    0cdh,055h,00ch,005h,0c3h,0d9h,00dh,0cdh,003h,00bh,079h,03dh,03dh,0e6h,010h,018h;
        defb    05ah,03eh,03fh,018h,06ch,011h,087h,00ah,032h,00fh,05ch,018h,00bh,011h,06dh,00ah;
        defb    018h,003h,011h,087h,00ah,032h,00eh,05ch,02ah,051h,05ch,073h,023h,072h,0c9h,011h;
        defb    0f4h,009h,0cdh,080h,00ah,02ah,00eh,05ch,057h,07dh,0feh,016h,0dah,011h,022h,020h;
        defb    029h,044h,04ah,03eh,01fh,091h,038h,00ch,0c6h,002h,04fh,0fdh,0cbh,001h,04eh,020h;
        defb    016h,03eh,016h,090h,0dah,09fh,01eh,03ch,047h,004h,0fdh,0cbh,002h,046h,0c2h,055h;
        defb    00ch,0fdh,0beh,031h,0dah,086h,00ch,0c3h,0d9h,00dh,07ch,0cdh,003h,00bh,081h,03dh;
        defb    0e6h,01fh,0c8h,057h,0fdh,0cbh,001h,0c6h,03eh,020h,0cdh,03bh,00ch,015h,020h,0f8h;
        defb    0c9h,0cdh,024h,00bh,0fdh,0cbh,001h,04eh,020h,01ah,0fdh,0cbh,002h,046h,020h,008h;
        defb    0edh,043h,088h,05ch,022h,084h,05ch,0c9h,0edh,043h,08ah,05ch,0edh,043h,082h,05ch;
        defb    022h,086h,05ch,0c9h,0fdh,071h,045h,022h,080h,05ch,0c9h,0fdh,0cbh,001h,04eh,020h;
        defb    014h,0edh,04bh,088h,05ch,02ah,084h,05ch,0fdh,0cbh,002h,046h,0c8h,0edh,04bh,08ah;
        defb    05ch,02ah,086h,05ch,0c9h,0fdh,04eh,045h,02ah,080h,05ch,0c9h,0feh,080h,038h,03dh;
        defb    0feh,090h,030h,026h,047h,0cdh,038h,00bh,0cdh,003h,00bh,011h,092h,05ch,018h,047h;
        defb    021h,092h,05ch,0cdh,03eh,00bh,0cbh,018h,09fh,0e6h,00fh,04fh,0cbh,018h,09fh,0e6h;
        defb    0f0h,0b1h,00eh,004h,077h,023h,00dh,020h,0fbh,0c9h,0d6h,0a5h,030h,009h,0c6h,015h;
        defb    0c5h,0edh,04bh,07bh,05ch,018h,00bh,0cdh,010h,00ch,0c3h,003h,00bh,0c5h,0edh,04bh;
        defb    036h,05ch,0ebh,021h,03bh,05ch,0cbh,086h,0feh,020h,020h,002h,0cbh,0c6h,026h,000h;
        defb    06fh,029h,029h,029h,009h,0c1h,0ebh,079h,03dh,03eh,021h,020h,00eh,005h,04fh,0fdh;
        defb    0cbh,001h,04eh,028h,006h,0d5h,0cdh,0cdh,00eh,0d1h,079h,0b9h,0d5h,0cch,055h,00ch;
        defb    0d1h,0c5h,0e5h,03ah,091h,05ch,006h,0ffh,01fh,038h,001h,004h,01fh,01fh,09fh,04fh;
        defb    03eh,008h,0a7h,0fdh,0cbh,001h,04eh,028h,005h,0fdh,0cbh,030h,0ceh,037h,0ebh,008h;
        defb    01ah,0a0h,0aeh,0a9h,012h,008h,038h,013h,014h,023h,03dh,020h,0f2h,0ebh,025h,0fdh;
        defb    0cbh,001h,04eh,0cch,0dbh,00bh,0e1h,0c1h,00dh,023h,0c9h,008h,03eh,020h,083h,05fh;
        defb    008h,018h,0e6h,07ch,00fh,00fh,00fh,0e6h,003h,0f6h,058h,067h,0edh,05bh,08fh,05ch;
        defb    07eh,0abh,0a2h,0abh,0fdh,0cbh,057h,076h,028h,008h,0e6h,0c7h,0cbh,057h,020h,002h;
        defb    0eeh,038h,0fdh,0cbh,057h,066h,028h,008h,0e6h,0f8h,0cbh,06fh,020h,002h,0eeh,007h;
        defb    077h,0c9h,0e5h,026h,000h,0e3h,018h,004h,011h,095h,000h,0f5h,0cdh,041h,00ch,038h;
        defb    009h,03eh,020h,0fdh,0cbh,001h,046h,0cch,03bh,00ch,01ah,0e6h,07fh,0cdh,03bh,00ch;
        defb    01ah,013h,087h,030h,0f5h,0d1h,0feh,048h,028h,003h,0feh,082h,0d8h,07ah,0feh,003h;
        defb    0d8h,03eh,020h,0d5h,0d9h,0d7h,0d9h,0d1h,0c9h,0f5h,0ebh,03ch,0cbh,07eh,023h,028h;
        defb    0fbh,03dh,020h,0f8h,0ebh,0f1h,0feh,020h,0d8h,01ah,0d6h,041h,0c9h,0fdh,0cbh,001h;
        defb    04eh,0c0h,011h,0d9h,00dh,0d5h,078h,0fdh,0cbh,002h,046h,0c2h,002h,00dh,0fdh,0beh;
        defb    031h,038h,01bh,0c0h,0fdh,0cbh,002h,066h,028h,016h,0fdh,05eh,02dh,01dh,028h,05ah;
        defb    03eh,000h,0cdh,001h,016h,0edh,07bh,03fh,05ch,0fdh,0cbh,002h,0a6h,0c9h,0cfh,004h;
        defb    0fdh,035h,052h,020h,045h,03eh,018h,090h,032h,08ch,05ch,02ah,08fh,05ch,0e5h,03ah;
        defb    091h,05ch,0f5h,03eh,0fdh,0cdh,001h,016h,0afh,011h,0f8h,00ch,0cdh,00ah,00ch,0fdh;
        defb    0cbh,002h,0eeh,021h,03bh,05ch,0cbh,0deh,0cbh,0aeh,0d9h,0cdh,0d4h,015h,0d9h,0feh;
        defb    020h,028h,045h,0feh,0e2h,028h,041h,0f6h,020h,0feh,06eh,028h,03bh,03eh,0feh,0cdh;
        defb    001h,016h,0f1h,032h,091h,05ch,0e1h,022h,08fh,05ch,0cdh,0feh,00dh,0fdh,046h,031h;
        defb    004h,00eh,021h,0c5h,0cdh,09bh,00eh,07ch,00fh,00fh,00fh,0e6h,003h,0f6h,058h,067h;
        defb    011h,0e0h,05ah,01ah,04eh,006h,020h,0ebh,012h,071h,013h,023h,010h,0fah,0c1h,0c9h;
        defb    080h,073h,063h,072h,06fh,06ch,06ch,0bfh,0cfh,00ch,0feh,002h,038h,080h,0fdh,086h;
        defb    031h,0d6h,019h,0d0h,0edh,044h,0c5h,047h,02ah,08fh,05ch,0e5h,02ah,091h,05ch,0e5h;
        defb    0cdh,04dh,00dh,078h,0f5h,021h,06bh,05ch,046h,078h,03ch,077h,021h,089h,05ch,0beh;
        defb    038h,003h,034h,006h,018h,0cdh,000h,00eh,0f1h,03dh,020h,0e8h,0e1h,0fdh,075h,057h;
        defb    0e1h,022h,08fh,05ch,0edh,04bh,088h,05ch,0fdh,0cbh,002h,086h,0cdh,0d9h,00dh,0fdh;
        defb    0cbh,002h,0c6h,0c1h,0c9h,0afh,02ah,08dh,05ch,0fdh,0cbh,002h,046h,028h,004h,067h;
        defb    0fdh,06eh,00eh,022h,08fh,05ch,021h,091h,05ch,020h,002h,07eh,00fh,0aeh,0e6h,055h;
        defb    0aeh,077h,0c9h,0cdh,0afh,00dh,021h,03ch,05ch,0cbh,0aeh,0cbh,0c6h,0cdh,04dh,00dh;
        defb    0fdh,046h,031h,0cdh,044h,00eh,021h,0c0h,05ah,03ah,08dh,05ch,005h,018h,007h,00eh;
        defb    020h,02bh,077h,00dh,020h,0fbh,010h,0f7h,0fdh,036h,031h,002h,03eh,0fdh,0cdh,001h;
        defb    016h,02ah,051h,05ch,011h,0f4h,009h,0a7h,073h,023h,072h,023h,011h,0a8h,010h,03fh;
        defb    038h,0f6h,001h,021h,017h,018h,02ah,021h,000h,000h,022h,07dh,05ch,0fdh,0cbh,030h;
        defb    086h,0cdh,094h,00dh,03eh,0feh,0cdh,001h,016h,0cdh,04dh,00dh,006h,018h,0cdh,044h;
        defb    00eh,02ah,051h,05ch,011h,0f4h,009h,073h,023h,072h,0fdh,036h,052h,001h,001h,021h;
        defb    018h,021h,000h,05bh,0fdh,0cbh,001h,04eh,020h,012h,078h,0fdh,0cbh,002h,046h,028h;
        defb    005h,0fdh,086h,031h,0d6h,018h,0c5h,047h,0cdh,09bh,00eh,0c1h,03eh,021h,091h,05fh;
        defb    016h,000h,019h,0c3h,0dch,00ah,006h,017h,0cdh,09bh,00eh,00eh,008h,0c5h,0e5h,078h;
        defb    0e6h,007h,078h,020h,00ch,0ebh,021h,0e0h,0f8h,019h,0ebh,001h,020h,000h,03dh,0edh;
        defb    0b0h,0ebh,021h,0e0h,0ffh,019h,0ebh,047h,0e6h,007h,00fh,00fh,00fh,04fh,078h,006h;
        defb    000h,0edh,0b0h,006h,007h,009h,0e6h,0f8h,020h,0dbh,0e1h,024h,0c1h,00dh,020h,0cdh;
        defb    0cdh,088h,00eh,021h,0e0h,0ffh,019h,0ebh,0edh,0b0h,006h,001h,0c5h,0cdh,09bh,00eh;
        defb    00eh,008h,0c5h,0e5h,078h,0e6h,007h,00fh,00fh,00fh,04fh,078h,006h,000h,00dh,054h;
        defb    05dh,036h,000h,013h,0edh,0b0h,011h,001h,007h,019h,03dh,0e6h,0f8h,047h,020h,0e5h;
        defb    0e1h,024h,0c1h,00dh,020h,0dch,0cdh,088h,00eh,062h,06bh,013h,03ah,08dh,05ch,0fdh;
        defb    0cbh,002h,046h,028h,003h,03ah,048h,05ch,077h,00bh,0edh,0b0h,0c1h,00eh,021h,0c9h;
        defb    07ch,00fh,00fh,00fh,03dh,0f6h,050h,067h,0ebh,061h,068h,029h,029h,029h,029h,029h;
        defb    044h,04dh,0c9h,03eh,018h,090h,057h,00fh,00fh,00fh,0e6h,0e0h,06fh,07ah,0e6h,018h;
        defb    0f6h,040h,067h,0c9h,0f3h,006h,0b0h,021h,000h,040h,0e5h,0c5h,0cdh,0f4h,00eh,0c1h;
        defb    0e1h,024h,07ch,0e6h,007h,020h,00ah,07dh,0c6h,020h,06fh,03fh,09fh,0e6h,0f8h,084h;
        defb    067h,010h,0e7h,018h,00dh,0f3h,021h,000h,05bh,006h,008h,0c5h,0cdh,0f4h,00eh,0c1h;
        defb    010h,0f9h,03eh,004h,0d3h,0fbh,0fbh,021h,000h,05bh,0fdh,075h,046h,0afh,047h,077h;
        defb    023h,010h,0fch,0fdh,0cbh,030h,08eh,00eh,021h,0c3h,0d9h,00dh,078h,0feh,003h,09fh;
        defb    0e6h,002h,0d3h,0fbh,057h,0cdh,054h,01fh,038h,00ah,03eh,004h,0d3h,0fbh,0fbh,0cdh;
        defb    0dfh,00eh,0cfh,00ch,0dbh,0fbh,087h,0f8h,030h,0ebh,00eh,020h,05eh,023h,006h,008h;
        defb    0cbh,012h,0cbh,013h,0cbh,01ah,0dbh,0fbh,01fh,030h,0fbh,07ah,0d3h,0fbh,010h,0f0h;
        defb    00dh,020h,0e9h,0c9h,02ah,03dh,05ch,0e5h,021h,07fh,010h,0e5h,0edh,073h,03dh,05ch;
        defb    0cdh,0d4h,015h,0f5h,016h,000h,0fdh,05eh,0ffh,021h,0c8h,000h,0cdh,0b5h,003h,0f1h;
        defb    021h,038h,00fh,0e5h,0feh,018h,030h,031h,0feh,007h,038h,02dh,0feh,010h,038h,03ah;
        defb    001h,002h,000h,057h,0feh,016h,038h,00ch,003h,0fdh,0cbh,037h,07eh,0cah,01eh,010h;
        defb    0cdh,0d4h,015h,05fh,0cdh,0d4h,015h,0d5h,02ah,05bh,05ch,0fdh,0cbh,007h,086h,0cdh;
        defb    055h,016h,0c1h,023h,070h,023h,071h,018h,00ah,0fdh,0cbh,007h,086h,02ah,05bh,05ch;
        defb    0cdh,052h,016h,012h,013h,0edh,053h,05bh,05ch,0c9h,05fh,016h,000h,021h,099h,00fh;
        defb    019h,05eh,019h,0e5h,02ah,05bh,05ch,0c9h,009h,066h,06ah,050h,0b5h,070h,07eh,0cfh;
        defb    0d4h,02ah,049h,05ch,0fdh,0cbh,037h,06eh,0c2h,097h,010h,0cdh,06eh,019h,0cdh,095h;
        defb    016h,07ah,0b3h,0cah,097h,010h,0e5h,023h,04eh,023h,046h,021h,00ah,000h,009h,044h;
        defb    04dh,0cdh,005h,01fh,0cdh,097h,010h,02ah,051h,05ch,0e3h,0e5h,03eh,0ffh,0cdh,001h;
        defb    016h,0e1h,02bh,0fdh,035h,00fh,0cdh,055h,018h,0fdh,034h,00fh,02ah,059h,05ch,023h;
        defb    023h,023h,023h,022h,05bh,05ch,0e1h,0cdh,015h,016h,0c9h,0fdh,0cbh,037h,06eh,020h;
        defb    008h,021h,049h,05ch,0cdh,00fh,019h,018h,06dh,0fdh,036h,000h,010h,018h,01dh,0cdh;
        defb    031h,010h,018h,005h,07eh,0feh,00dh,0c8h,023h,022h,05bh,05ch,0c9h,0cdh,031h,010h;
        defb    001h,001h,000h,0c3h,0e8h,019h,0cdh,0d4h,015h,0cdh,0d4h,015h,0e1h,0e1h,0e1h,022h;
        defb    03dh,05ch,0fdh,0cbh,000h,07eh,0c0h,0f9h,0c9h,037h,0cdh,095h,011h,0edh,052h,019h;
        defb    023h,0c1h,0d8h,0c5h,044h,04dh,062h,06bh,023h,01ah,0e6h,0f0h,0feh,010h,020h,009h;
        defb    023h,01ah,0d6h,017h,0ceh,000h,020h,001h,023h,0a7h,0edh,042h,009h,0ebh,038h,0e6h;
        defb    0c9h,0fdh,0cbh,037h,06eh,0c0h,02ah,049h,05ch,0cdh,06eh,019h,0ebh,0cdh,095h,016h;
        defb    021h,04ah,05ch,0cdh,01ch,019h,0cdh,095h,017h,03eh,000h,0c3h,001h,016h,0fdh,0cbh;
        defb    037h,07eh,028h,0a8h,0c3h,081h,00fh,0fdh,0cbh,030h,066h,028h,0a1h,0fdh,036h,000h;
        defb    0ffh,016h,000h,0fdh,05eh,0feh,021h,090h,01ah,0cdh,0b5h,003h,0c3h,030h,00fh,0e5h;
        defb    0cdh,090h,011h,02bh,0cdh,0e5h,019h,022h,05bh,05ch,0fdh,036h,007h,000h,0e1h,0c9h;
        defb    0fdh,0cbh,002h,05eh,0c4h,01dh,011h,0a7h,0fdh,0cbh,001h,06eh,0c8h,03ah,008h,05ch;
        defb    0fdh,0cbh,001h,0aeh,0f5h,0fdh,0cbh,002h,06eh,0c4h,06eh,00dh,0f1h,0feh,020h,030h;
        defb    052h,0feh,010h,030h,02dh,0feh,006h,030h,00ah,047h,0e6h,001h,04fh,078h,01fh,0c6h;
        defb    012h,018h,02ah,020h,009h,021h,06ah,05ch,03eh,008h,0aeh,077h,018h,00eh,0feh,00eh;
        defb    0d8h,0d6h,00dh,021h,041h,05ch,0beh,077h,020h,002h,036h,000h,0fdh,0cbh,002h,0deh;
        defb    0bfh,0c9h,047h,0e6h,007h,04fh,03eh,010h,0cbh,058h,020h,001h,03ch,0fdh,071h,0d3h;
        defb    011h,00dh,011h,018h,006h,03ah,00dh,05ch,011h,0a8h,010h,02ah,04fh,05ch,023h,023h;
        defb    073h,023h,072h,037h,0c9h,0cdh,04dh,00dh,0fdh,0cbh,002h,09eh,0fdh,0cbh,002h,0aeh;
        defb    02ah,08ah,05ch,0e5h,02ah,03dh,05ch,0e5h,021h,067h,011h,0e5h,0edh,073h,03dh,05ch;
        defb    02ah,082h,05ch,0e5h,037h,0cdh,095h,011h,0ebh,0cdh,07dh,018h,0ebh,0cdh,0e1h,018h;
        defb    02ah,08ah,05ch,0e3h,0ebh,0cdh,04dh,00dh,03ah,08bh,05ch,092h,038h,026h,020h,006h;
        defb    07bh,0fdh,096h,050h,030h,01eh,03eh,020h,0d5h,0cdh,0f4h,009h,0d1h,018h,0e9h,016h;
        defb    000h,0fdh,05eh,0feh,021h,090h,01ah,0cdh,0b5h,003h,0fdh,036h,000h,0ffh,0edh,05bh;
        defb    08ah,05ch,018h,002h,0d1h,0e1h,0e1h,022h,03dh,05ch,0c1h,0d5h,0cdh,0d9h,00dh,0e1h;
        defb    022h,082h,05ch,0fdh,036h,026h,000h,0c9h,02ah,061h,05ch,02bh,0a7h,0edh,05bh,059h;
        defb    05ch,0fdh,0cbh,037h,06eh,0c8h,0edh,05bh,061h,05ch,0d8h,02ah,063h,05ch,0c9h,07eh;
        defb    0feh,00eh,001h,006h,000h,0cch,0e8h,019h,07eh,023h,0feh,00dh,020h,0f1h,0c9h,0f3h;
        defb    03eh,0ffh,0edh,05bh,0b2h,05ch,0d9h,0edh,04bh,0b4h,05ch,0edh,05bh,038h,05ch,02ah;
        defb    07bh,05ch,0d9h,047h,03eh,007h,0d3h,0feh,03eh,03fh,0edh,047h,000h,000h,000h,000h;
        defb    000h,000h,062h,06bh,036h,002h,02bh,0bch,020h,0fah,0a7h,0edh,052h,019h,023h,030h;
        defb    006h,035h,028h,003h,035h,028h,0f3h,02bh,0d9h,0edh,043h,0b4h,05ch,0edh,053h,038h;
        defb    05ch,022h,07bh,05ch,0d9h,004h,028h,019h,022h,0b4h,05ch,011h,0afh,03eh,001h,0a8h;
        defb    000h,0ebh,0edh,0b8h,0ebh,023h,022h,07bh,05ch,02bh,001h,040h,000h,0edh,043h,038h;
        defb    05ch,022h,0b2h,05ch,021h,000h,03ch,022h,036h,05ch,02ah,0b2h,05ch,036h,03eh,02bh;
        defb    0f9h,02bh,02bh,022h,03dh,05ch,0edh,056h,0fdh,021h,03ah,05ch,0fbh,021h,0b6h,05ch;
        defb    022h,04fh,05ch,011h,0afh,015h,001h,015h,000h,0ebh,0edh,0b0h,0ebh,02bh,022h,057h;
        defb    05ch,023h,022h,053h,05ch,022h,04bh,05ch,036h,080h,023h,022h,059h,05ch,036h,00dh;
        defb    023h,036h,080h,023h,022h,061h,05ch,022h,063h,05ch,022h,065h,05ch,03eh,038h,032h;
        defb    08dh,05ch,032h,08fh,05ch,032h,048h,05ch,021h,023h,005h,022h,009h,05ch,0fdh,035h;
        defb    0c6h,0fdh,035h,0cah,021h,0c6h,015h,011h,010h,05ch,001h,00eh,000h,0edh,0b0h,0fdh;
        defb    0cbh,001h,0ceh,0cdh,0dfh,00eh,0fdh,036h,031h,002h,0cdh,06bh,00dh,0afh,011h,038h;
        defb    015h,0cdh,00ah,00ch,0fdh,0cbh,002h,0eeh,018h,007h,0fdh,036h,031h,002h,0cdh,095h;
        defb    017h,0cdh,0b0h,016h,03eh,000h,0cdh,001h,016h,0cdh,02ch,00fh,0cdh,017h,01bh,0fdh;
        defb    0cbh,000h,07eh,020h,012h,0fdh,0cbh,030h,066h,028h,040h,02ah,059h,05ch,0cdh,0a7h;
        defb    011h,0fdh,036h,000h,0ffh,018h,0ddh,02ah,059h,05ch,022h,05dh,05ch,0cdh,0fbh,019h;
        defb    078h,0b1h,0c2h,05dh,015h,0dfh,0feh,00dh,028h,0c0h,0fdh,0cbh,030h,046h,0c4h,0afh;
        defb    00dh,0cdh,06eh,00dh,03eh,019h,0fdh,096h,04fh,032h,08ch,05ch,0fdh,0cbh,001h,0feh;
        defb    0fdh,036h,000h,0ffh,0fdh,036h,00ah,001h,0cdh,08ah,01bh,076h,0fdh,0cbh,001h,0aeh;
        defb    0fdh,0cbh,030h,04eh,0c4h,0cdh,00eh,03ah,03ah,05ch,03ch,0f5h,021h,000h,000h,0fdh;
        defb    074h,037h,0fdh,074h,026h,022h,00bh,05ch,021h,001h,000h,022h,016h,05ch,0cdh,0b0h;
        defb    016h,0fdh,0cbh,037h,0aeh,0cdh,06eh,00dh,0fdh,0cbh,002h,0eeh,0f1h,047h,0feh,00ah;
        defb    038h,002h,0c6h,007h,0cdh,0efh,015h,03eh,020h,0d7h,078h,011h,091h,013h,0cdh,00ah;
        defb    00ch,0afh,011h,036h,015h,0cdh,00ah,00ch,0edh,04bh,045h,05ch,0cdh,01bh,01ah,03eh;
        defb    03ah,0d7h,0fdh,04eh,00dh,006h,000h,0cdh,01bh,01ah,0cdh,097h,010h,03ah,03ah,05ch;
        defb    03ch,028h,01bh,0feh,009h,028h,004h,0feh,015h,020h,003h,0fdh,034h,00dh,001h,003h;
        defb    000h,011h,070h,05ch,021h,044h,05ch,0cbh,07eh,028h,001h,009h,0edh,0b8h,0fdh,036h;
        defb    00ah,0ffh,0fdh,0cbh,001h,09eh,0c3h,0ach,012h,080h,04fh,0cbh,04eh,045h,058h,054h;
        defb    020h,077h,069h,074h,068h,06fh,075h,074h,020h,046h,04fh,0d2h,056h,061h,072h,069h;
        defb    061h,062h,06ch,065h,020h,06eh,06fh,074h,020h,066h,06fh,075h,06eh,0e4h,053h,075h;
        defb    062h,073h,063h,072h,069h,070h,074h,020h,077h,072h,06fh,06eh,0e7h,04fh,075h,074h;
        defb    020h,06fh,066h,020h,06dh,065h,06dh,06fh,072h,0f9h,04fh,075h,074h,020h,06fh,066h;
        defb    020h,073h,063h,072h,065h,065h,0eeh,04eh,075h,06dh,062h,065h,072h,020h,074h,06fh;
        defb    06fh,020h,062h,069h,0e7h,052h,045h,054h,055h,052h,04eh,020h,077h,069h,074h,068h;
        defb    06fh,075h,074h,020h,047h,04fh,053h,055h,0c2h,045h,06eh,064h,020h,06fh,066h,020h;
        defb    066h,069h,06ch,0e5h,053h,054h,04fh,050h,020h,073h,074h,061h,074h,065h,06dh,065h;
        defb    06eh,0f4h,049h,06eh,076h,061h,06ch,069h,064h,020h,061h,072h,067h,075h,06dh,065h;
        defb    06eh,0f4h,049h,06eh,074h,065h,067h,065h,072h,020h,06fh,075h,074h,020h,06fh,066h;
        defb    020h,072h,061h,06eh,067h,0e5h,04eh,06fh,06eh,073h,065h,06eh,073h,065h,020h,069h;
        defb    06eh,020h,042h,041h,053h,049h,0c3h,042h,052h,045h,041h,04bh,020h,02dh,020h,043h;
        defb    04fh,04eh,054h,020h,072h,065h,070h,065h,061h,074h,0f3h,04fh,075h,074h,020h,06fh;
        defb    066h,020h,044h,041h,054h,0c1h,049h,06eh,076h,061h,06ch,069h,064h,020h,066h,069h;
        defb    06ch,065h,020h,06eh,061h,06dh,0e5h,04eh,06fh,020h,072h,06fh,06fh,06dh,020h,066h;
        defb    06fh,072h,020h,06ch,069h,06eh,0e5h,053h,054h,04fh,050h,020h,069h,06eh,020h,049h;
        defb    04eh,050h,055h,0d4h,046h,04fh,052h,020h,077h,069h,074h,068h,06fh,075h,074h,020h;
        defb    04eh,045h,058h,0d4h,049h,06eh,076h,061h,06ch,069h,064h,020h,049h,02fh,04fh,020h;
        defb    064h,065h,076h,069h,063h,0e5h,049h,06eh,076h,061h,06ch,069h,064h,020h,063h,06fh;
        defb    06ch,06fh,075h,0f2h,042h,052h,045h,041h,04bh,020h,069h,06eh,074h,06fh,020h,070h;
        defb    072h,06fh,067h,072h,061h,0edh,052h,041h,04dh,054h,04fh,050h,020h,06eh,06fh,020h;
        defb    067h,06fh,06fh,0e4h,053h,074h,061h,074h,065h,06dh,065h,06eh,074h,020h,06ch,06fh;
        defb    073h,0f4h,049h,06eh,076h,061h,06ch,069h,064h,020h,073h,074h,072h,065h,061h,0edh;
        defb    046h,04eh,020h,077h,069h,074h,068h,06fh,075h,074h,020h,044h,045h,0c6h,050h,061h;
        defb    072h,061h,06dh,065h,074h,065h,072h,020h,065h,072h,072h,06fh,0f2h,054h,061h,070h;
        defb    065h,020h,06ch,06fh,061h,064h,069h,06eh,067h,020h,065h,072h,072h,06fh,0f2h,02ch;
        defb    0a0h,07fh,020h,031h,039h,038h,032h,020h,053h,069h,06eh,063h,06ch,061h,069h,072h;
        defb    020h,052h,065h,073h,065h,061h,072h,063h,068h,020h,04ch,074h,0e4h,03eh,010h,001h;
        defb    000h,000h,0c3h,013h,013h,0edh,043h,049h,05ch,02ah,05dh,05ch,0ebh,021h,055h,015h;
        defb    0e5h,02ah,061h,05ch,037h,0edh,052h,0e5h,060h,069h,0cdh,06eh,019h,020h,006h,0cdh;
        defb    0b8h,019h,0cdh,0e8h,019h,0c1h,079h,03dh,0b0h,028h,028h,0c5h,003h,003h,003h,003h;
        defb    02bh,0edh,05bh,053h,05ch,0d5h,0cdh,055h,016h,0e1h,022h,053h,05ch,0c1h,0c5h,013h;
        defb    02ah,061h,05ch,02bh,02bh,0edh,0b8h,02ah,049h,05ch,0ebh,0c1h,070h,02bh,071h,02bh;
        defb    073h,02bh,072h,0f1h,0c3h,0a2h,012h,0f4h,009h,0a8h,010h,04bh,0f4h,009h,0c4h,015h;
        defb    053h,081h,00fh,0c4h,015h,052h,0f4h,009h,0c4h,015h,050h,080h,0cfh,012h,001h,000h;
        defb    006h,000h,00bh,000h,001h,000h,001h,000h,006h,000h,010h,000h,0fdh,0cbh,002h,06eh;
        defb    020h,004h,0fdh,0cbh,002h,0deh,0cdh,0e6h,015h,0d8h,028h,0fah,0cfh,007h,0d9h,0e5h;
        defb    02ah,051h,05ch,023h,023h,018h,008h,01eh,030h,083h,0d9h,0e5h,02ah,051h,05ch,05eh;
        defb    023h,056h,0ebh,0cdh,02ch,016h,0e1h,0d9h,0c9h,087h,0c6h,016h,06fh,026h,05ch,05eh;
        defb    023h,056h,07ah,0b3h,020h,002h,0cfh,017h,01bh,02ah,04fh,05ch,019h,022h,051h,05ch;
        defb    0fdh,0cbh,030h,0a6h,023h,023h,023h,023h,04eh,021h,02dh,016h,0cdh,0dch,016h,0d0h;
        defb    016h,000h,05eh,019h,0e9h,04bh,006h,053h,012h,050h,01bh,000h,0fdh,0cbh,002h,0c6h;
        defb    0fdh,0cbh,001h,0aeh,0fdh,0cbh,030h,0e6h,018h,004h,0fdh,0cbh,002h,086h,0fdh,0cbh;
        defb    001h,08eh,0c3h,04dh,00dh,0fdh,0cbh,001h,0ceh,0c9h,001h,001h,000h,0e5h,0cdh,005h;
        defb    01fh,0e1h,0cdh,064h,016h,02ah,065h,05ch,0ebh,0edh,0b8h,0c9h,0f5h,0e5h,021h,04bh;
        defb    05ch,03eh,00eh,05eh,023h,056h,0e3h,0a7h,0edh,052h,019h,0e3h,030h,009h,0d5h,0ebh;
        defb    009h,0ebh,072h,02bh,073h,023h,0d1h,023h,03dh,020h,0e8h,0ebh,0d1h,0f1h,0a7h,0edh;
        defb    052h,044h,04dh,003h,019h,0ebh,0c9h,000h,000h,0ebh,011h,08fh,016h,07eh,0e6h,0c0h;
        defb    020h,0f7h,056h,023h,05eh,0c9h,02ah,063h,05ch,02bh,0cdh,055h,016h,023h,023h,0c1h;
        defb    0edh,043h,061h,05ch,0c1h,0ebh,023h,0c9h,02ah,059h,05ch,036h,00dh,022h,05bh,05ch;
        defb    023h,036h,080h,023h,022h,061h,05ch,02ah,061h,05ch,022h,063h,05ch,02ah,063h,05ch;
        defb    022h,065h,05ch,0e5h,021h,092h,05ch,022h,068h,05ch,0e1h,0c9h,0edh,05bh,059h,05ch;
        defb    0c3h,0e5h,019h,023h,07eh,0a7h,0c8h,0b9h,023h,020h,0f8h,037h,0c9h,0cdh,01eh,017h;
        defb    0cdh,001h,017h,001h,000h,000h,011h,0e2h,0a3h,0ebh,019h,038h,007h,001h,0d4h,015h;
        defb    009h,04eh,023h,046h,0ebh,071h,023h,070h,0c9h,0e5h,02ah,04fh,05ch,009h,023h,023h;
        defb    023h,04eh,0ebh,021h,016h,017h,0cdh,0dch,016h,04eh,006h,000h,009h,0e9h,04bh,005h;
        defb    053h,003h,050h,001h,0e1h,0c9h,0cdh,094h,01eh,0feh,010h,038h,002h,0cfh,017h,0c6h;
        defb    003h,007h,021h,010h,05ch,04fh,006h,000h,009h,04eh,023h,046h,02bh,0c9h,0efh,001h;
        defb    038h,0cdh,01eh,017h,078h,0b1h,028h,016h,0ebh,02ah,04fh,05ch,009h,023h,023h,023h;
        defb    07eh,0ebh,0feh,04bh,028h,008h,0feh,053h,028h,004h,0feh,050h,020h,0cfh,0cdh,05dh;
        defb    017h,073h,023h,072h,0c9h,0e5h,0cdh,0f1h,02bh,078h,0b1h,020h,002h,0cfh,00eh,0c5h;
        defb    01ah,0e6h,0dfh,04fh,021h,07ah,017h,0cdh,0dch,016h,030h,0f1h,04eh,006h,000h,009h;
        defb    0c1h,0e9h,04bh,006h,053h,008h,050h,00ah,000h,01eh,001h,018h,006h,01eh,006h,018h;
        defb    002h,01eh,010h,00bh,078h,0b1h,020h,0d5h,057h,0e1h,0c9h,018h,090h,0edh,073h,03fh;
        defb    05ch,0fdh,036h,002h,010h,0cdh,0afh,00dh,0fdh,0cbh,002h,0c6h,0fdh,046h,031h,0cdh;
        defb    044h,00eh,0fdh,0cbh,002h,086h,0fdh,0cbh,030h,0c6h,02ah,049h,05ch,0edh,05bh,06ch;
        defb    05ch,0a7h,0edh,052h,019h,038h,022h,0d5h,0cdh,06eh,019h,011h,0c0h,002h,0ebh,0edh;
        defb    052h,0e3h,0cdh,06eh,019h,0c1h,0c5h,0cdh,0b8h,019h,0c1h,009h,038h,00eh,0ebh,056h;
        defb    023h,05eh,02bh,0edh,053h,06ch,05ch,018h,0edh,022h,06ch,05ch,02ah,06ch,05ch,0cdh;
        defb    06eh,019h,028h,001h,0ebh,0cdh,033h,018h,0fdh,0cbh,002h,0a6h,0c9h,03eh,003h,018h;
        defb    002h,03eh,002h,0fdh,036h,002h,000h,0cdh,030h,025h,0c4h,001h,016h,0dfh,0cdh,070h;
        defb    020h,038h,014h,0dfh,0feh,03bh,028h,004h,0feh,02ch,020h,006h,0e7h,0cdh,082h,01ch;
        defb    018h,008h,0cdh,0e6h,01ch,018h,003h,0cdh,0deh,01ch,0cdh,0eeh,01bh,0cdh,099h,01eh;
        defb    078h,0e6h,03fh,067h,069h,022h,049h,05ch,0cdh,06eh,019h,01eh,001h,0cdh,055h,018h;
        defb    0d7h,0fdh,0cbh,002h,066h,028h,0f6h,03ah,06bh,05ch,0fdh,096h,04fh,020h,0eeh,0abh;
        defb    0c8h,0e5h,0d5h,021h,06ch,05ch,0cdh,00fh,019h,0d1h,0e1h,018h,0e0h,0edh,04bh,049h;
        defb    05ch,0cdh,080h,019h,016h,03eh,028h,005h,011h,000h,000h,0cbh,013h,0fdh,073h,02dh;
        defb    07eh,0feh,040h,0c1h,0d0h,0c5h,0cdh,028h,01ah,023h,023h,023h,0fdh,0cbh,001h,086h;
        defb    07ah,0a7h,028h,005h,0d7h,0fdh,0cbh,001h,0c6h,0d5h,0ebh,0fdh,0cbh,030h,096h,021h;
        defb    03bh,05ch,0cbh,096h,0fdh,0cbh,037h,06eh,028h,002h,0cbh,0d6h,02ah,05fh,05ch,0a7h;
        defb    0edh,052h,020h,005h,03eh,03fh,0cdh,0c1h,018h,0cdh,0e1h,018h,0ebh,07eh,0cdh,0b6h;
        defb    018h,023h,0feh,00dh,028h,006h,0ebh,0cdh,037h,019h,018h,0e0h,0d1h,0c9h,0feh,00eh;
        defb    0c0h,023h,023h,023h,023h,023h,023h,07eh,0c9h,0d9h,02ah,08fh,05ch,0e5h,0cbh,0bch;
        defb    0cbh,0fdh,022h,08fh,05ch,021h,091h,05ch,056h,0d5h,036h,000h,0cdh,0f4h,009h,0e1h;
        defb    0fdh,074h,057h,0e1h,022h,08fh,05ch,0d9h,0c9h,02ah,05bh,05ch,0a7h,0edh,052h,0c0h;
        defb    03ah,041h,05ch,0cbh,007h,028h,004h,0c6h,043h,018h,016h,021h,03bh,05ch,0cbh,09eh;
        defb    03eh,04bh,0cbh,056h,028h,00bh,0cbh,0deh,03ch,0fdh,0cbh,030h,05eh,028h,002h,03eh;
        defb    043h,0d5h,0cdh,0c1h,018h,0d1h,0c9h,05eh,023h,056h,0e5h,0ebh,023h,0cdh,06eh,019h;
        defb    0cdh,095h,016h,0e1h,0fdh,0cbh,037h,06eh,0c0h,072h,02bh,073h,0c9h,07bh,0a7h,0f8h;
        defb    018h,00dh,0afh,009h,03ch,038h,0fch,0edh,042h,03dh,028h,0f1h,0c3h,0efh,015h,0cdh;
        defb    01bh,02dh,030h,030h,0feh,021h,038h,02ch,0fdh,0cbh,001h,096h,0feh,0cbh,028h,024h;
        defb    0feh,03ah,020h,00eh,0fdh,0cbh,037h,06eh,020h,016h,0fdh,0cbh,030h,056h,028h,014h;
        defb    018h,00eh,0feh,022h,020h,00ah,0f5h,03ah,06ah,05ch,0eeh,004h,032h,06ah,05ch,0f1h;
        defb    0fdh,0cbh,001h,0d6h,0d7h,0c9h,0e5h,02ah,053h,05ch,054h,05dh,0c1h,0cdh,080h,019h;
        defb    0d0h,0c5h,0cdh,0b8h,019h,0ebh,018h,0f4h,07eh,0b8h,0c0h,023h,07eh,02bh,0b9h,0c9h;
        defb    023h,023h,023h,022h,05dh,05ch,00eh,000h,015h,0c8h,0e7h,0bbh,020h,004h,0a7h,0c9h;
        defb    023h,07eh,0cdh,0b6h,018h,022h,05dh,05ch,0feh,022h,020h,001h,00dh,0feh,03ah,028h;
        defb    004h,0feh,0cbh,020h,004h,0cbh,041h,028h,0dfh,0feh,00dh,020h,0e3h,015h,037h,0c9h;
        defb    0e5h,07eh,0feh,040h,038h,017h,0cbh,06fh,028h,014h,087h,0fah,0c7h,019h,03fh,001h;
        defb    005h,000h,030h,002h,00eh,012h,017h,023h,07eh,030h,0fbh,018h,006h,023h,023h,04eh;
        defb    023h,046h,023h,009h,0d1h,0a7h,0edh,052h,044h,04dh,019h,0ebh,0c9h,0cdh,0ddh,019h;
        defb    0c5h,078h,02fh,047h,079h,02fh,04fh,003h,0cdh,064h,016h,0ebh,0e1h,019h,0d5h,0edh;
        defb    0b0h,0e1h,0c9h,02ah,059h,05ch,02bh,022h,05dh,05ch,0e7h,021h,092h,05ch,022h,065h;
        defb    05ch,0cdh,03bh,02dh,0cdh,0a2h,02dh,038h,004h,021h,0f0h,0d8h,009h,0dah,08ah,01ch;
        defb    0c3h,0c5h,016h,0d5h,0e5h,0afh,0cbh,078h,020h,020h,060h,069h,01eh,0ffh,018h,008h;
        defb    0d5h,056h,023h,05eh,0e5h,0ebh,01eh,020h,001h,018h,0fch,0cdh,02ah,019h,001h,09ch;
        defb    0ffh,0cdh,02ah,019h,00eh,0f6h,0cdh,02ah,019h,07dh,0cdh,0efh,015h,0e1h,0d1h,0c9h;
        defb    0b1h,0cbh,0bch,0bfh,0c4h,0afh,0b4h,093h,091h,092h,095h,098h,098h,098h,098h,098h;
        defb    098h,098h,07fh,081h,02eh,06ch,06eh,070h,048h,094h,056h,03fh,041h,02bh,017h,01fh;
        defb    037h,077h,044h,00fh,059h,02bh,043h,02dh,051h,03ah,06dh,042h,00dh,049h,05ch,044h;
        defb    015h,05dh,001h,03dh,002h,006h,000h,067h,01eh,006h,0cbh,005h,0f0h,01ch,006h,000h;
        defb    0edh,01eh,000h,0eeh,01ch,000h,023h,01fh,004h,03dh,006h,0cch,006h,005h,003h,01dh;
        defb    004h,000h,0abh,01dh,005h,0cdh,01fh,005h,089h,020h,005h,002h,02ch,005h,0b2h,01bh;
        defb    000h,0b7h,011h,003h,0a1h,01eh,005h,0f9h,017h,008h,000h,080h,01eh,003h,04fh,01eh;
        defb    000h,05fh,01eh,003h,0ach,01eh,000h,06bh,00dh,009h,000h,0dch,022h,006h,000h,03ah;
        defb    01fh,005h,0edh,01dh,005h,027h,01eh,003h,042h,01eh,009h,005h,082h,023h,000h,0ach;
        defb    00eh,005h,0c9h,01fh,005h,0f5h,017h,00bh,00bh,00bh,00bh,008h,000h,0f8h,003h,009h;
        defb    005h,020h,023h,007h,007h,007h,007h,007h,007h,008h,000h,07ah,01eh,006h,000h,094h;
        defb    022h,005h,060h,01fh,006h,02ch,00ah,000h,036h,017h,006h,000h,0e5h,016h,00ah,000h;
        defb    093h,017h,00ah,02ch,00ah,000h,093h,017h,00ah,000h,093h,017h,000h,093h,017h,0fdh;
        defb    0cbh,001h,0beh,0cdh,0fbh,019h,0afh,032h,047h,05ch,03dh,032h,03ah,05ch,018h,001h;
        defb    0e7h,0cdh,0bfh,016h,0fdh,034h,00dh,0fah,08ah,01ch,0dfh,006h,000h,0feh,00dh,028h;
        defb    07ah,0feh,03ah,028h,0ebh,021h,076h,01bh,0e5h,04fh,0e7h,079h,0d6h,0ceh,0dah,08ah;
        defb    01ch,04fh,021h,048h,01ah,009h,04eh,009h,018h,003h,02ah,074h,05ch,07eh,023h,022h;
        defb    074h,05ch,001h,052h,01bh,0c5h,04fh,0feh,020h,030h,00ch,021h,001h,01ch,006h,000h;
        defb    009h,04eh,009h,0e5h,0dfh,005h,0c9h,0dfh,0b9h,0c2h,08ah,01ch,0e7h,0c9h,0cdh,054h;
        defb    01fh,038h,002h,0cfh,014h,0fdh,0cbh,00ah,07eh,020h,071h,02ah,042h,05ch,0cbh,07ch;
        defb    028h,014h,021h,0feh,0ffh,022h,045h,05ch,02ah,061h,05ch,02bh,0edh,05bh,059h,05ch;
        defb    01bh,03ah,044h,05ch,018h,033h,0cdh,06eh,019h,03ah,044h,05ch,028h,019h,0a7h,020h;
        defb    043h,047h,07eh,0e6h,0c0h,078h,028h,00fh,0cfh,0ffh,0c1h,0cdh,030h,025h,0c8h,02ah;
        defb    055h,05ch,03eh,0c0h,0a6h,0c0h,0afh,0feh,001h,0ceh,000h,056h,023h,05eh,0edh,053h;
        defb    045h,05ch,023h,05eh,023h,056h,0ebh,019h,023h,022h,055h,05ch,0ebh,022h,05dh,05ch;
        defb    057h,01eh,000h,0fdh,036h,00ah,0ffh,015h,0fdh,072h,00dh,0cah,028h,01bh,014h,0cdh;
        defb    08bh,019h,028h,008h,0cfh,016h,0cdh,030h,025h,0c0h,0c1h,0c1h,0dfh,0feh,00dh,028h;
        defb    0bah,0feh,03ah,0cah,028h,01bh,0c3h,08ah,01ch,00fh,01dh,04bh,009h,067h,00bh,07bh;
        defb    08eh,071h,0b4h,081h,0cfh,0cdh,0deh,01ch,0bfh,0c1h,0cch,0eeh,01bh,0ebh,02ah,074h;
        defb    05ch,04eh,023h,046h,0ebh,0c5h,0c9h,0cdh,0b2h,028h,0fdh,036h,037h,000h,030h,008h;
        defb    0fdh,0cbh,037h,0ceh,020h,018h,0cfh,001h,0cch,096h,029h,0fdh,0cbh,001h,076h,020h;
        defb    00dh,0afh,0cdh,030h,025h,0c4h,0f1h,02bh,021h,071h,05ch,0b6h,077h,0ebh,0edh,043h;
        defb    072h,05ch,022h,04dh,05ch,0c9h,0c1h,0cdh,056h,01ch,0cdh,0eeh,01bh,0c9h,03ah,03bh;
        defb    05ch,0f5h,0cdh,0fbh,024h,0f1h,0fdh,056h,001h,0aah,0e6h,040h,020h,024h,0cbh,07ah;
        defb    0c2h,0ffh,02ah,0c9h,0cdh,0b2h,028h,0f5h,079h,0f6h,09fh,03ch,020h,014h,0f1h,018h;
        defb    0a9h,0e7h,0cdh,082h,01ch,0feh,02ch,020h,009h,0e7h,0cdh,0fbh,024h,0fdh,0cbh,001h;
        defb    076h,0c0h,0cfh,00bh,0cdh,0fbh,024h,0fdh,0cbh,001h,076h,0c8h,018h,0f4h,0fdh,0cbh;
        defb    001h,07eh,0fdh,0cbh,002h,086h,0c4h,04dh,00dh,0f1h,03ah,074h,05ch,0d6h,013h,0cdh;
        defb    0fch,021h,0cdh,0eeh,01bh,02ah,08fh,05ch,022h,08dh,05ch,021h,091h,05ch,07eh,007h;
        defb    0aeh,0e6h,0aah,0aeh,077h,0c9h,0cdh,030h,025h,028h,013h,0fdh,0cbh,002h,086h,0cdh;
        defb    04dh,00dh,021h,090h,05ch,07eh,0f6h,0f8h,077h,0fdh,0cbh,057h,0b6h,0dfh,0cdh,0e2h;
        defb    021h,018h,09fh,0c3h,005h,006h,0feh,00dh,028h,004h,0feh,03ah,020h,09ch,0cdh,030h;
        defb    025h,0c8h,0efh,0a0h,038h,0c9h,0cfh,008h,0c1h,0cdh,030h,025h,028h,00ah,0efh,002h;
        defb    038h,0ebh,0cdh,0e9h,034h,0dah,0b3h,01bh,0c3h,029h,01bh,0feh,0cdh,020h,009h,0e7h;
        defb    0cdh,082h,01ch,0cdh,0eeh,01bh,018h,006h,0cdh,0eeh,01bh,0efh,0a1h,038h,0efh,0c0h;
        defb    002h,001h,0e0h,001h,038h,0cdh,0ffh,02ah,022h,068h,05ch,02bh,07eh,0cbh,0feh,001h;
        defb    006h,000h,009h,007h,038h,006h,00eh,00dh,0cdh,055h,016h,023h,0e5h,0efh,002h,002h;
        defb    038h,0e1h,0ebh,00eh,00ah,0edh,0b0h,02ah,045h,05ch,0ebh,073h,023h,072h,0fdh,056h;
        defb    00dh,014h,023h,072h,0cdh,0dah,01dh,0d0h,0fdh,046h,038h,02ah,045h,05ch,022h,042h;
        defb    05ch,03ah,047h,05ch,0edh,044h,057h,02ah,05dh,05ch,01eh,0f3h,0c5h,0edh,04bh,055h;
        defb    05ch,0cdh,086h,01dh,0edh,043h,055h,05ch,0c1h,038h,011h,0e7h,0f6h,020h,0b8h,028h;
        defb    003h,0e7h,018h,0e8h,0e7h,03eh,001h,092h,032h,044h,05ch,0c9h,0cfh,011h,07eh,0feh;
        defb    03ah,028h,018h,023h,07eh,0e6h,0c0h,037h,0c0h,046h,023h,04eh,0edh,043h,042h,05ch;
        defb    023h,04eh,023h,046h,0e5h,009h,044h,04dh,0e1h,016h,000h,0c5h,0cdh,08bh,019h,0c1h;
        defb    0d0h,018h,0e0h,0fdh,0cbh,037h,04eh,0c2h,02eh,01ch,02ah,04dh,05ch,0cbh,07eh,028h;
        defb    01fh,023h,022h,068h,05ch,0efh,0e0h,0e2h,00fh,0c0h,002h,038h,0cdh,0dah,01dh,0d8h;
        defb    02ah,068h,05ch,011h,00fh,000h,019h,05eh,023h,056h,023h,066h,0ebh,0c3h,073h,01eh;
        defb    0cfh,000h,0efh,0e1h,0e0h,0e2h,036h,000h,002h,001h,003h,037h,000h,004h,038h,0a7h;
        defb    0c9h,038h,037h,0c9h,0e7h,0cdh,01fh,01ch,0cdh,030h,025h,028h,029h,0dfh,022h,05fh;
        defb    05ch,02ah,057h,05ch,07eh,0feh,02ch,028h,009h,01eh,0e4h,0cdh,086h,01dh,030h,002h;
        defb    0cfh,00dh,0cdh,077h,000h,0cdh,056h,01ch,0dfh,022h,057h,05ch,02ah,05fh,05ch,0fdh;
        defb    036h,026h,000h,0cdh,078h,000h,0dfh,0feh,02ch,028h,0c9h,0cdh,0eeh,01bh,0c9h,0cdh;
        defb    030h,025h,020h,00bh,0cdh,0fbh,024h,0feh,02ch,0c4h,0eeh,01bh,0e7h,018h,0f5h,03eh;
        defb    0e4h,047h,0edh,0b9h,011h,000h,002h,0c3h,08bh,019h,0cdh,099h,01eh,060h,069h,0cdh;
        defb    06eh,019h,02bh,022h,057h,05ch,0c9h,0cdh,099h,01eh,078h,0b1h,020h,004h,0edh,04bh;
        defb    078h,05ch,0edh,043h,076h,05ch,0c9h,02ah,06eh,05ch,0fdh,056h,036h,018h,00ch,0cdh;
        defb    099h,01eh,060h,069h,016h,000h,07ch,0feh,0f0h,030h,02ch,022h,042h,05ch,0fdh,072h;
        defb    00ah,0c9h,0cdh,085h,01eh,0edh,079h,0c9h,0cdh,085h,01eh,002h,0c9h,0cdh,0d5h,02dh;
        defb    038h,015h,028h,002h,0edh,044h,0f5h,0cdh,099h,01eh,0f1h,0c9h,0cdh,0d5h,02dh,018h;
        defb    003h,0cdh,0a2h,02dh,038h,001h,0c8h,0cfh,00ah,0cdh,067h,01eh,001h,000h,000h,0cdh;
        defb    045h,01eh,018h,003h,0cdh,099h,01eh,078h,0b1h,020h,004h,0edh,04bh,0b2h,05ch,0c5h;
        defb    0edh,05bh,04bh,05ch,02ah,059h,05ch,02bh,0cdh,0e5h,019h,0cdh,06bh,00dh,02ah,065h;
        defb    05ch,011h,032h,000h,019h,0d1h,0edh,052h,030h,008h,02ah,0b4h,05ch,0a7h,0edh,052h;
        defb    030h,002h,0cfh,015h,0ebh,022h,0b2h,05ch,0d1h,0c1h,036h,03eh,02bh,0f9h,0c5h,0edh;
        defb    073h,03dh,05ch,0ebh,0e9h,0d1h,0fdh,066h,00dh,024h,0e3h,033h,0edh,04bh,045h,05ch;
        defb    0c5h,0e5h,0edh,073h,03dh,05ch,0d5h,0cdh,067h,01eh,001h,014h,000h,02ah,065h,05ch;
        defb    009h,038h,00ah,0ebh,021h,050h,000h,019h,038h,003h,0edh,072h,0d8h,02eh,003h,0c3h;
        defb    055h,000h,001h,000h,000h,0cdh,005h,01fh,044h,04dh,0c9h,0c1h,0e1h,0d1h,07ah,0feh;
        defb    03eh,028h,00bh,03bh,0e3h,0ebh,0edh,073h,03dh,05ch,0c5h,0c3h,073h,01eh,0d5h,0e5h;
        defb    0cfh,006h,0cdh,099h,01eh,076h,00bh,078h,0b1h,028h,00ch,078h,0a1h,03ch,020h,001h;
        defb    003h,0fdh,0cbh,001h,06eh,028h,0eeh,0fdh,0cbh,001h,0aeh,0c9h,03eh,07fh,0dbh,0feh;
        defb    01fh,0d8h,03eh,0feh,0dbh,0feh,01fh,0c9h,0cdh,030h,025h,028h,005h,03eh,0ceh,0c3h;
        defb    039h,01eh,0fdh,0cbh,001h,0f6h,0cdh,08dh,02ch,030h,016h,0e7h,0feh,024h,020h,005h;
        defb    0fdh,0cbh,001h,0b6h,0e7h,0feh,028h,020h,03ch,0e7h,0feh,029h,028h,020h,0cdh,08dh;
        defb    02ch,0d2h,08ah,01ch,0ebh,0e7h,0feh,024h,020h,002h,0ebh,0e7h,0ebh,001h,006h,000h;
        defb    0cdh,055h,016h,023h,023h,036h,00eh,0feh,02ch,020h,003h,0e7h,018h,0e0h,0feh,029h;
        defb    020h,013h,0e7h,0feh,03dh,020h,00eh,0e7h,03ah,03bh,05ch,0f5h,0cdh,0fbh,024h,0f1h;
        defb    0fdh,0aeh,001h,0e6h,040h,0c2h,08ah,01ch,0cdh,0eeh,01bh,0cdh,030h,025h,0e1h,0c8h;
        defb    0e9h,03eh,003h,018h,002h,03eh,002h,0cdh,030h,025h,0c4h,001h,016h,0cdh,04dh,00dh;
        defb    0cdh,0dfh,01fh,0cdh,0eeh,01bh,0c9h,0dfh,0cdh,045h,020h,028h,00dh,0cdh,04eh,020h;
        defb    028h,0fbh,0cdh,0fch,01fh,0cdh,04eh,020h,028h,0f3h,0feh,029h,0c8h,0cdh,0c3h,01fh;
        defb    03eh,00dh,0d7h,0c9h,0dfh,0feh,0ach,020h,00dh,0cdh,079h,01ch,0cdh,0c3h,01fh,0cdh;
        defb    007h,023h,03eh,016h,018h,010h,0feh,0adh,020h,012h,0e7h,0cdh,082h,01ch,0cdh,0c3h;
        defb    01fh,0cdh,099h,01eh,03eh,017h,0d7h,079h,0d7h,078h,0d7h,0c9h,0cdh,0f2h,021h,0d0h;
        defb    0cdh,070h,020h,0d0h,0cdh,0fbh,024h,0cdh,0c3h,01fh,0fdh,0cbh,001h,076h,0cch,0f1h;
        defb    02bh,0c2h,0e3h,02dh,078h,0b1h,00bh,0c8h,01ah,013h,0d7h,018h,0f7h,0feh,029h,0c8h;
        defb    0feh,00dh,0c8h,0feh,03ah,0c9h,0dfh,0feh,03bh,028h,014h,0feh,02ch,020h,00ah,0cdh;
        defb    030h,025h,028h,00bh,03eh,006h,0d7h,018h,006h,0feh,027h,0c0h,0cdh,0f5h,01fh,0e7h;
        defb    0cdh,045h,020h,020h,001h,0c1h,0bfh,0c9h,0feh,023h,037h,0c0h,0e7h,0cdh,082h,01ch;
        defb    0a7h,0cdh,0c3h,01fh,0cdh,094h,01eh,0feh,010h,0d2h,00eh,016h,0cdh,001h,016h,0a7h;
        defb    0c9h,0cdh,030h,025h,028h,008h,03eh,001h,0cdh,001h,016h,0cdh,06eh,00dh,0fdh,036h;
        defb    002h,001h,0cdh,0c1h,020h,0cdh,0eeh,01bh,0edh,04bh,088h,05ch,03ah,06bh,05ch,0b8h;
        defb    038h,003h,00eh,021h,047h,0edh,043h,088h,05ch,03eh,019h,090h,032h,08ch,05ch,0fdh;
        defb    0cbh,002h,086h,0cdh,0d9h,00dh,0c3h,06eh,00dh,0cdh,04eh,020h,028h,0fbh,0feh,028h;
        defb    020h,00eh,0e7h,0cdh,0dfh,01fh,0dfh,0feh,029h,0c2h,08ah,01ch,0e7h,0c3h,0b2h,021h;
        defb    0feh,0cah,020h,011h,0e7h,0cdh,01fh,01ch,0fdh,0cbh,037h,0feh,0fdh,0cbh,001h,076h;
        defb    0c2h,08ah,01ch,018h,00dh,0cdh,08dh,02ch,0d2h,0afh,021h,0cdh,01fh,01ch,0fdh,0cbh;
        defb    037h,0beh,0cdh,030h,025h,0cah,0b2h,021h,0cdh,0bfh,016h,021h,071h,05ch,0cbh,0b6h;
        defb    0cbh,0eeh,001h,001h,000h,0cbh,07eh,020h,00bh,03ah,03bh,05ch,0e6h,040h,020h,002h;
        defb    00eh,003h,0b6h,077h,0f7h,036h,00dh,079h,00fh,00fh,030h,005h,03eh,022h,012h,02bh;
        defb    077h,022h,05bh,05ch,0fdh,0cbh,037h,07eh,020h,02ch,02ah,05dh,05ch,0e5h,02ah,03dh;
        defb    05ch,0e5h,021h,03ah,021h,0e5h,0fdh,0cbh,030h,066h,028h,004h,0edh,073h,03dh,05ch;
        defb    02ah,061h,05ch,0cdh,0a7h,011h,0fdh,036h,000h,0ffh,0cdh,02ch,00fh,0fdh,0cbh,001h;
        defb    0beh,0cdh,0b9h,021h,018h,003h,0cdh,02ch,00fh,0fdh,036h,022h,000h,0cdh,0d6h,021h;
        defb    020h,00ah,0cdh,01dh,011h,0edh,04bh,082h,05ch,0cdh,0d9h,00dh,021h,071h,05ch,0cbh;
        defb    0aeh,0cbh,07eh,0cbh,0beh,020h,01ch,0e1h,0e1h,022h,03dh,05ch,0e1h,022h,05fh,05ch;
        defb    0fdh,0cbh,001h,0feh,0cdh,0b9h,021h,02ah,05fh,05ch,0fdh,036h,026h,000h,022h,05dh;
        defb    05ch,018h,017h,02ah,063h,05ch,0edh,05bh,061h,05ch,037h,0edh,052h,044h,04dh,0cdh;
        defb    0b2h,02ah,0cdh,0ffh,02ah,018h,003h,0cdh,0fch,01fh,0cdh,04eh,020h,0cah,0c1h,020h;
        defb    0c9h,02ah,061h,05ch,022h,05dh,05ch,0dfh,0feh,0e2h,028h,00ch,03ah,071h,05ch,0cdh;
        defb    059h,01ch,0dfh,0feh,00dh,0c8h,0cfh,00bh,0cdh,030h,025h,0c8h,0cfh,010h,02ah,051h;
        defb    05ch,023h,023h,023h,023h,07eh,0feh,04bh,0c9h,0e7h,0cdh,0f2h,021h,0d8h,0dfh,0feh;
        defb    02ch,028h,0f6h,0feh,03bh,028h,0f2h,0c3h,08ah,01ch,0feh,0d9h,0d8h,0feh,0dfh,03fh;
        defb    0d8h,0f5h,0e7h,0f1h,0d6h,0c9h,0f5h,0cdh,082h,01ch,0f1h,0a7h,0cdh,0c3h,01fh,0f5h;
        defb    0cdh,094h,01eh,057h,0f1h,0d7h,07ah,0d7h,0c9h,0d6h,011h,0ceh,000h,028h,01dh,0d6h;
        defb    002h,0ceh,000h,028h,056h,0feh,001h,07ah,006h,001h,020h,004h,007h,007h,006h,004h;
        defb    04fh,07ah,0feh,002h,030h,016h,079h,021h,091h,05ch,018h,038h,07ah,006h,007h,038h;
        defb    005h,007h,007h,007h,006h,038h,04fh,07ah,0feh,00ah,038h,002h,0cfh,013h,021h,08fh;
        defb    05ch,0feh,008h,038h,00bh,07eh,028h,007h,0b0h,02fh,0e6h,024h,028h,001h,078h,04fh;
        defb    079h,0cdh,06ch,022h,03eh,007h,0bah,09fh,0cdh,06ch,022h,007h,007h,0e6h,050h,047h;
        defb    03eh,008h,0bah,09fh,0aeh,0a0h,0aeh,077h,023h,078h,0c9h,09fh,07ah,00fh,006h,080h;
        defb    020h,003h,00fh,006h,040h,04fh,07ah,0feh,008h,028h,004h,0feh,002h,030h,0bdh,079h;
        defb    021h,08fh,05ch,0cdh,06ch,022h,079h,00fh,00fh,00fh,018h,0d8h,0cdh,094h,01eh,0feh;
        defb    008h,030h,0a9h,0d3h,0feh,007h,007h,007h,0cbh,06fh,020h,002h,0eeh,007h,032h,048h;
        defb    05ch,0c9h,03eh,0afh,090h,0dah,0f9h,024h,047h,0a7h,01fh,037h,01fh,0a7h,01fh,0a8h;
        defb    0e6h,0f8h,0a8h,067h,079h,007h,007h,007h,0a8h,0e6h,0c7h,0a8h,007h,007h,06fh,079h;
        defb    0e6h,007h,0c9h,0cdh,007h,023h,0cdh,0aah,022h,047h,004h,07eh,007h,010h,0fdh,0e6h;
        defb    001h,0c3h,028h,02dh,0cdh,007h,023h,0cdh,0e5h,022h,0c3h,04dh,00dh,0edh,043h,07dh;
        defb    05ch,0cdh,0aah,022h,047h,004h,03eh,0feh,00fh,010h,0fdh,047h,07eh,0fdh,04eh,057h;
        defb    0cbh,041h,020h,001h,0a0h,0cbh,051h,020h,002h,0a8h,02fh,077h,0c3h,0dbh,00bh,0cdh;
        defb    014h,023h,047h,0c5h,0cdh,014h,023h,059h,0c1h,051h,04fh,0c9h,0cdh,0d5h,02dh,0dah;
        defb    0f9h,024h,00eh,001h,0c8h,00eh,0ffh,0c9h,0dfh,0feh,02ch,0c2h,08ah,01ch,0e7h,0cdh;
        defb    082h,01ch,0cdh,0eeh,01bh,0efh,02ah,03dh,038h,07eh,0feh,081h,030h,005h,0efh,002h;
        defb    038h,018h,0a1h,0efh,0a3h,038h,036h,083h,0efh,0c5h,002h,038h,0cdh,07dh,024h,0c5h;
        defb    0efh,031h,0e1h,004h,038h,07eh,0feh,080h,030h,008h,0efh,002h,002h,038h,0c1h,0c3h;
        defb    0dch,022h,0efh,0c2h,001h,0c0h,002h,003h,001h,0e0h,00fh,0c0h,001h,031h,0e0h,001h;
        defb    031h,0e0h,0a0h,0c1h,002h,038h,0fdh,034h,062h,0cdh,094h,01eh,06fh,0e5h,0cdh,094h;
        defb    01eh,0e1h,067h,022h,07dh,05ch,0c1h,0c3h,020h,024h,0dfh,0feh,02ch,028h,006h,0cdh;
        defb    0eeh,01bh,0c3h,077h,024h,0e7h,0cdh,082h,01ch,0cdh,0eeh,01bh,0efh,0c5h,0a2h,004h;
        defb    01fh,031h,030h,030h,000h,006h,002h,038h,0c3h,077h,024h,0c0h,002h,0c1h,002h,031h;
        defb    02ah,0e1h,001h,0e1h,02ah,00fh,0e0h,005h,02ah,0e0h,001h,03dh,038h,07eh,0feh,081h;
        defb    030h,007h,0efh,002h,002h,038h,0c3h,077h,024h,0cdh,07dh,024h,0c5h,0efh,002h,0e1h;
        defb    001h,005h,0c1h,002h,001h,031h,0e1h,004h,0c2h,002h,001h,031h,0e1h,004h,0e2h,0e5h;
        defb    0e0h,003h,0a2h,004h,031h,01fh,0c5h,002h,020h,0c0h,002h,0c2h,002h,0c1h,0e5h,004h;
        defb    0e0h,0e2h,004h,00fh,0e1h,001h,0c1h,002h,0e0h,004h,0e2h,0e5h,004h,003h,0c2h,02ah;
        defb    0e1h,02ah,00fh,002h,038h,01ah,0feh,081h,0c1h,0dah,077h,024h,0c5h,0efh,001h,038h;
        defb    03ah,07dh,05ch,0cdh,028h,02dh,0efh,0c0h,00fh,001h,038h,03ah,07eh,05ch,0cdh,028h;
        defb    02dh,0efh,0c5h,00fh,0e0h,0e5h,038h,0c1h,005h,028h,03ch,018h,014h,0efh,0e1h,031h;
        defb    0e3h,004h,0e2h,0e4h,004h,003h,0c1h,002h,0e4h,004h,0e2h,0e3h,004h,00fh,0c2h,002h;
        defb    038h,0c5h,0efh,0c0h,002h,0e1h,00fh,031h,038h,03ah,07dh,05ch,0cdh,028h,02dh,0efh;
        defb    003h,0e0h,0e2h,00fh,0c0h,001h,0e0h,038h,03ah,07eh,05ch,0cdh,028h,02dh,0efh,003h;
        defb    038h,0cdh,0b7h,024h,0c1h,010h,0c6h,0efh,002h,002h,001h,038h,03ah,07dh,05ch,0cdh;
        defb    028h,02dh,0efh,003h,001h,038h,03ah,07eh,05ch,0cdh,028h,02dh,0efh,003h,038h,0cdh;
        defb    0b7h,024h,0c3h,04dh,00dh,0efh,031h,028h,034h,032h,000h,001h,005h,0e5h,001h,005h;
        defb    02ah,038h,0cdh,0d5h,02dh,038h,006h,0e6h,0fch,0c6h,004h,030h,002h,03eh,0fch,0f5h;
        defb    0cdh,028h,02dh,0efh,0e5h,001h,005h,031h,01fh,0c4h,002h,031h,0a2h,004h,01fh,0c1h;
        defb    001h,0c0h,002h,031h,004h,031h,00fh,0a1h,003h,01bh,0c3h,002h,038h,0c1h,0c9h,0cdh;
        defb    007h,023h,079h,0b8h,030h,006h,069h,0d5h,0afh,05fh,018h,007h,0b1h,0c8h,068h,041h;
        defb    0d5h,016h,000h,060h,078h,01fh,085h,038h,003h,0bch,038h,007h,094h,04fh,0d9h,0c1h;
        defb    0c5h,018h,004h,04fh,0d5h,0d9h,0c1h,02ah,07dh,05ch,078h,084h,047h,079h,03ch,085h;
        defb    038h,00dh,028h,00dh,03dh,04fh,0cdh,0e5h,022h,0d9h,079h,010h,0d9h,0d1h,0c9h,028h;
        defb    0f3h,0cfh,00ah,0dfh,006h,000h,0c5h,04fh,021h,096h,025h,0cdh,0dch,016h,079h,0d2h;
        defb    084h,026h,006h,000h,04eh,009h,0e9h,0cdh,074h,000h,003h,0feh,00dh,0cah,08ah,01ch;
        defb    0feh,022h,020h,0f3h,0cdh,074h,000h,0feh,022h,0c9h,0e7h,0feh,028h,020h,006h,0cdh;
        defb    079h,01ch,0dfh,0feh,029h,0c2h,08ah,01ch,0fdh,0cbh,001h,07eh,0c9h,0cdh,007h,023h;
        defb    02ah,036h,05ch,011h,000h,001h,019h,079h,00fh,00fh,00fh,0e6h,0e0h,0a8h,05fh,079h;
        defb    0e6h,018h,0eeh,040h,057h,006h,060h,0c5h,0d5h,0e5h,01ah,0aeh,028h,004h,03ch,020h;
        defb    01ah,03dh,04fh,006h,007h,014h,023h,01ah,0aeh,0a9h,020h,00fh,010h,0f7h,0c1h,0c1h;
        defb    0c1h,03eh,080h,090h,001h,001h,000h,0f7h,012h,018h,00ah,0e1h,011h,008h,000h,019h;
        defb    0d1h,0c1h,010h,0d3h,048h,0c3h,0b2h,02ah,0cdh,007h,023h,079h,00fh,00fh,00fh,04fh;
        defb    0e6h,0e0h,0a8h,06fh,079h,0e6h,003h,0eeh,058h,067h,07eh,0c3h,028h,02dh,022h,01ch;
        defb    028h,04fh,02eh,0f2h,02bh,012h,0a8h,056h,0a5h,057h,0a7h,084h,0a6h,08fh,0c4h,0e6h;
        defb    0aah,0bfh,0abh,0c7h,0a9h,0ceh,000h,0e7h,0c3h,0ffh,024h,0dfh,023h,0e5h,001h,000h;
        defb    000h,0cdh,00fh,025h,020h,01bh,0cdh,00fh,025h,028h,0fbh,0cdh,030h,025h,028h,011h;
        defb    0f7h,0e1h,0d5h,07eh,023h,012h,013h,0feh,022h,020h,0f8h,07eh,023h,0feh,022h,028h;
        defb    0f2h,00bh,0d1h,021h,03bh,05ch,0cbh,0b6h,0cbh,07eh,0c4h,0b2h,02ah,0c3h,012h,027h;
        defb    0e7h,0cdh,0fbh,024h,0feh,029h,0c2h,08ah,01ch,0e7h,0c3h,012h,027h,0c3h,0bdh,027h;
        defb    0cdh,030h,025h,028h,028h,0edh,04bh,076h,05ch,0cdh,02bh,02dh,0efh,0a1h,00fh,034h;
        defb    037h,016h,004h,034h,080h,041h,000h,000h,080h,032h,002h,0a1h,003h,031h,038h,0cdh;
        defb    0a2h,02dh,0edh,043h,076h,05ch,07eh,0a7h,028h,003h,0d6h,010h,077h,018h,009h,0cdh;
        defb    030h,025h,028h,004h,0efh,0a3h,038h,034h,0e7h,0c3h,0c3h,026h,001h,05ah,010h,0e7h;
        defb    0feh,023h,0cah,00dh,027h,021h,03bh,05ch,0cbh,0b6h,0cbh,07eh,028h,01fh,0cdh,08eh;
        defb    002h,00eh,000h,020h,013h,0cdh,01eh,003h,030h,00eh,015h,05fh,0cdh,033h,003h,0f5h;
        defb    001h,001h,000h,0f7h,0f1h,012h,00eh,001h,006h,000h,0cdh,0b2h,02ah,0c3h,012h,027h;
        defb    0cdh,022h,025h,0c4h,035h,025h,0e7h,0c3h,0dbh,025h,0cdh,022h,025h,0c4h,080h,025h;
        defb    0e7h,018h,048h,0cdh,022h,025h,0c4h,0cbh,022h,0e7h,018h,03fh,0cdh,088h,02ch,030h;
        defb    056h,0feh,041h,030h,03ch,0cdh,030h,025h,020h,023h,0cdh,09bh,02ch,0dfh,001h,006h;
        defb    000h,0cdh,055h,016h,023h,036h,00eh,023h,0ebh,02ah,065h,05ch,00eh,005h,0a7h,0edh;
        defb    042h,022h,065h,05ch,0edh,0b0h,0ebh,02bh,0cdh,077h,000h,018h,00eh,0dfh,023h,07eh;
        defb    0feh,00eh,020h,0fah,023h,0cdh,0b4h,033h,022h,05dh,05ch,0fdh,0cbh,001h,0f6h,018h;
        defb    014h,0cdh,0b2h,028h,0dah,02eh,01ch,0cch,096h,029h,03ah,03bh,05ch,0feh,0c0h,038h;
        defb    004h,023h,0cdh,0b4h,033h,018h,033h,001h,0dbh,009h,0feh,02dh,028h,027h,001h,018h;
        defb    010h,0feh,0aeh,028h,020h,0d6h,0afh,0dah,08ah,01ch,001h,0f0h,004h,0feh,014h,028h;
        defb    014h,0d2h,08ah,01ch,006h,010h,0c6h,0dch,04fh,0feh,0dfh,030h,002h,0cbh,0b1h,0feh;
        defb    0eeh,038h,002h,0cbh,0b9h,0c5h,0e7h,0c3h,0ffh,024h,0dfh,0feh,028h,020h,00ch,0fdh;
        defb    0cbh,001h,076h,020h,017h,0cdh,052h,02ah,0e7h,018h,0f0h,006h,000h,04fh,021h,095h;
        defb    027h,0cdh,0dch,016h,030h,006h,04eh,021h,0edh,026h,009h,046h,0d1h,07ah,0b8h,038h;
        defb    03ah,0a7h,0cah,018h,000h,0c5h,021h,03bh,05ch,07bh,0feh,0edh,020h,006h,0cbh,076h;
        defb    020h,002h,01eh,099h,0d5h,0cdh,030h,025h,028h,009h,07bh,0e6h,03fh,047h,0efh,03bh;
        defb    038h,018h,009h,07bh,0fdh,0aeh,001h,0e6h,040h,0c2h,08ah,01ch,0d1h,021h,03bh,05ch;
        defb    0cbh,0f6h,0cbh,07bh,020h,002h,0cbh,0b6h,0c1h,018h,0c1h,0d5h,079h,0fdh,0cbh,001h;
        defb    076h,020h,015h,0e6h,03fh,0c6h,008h,04fh,0feh,010h,020h,004h,0cbh,0f1h,018h,008h;
        defb    038h,0d7h,0feh,017h,028h,002h,0cbh,0f9h,0c5h,0e7h,0c3h,0ffh,024h,02bh,0cfh,02dh;
        defb    0c3h,02ah,0c4h,02fh,0c5h,05eh,0c6h,03dh,0ceh,03eh,0cch,03ch,0cdh,0c7h,0c9h,0c8h;
        defb    0cah,0c9h,0cbh,0c5h,0c7h,0c6h,0c8h,000h,006h,008h,008h,00ah,002h,003h,005h,005h;
        defb    005h,005h,005h,005h,006h,0cdh,030h,025h,020h,035h,0e7h,0cdh,08dh,02ch,0d2h,08ah;
        defb    01ch,0e7h,0feh,024h,0f5h,020h,001h,0e7h,0feh,028h,020h,012h,0e7h,0feh,029h,028h;
        defb    010h,0cdh,0fbh,024h,0dfh,0feh,02ch,020h,003h,0e7h,018h,0f5h,0feh,029h,0c2h,08ah;
        defb    01ch,0e7h,021h,03bh,05ch,0cbh,0b6h,0f1h,028h,002h,0cbh,0f6h,0c3h,012h,027h,0e7h;
        defb    0e6h,0dfh,047h,0e7h,0d6h,024h,04fh,020h,001h,0e7h,0e7h,0e5h,02ah,053h,05ch,02bh;
        defb    011h,0ceh,000h,0c5h,0cdh,086h,01dh,0c1h,030h,002h,0cfh,018h,0e5h,0cdh,0abh,028h;
        defb    0e6h,0dfh,0b8h,020h,008h,0cdh,0abh,028h,0d6h,024h,0b9h,028h,00ch,0e1h,02bh,011h;
        defb    000h,002h,0c5h,0cdh,08bh,019h,0c1h,018h,0d7h,0a7h,0cch,0abh,028h,0d1h,0d1h,0edh;
        defb    053h,05dh,05ch,0cdh,0abh,028h,0e5h,0feh,029h,028h,042h,023h,07eh,0feh,00eh,016h;
        defb    040h,028h,007h,02bh,0cdh,0abh,028h,023h,016h,000h,023h,0e5h,0d5h,0cdh,0fbh,024h;
        defb    0f1h,0fdh,0aeh,001h,0e6h,040h,020h,02bh,0e1h,0ebh,02ah,065h,05ch,001h,005h,000h;
        defb    0edh,042h,022h,065h,05ch,0edh,0b0h,0ebh,02bh,0cdh,0abh,028h,0feh,029h,028h,00dh;
        defb    0e5h,0dfh,0feh,02ch,020h,00dh,0e7h,0e1h,0cdh,0abh,028h,018h,0beh,0e5h,0dfh,0feh;
        defb    029h,028h,002h,0cfh,019h,0d1h,0ebh,022h,05dh,05ch,02ah,00bh,05ch,0e3h,022h,00bh;
        defb    05ch,0d5h,0e7h,0e7h,0cdh,0fbh,024h,0e1h,022h,05dh,05ch,0e1h,022h,00bh,05ch,0e7h;
        defb    0c3h,012h,027h,023h,07eh,0feh,021h,038h,0fah,0c9h,0fdh,0cbh,001h,0f6h,0dfh,0cdh;
        defb    08dh,02ch,0d2h,08ah,01ch,0e5h,0e6h,01fh,04fh,0e7h,0e5h,0feh,028h,028h,028h,0cbh;
        defb    0f1h,0feh,024h,028h,011h,0cbh,0e9h,0cdh,088h,02ch,030h,00fh,0cdh,088h,02ch,030h;
        defb    016h,0cbh,0b1h,0e7h,018h,0f6h,0e7h,0fdh,0cbh,001h,0b6h,03ah,00ch,05ch,0a7h,028h;
        defb    006h,0cdh,030h,025h,0c2h,051h,029h,041h,0cdh,030h,025h,020h,008h,079h,0e6h,0e0h;
        defb    0cbh,0ffh,04fh,018h,037h,02ah,04bh,05ch,07eh,0e6h,07fh,028h,02dh,0b9h,020h,022h;
        defb    017h,087h,0f2h,03fh,029h,038h,030h,0d1h,0d5h,0e5h,023h,01ah,013h,0feh,020h,028h;
        defb    0fah,0f6h,020h,0beh,028h,0f4h,0f6h,080h,0beh,020h,006h,01ah,0cdh,088h,02ch,030h;
        defb    015h,0e1h,0c5h,0cdh,0b8h,019h,0ebh,0c1h,018h,0ceh,0cbh,0f8h,0d1h,0dfh,0feh,028h;
        defb    028h,009h,0cbh,0e8h,018h,00dh,0d1h,0d1h,0d1h,0e5h,0dfh,0cdh,088h,02ch,030h,003h;
        defb    0e7h,018h,0f8h,0e1h,0cbh,010h,0cbh,070h,0c9h,02ah,00bh,05ch,07eh,0feh,029h,0cah;
        defb    0efh,028h,07eh,0f6h,060h,047h,023h,07eh,0feh,00eh,028h,007h,02bh,0cdh,0abh,028h;
        defb    023h,0cbh,0a8h,078h,0b9h,028h,012h,023h,023h,023h,023h,023h,0cdh,0abh,028h,0feh;
        defb    029h,0cah,0efh,028h,0cdh,0abh,028h,018h,0d9h,0cbh,069h,020h,00ch,023h,0edh,05bh;
        defb    065h,05ch,0cdh,0c0h,033h,0ebh,022h,065h,05ch,0d1h,0d1h,0afh,03ch,0c9h,0afh,047h;
        defb    0cbh,079h,020h,04bh,0cbh,07eh,020h,00eh,03ch,023h,04eh,023h,046h,023h,0ebh,0cdh;
        defb    0b2h,02ah,0dfh,0c3h,049h,02ah,023h,023h,023h,046h,0cbh,071h,028h,00ah,005h,028h;
        defb    0e8h,0ebh,0dfh,0feh,028h,020h,061h,0ebh,0ebh,018h,024h,0e5h,0dfh,0e1h,0feh,02ch;
        defb    028h,020h,0cbh,079h,028h,052h,0cbh,071h,020h,006h,0feh,029h,020h,03ch,0e7h,0c9h;
        defb    0feh,029h,028h,06ch,0feh,0cch,020h,032h,0dfh,02bh,022h,05dh,05ch,018h,05eh,021h;
        defb    000h,000h,0e5h,0e7h,0e1h,079h,0feh,0c0h,020h,009h,0dfh,0feh,029h,028h,051h,0feh;
        defb    0cch,028h,0e5h,0c5h,0e5h,0cdh,0eeh,02ah,0e3h,0ebh,0cdh,0cch,02ah,038h,019h,00bh;
        defb    0cdh,0f4h,02ah,009h,0d1h,0c1h,010h,0b3h,0cbh,079h,020h,066h,0e5h,0cbh,071h,020h;
        defb    013h,042h,04bh,0dfh,0feh,029h,028h,002h,0cfh,002h,0e7h,0e1h,011h,005h,000h,0cdh;
        defb    0f4h,02ah,009h,0c9h,0cdh,0eeh,02ah,0e3h,0cdh,0f4h,02ah,0c1h,009h,023h,042h,04bh;
        defb    0ebh,0cdh,0b1h,02ah,0dfh,0feh,029h,028h,007h,0feh,02ch,020h,0dbh,0cdh,052h,02ah;
        defb    0e7h,0feh,028h,028h,0f8h,0fdh,0cbh,001h,0b6h,0c9h,0cdh,030h,025h,0c4h,0f1h,02bh;
        defb    0e7h,0feh,029h,028h,050h,0d5h,0afh,0f5h,0c5h,011h,001h,000h,0dfh,0e1h,0feh,0cch;
        defb    028h,017h,0f1h,0cdh,0cdh,02ah,0f5h,050h,059h,0e5h,0dfh,0e1h,0feh,0cch,028h,009h;
        defb    0feh,029h,0c2h,08ah,01ch,062h,06bh,018h,013h,0e5h,0e7h,0e1h,0feh,029h,028h,00ch;
        defb    0f1h,0cdh,0cdh,02ah,0f5h,0dfh,060h,069h,0feh,029h,020h,0e6h,0f1h,0e3h,019h,02bh;
        defb    0e3h,0a7h,0edh,052h,001h,000h,000h,038h,007h,023h,0a7h,0fah,020h,02ah,044h,04dh;
        defb    0d1h,0fdh,0cbh,001h,0b6h,0cdh,030h,025h,0c8h,0afh,0fdh,0cbh,001h,0b6h,0c5h,0cdh;
        defb    0a9h,033h,0c1h,02ah,065h,05ch,077h,023h,073h,023h,072h,023h,071h,023h,070h,023h;
        defb    022h,065h,05ch,0c9h,0afh,0d5h,0e5h,0f5h,0cdh,082h,01ch,0f1h,0cdh,030h,025h,028h;
        defb    012h,0f5h,0cdh,099h,01eh,0d1h,078h,0b1h,037h,028h,005h,0e1h,0e5h,0a7h,0edh,042h;
        defb    07ah,0deh,000h,0e1h,0d1h,0c9h,0ebh,023h,05eh,023h,056h,0c9h,0cdh,030h,025h,0c8h;
        defb    0cdh,0a9h,030h,0dah,015h,01fh,0c9h,02ah,04dh,05ch,0fdh,0cbh,037h,04eh,028h,05eh;
        defb    001h,005h,000h,003h,023h,07eh,0feh,020h,028h,0fah,030h,00bh,0feh,010h,038h,011h;
        defb    0feh,016h,030h,00dh,023h,018h,0edh,0cdh,088h,02ch,038h,0e7h,0feh,024h,0cah,0c0h;
        defb    02bh,079h,02ah,059h,05ch,02bh,0cdh,055h,016h,023h,023h,0ebh,0d5h,02ah,04dh,05ch;
        defb    01bh,0d6h,006h,047h,028h,011h,023h,07eh,0feh,021h,038h,0fah,0f6h,020h,013h,012h;
        defb    010h,0f4h,0f6h,080h,012h,03eh,0c0h,02ah,04dh,05ch,0aeh,0f6h,020h,0e1h,0cdh,0eah;
        defb    02bh,0e5h,0efh,002h,038h,0e1h,001h,005h,000h,0a7h,0edh,042h,018h,040h,0fdh,0cbh;
        defb    001h,076h,028h,006h,011h,006h,000h,019h,018h,0e7h,02ah,04dh,05ch,0edh,04bh,072h;
        defb    05ch,0fdh,0cbh,037h,046h,020h,030h,078h,0b1h,0c8h,0e5h,0f7h,0d5h,0c5h,054h,05dh;
        defb    023h,036h,020h,0edh,0b8h,0e5h,0cdh,0f1h,02bh,0e1h,0e3h,0a7h,0edh,042h,009h,030h;
        defb    002h,044h,04dh,0e3h,0ebh,078h,0b1h,028h,002h,0edh,0b0h,0c1h,0d1h,0e1h,0ebh,078h;
        defb    0b1h,0c8h,0d5h,0edh,0b0h,0e1h,0c9h,02bh,02bh,02bh,07eh,0e5h,0c5h,0cdh,0c6h,02bh;
        defb    0c1h,0e1h,003h,003h,003h,0c3h,0e8h,019h,03eh,0dfh,02ah,04dh,05ch,0a6h,0f5h,0cdh;
        defb    0f1h,02bh,0ebh,009h,0c5h,02bh,022h,04dh,05ch,003h,003h,003h,02ah,059h,05ch,02bh;
        defb    0cdh,055h,016h,02ah,04dh,05ch,0c1h,0c5h,003h,0edh,0b8h,0ebh,023h,0c1h,070h,02bh;
        defb    071h,0f1h,02bh,077h,02ah,059h,05ch,02bh,0c9h,02ah,065h,05ch,02bh,046h,02bh,04eh;
        defb    02bh,056h,02bh,05eh,02bh,07eh,022h,065h,05ch,0c9h,0cdh,0b2h,028h,0c2h,08ah,01ch;
        defb    0cdh,030h,025h,020h,008h,0cbh,0b1h,0cdh,096h,029h,0cdh,0eeh,01bh,038h,008h,0c5h;
        defb    0cdh,0b8h,019h,0cdh,0e8h,019h,0c1h,0cbh,0f9h,006h,000h,0c5h,021h,001h,000h,0cbh;
        defb    071h,020h,002h,02eh,005h,0ebh,0e7h,026h,0ffh,0cdh,0cch,02ah,0dah,020h,02ah,0e1h;
        defb    0c5h,024h,0e5h,060h,069h,0cdh,0f4h,02ah,0ebh,0dfh,0feh,02ch,028h,0e8h,0feh,029h;
        defb    020h,0bbh,0e7h,0c1h,079h,068h,026h,000h,023h,023h,029h,019h,0dah,015h,01fh,0d5h;
        defb    0c5h,0e5h,044h,04dh,02ah,059h,05ch,02bh,0cdh,055h,016h,023h,077h,0c1h,00bh,00bh;
        defb    00bh,023h,071h,023h,070h,0c1h,078h,023h,077h,062h,06bh,01bh,036h,000h,0cbh,071h;
        defb    028h,002h,036h,020h,0c1h,0edh,0b8h,0c1h,070h,02bh,071h,02bh,03dh,020h,0f8h,0c9h;
        defb    0cdh,01bh,02dh,03fh,0d8h,0feh,041h,03fh,0d0h,0feh,05bh,0d8h,0feh,061h,03fh,0d0h;
        defb    0feh,07bh,0c9h,0feh,0c4h,020h,019h,011h,000h,000h,0e7h,0d6h,031h,0ceh,000h,020h;
        defb    00ah,0ebh,03fh,0edh,06ah,0dah,0adh,031h,0ebh,018h,0efh,042h,04bh,0c3h,02bh,02dh;
        defb    0feh,02eh,028h,00fh,0cdh,03bh,02dh,0feh,02eh,020h,028h,0e7h,0cdh,01bh,02dh,038h;
        defb    022h,018h,00ah,0e7h,0cdh,01bh,02dh,0dah,08ah,01ch,0efh,0a0h,038h,0efh,0a1h,0c0h;
        defb    002h,038h,0dfh,0cdh,022h,02dh,038h,00bh,0efh,0e0h,0a4h,005h,0c0h,004h,00fh,038h;
        defb    0e7h,018h,0efh,0feh,045h,028h,003h,0feh,065h,0c0h,006h,0ffh,0e7h,0feh,02bh,028h;
        defb    005h,0feh,02dh,020h,002h,004h,0e7h,0cdh,01bh,02dh,038h,0cbh,0c5h,0cdh,03bh,02dh;
        defb    0cdh,0d5h,02dh,0c1h,0dah,0adh,031h,0a7h,0fah,0adh,031h,004h,028h,002h,0edh,044h;
        defb    0c3h,04fh,02dh,0feh,030h,0d8h,0feh,03ah,03fh,0c9h,0cdh,01bh,02dh,0d8h,0d6h,030h;
        defb    04fh,006h,000h,0fdh,021h,03ah,05ch,0afh,05fh,051h,048h,047h,0cdh,0b6h,02ah,0efh;
        defb    038h,0a7h,0c9h,0f5h,0efh,0a0h,038h,0f1h,0cdh,022h,02dh,0d8h,0efh,001h,0a4h,004h;
        defb    00fh,038h,0cdh,074h,000h,018h,0f1h,007h,00fh,030h,002h,02fh,03ch,0f5h,021h,092h;
        defb    05ch,0cdh,00bh,035h,0efh,0a4h,038h,0f1h,0cbh,03fh,030h,00dh,0f5h,0efh,0c1h,0e0h;
        defb    000h,004h,004h,033h,002h,005h,0e1h,038h,0f1h,028h,008h,0f5h,0efh,031h,004h,038h;
        defb    0f1h,018h,0e5h,0efh,002h,038h,0c9h,023h,04eh,023h,07eh,0a9h,091h,05fh,023h,07eh;
        defb    089h,0a9h,057h,0c9h,00eh,000h,0e5h,036h,000h,023h,071h,023h,07bh,0a9h,091h,077h;
        defb    023h,07ah,089h,0a9h,077h,023h,036h,000h,0e1h,0c9h,0efh,038h,07eh,0a7h,028h,005h;
        defb    0efh,0a2h,00fh,027h,038h,0efh,002h,038h,0e5h,0d5h,0ebh,046h,0cdh,07fh,02dh,0afh;
        defb    090h,0cbh,079h,042h,04bh,07bh,0d1h,0e1h,0c9h,057h,017h,09fh,05fh,04fh,0afh,047h;
        defb    0cdh,0b6h,02ah,0efh,034h,0efh,01ah,020h,09ah,085h,004h,027h,038h,0cdh,0a2h,02dh;
        defb    0d8h,0f5h,005h,004h,028h,003h,0f1h,037h,0c9h,0f1h,0c9h,0efh,031h,036h,000h,00bh;
        defb    031h,037h,000h,00dh,002h,038h,03eh,030h,0d7h,0c9h,02ah,038h,03eh,02dh,0d7h,0efh;
        defb    0a0h,0c3h,0c4h,0c5h,002h,038h,0d9h,0e5h,0d9h,0efh,031h,027h,0c2h,003h,0e2h,001h;
        defb    0c2h,002h,038h,07eh,0a7h,020h,047h,0cdh,07fh,02dh,006h,010h,07ah,0a7h,020h,006h;
        defb    0b3h,028h,009h,053h,006h,008h,0d5h,0d9h,0d1h,0d9h,018h,057h,0efh,0e2h,038h,07eh;
        defb    0d6h,07eh,0cdh,0c1h,02dh,057h,03ah,0ach,05ch,092h,032h,0ach,05ch,07ah,0cdh,04fh;
        defb    02dh,0efh,031h,027h,0c1h,003h,0e1h,038h,0cdh,0d5h,02dh,0e5h,032h,0a1h,05ch,03dh;
        defb    017h,09fh,03ch,021h,0abh,05ch,077h,023h,086h,077h,0e1h,0c3h,0cfh,02eh,0d6h,080h;
        defb    0feh,01ch,038h,013h,0cdh,0c1h,02dh,0d6h,007h,047h,021h,0ach,05ch,086h,077h,078h;
        defb    0edh,044h,0cdh,04fh,02dh,018h,092h,0ebh,0cdh,0bah,02fh,0d9h,0cbh,0fah,07dh,0d9h;
        defb    0d6h,080h,047h,0cbh,023h,0cbh,012h,0d9h,0cbh,013h,0cbh,012h,0d9h,021h,0aah,05ch;
        defb    00eh,005h,07eh,08fh,027h,077h,02bh,00dh,020h,0f8h,010h,0e7h,0afh,021h,0a6h,05ch;
        defb    011h,0a1h,05ch,006h,009h,0edh,06fh,00eh,0ffh,0edh,06fh,020h,004h,00dh,00ch,020h;
        defb    00ah,012h,013h,0fdh,034h,071h,0fdh,034h,072h,00eh,000h,0cbh,040h,028h,001h,023h;
        defb    010h,0e7h,03ah,0abh,05ch,0d6h,009h,038h,00ah,0fdh,035h,071h,03eh,004h,0fdh,0beh;
        defb    06fh,018h,041h,0efh,002h,0e2h,038h,0ebh,0cdh,0bah,02fh,0d9h,03eh,080h,095h,02eh;
        defb    000h,0cbh,0fah,0d9h,0cdh,0ddh,02fh,0fdh,07eh,071h,0feh,008h,038h,006h,0d9h,0cbh;
        defb    012h,0d9h,018h,020h,001h,000h,002h,07bh,0cdh,08bh,02fh,05fh,07ah,0cdh,08bh,02fh;
        defb    057h,0c5h,0d9h,0c1h,010h,0f1h,021h,0a1h,05ch,079h,0fdh,04eh,071h,009h,077h,0fdh;
        defb    034h,071h,018h,0d3h,0f5h,021h,0a1h,05ch,0fdh,04eh,071h,006h,000h,009h,041h,0f1h;
        defb    02bh,07eh,0ceh,000h,077h,0a7h,028h,005h,0feh,00ah,03fh,030h,008h,010h,0f1h,036h;
        defb    001h,004h,0fdh,034h,072h,0fdh,070h,071h,0efh,002h,038h,0d9h,0e1h,0d9h,0edh,04bh;
        defb    0abh,05ch,021h,0a1h,05ch,078h,0feh,009h,038h,004h,0feh,0fch,038h,026h,0a7h,0cch;
        defb    0efh,015h,0afh,090h,0fah,052h,02fh,047h,018h,00ch,079h,0a7h,028h,003h,07eh,023h;
        defb    00dh,0cdh,0efh,015h,010h,0f4h,079h,0a7h,0c8h,004h,03eh,02eh,0d7h,03eh,030h,010h;
        defb    0fbh,041h,018h,0e6h,050h,015h,006h,001h,0cdh,04ah,02fh,03eh,045h,0d7h,04ah,079h;
        defb    0a7h,0f2h,083h,02fh,0edh,044h,04fh,03eh,02dh,018h,002h,03eh,02bh,0d7h,006h,000h;
        defb    0c3h,01bh,01ah,0d5h,06fh,026h,000h,05dh,054h,029h,029h,019h,029h,059h,019h,04ch;
        defb    07dh,0d1h,0c9h,07eh,036h,000h,0a7h,0c8h,023h,0cbh,07eh,0cbh,0feh,02bh,0c8h,0c5h;
        defb    001h,005h,000h,009h,041h,04fh,037h,02bh,07eh,02fh,0ceh,000h,077h,010h,0f8h,079h;
        defb    0c1h,0c9h,0e5h,0f5h,04eh,023h,046h,077h,023h,079h,04eh,0c5h,023h,04eh,023h,046h;
        defb    0ebh,057h,05eh,0d5h,023h,056h,023h,05eh,0d5h,0d9h,0d1h,0e1h,0c1h,0d9h,023h,056h;
        defb    023h,05eh,0f1h,0e1h,0c9h,0a7h,0c8h,0feh,021h,030h,016h,0c5h,047h,0d9h,0cbh,02dh;
        defb    0cbh,01ah,0cbh,01bh,0d9h,0cbh,01ah,0cbh,01bh,010h,0f2h,0c1h,0d0h,0cdh,004h,030h;
        defb    0c0h,0d9h,0afh,02eh,000h,057h,05dh,0d9h,011h,000h,000h,0c9h,01ch,0c0h,014h,0c0h;
        defb    0d9h,01ch,020h,001h,014h,0d9h,0c9h,0ebh,0cdh,06eh,034h,0ebh,01ah,0b6h,020h,026h;
        defb    0d5h,023h,0e5h,023h,05eh,023h,056h,023h,023h,023h,07eh,023h,04eh,023h,046h,0e1h;
        defb    0ebh,009h,0ebh,08eh,00fh,0ceh,000h,020h,00bh,09fh,077h,023h,073h,023h,072h,02bh;
        defb    02bh,02bh,0d1h,0c9h,02bh,0d1h,0cdh,093h,032h,0d9h,0e5h,0d9h,0d5h,0e5h,0cdh,09bh;
        defb    02fh,047h,0ebh,0cdh,09bh,02fh,04fh,0b8h,030h,003h,078h,041h,0ebh,0f5h,090h,0cdh;
        defb    0bah,02fh,0cdh,0ddh,02fh,0f1h,0e1h,077h,0e5h,068h,061h,019h,0d9h,0ebh,0edh,04ah;
        defb    0ebh,07ch,08dh,06fh,01fh,0adh,0d9h,0ebh,0e1h,01fh,030h,008h,03eh,001h,0cdh,0ddh;
        defb    02fh,034h,028h,023h,0d9h,07dh,0e6h,080h,0d9h,023h,077h,02bh,028h,01fh,07bh,0edh;
        defb    044h,03fh,05fh,07ah,02fh,0ceh,000h,057h,0d9h,07bh,02fh,0ceh,000h,05fh,07ah,02fh;
        defb    0ceh,000h,030h,007h,01fh,0d9h,034h,0cah,0adh,031h,0d9h,057h,0d9h,0afh,0c3h,055h;
        defb    031h,0c5h,006h,010h,07ch,04dh,021h,000h,000h,029h,038h,00ah,0cbh,011h,017h,030h;
        defb    003h,019h,038h,002h,010h,0f3h,0c1h,0c9h,0cdh,0e9h,034h,0d8h,023h,0aeh,0cbh,0feh;
        defb    02bh,0c9h,01ah,0b6h,020h,022h,0d5h,0e5h,0d5h,0cdh,07fh,02dh,0ebh,0e3h,041h,0cdh;
        defb    07fh,02dh,078h,0a9h,04fh,0e1h,0cdh,0a9h,030h,0ebh,0e1h,038h,00ah,07ah,0b3h,020h;
        defb    001h,04fh,0cdh,08eh,02dh,0d1h,0c9h,0d1h,0cdh,093h,032h,0afh,0cdh,0c0h,030h,0d8h;
        defb    0d9h,0e5h,0d9h,0d5h,0ebh,0cdh,0c0h,030h,0ebh,038h,05ah,0e5h,0cdh,0bah,02fh,078h;
        defb    0a7h,0edh,062h,0d9h,0e5h,0edh,062h,0d9h,006h,021h,018h,011h,030h,005h,019h,0d9h;
        defb    0edh,05ah,0d9h,0d9h,0cbh,01ch,0cbh,01dh,0d9h,0cbh,01ch,0cbh,01dh,0d9h,0cbh,018h;
        defb    0cbh,019h,0d9h,0cbh,019h,01fh,010h,0e4h,0ebh,0d9h,0ebh,0d9h,0c1h,0e1h,078h,081h;
        defb    020h,001h,0a7h,03dh,03fh,017h,03fh,01fh,0f2h,046h,031h,030h,068h,0a7h,03ch,020h;
        defb    008h,038h,006h,0d9h,0cbh,07ah,0d9h,020h,05ch,077h,0d9h,078h,0d9h,030h,015h,07eh;
        defb    0a7h,03eh,080h,028h,001h,0afh,0d9h,0a2h,0cdh,0fbh,02fh,007h,077h,038h,02eh,023h;
        defb    077h,02bh,018h,029h,006h,020h,0d9h,0cbh,07ah,0d9h,020h,012h,007h,0cbh,013h,0cbh;
        defb    012h,0d9h,0cbh,013h,0cbh,012h,0d9h,035h,028h,0d7h,010h,0eah,018h,0d7h,017h,030h;
        defb    00ch,0cdh,004h,030h,020h,007h,0d9h,016h,080h,0d9h,034h,028h,018h,0e5h,023h,0d9h;
        defb    0d5h,0d9h,0c1h,078h,017h,0cbh,016h,01fh,077h,023h,071h,023h,072h,023h,073h,0e1h;
        defb    0d1h,0d9h,0e1h,0d9h,0c9h,0cfh,005h,0cdh,093h,032h,0ebh,0afh,0cdh,0c0h,030h,038h;
        defb    0f4h,0ebh,0cdh,0c0h,030h,0d8h,0d9h,0e5h,0d9h,0d5h,0e5h,0cdh,0bah,02fh,0d9h,0e5h;
        defb    060h,069h,0d9h,061h,068h,0afh,006h,0dfh,018h,010h,017h,0cbh,011h,0d9h,0cbh,011h;
        defb    0cbh,010h,0d9h,029h,0d9h,0edh,06ah,0d9h,038h,010h,0edh,052h,0d9h,0edh,052h,0d9h;
        defb    030h,00fh,019h,0d9h,0edh,05ah,0d9h,0a7h,018h,008h,0a7h,0edh,052h,0d9h,0edh,052h;
        defb    0d9h,037h,004h,0fah,0d2h,031h,0f5h,028h,0e1h,05fh,051h,0d9h,059h,050h,0f1h,0cbh;
        defb    018h,0f1h,0cbh,018h,0d9h,0c1h,0e1h,078h,091h,0c3h,03dh,031h,07eh,0a7h,0c8h,0feh;
        defb    081h,030h,006h,036h,000h,03eh,020h,018h,051h,0feh,091h,020h,01ah,023h,023h,023h;
        defb    03eh,080h,0a6h,02bh,0b6h,02bh,020h,003h,03eh,080h,0aeh,02bh,020h,036h,077h,023h;
        defb    036h,0ffh,02bh,03eh,018h,018h,033h,030h,02ch,0d5h,02fh,0c6h,091h,023h,056h,023h;
        defb    05eh,02bh,02bh,00eh,000h,0cbh,07ah,028h,001h,00dh,0cbh,0fah,006h,008h,090h,080h;
        defb    038h,004h,05ah,016h,000h,090h,028h,007h,047h,0cbh,03ah,0cbh,01bh,010h,0fah,0cdh;
        defb    08eh,02dh,0d1h,0c9h,07eh,0d6h,0a0h,0f0h,0edh,044h,0d5h,0ebh,02bh,047h,0cbh,038h;
        defb    0cbh,038h,0cbh,038h,028h,005h,036h,000h,02bh,010h,0fbh,0e6h,007h,028h,009h,047h;
        defb    03eh,0ffh,0cbh,027h,010h,0fch,0a6h,077h,0ebh,0d1h,0c9h,0cdh,096h,032h,0ebh,07eh;
        defb    0a7h,0c0h,0d5h,0cdh,07fh,02dh,0afh,023h,077h,02bh,077h,006h,091h,07ah,0a7h,020h;
        defb    008h,0b3h,042h,028h,010h,053h,058h,006h,089h,0ebh,005h,029h,030h,0fch,0cbh,009h;
        defb    0cbh,01ch,0cbh,01dh,0ebh,02bh,073h,02bh,072h,02bh,070h,0d1h,0c9h,000h,0b0h,000h;
        defb    040h,0b0h,000h,001h,030h,000h,0f1h,049h,00fh,0dah,0a2h,040h,0b0h,000h,00ah,08fh;
        defb    036h,03ch,034h,0a1h,033h,00fh,030h,0cah,030h,0afh,031h,051h,038h,01bh,035h,024h;
        defb    035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,014h,030h,02dh;
        defb    035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,03bh,035h,09ch,035h,0deh;
        defb    035h,0bch,034h,045h,036h,06eh,034h,069h,036h,0deh,035h,074h,036h,0b5h,037h,0aah;
        defb    037h,0dah,037h,033h,038h,043h,038h,0e2h,037h,013h,037h,0c4h,036h,0afh,036h,04ah;
        defb    038h,092h,034h,06ah,034h,0ach,034h,0a5h,034h,0b3h,034h,01fh,036h,0c9h,035h,001h;
        defb    035h,0c0h,033h,0a0h,036h,086h,036h,0c6h,033h,07ah,036h,006h,035h,0f9h,034h,09bh;
        defb    036h,083h,037h,014h,032h,0a2h,033h,04fh,02dh,097h,032h,049h,034h,01bh,034h,02dh;
        defb    034h,00fh,034h,0cdh,0bfh,035h,078h,032h,067h,05ch,0d9h,0e3h,0d9h,0edh,053h,065h;
        defb    05ch,0d9h,07eh,023h,0e5h,0a7h,0f2h,080h,033h,057h,0e6h,060h,00fh,00fh,00fh,00fh;
        defb    0c6h,07ch,06fh,07ah,0e6h,01fh,018h,00eh,0feh,018h,030h,008h,0d9h,001h,0fbh,0ffh;
        defb    054h,05dh,009h,0d9h,007h,06fh,011h,0d7h,032h,026h,000h,019h,05eh,023h,056h,021h;
        defb    065h,033h,0e3h,0d5h,0d9h,0edh,04bh,066h,05ch,0c9h,0f1h,03ah,067h,05ch,0d9h,018h;
        defb    0c3h,0d5h,0e5h,001h,005h,000h,0cdh,005h,01fh,0e1h,0d1h,0c9h,0edh,05bh,065h,05ch;
        defb    0cdh,0c0h,033h,0edh,053h,065h,05ch,0c9h,0cdh,0a9h,033h,0edh,0b0h,0c9h,062h,06bh;
        defb    0cdh,0a9h,033h,0d9h,0e5h,0d9h,0e3h,0c5h,07eh,0e6h,0c0h,007h,007h,04fh,00ch,07eh;
        defb    0e6h,03fh,020h,002h,023h,07eh,0c6h,050h,012h,03eh,005h,091h,023h,013h,006h,000h;
        defb    0edh,0b0h,0c1h,0e3h,0d9h,0e1h,0d9h,047h,0afh,005h,0c8h,012h,013h,018h,0fah,0a7h;
        defb    0c8h,0f5h,0d5h,011h,000h,000h,0cdh,0c8h,033h,0d1h,0f1h,03dh,018h,0f2h,04fh,007h;
        defb    007h,081h,04fh,006h,000h,009h,0c9h,0d5h,02ah,068h,05ch,0cdh,006h,034h,0cdh,0c0h;
        defb    033h,0e1h,0c9h,062h,06bh,0d9h,0e5h,021h,0c5h,032h,0d9h,0cdh,0f7h,033h,0cdh,0c8h;
        defb    033h,0d9h,0e1h,0d9h,0c9h,0e5h,0ebh,02ah,068h,05ch,0cdh,006h,034h,0ebh,0cdh,0c0h;
        defb    033h,0ebh,0e1h,0c9h,006h,005h,01ah,04eh,0ebh,012h,071h,023h,013h,010h,0f7h,0ebh;
        defb    0c9h,047h,0cdh,05eh,033h,031h,00fh,0c0h,002h,0a0h,0c2h,031h,0e0h,004h,0e2h,0c1h;
        defb    003h,038h,0cdh,0c6h,033h,0cdh,062h,033h,00fh,001h,0c2h,002h,035h,0eeh,0e1h,003h;
        defb    038h,0c9h,006h,0ffh,018h,006h,0cdh,0e9h,034h,0d8h,006h,000h,07eh,0a7h,028h,00bh;
        defb    023h,078h,0e6h,080h,0b6h,017h,03fh,01fh,077h,02bh,0c9h,0d5h,0e5h,0cdh,07fh,02dh;
        defb    0e1h,078h,0b1h,02fh,04fh,0cdh,08eh,02dh,0d1h,0c9h,0cdh,0e9h,034h,0d8h,0d5h,011h;
        defb    001h,000h,023h,0cbh,016h,02bh,09fh,04fh,0cdh,08eh,02dh,0d1h,0c9h,0cdh,099h,01eh;
        defb    0edh,078h,018h,004h,0cdh,099h,01eh,00ah,0c3h,028h,02dh,0cdh,099h,01eh,021h,02bh;
        defb    02dh,0e5h,0c5h,0c9h,0cdh,0f1h,02bh,00bh,078h,0b1h,020h,023h,01ah,0cdh,08dh,02ch;
        defb    038h,009h,0d6h,090h,038h,019h,0feh,015h,030h,015h,03ch,03dh,087h,087h,087h,0feh;
        defb    0a8h,030h,00ch,0edh,04bh,07bh,05ch,081h,04fh,030h,001h,004h,0c3h,02bh,02dh,0cfh;
        defb    009h,0e5h,0c5h,047h,07eh,023h,0b6h,023h,0b6h,023h,0b6h,078h,0c1h,0e1h,0c0h,037h;
        defb    0c9h,0cdh,0e9h,034h,0d8h,03eh,0ffh,018h,006h,0cdh,0e9h,034h,018h,005h,0afh,023h;
        defb    0aeh,02bh,007h,0e5h,03eh,000h,077h,023h,077h,023h,017h,077h,01fh,023h,077h,023h;
        defb    077h,0e1h,0c9h,0ebh,0cdh,0e9h,034h,0ebh,0d8h,037h,018h,0e7h,0ebh,0cdh,0e9h,034h;
        defb    0ebh,0d0h,0a7h,018h,0deh,0ebh,0cdh,0e9h,034h,0ebh,0d0h,0d5h,01bh,0afh,012h,01bh;
        defb    012h,0d1h,0c9h,078h,0d6h,008h,0cbh,057h,020h,001h,03dh,00fh,030h,008h,0f5h,0e5h;
        defb    0cdh,03ch,034h,0d1h,0ebh,0f1h,0cbh,057h,020h,007h,00fh,0f5h,0cdh,00fh,030h,018h;
        defb    033h,00fh,0f5h,0cdh,0f1h,02bh,0d5h,0c5h,0cdh,0f1h,02bh,0e1h,07ch,0b5h,0e3h,078h;
        defb    020h,00bh,0b1h,0c1h,028h,004h,0f1h,03fh,018h,016h,0f1h,018h,013h,0b1h,028h,00dh;
        defb    01ah,096h,038h,009h,020h,0edh,00bh,013h,023h,0e3h,02bh,018h,0dfh,0c1h,0f1h,0a7h;
        defb    0f5h,0efh,0a0h,038h,0f1h,0f5h,0dch,001h,035h,0f1h,0f5h,0d4h,0f9h,034h,0f1h,00fh;
        defb    0d4h,001h,035h,0c9h,0cdh,0f1h,02bh,0d5h,0c5h,0cdh,0f1h,02bh,0e1h,0e5h,0d5h,0c5h;
        defb    009h,044h,04dh,0f7h,0cdh,0b2h,02ah,0c1h,0e1h,078h,0b1h,028h,002h,0edh,0b0h,0c1h;
        defb    0e1h,078h,0b1h,028h,002h,0edh,0b0h,02ah,065h,05ch,011h,0fbh,0ffh,0e5h,019h,0d1h;
        defb    0c9h,0cdh,0d5h,02dh,038h,00eh,020h,00ch,0f5h,001h,001h,000h,0f7h,0f1h,012h,0cdh;
        defb    0b2h,02ah,0ebh,0c9h,0cfh,00ah,02ah,05dh,05ch,0e5h,078h,0c6h,0e3h,09fh,0f5h,0cdh;
        defb    0f1h,02bh,0d5h,003h,0f7h,0e1h,0edh,053h,05dh,05ch,0d5h,0edh,0b0h,0ebh,02bh,036h;
        defb    00dh,0fdh,0cbh,001h,0beh,0cdh,0fbh,024h,0dfh,0feh,00dh,020h,007h,0e1h,0f1h,0fdh;
        defb    0aeh,001h,0e6h,040h,0c2h,08ah,01ch,022h,05dh,05ch,0fdh,0cbh,001h,0feh,0cdh,0fbh;
        defb    024h,0e1h,022h,05dh,05ch,018h,0a0h,001h,001h,000h,0f7h,022h,05bh,05ch,0e5h,02ah;
        defb    051h,05ch,0e5h,03eh,0ffh,0cdh,001h,016h,0cdh,0e3h,02dh,0e1h,0cdh,015h,016h,0d1h;
        defb    02ah,05bh,05ch,0a7h,0edh,052h,044h,04dh,0cdh,0b2h,02ah,0ebh,0c9h,0cdh,094h,01eh;
        defb    0feh,010h,0d2h,09fh,01eh,02ah,051h,05ch,0e5h,0cdh,001h,016h,0cdh,0e6h,015h,001h;
        defb    000h,000h,030h,003h,00ch,0f7h,012h,0cdh,0b2h,02ah,0e1h,0cdh,015h,016h,0c3h,0bfh;
        defb    035h,0cdh,0f1h,02bh,078h,0b1h,028h,001h,01ah,0c3h,028h,02dh,0cdh,0f1h,02bh,0c3h;
        defb    02bh,02dh,0d9h,0e5h,021h,067h,05ch,035h,0e1h,020h,004h,023h,0d9h,0c9h,0d9h,05eh;
        defb    07bh,017h,09fh,057h,019h,0d9h,0c9h,013h,013h,01ah,01bh,01bh,0a7h,020h,0efh,0d9h;
        defb    023h,0d9h,0c9h,0f1h,0d9h,0e3h,0d9h,0c9h,0efh,0c0h,002h,031h,0e0h,005h,027h,0e0h;
        defb    001h,0c0h,004h,003h,0e0h,038h,0c9h,0efh,031h,036h,000h,004h,03ah,038h,0c9h,031h;
        defb    03ah,0c0h,003h,0e0h,001h,030h,000h,003h,0a1h,003h,038h,0c9h,0efh,03dh,034h,0f1h;
        defb    038h,0aah,03bh,029h,004h,031h,027h,0c3h,003h,031h,00fh,0a1h,003h,088h,013h,036h;
        defb    058h,065h,066h,09dh,078h,065h,040h,0a2h,060h,032h,0c9h,0e7h,021h,0f7h,0afh,024h;
        defb    0ebh,02fh,0b0h,0b0h,014h,0eeh,07eh,0bbh,094h,058h,0f1h,03ah,07eh,0f8h,0cfh,0e3h;
        defb    038h,0cdh,0d5h,02dh,020h,007h,038h,003h,086h,030h,009h,0cfh,005h,038h,007h,096h;
        defb    030h,004h,0edh,044h,077h,0c9h,0efh,002h,0a0h,038h,0c9h,0efh,03dh,031h,037h,000h;
        defb    004h,038h,0cfh,009h,0a0h,002h,038h,07eh,036h,080h,0cdh,028h,02dh,0efh,034h,038h;
        defb    000h,003h,001h,031h,034h,0f0h,04ch,0cch,0cch,0cdh,003h,037h,000h,008h,001h,0a1h;
        defb    003h,001h,038h,034h,0efh,001h,034h,0f0h,031h,072h,017h,0f8h,004h,001h,0a2h,003h;
        defb    0a2h,003h,031h,034h,032h,020h,004h,0a2h,003h,08ch,011h,0ach,014h,009h,056h,0dah;
        defb    0a5h,059h,030h,0c5h,05ch,090h,0aah,09eh,070h,06fh,061h,0a1h,0cbh,0dah,096h,0a4h;
        defb    031h,09fh,0b4h,0e7h,0a0h,0feh,05ch,0fch,0eah,01bh,043h,0cah,036h,0edh,0a7h,09ch;
        defb    07eh,05eh,0f0h,06eh,023h,080h,093h,004h,00fh,038h,0c9h,0efh,03dh,034h,0eeh,022h;
        defb    0f9h,083h,06eh,004h,031h,0a2h,00fh,027h,003h,031h,00fh,031h,00fh,031h,02ah,0a1h;
        defb    003h,031h,037h,0c0h,000h,004h,002h,038h,0c9h,0a1h,003h,001h,036h,000h,002h,01bh;
        defb    038h,0c9h,0efh,039h,02ah,0a1h,003h,0e0h,000h,006h,01bh,033h,003h,0efh,039h,031h;
        defb    031h,004h,031h,00fh,0a1h,003h,086h,014h,0e6h,05ch,01fh,00bh,0a3h,08fh,038h,0eeh;
        defb    0e9h,015h,063h,0bbh,023h,0eeh,092h,00dh,0cdh,0edh,0f1h,023h,05dh,01bh,0eah,004h;
        defb    038h,0c9h,0efh,031h,01fh,001h,020h,005h,038h,0c9h,0cdh,097h,032h,07eh,0feh,081h;
        defb    038h,00eh,0efh,0a1h,01bh,001h,005h,031h,036h,0a3h,001h,000h,006h,01bh,033h,003h;
        defb    0efh,0a0h,001h,031h,031h,004h,031h,00fh,0a1h,003h,08ch,010h,0b2h,013h,00eh,055h;
        defb    0e4h,08dh,058h,039h,0bch,05bh,098h,0fdh,09eh,000h,036h,075h,0a0h,0dbh,0e8h,0b4h;
        defb    063h,042h,0c4h,0e6h,0b5h,009h,036h,0beh,0e9h,036h,073h,01bh,05dh,0ech,0d8h,0deh;
        defb    063h,0beh,0f0h,061h,0a1h,0b3h,00ch,004h,00fh,038h,0c9h,0efh,031h,031h,004h,0a1h;
        defb    003h,01bh,028h,0a1h,00fh,005h,024h,031h,00fh,038h,0c9h,0efh,022h,0a3h,003h,01bh;
        defb    038h,0c9h,0efh,031h,030h,000h,01eh,0a2h,038h,0efh,001h,031h,030h,000h,007h,025h;
        defb    004h,038h,0c3h,0c4h,036h,002h,031h,030h,000h,009h,0a0h,001h,037h,000h,006h,0a1h;
        defb    001h,005h,002h,0a1h,038h,0c9h;

Characters:
        defb                                   010h,010h,010h,010h,000h,010h;
        defb    024h,024h,000h,000h,000h,000h, 024h,07eh,024h,024h,07eh,024h;
        defb    008h,03eh,028h,03eh,00ah,03eh, 062h,064h,008h,010h,026h,046h;
        defb    010h,028h,010h,02ah,044h,03ah, 008h,010h,000h,000h,000h,000h;
        defb    004h,008h,008h,008h,008h,004h, 020h,010h,010h,010h,010h,020h;
        defb    000h,014h,008h,03eh,008h,014h, 000h,008h,008h,03eh,008h,008h;
        defb    000h,000h,000h,000h,008h,008h, 000h,000h,000h,03eh,000h,000h;
        defb    000h,000h,000h,000h,018h,018h, 000h,002h,004h,008h,010h,020h;
        defb    03ch,046h,04ah,052h,062h,03ch, 018h,028h,008h,008h,008h,03eh;
        defb    03ch,042h,002h,03ch,040h,07eh, 03ch,042h,00ch,002h,042h,03ch;
        defb    008h,018h,028h,048h,07eh,008h, 07eh,040h,07ch,002h,042h,03ch;
        defb    03ch,040h,07ch,042h,042h,03ch, 07eh,002h,004h,008h,010h,010h;
        defb    03ch,042h,03ch,042h,042h,03ch, 03ch,042h,042h,03eh,002h,03ch;
        defb    000h,000h,010h,000h,000h,010h, 000h,010h,000h,000h,010h,010h;
        defb    000h,004h,008h,010h,008h,004h, 000h,000h,03eh,000h,03eh,000h;
        defb    000h,010h,008h,004h,008h,010h, 03ch,042h,004h,008h,000h,008h;
        defb    03ch,04ah,056h,05eh,040h,03ch, 03ch,042h,042h,07eh,042h,042h;
        defb    07ch,042h,07ch,042h,042h,07ch, 03ch,042h,040h,040h,042h,03ch;
        defb    078h,044h,042h,042h,044h,078h, 07eh,040h,07ch,040h,040h,07eh;
        defb    07eh,040h,07ch,040h,040h,040h, 03ch,042h,040h,04eh,042h,03ch;
        defb    042h,042h,07eh,042h,042h,042h, 03eh,008h,008h,008h,008h,03eh;
        defb    002h,002h,002h,042h,042h,03ch, 044h,048h,070h,048h,044h,042h;
        defb    040h,040h,040h,040h,040h,07eh, 042h,066h,05ah,042h,042h,042h;
        defb    042h,062h,052h,04ah,046h,042h, 03ch,042h,042h,042h,042h,03ch;
        defb    07ch,042h,042h,07ch,040h,040h, 03ch,042h,042h,052h,04ah,03ch;
        defb    07ch,042h,042h,07ch,044h,042h, 03ch,040h,03ch,002h,042h,03ch;
        defb    0feh,010h,010h,010h,010h,010h, 042h,042h,042h,042h,042h,03ch;
        defb    042h,042h,042h,042h,024h,018h, 042h,042h,042h,042h,05ah,024h;
        defb    042h,024h,018h,018h,024h,042h, 082h,044h,028h,010h,010h,010h;
        defb    07eh,004h,008h,010h,020h,07eh, 00eh,008h,008h,008h,008h,00eh;
        defb    000h,040h,020h,010h,008h,004h, 070h,010h,010h,010h,010h,070h;
        defb    010h,038h,054h,010h,010h,010h, 000h,000h,000h,000h,000h,000h;
        defb    01ch,022h,078h,020h,020h,07eh, 000h,038h,004h,03ch,044h,03ch;
        defb    020h,020h,03ch,022h,022h,03ch, 000h,01ch,020h,020h,020h,01ch;
        defb    004h,004h,03ch,044h,044h,03ch, 000h,038h,044h,078h,040h,03ch;
        defb    00ch,010h,018h,010h,010h,010h, 000h,03ch,044h,044h,03ch,004h;
        defb    040h,040h,078h,044h,044h,044h, 010h,000h,030h,010h,010h,038h;
        defb    004h,000h,004h,004h,004h,024h, 020h,028h,030h,030h,028h,024h;
        defb    010h,010h,010h,010h,010h,00ch, 000h,068h,054h,054h,054h,054h;
        defb    000h,078h,044h,044h,044h,044h, 000h,038h,044h,044h,044h,038h;
        defb    000h,078h,044h,044h,078h,040h, 000h,03ch,044h,044h,03ch,004h;
        defb    000h,01ch,020h,020h,020h,020h, 000h,038h,040h,038h,004h,078h;
        defb    010h,038h,010h,010h,010h,00ch, 000h,044h,044h,044h,044h,038h;
        defb    000h,044h,044h,028h,028h,010h, 000h,044h,054h,054h,054h,028h;
        defb    000h,044h,028h,010h,028h,044h, 000h,044h,044h,044h,03ch,004h;
        defb    000h,07ch,008h,010h,020h,07ch, 00eh,008h,030h,008h,008h,00eh;
        defb    008h,008h,008h,008h,008h,008h, 070h,010h,00ch,010h,010h,070h;
        defb    014h,028h,000h,000h,000h,000h, 042h,099h,0a1h,0a1h,099h,042h;

        end

