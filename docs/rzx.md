# RZX playback and recording

`Emulator::loadMedia` / `loadMediaFile` accept RZX 0.12 and 0.13 streams.
Playback follows recorded M1 fetch counts and interrupt boundaries rather than
assuming one recorded interval per raster frame. Prefix fetches, `LD R,A`, HALT
and short interrupt retriggers are counted explicitly; interrupt acknowledge
cycles are excluded. Normal emulation uses the uncounted CPU implementation.

Embedded SNA, Z80 and SZX snapshots select their base machine: 16K, 48K/NTSC,
128K, +2/+2A, +3/+3e or Pentagon 128K. Other machine snapshots are explicitly
unsupported until their paging/peripheral state can be preserved. Snapshot and input
blocks execute in file order, including subsequent snapshots and each input
block's initial T-state position. Input values override port reads only during
playback; completion, stop and failure restore ordinary hardware reads. Consuming
too many or too few input values, or splitting an instruction's fetches, raises
`FormatException` with the failing frame and leaves `RzxState::Error` observable.

`emulator.rzx().progress()` returns state, completed intervals, total intervals
and an error string. Counts refer to the entire recording, even when a snapshot
resets the emulated clock. Completed and stopped positions remain available;
`stopRzxPlayback()` detaches playback without clearing those counts. A host can
sample progress at UI refresh frequency; it needs no callback per instruction or
frame. All accesses follow the host's usual emulator locking rules.

Use `startRzxRecording()` and `stopRzxRecording()` on the emulator, rather than
assembling a stream from video frames. These capture the initial snapshot and
T-state position, count actual CPU fetches and input readings, preserve AY
registers and finish partial intervals. Declared 48K AY interfaces (standard
128K ports or the existing Fuller Box) are restored from Z80/SZX flags and
preserved in recordings, so later sound-port writes keep working. The output includes a creator block and
compressed SZX/input blocks. Snapshot-representable standard machines at normal
CPU speed are supported; unsupported machine/turbo combinations are rejected.
Reset, snapshot replacement and machine changes during recording are rejected
instead of silently producing invalid recordings. Use the RZX seek API for
navigation; generic debugger rewind does not move an input-recording cursor.

## Recording and playback transport

Pause the host `Engine` before seeking or finalising. `beginRzxSeekFrame()` moves
to a recorded interval boundary; `beginRzxSeek(milliseconds)` converts time to
frame boundaries. Short INT retrigger intervals share a presentation-frame tick.
`rzxPositionMilliseconds()` and `rzxDurationMilliseconds()` expose the cursor and
the complete available timeline. Seeks clamp at its beginning/end.

Call `advanceRzxSeek()` in small batches until it returns true, yielding between
batches so a UI stays responsive. Sparse native checkpoints restore CPU/RAM,
audio history, tape state and scheduled events; recorded input replays the gap.
The origin is retained and older intermediate checkpoints are evicted within a
64 MiB budget. No checkpoint work runs outside RZX playback/recording.
Checkpoints stay in memory; saved files remain ordinary portable RZX files.
Seeking across host peripheral configuration changes is explicitly rejected;
Fuller interfaces declared by embedded snapshots are restored automatically.

A recording seek enters `Reviewing`: the later attempt remains available for
forward navigation. `resumeRzxRecording()` cuts that future at the cursor and
switches back to live input. `stopRzxRecording()` finalises at the cursor as well.
Playback seeks never switch into recording or alter the loaded movie. Playback
completion requests an Engine pause at the final frame. `cancelRzxSeek()` cancels
an incremental seek without running the remaining target frames.

Spectacol's controls are under Media → Recording. Seeking owns an emulator pause
until a fresh gameplay key/button or a stick movement beyond the dead zone
resumes the current mode. Held inputs, key repeat, stick noise and menu controls
do not resume it. The triggering input reaches the guest before the Engine
restarts. F8 seeks back five seconds; F9 seeks forward five seconds; Shift+F9
seeks to the end (`fastForward(-1)`). The controls page also offers pause/resume,
a selectable seek step and Stop/finalise. Recording Stop trims at the cursor.

External snapshot descriptors and encrypted input blocks are explicitly
unsupported. Unknown optional blocks are skipped by their declared length.
Malformed lengths, repeat records without a predecessor, unknown flags,
truncated/trailing compressed data and unsupported revisions fail clearly.
Limits are 128 MiB per file/decoded payload, 16 MiB per snapshot, two million
intervals and 100,000 snapshot/position commands. A playback interval cannot
exceed sixteen raster frames. These limits do not introduce creator-specific
recovery or attempt to repair nonconforming recordings.

## Verification

`test_rzx` runs without Spectacol or a downloaded game collection. See
[`tests/fixtures/rzx/README.md`](../tests/fixtures/rzx/README.md) for the authored
fixtures, format provenance and independently obtained reference states. It
covers multiple snapshots/input blocks, repeats, fetch prefixes, EI/HALT/IM2,
input desynchronization, stop/completion, parser validation, callback lifetime,
recording round trips and standard machine/AY preservation.

Development audit on 2026-11-03: the five recordings named in Spectacol issue 48
played to completion in ZxKit. Final PC, HL, R and sampled RAM matched SkoolKit
10.1's independent player; all recorded input reads were consumed exactly.

| Recording | Recorded intervals |
| --- | ---: |
| Switchblade | 142989 |
| SAS Combat Assault | 14479 |
| Samurai Warrior | 67559 |
| Skurff | 22998 |
| Sabotage | 80677 |

These downloaded game recordings are development inputs, not redistributed test
fixtures. No changes to other emulators or producer-specific workarounds were
needed. A separate ZxKit-generated recording also played successfully in the
independent player, with matching final CPU state.

Format: [RZX 0.13 specification](https://worldofspectrum.net/RZXformat.html).
Independent reference: [SkoolKit](https://github.com/skoolkid/skoolkit).
