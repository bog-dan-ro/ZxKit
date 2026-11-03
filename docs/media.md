# Media formats and host boot actions

SZX is the default snapshot format. `Emulator::saveMedia()` and `saveSnapshot()` accept an extension,
with or without a leading dot; the host writes the resulting bytes after releasing its emulator lock.
Loading an interchange snapshot selects the machine declared by its header.

| Format | Reading | Writing / limitations |
|---|---|---|
| TAP | Standard ROM blocks | Standard ROM blocks only; descriptive metadata is omitted. Other signal/control blocks raise an error; use TZX. |
| TZX | Existing block/control model | Standard, turbo, pure data, tones/pulses, direct/CSW/generalized recordings, loops/jumps/calls/selections and metadata. Original descriptive bodies survive export. |
| PZX 1.x | PULS, DATA, PAUS, STOP, browse/header information; unknown optional tags are skipped | Export through TZX, including initial polarity and zero-duration edges. PZX pulse repetitions and packed DATA remain compact during playback. |
| CSW 1.x / 2.x | RLE and zlib-RLE, initial level, cumulative sample-rate conversion | Export through TZX CSW blocks. Variable compressed payloads are bounded to 64 MiB; invalid/truncated input is rejected. |
| SNA | 48K / 128K | 48K, 128K/+2/Pentagon. 48K needs two writable stack bytes for PC and modifies those bytes in the exported image only. 128K preserves PC separately and writes all banks, including duplicate top bank 2/5 cases. AY state and richer model state need SZX. |
| Z80 | v1 / v2 / v3 | v3 with uncompressed 16K pages, classic machine type, paging, AY registers and frame position. Supports 48K, 128K, +2/+2A/+3/+3e and Pentagon 128. +3e uses the +3 interchange type. |
| SZX | ZX-State chunks | Default. Classic machine identity, CPU/register latches, RAM, AY, paging and frame position. Next uses the ZxKit extension described below. |
| NEX | v1.0–v1.2 | v1.2 **program export**, Next only: RAM banks, entry bank, PC/SP and border. NEX is not a full CPU/device snapshot; software must initialize other registers/devices. Export requires bank 5 at 4000, bank 2 at 8000 and an aligned 16K entry bank at C000. |

TZX export remaps relative control targets if conversion inserts extra level blocks. It has explicit field,
step and 64 MiB output limits rather than truncating an unrepresentable tape. Original unsupported TZX
signal blocks cannot be reconstructed after parsing; an unsupported block remains a descriptive placeholder.

## Joystick preferences

`SnapshotFormats::joystickInfo()` reports whether a Z80/SZX file declares host input preferences and,
when unambiguous, which `JoystickType` it requests. It handles Z80 v1/v2 joystick flags, v3 programmable
mappings matching Cursor or Sinclair 1/2, SZX `JOY` and `KEYB` blocks, and explicit disabled input.
Conflicting or unsupported declarations return a present preference without a selected type. SNA and NEX
have no such metadata. A zero Z80 joystick flag is a real Cursor declaration, not an absent field.

`setJoystickInfo()` writes preferences without changing CPU/device state. SZX supports all the common
interfaces; Z80 supports Cursor, Kempston and Sinclair 1/2. Unsupported exports return false unchanged.
A core-generated Z80 initially has an unassigned programmable mapping; consumers can supply a supported
preference. Loading machine state does not itself configure the consumer's controller or prompt UI.

## Next state in SZX

Standard SZX 1.5 has no machine ID or device chunks for the Spectrum Next. ZxKit adds a `ZKNX` block,
following the classic 128K base/preview chunks. A ZxKit reader recognizes it before choosing the machine;
other readers can ignore it, but the classic fallback is **not a playable full Next snapshot**.

The block body is little-endian: `uint32 schema` (currently 1), `uint16 scanline`, two byte booleans
(frame started / line started), `uint32 peripheral count`, that many length-prefixed UTF-8 peripheral IDs
(`uint32 byte length`), then a zlib stream containing ZxKit's native state. This includes all Next RAM,
MMU, CPU/clock, video/palettes/sprites, sound and built-in device state. The native schema is versioned;
it must change when the native serialized layout changes. It is intended for ZxKit consumers with the
same attached peripheral configuration, not as a portable Next interchange standard.

External SD/disk contents and inserted tape media are not embedded or rolled back by a snapshot. Preserve
the associated media. Compressed native state is bounded to 32 MiB; malformed memory mappings and CPU speeds
are rejected, and a failed native restore rolls back the current machine state.

## Explicit boot actions

```cpp
emulator.loadTape(bytes, ".pzx");
if (!emulator.autoLoadTape(selectedBlock)) {
    // This machine has no supported ROM auto-load sequence; let the user load manually.
}

emulator.insertDisk(0, bytes, ".dsk");
const bool bootScheduled = emulator.autoBootDisk();
```

`autoLoadTape()` resets a classic Sinclair machine and seeks to the selected block before scheduling
`LOAD ""` (48K) or the ROM Loader (128K). It never executes frames on the caller's thread. `autoBootDisk()`
acts on drive A only: TR-DOS machines reset with their ROM paged in; +3/+3e reset and schedule Enter on Loader.
The existing optional TR-DOS boot-file helper operates on the in-memory disk image. Unsupported machines or
an empty drive return false. Reset/boot is rejected during an active RZX recording/edit.

The host continues normal Engine execution to advance the scheduled keys. Pause therefore also pauses
automatic typing. An explicit reset or machine/snapshot replacement cancels it. Normal tape block selection
only seeks; resetting and loading is a separate user action. Spectacol applies these boot actions when its
Auto load setting is enabled and offers reset/load in its tape browser.

## Disk timing model

The controllers remain sector based. They model seek/head movement and rate selection, uniform 300 rpm
rotation, delayed first data, per-byte readiness, result CRC delay, and missed-transfer errors. uPD765
SPECIFY programs step/head load/unload times; MFM bytes take 32 us and FM bytes 64 us. Its existing four-byte
overrun allowance is retained for the scanline-driven CoBra interrupt wiring. WD1793 uses the 1 MHz step-rate
table and a one-byte DRQ deadline. Motor spin-up, physical flux, arbitrary gap layouts and exact protections
require a richer disk representation; these tests do not claim that coverage.

## Specifications and verification

- [World of Spectrum format reference](https://worldofspectrum.org/faq/reference/formats.htm)
- [Z80 snapshot specification](https://worldofspectrum.org/faq/reference/z80format.htm)
- [SZX 1.5 reference](https://www.spectaculator.com/docs/zx-state/intro.html)
- [TZX specification](https://worldofspectrum.net/zx-modules/fileformats/tzxformat.html)
- [PZX specification and original author's tools](https://github.com/raxoft/pzxtools/blob/master/docs/pzx_format.txt)
- [CSW specification](https://acorn.huininga.nl/pub/unsorted/software/pc/CSW/csw.html)
- [NEX specification](https://wiki.specnext.dev/NEX_file_format)
- [Western Digital FD179X data sheet](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/Western%20Digital%20Floppy%20Controller%20-%20WD179X.pdf)
- [NEC uPD765 data sheet](https://datasheets.pl/elementy_czynne/IC/UPD/UPD765-2.pdf)

`test_tape`, `test_storage_next`, `test_disks`, `test_cobra` and the machine tests include authored format
vectors, exported signal/control checks, all-bank snapshot comparisons, native Next round trips, selected-block
BASIC loading, +3 Loader disk access and controller deadlines. These tests use bundled fixtures and run without
an application checkout or downloaded collection. Additional local collections and original PZX decoder
comparisons are development checks only; reports live under `build/media-review`.
