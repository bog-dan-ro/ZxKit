# RZX interoperability fixtures

These small diagnostics are authored for ZxKit (Copyright 2026 BogDan Vatra,
GPL-3.0-or-later). They contain no game images or Spectrum ROM data. Required ROMs
for the standalone test are already in `../roms`; no application, external player,
downloaded game collection or network access is required to run the tests.

`generate.py` builds the RZX bytes directly from the documented 0.13 layout,
independently of ZxKit's writer. `reference.json` records CPU and memory results
from **SkoolKit 10.1's independent Z80/RZX player**, plus fixture SHA-256 values.
This is not just a ZxKit self-recorded round trip.

- `fetches.rzx`: v3 48K Z80 snapshot, initial T-state offset, ignored DD prefixes,
  DD-CB operations, `LD R,A`, distinct fetch budgets and logged input values.
- `sequence.rzx`: SNA then Z80 snapshots, multiple input blocks/positions, raw
  and compressed data, and repeated input records. The later snapshot deliberately
  replaces earlier RAM and moves the input destination.
- `interrupts.rzx`: IM2, HALT, acknowledge-cycle exclusion and a short EI/RET
  retrigger record. The final boundary's interrupt is included in the reference.
- `ei-boundary.rzx`: EI at an interrupt boundary followed by a one-fetch interval
  containing an IN. The next short frame must defer INT for that instruction.

To regenerate the fixtures, run `python3 tests/fixtures/rzx/generate.py` from the
repository root (its output path also works independently of the working directory).
For independent development verification, with SkoolKit installed separately:

```sh
rzxplay.py --quiet --no-screen --fps 0 tests/fixtures/rzx/fetches.rzx build/fetches.z80
rzxplay.py --quiet --no-screen --fps 0 tests/fixtures/rzx/sequence.rzx build/sequence.z80
rzxplay.py --quiet --no-screen --fps 0 tests/fixtures/rzx/interrupts.rzx build/interrupts.z80
rzxplay.py --quiet --no-screen --fps 0 --flags 2 tests/fixtures/rzx/ei-boundary.rzx build/ei.z80
```

Flag 2 selects SkoolKit's documented short-frame EI convention for the last
fixture. It corresponds to the format's interrupt-retrigger guidance, rather
than a creator-specific recovery heuristic. The test expectations are fixed;
SkoolKit is a development reference, not a ZxKit build/test dependency.

Primary format specification: https://worldofspectrum.net/RZXformat.html
Independent player: https://github.com/skoolkid/skoolkit (GPL-3.0-or-later).
