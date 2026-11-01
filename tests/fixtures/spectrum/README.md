# Spectrum hardware regression fixtures

Source: [redcode's test collection](https://github.com/redcode/ZXSpectrum/wiki/Tests),
local download inspected on 2026-11-01. Original filenames, author credits and
archive contents are retained. These are third-party test programs; their
original notices/licenses apply. ZxKit's GPL does not relicense the fixtures.

`selected.json` lists the copied fixtures. The selection emphasizes the wiki's
recommended timing/contention/floating-bus suites, CPU diagnostics, paging,
interrupts, and the specific programs reported broken in Spectacol.

| Directory | Target |
| --- | --- |
| `48k` | PAL 48K, including NEC contention and both Butler TAP loaders |
| `128k-plus2` | Sinclair 128K / grey +2, including floating bus and shadow screen |
| `plus2a-plus3` | Amstrad +2A/+3 ASIC, paging and screen switching |
| `common` | Shared CPU tests and programs supporting several models |

The full inventory also has a `pentagon` group. No Pentagon-specific fixture was
selected for this first repair. Inventory groups are inferred primary targets
from filenames, not a declaration that a program supports every model in a
family. Revisions, alternate loaders, source archives, audio recordings and
reference screenshots are not separate hardware assertions.

## Automated coverage

Build normally, then run `ctest --test-dir build --output-on-failure`.

* `spectrum_timing_boundaries`: no external ROMs needed. All 192 contention rows,
  start/end boundaries and eight delay phases on 48K, 128K, +2, +2A and +3;
  adjacent floating-bus bitmap/attribute fetches; interrupt expiry and retrigger.
* `spectrum_48k_suites`: Butler snapshot tests 1–35, separately selected through
  the keyboard. Each must identify its requested test and print the complete
  success message. Also loads the original 2010 TAP and 2025 Sane Loader TAP and
  runs test 3 through each, plus Woodmass's Interrupt Retriggering TAP, which
  must print `Your emulator is good`.
* `spectrum_128k_suites`: 29 passing Butler cases. Five known failures are
  explicitly excluded: **4, 17, 18, 26, 33**, all in their contended halves.
  Each included case must print two passes and reach the suite's BASIC STOP.

The integration checks use ZxKit's bundled `tests/fixtures/roms` directory
(with original notices and SHA-256 hashes). The 48K/128K ROMs serve unit tests;
the +2/+3 sets also support the bundled fixture audit.
Missing required ROM fixtures fail configuration or testing rather than silently
skipping. Verdicts are decoded from display RAM using the ROM font, independently
of the host renderer.

To execute **every** Butler case, including known failures, and print evidence:

```sh
build/tests/test_spectrum_timing tests/fixtures/spectrum \
  tests/fixtures/roms --audit
```

Audit mode intentionally reports results without asserting success. The 48K
suite has 35 tests, the 128K suite has 34. Keys are released long enough for the
ROM debounce logic to recognize repeated digits (11, 22, 33).

## Raster regressions

`spectrum_ula_raster` tests horizontal border changes, attribute fetch latches,
shadow paging between bitmap and attribute fetch, and fresh frame timelines.
`spectrum_48k_basic_border` boots the original BASIC Border snapshot and compares
all 81,920 pixels against `reference/basic-border-colours.bin`, derived from the
collection's original GIF. Palette brightness and the different border crop
are normalized; colour and position must match exactly.

The incremental raster applies to Sinclair 16K/48K/128K/+2/+2A/+3/+3e. Other
machine renderers retain their existing behavior. Video memory writes and I/O
writes draw the beam up to the mutation before applying it. This also tracks
writes through paged memory aliases rather than assuming screen RAM always
lives at CPU address 0x4000.

## Collection audit and limitations

`collection-inventory.json` records all **320 top-level files**, SHA-256, size,
ZIP CRC checks, TAR indexes and archive member names. No archive paths are
extracted onto the host. `reports/runtime-smoke.json` records the initial
**388 media smoke runs**, including archive members. **385 completed**, one
Tape Edge member was rejected as truncated, and two rejected files were macOS
AppleDouble metadata rather than TAPs. The reusable script now filters those
metadata files. Screenshots from that initial development run are not bundled;
regenerate them using the audit script.

These smoke runs are **not hardware passes**: a program can be loading,
waiting for input, or displaying an incorrect raster without returning an
error. Audio-only releases, source-only archives, hardware-dependent tests and
interactive diagnostics still require their own adapters or manual review.
The reports precede the final interrupt fixes; the CTest and Butler verdicts
above describe the final code. `reports/butler-verdicts.json` contains the final
69-case audit: 64 passes and the five listed 128K failures.

Audit the bundled selection with no external files:

```sh
python3 tests/tools/audit_spectrum_collection.py
```

The default runner is `build/tests/zxrun`, ROMs are in `tests/fixtures/roms`, and
output goes to `build/spectrum-audit`. The script inventories `selected.json`,
checks archives and runs their media members. The bundled ROM sets cover every
machine group in the selected fixtures.

To inspect a separate full collection, supply it explicitly (other settings can
also be overridden with `--runner`, `--roms` and `--output`):

```sh
python3 tests/tools/audit_spectrum_collection.py --collection /path/to/collection
```

Pentagon tests in an external collection need their model ROMs supplied through
`--roms`. The audit reports missing ROMs explicitly. It does not count a blank
ROM execution as a successful smoke run.

Remaining work, in priority order:

1. Resolve the five 128K contention failures against real hardware/reference
   traces. Uncontended cases now pass after anchoring the interrupt pulse to
   the frame edge rather than the instruction's overshoot.
2. Finish the remaining 128K ULA phase details. Sinclair machines now render
   within scanlines: border changes, independent bitmap/attribute latches and
   shadow-screen switching follow the emulated timeline. BASIC Border matches
   the collection reference at every cropped pixel. ULA 128 Timing is much
   closer, but its reference comparison still finds one-clock edge differences;
   it is not yet an exact hardware-image pass.
3. Add verdict adapters for NEC contention, Rak's CPU tests and +2A/+3 paging;
   these are preserved fixtures, not yet automated pass/fail checks.
4. Implement the +2A/+3 floating-bus peculiarities and snow/refresh behavior;
   the generic Sinclair floating-bus correction does not cover these.
5. Restore snapshot sub-frame timing and CPU hidden state fully. A successful
   media load does not prove snapshot fidelity.
