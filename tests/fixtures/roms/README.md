# Test ROMs

These ROMs are bundled so ZxKit's tests and fixture audit are self-contained.
`48.rom` serves the 48K tape and timing tests; `128-0.rom` and `128-1.rom`
serve the 128K timing tests. The +2 and +3 ROM sets support manual runs of the
fixtures grouped for those machines. `test_timex` boots the Timex
(`tc2048.rom`, `tc2068-0.rom`, `tc2068-1.rom`) and Spectrum SE (`se-0.rom`,
`se-1.rom`) ROMs; `test_cobra` boots the CoBra 1 (`cobra1_*.rom`) and CoBra 2
(`cobra2_ts_boot.rom`) ones, the CoBra 1 also from the CP/M system disk in
[`../cobra`](../cobra).

They were copied byte for byte from the existing Spectacol ROM collection,
the CoBra ones from their official sites (see the notices).
See [README.copyright](README.copyright) for the original distribution notices;
these ROMs are not relicensed under ZxKit's GPL. [sha256.json](sha256.json)
records their exact hashes.

No Spectacol checkout is required. The `zxrun` runner and collection audit use
this directory by default; explicit `--roms DIR` arguments take priority.
