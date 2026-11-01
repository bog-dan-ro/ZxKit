
                        org   #8000
                        ds    257, #81

                        org   #8181
                        inc   c
                        ei
                        ei
                        ld    a,r
                        jp    (iy)

syncint                 ld    hl,0
                        ld    de,0
                        ld    bc,3324 ;5
                        ei
                        ldir
                        dec   hl
                        di

                        ld    iy,mainint

                        ld    a,0   ; for int sync
                        ei
                        halt

                        ; 48 cycles into frame on entry; C = 1
mainint                 ld    iy,23610
                        ld    a,63
                        ld    i,a
                        im    1
      oldsp             ld    sp,0

                        ld    de,good.txt
                        dec   c
                        jr    z,prtres
                        ld    de,crap.txt
      prtres            call  Print_String
                        ei
                        ret

Print_String            ld    a,(de)
                        inc   de
                        cp    255
                        ret   z
                        rst   16
                        jr    Print_String

youremuis.txt           db    "Your emulator is ", 255
good.txt                db    "good", 255
crap.txt                db    "crap", 255

start                   call  3435
                        ld    a,2
                        call  #1601

                        ld    de,youremuis.txt
                        call  Print_String

                        di
                        ld    (oldsp+1),sp
                        ld    sp,#8181

                        ld    a,#80
                        ld    i,a
                        im    2
                        ld    iy,syncint
                        ei
                        halt

                        end   start

