# Test ROMs

These original Sinclair ROMs are bundled so ZxKit's tests and fixture audit
are self-contained. `48.rom` serves the 48K tape and timing tests;
`128-0.rom` and `128-1.rom` serve the 128K timing tests. The +2 and +3 ROM
sets support manual runs of the fixtures grouped for those machines.

They were copied byte for byte from the existing Spectacol ROM collection.
See [README.copyright](README.copyright) for the original distribution notice;
these ROMs are not relicensed under ZxKit's GPL. [sha256.json](sha256.json)
records their exact hashes.

No Spectacol checkout is required. The `zxrun` runner and collection audit use
this directory by default; explicit `--roms DIR` arguments take priority.
