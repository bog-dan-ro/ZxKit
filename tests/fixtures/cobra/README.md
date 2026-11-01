# CoBra 1 test disk

`KRYS80_CL_SYSDISK.img` is a CP/M 2.2 system disk for the 80K CoBra 1 (the KRYS80
system with the CoBra 53K BIOS and 80 characters per line), a raw 720K image
(80 cylinders, 2 sides, 9 sectors of 512 bytes). It was downloaded byte for byte
from the CoBra Project, which publishes it for the computer and its emulators:

https://cobrasov.com/CoBra%20Project/Software/CPM/CPM%20KRYSS%2080%20chr_line%20analysis/Modification%20for%20all%20drives%20DD/KRYS80_CL_SYSDISK.img

(the "CP/M 80 chr/line" page, https://cobrasov.com/CoBra%20Project/cpm80.html).
CP/M 2.2 is copyright Digital Research, released for free use by its successors.

`test_cobra` boots it from the 80K standard boot (Enter, then D) and saves a file on it.

SHA-256: `d4055063f2dd67a297c9ec9b6c0ae6ded93eaee224a1899f3d10a03b2fe12cce`.

The test modifies an in-memory copy; the fixture file is never written.

## DEVIL utility disk

`UTILS1.TD0` is the unmodified Teledisk image from Cristian Savu's CoBra Project:
https://cobrasov.com/CoBra%20Project/Software/CoBra.sys/3.5%22/DEVIL/Teledisk%20Images/UTILS1.TD0
The format and collection are described at https://cobrasov.com/CoBra%20Project/devil.html.
The original authors retain copyright in the included utilities; ZxKit's GPL does
not relicense them. The image retains all 82 recorded cylinders (including the
extra two), two heads and 18 sectors of 256 bytes per head. `test_teledisk` checks
its sectors and `test_cobra` boots the official DEVIL ROM and verifies its CAT output.

SHA-256: `a684d7e1a22c9e40c2a9fd57e170c1997838601798594bb173adc74e5f0ddb01`.

## Original 64K boot loader

`test_cobra` builds diagnostic disks in memory for both formats understood by the
original 64K boot: 26 × 128-byte FM sectors and 10 × 512-byte MFM sectors, with the
ROM's sector interleave tables. Its original SYS.COM directory search, allocation
block reads and transition to CP/M memory configuration must execute a diagnostic
transient at 0100. This covers the 64K loader and hardware without substituting an
80K system disk. A complete historical 64K CP/M system remains unverified: no
preserved SYS.COM system has been located in the CoBra Project or local collection.
