# Task workflows

## Media and BASIC

Use `inspectMedia` for a container's files and `listMediaProfiles` for disk
profiles. Read or extract named entries rather than assuming tape block order.
For a loaded tape, `inspectTape` and `readTapeBlock` expose block contents and
header/checksum information. Look up their current schemas in the tool catalog.
Checksums establish block integrity, not a program's memory layout.

BASIC decompilation distinguishes three representations:

- `listing`: readable visible text, suitable for explaining the program.
- `source`: lossless text preserving tokens, control bytes and hidden numbers;
  use this for round-trip compilation.
- `effectiveListing` and `numericConstants`: stored numeric values and any
  disagreement with visible digits. The interpreter uses the stored values.

Do not silently substitute visible numbers when reconstructing a loader.
`compileBasicToMemory` writes bytes but does not update interpreter pointers;
use `setBasicProgram` to install a program in a booted compatible interpreter.
Use separate output files for archive edits and media conversion so the
reference image remains available for comparison.

## Code and debugging

Disassemble from an observed entry point and correlate bytes with live execution.
Data tables can resemble instructions. `disassembleData` marks incomplete final
instructions as truncated; do not treat their padded operands as actual bytes.

`assemble` and `assembleToMemory` handle inline instructions and DB, not labels,
macros, ORG or linking. For complete Z80/Z80N projects, use an available external
assembler such as sjasmplus, import the resulting binary, and load its SLD text
with `loadSymbols`. Symbol/source addresses are logical; maintain separate
contexts for code that occupies the same addresses in different banks.

A typical experiment is to checkpoint, clone to `work`, identify a state write
using a memory watchpoint, disassemble its caller, then compare an isolated
patch with the baseline. An exact logical-address breakpoint uses `mask=65535`;
use `bankId=-1` and `page1k=-1` only when any physical mapping is intended.
Set a temporary breakpoint when it should be removed after the first hit.

For debugger-injected CALLs, establish a disposable executable RAM region and
stack from the current memory map. Preserve CPU state or reload the checkpoint
afterward. A scratch address safe for one program may contain code, data or
stack in another. Host memory writes bypass emulated access watchpoints;
execute the code to observe its writes.

## Graphics, maps and sound

Find drawing routines through execution or memory watchpoints before declaring
an asset format. Candidate graphics may be packed, mirrored, compressed, masked
or interleaved. `renderGraphics` assumes row-major 1bpp data; it does not infer
these layouts. Record address/bank, stride, dimensions, bit order, mask semantics
and palette separately. Validate several decoded assets against live rendering.

Room counts, map streams and sprite tables need bounds established from their
consumers. Unused RAM and embedded source fragments are not additional rooms.
Distinguish background placements, object state and collision data.

Use keyboard-driven captures for sounds associated with gameplay. One beeper
routine may receive changing parameters, so a recording of each entry point
does not necessarily cover jump, landing, damage or respawn behavior. Correlate
`captureAudio` with I/O writes and game state. Inspect `ioWrites.truncated` and
`totalEvents`; timestamps and clock provenance matter when CPU speed changes.
Use `captureIo` with a port mask to focus an experiment, and shorter captures
when unrelated writes consume the event budget. Keep AY and beeper analysis
distinct when both are active.

## Next and port comparison

Inspect `nextPlatformInfo` before downloading. `prepareNextSdCard` can reuse
its cached official distribution; `discoverNextDistribution` finds a newer
one when requested. Setup is optional when an appropriate raw image is already
available. Downloaded firmware is not a dependency of ordinary ZxKit tests.

Create a Next session with `createNextSession(sessionId, image, readOnly)`.
Multiple sessions can share a read-only SD image. A writable image cannot be
shared with any other session. Use separate copies for writable experiments;
reset after host filesystem edits because guest filesystem caches can be stale.
An SZX clone of a Next session retains the SD association only when it can
share the original image read-only.

Use `readNextRegisters`, `memoryMap`, and physical bank reads alongside screen
captures. Track MMU mappings, display modes, palettes, layer priority, sprite
attributes and CPU speed when those affect the task. Do not infer visible pixel
colors solely from ULA attributes when Next video layers are active.

For a port, preserve a reference machine and a separate target session. Align
comparisons at an observed input loop, interrupt count or other program event;
different loading/rendering speeds make a fixed startup delay unreliable.
Compare input sequences, movement, collisions, object state, room transitions,
dialogue, sound and completion paths as relevant to the requested scope.
Document seeded state and remaining manual tests. Reproducing an asset or a
single room is not evidence that the whole program behaves equivalently.

## Native display ports

Choose the architecture to match the requested scope. A graphics-only port can
retain the original engine while replacing its visible display path; a full
rewrite needs independent gameplay reconstruction. Establish entry points,
interrupt routines, stack use, data ownership and any ROM dependencies before
mapping new code over existing address space. Verify expected original bytes
and the input edition before applying binary hooks.

Recover each drawing contract: coordinates and units, offsets, dimensions,
stride, mirroring, clipping and copy/XOR/mask semantics. A masked sprite can
distinguish transparent pixels from opaque black; rendering every zero as
transparent changes its shape. Reproduce the compositor actually used by the
program instead of assuming a mask convention.

Trace display writes beyond the main blitter. Common separate paths include
the title/HUD, side decorations, dialogue interiors, energy meters, beams and
ending animations. Check that room clearing preserves surrounding artwork and
that menu overlays hide moving hardware sprites when appropriate. Test sprites
at both room edges and the top/bottom clipping limits.

For Layer 2 or hardware sprites, assign colors per pixel rather than repeatedly
converting a shared ULA attribute cell. Existing ULA memory may still be needed
as hidden collision/state data even when its visible layer is disabled. Check
that assumption before removing original bitmap or attribute updates.
Distinguish a transparent pixel index from a palette color used as a transparency
key; choose a key outside visible colors. Confirm palette bit depth and channel
encoding instead of assuming RGB332 and RGB333 writes are interchangeable.

Preserve a hook's register/flag and interrupt contract. Include alternate
registers, IX/IY and stack effects where relevant; restore the prior interrupt
state rather than enabling interrupts unconditionally. Preserve a banked window's
mapping across interrupt handlers and avoid banking out executing code. Renderer
scratch data accessed by the main loop and ISR needs explicit ownership.

Higher CPU speed can improve rendering while changing busy-loop sound pitch
and game timing. Identify interrupt pacing separately from CPU-bound delays;
retain or compensate the clock used by legacy beeper routines. Count actual
game interrupts across a known frame interval and test during heavy drawing,
pattern uploads and sound. Cache unchanged sprite patterns when upload cost
would otherwise consume the frame budget.

For NEX or another banked executable, verify the entry mapping, bootstrap,
stack, every required RAM bank and initialization of video/paging state. Test
the packaged executable from a fresh target session, not just a debugger snapshot
that already contains its patterns or palettes. Record input/output hashes and
patch locations in generated build metadata. Compare ordinary play, item menus,
hazards, death/respawn and ending transitions, with seeded tests labeled clearly.
