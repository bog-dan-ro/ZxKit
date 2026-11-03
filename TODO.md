# ZxKit TODO

What is missing or incomplete. Spectacol keeps its own TODO.md in its separate application repository.
Linked Spectacol issues are a curated selection reviewed on 2026-11-01; UI and platform work stays in its list.

Work in the priority order below. Audio continuity comes before frame delivery: a dropped video frame is
less disruptive than a crackle or gap in the sound. Clone and extension work stays at the very end.

## Audio

- [x] **Continuous audio delivery.** Carry fractional samples and boundary-crossing beeper/AY writes between
      video frames; do not count CPU instruction overshoot twice. Publish audio before video callbacks and expose
      automatic tape turbo to hosts. Regressions cover exact long-run sample counts, waveform partitioning,
      actual CPU frame boundaries and callback ordering. Spectacol's output now bypasses GUI event delivery.
- [x] AY: band-limited synthesis with a shared sinc step filter and measured AY/YM volume curves.
      All envelope shapes, YM's 32 steps, long periods, fractional chip clocks, reset and rewind are covered by
      standalone audio regressions. Spectacol exposes the chip model in Sound settings; physical listening stays
      on its checklist.
- [x] Speaker models: representative TV (ULA/AY), internal beeper (ULA/tape only), and unfiltered responses.
      External DACs remain direct; runtime changes crossfade, and reset/rewind preserve host preferences and
      restore filter/edge history. Response, routing and waveform continuity regressions run standalone.
- [x] Engine: optional host audio-clock pacing via a weak deadline callback, with wall-clock fallback and
      interruptible waits for pause, speed, turbo, callback changes and stop. Standalone regressions cover queue
      admission, callback lifetime/failure, control races and manual/tape turbo; see `docs/audio-pacing.md`.

## RZX compatibility and progress

- [x] **RZX playback compatibility** ([Spectacol #48](https://github.com/bog-dan-ro/spectacol/issues/48)):
      recorded M1/interrupt boundaries, ordered embedded snapshots/input blocks and explicit desynchronization
      errors. All five reported recordings reach completion in ZxKit and match independent reference states;
      authored standalone fixtures cover prefixes, LD R,A, EI/HALT/retriggers and recording interoperability.
      See `docs/rzx.md` for coverage and explicit format limits.
- [x] RZX playback progress ([Spectacol #47](https://github.com/bog-dan-ro/spectacol/issues/47)): current/total
      intervals, retained completion/stop positions and contextual errors through `RzxProgress`.
- [x] RZX transport/editing: pause through Engine, seek backward/forward by interval or milliseconds,
      retain the future during review, truncate on resume or finalise at the cursor. Sparse bounded checkpoints
      avoid per-frame state copies; standalone branch/replay tests cover interrupts and independently playable output.

## Media formats and disk auto-boot

- [ ] More tape formats: PZX and standalone CSW (the block model and `TapeCursor` already cover them); saving TZX
      (only TAP today).
- [ ] Snapshot saving: 128K `.sna`, `.z80` and `.nex` (only 48K SNA and SZX today).
- [ ] Disk auto-load for hosts (FUSE resets into TR-DOS, or picks the +3 Loader, when a disk is opened): a
      public API to agree on first.
- [ ] The WD1793 and the uPD765 are sector level: commands complete at once (no head stepping, no rotation
      delays). Enough for the loaders tried so far; protections that time the disk would need more.

## Debugger

- [ ] `BreakpointType::NextRegWrite` and `BreakpointType::RasterLine` exist but are never checked, and
      `PauseReason::NextRegWatchpoint`, `RasterBreakpoint`, `EventTrap` and `StepComplete` are never reported.
- [ ] Step out (run to the RET) and run to cursor ([Spectacol #27](https://github.com/bog-dan-ro/spectacol/issues/27)).

## Performance and resource usage profiling

- [ ] Profile the standalone Engine, audio generation and video filters with `perf`: CPU time, allocations,
      memory footprint, synchronization and wakeups. Separate core cost from each filter's cost and host overhead.
- [ ] Establish reproducible before/after baselines with the same optimized build, machine, media, speed and
      display workload. Investigate the consumer's reported increase since the initial ZxKit port; keep battery
      usage and audio continuity central to optimizations. QML tracing belongs in the consumer repository.

## CI, tests, installation and documentation

- [ ] CI: build and test (Linux; Debug and Release; ASan/UBSan, and TSan: the Engine threading test is a good target).
- [ ] Boot smoke tests: every machine reaches its ROM screen; keep required ROMs in `tests/fixtures`.
- [ ] Tests for snapshot round trips on 128K/+3/Next (`test_tape` covers the tapes, `test_disks` the uPD765, the
      WD1793, the Beta 128 and the TRD/SCL images).
- [x] The tests keep `assert()` in Release builds (they were no-ops there).
- [ ] A reference for the tests: the FUSE fork in `~/work/hc/fuse` builds with `--with-null-ui --enable-automation`
      and runs headless (`--automation-frames N --automation-capture-screen`, PC conditions): handy to compare a
      game's behaviour with ZxKit's.
- [ ] Check installing and `find_package(zxkit)` from an installed tree (SafeCpp is exported as a dependency).
- [ ] API documentation and a minimal example host.

## Completed emulation and host work

- [x] **Real-time tape playback.** `TapeCursor` plays every TZX block (and TAP) as edges, the same waveform as
      libspectrum (checked edge by edge on ~5000 tapes); `TapeDeck` puts them on the EAR input at T-state accuracy,
      at any CPU speed and machine clock. ROM traps for standard blocks, auto play/stop (FUSE's loader detection),
      loader acceleration (FUSE's patterns), the loading sound, turbo while playing (`TapeOptions`).
      Speedlock 3/4/7, Alkatraz-style turbo, direct recording, CSW and generalized data tapes load.
- [x] **TR-DOS.** A new `Wd1793` and `Beta128Peripheral` (FUSE's paging: in at 0x3Dxx, out at 0x4000+), built
      into the Pentagon and the Scorpion (four drives). TRD (geometry from the disk type) and SCL (with the
      system sector TR-DOS needs). `insertTrDosBootLoader()` adds FUSE's "boot" to a disk without one.
      Metal Man, Dizzy SCL/TRD and demos boot (menu → TR-DOS → RUN).
- [x] **+3 floppy drives.** The `Upd765Fdc` on ports 0x2FFD/0x3FFD, the motor on bit 3 of 0x1FFD, two drives
      (not on the +2A). Overrun timing, Control Mark / SK, READ TRACK, EDSK weak sectors and FUSE's Speedlock
      hack: disks compared with FUSE load (Turrican, California Games, Target Renegade, ...).
- [x] **CoBra 1.** Checked against the cobrasov.com documentation (the hardware manual, the 80K and 80 characters
      modifications, the floppy board). The floppy board: an 8272 in non-DMA mode (status 0xF5, data 0xFD) and a
      new generic `Z80Ctc` (0xE3/0xEB/0xF3/0xFB) wired as on the board (INT counted by channel 0, bytes by 1,
      sectors by 2 whose zero count is the TC, the vertical sync on 3), its interrupts in IM2. Four drives, raw
      720K `.img`. The keyboard's sixth column (CTRL, NO SCROLL, LINE FEED, ESC, F1-F4). The 50 Hz interrupt in the
      BASIC configuration only; `LD R,A` switches the map after the next opcode fetch. The 80K model's video always
      reads the video RAM (AV13 from O5) and the 80 characters latch (a 320 pixel screen). The boot manager,
      BASIC, OPUS, DEVIL, ROM CP/M and the KRYS80 system disks (CP/M 2.2, CoBra 53K BIOS) work: DIR, SAVE, REN,
      ERA, drive B, warm boot. The 64K model's map matches the manual's Fig. 4.
  - [x] The 64K ROM's SYS.COM loader: diagnostic disks verify both FM (26 × 128) and MFM
        (10 × 512) formats, allocation blocks, and execution at 0100 in the CP/M map. Seek
        interrupts are delayed and READ ID honors recording density. No complete historical
        64K CP/M system disk has been located, so its OS/BIOS remains unverified.
  - [x] DEVIL disks: Teledisk normal and advanced compression, all three sector encodings,
        CRC checks and FM/MFM metadata. The original UTILS1.TD0 boots in DEVIL and CAT prints
        real file names. Non-DMA readiness now agrees with the first byte's INT request.
  - [x] Standalone fixtures: the official boot/system ROMs and the KRYS80 CP/M disk are in
        `tests/fixtures`, with provenance and notices. `test_cobra` covers the hardware ports, disk and ROM
        CP/M SAVE commands, the Turbo Spectrum boot and BASIC, and reset/rewind of the configurations.
- [x] **CoBra 2 (Turbo Spectrum).** Any IN loads the CE / SM flops (TS specification 2.7), not only port 0x7E: the
      boot's TURBO SPECTRUM logo, the 64K DRAM mode and the switch to Spectrum mode (locked until a reset) work.
  - [x] Use the documented 16K EEPROM and its official MIT-licensed boot. The earlier four-page
        CoBra 1 system ROM mapping does not belong to the Turbo Spectrum; removed it from both projects.
- [x] Media activity: `Emulator::mediaActivity()` (the tape's state and block, the floppy drives and the SD card
      accessed in the last 0.2 s) and the Engine's `mediaActivity` callback when it changes.
- [x] UART hardware (ports 0x133B–0x163B): two timed FIFOs, framing, CTS, reset and state.
      Host byte hooks leave ESP AT commands, sockets and Pi services to consumers; see `docs/next-devices.md`.
- [x] DS1307 RTC (I2C on ports 0x103B/0x113B), BCD calendar, halt, 12/24-hour time and battery RAM.
- [x] Extended keyboard (NR 0xB0/0xB1, NR68 matrix suppression) and NMI buttons (Multiface, DivMMC drive).
- [x] All four DAC channels, alias/decode precedence and NextREG 0x2C–0x2E writes.
- [x] FAT writing: `FatVolume` lists, writes and removes files and makes directories (FAT12/16/32, long file
      names, every FAT copy and FAT32's FSInfo kept up to date; checked with `fsck.fat` and mtools, and NextZXOS
      loads the files). `Emulator::sdCardVolume()` gives it for the inserted card: write while the emulation is
      paused and reset the machine after, NextZXOS caches the card.
- [x] **Full border output for host cropping** ([Spectacol #137](https://github.com/bog-dan-ro/spectacol/issues/137)).
      `EmulatorConfig::fullBorder` opts into the full scanned border; the default remains the traditional crop.
      `VideoFrame::paperRect()` locates the ULA paper in native pixels, including Timex and Next resolutions.
      Extra Sinclair border pixels come from the raster timeline, including colour effects. Spectacol crops
      each side independently at runtime without changing core settings or machine state. Standalone `test_borders`
      compares crops, paper metadata, snapshots, machine timing and Copper execution outside the displayed area.

## Deferred clones and extensions — lowest priority

Keep these after all of the work above. Standalone DivMMC remains postponed.

- [ ] The Beta 128 as an add-on for the 48K/128K (FUSE pages at 0x3C00-0x3DFF on 48K machines, and can boot
      into TR-DOS); it needs the 128K's "48 BASIC ROM selected" (`Beta128Options::basicRomSelected`).
- [ ] Pre-M1 hooks: only the Beta 128 names the pages it watches (`addPreM1Hook(fn, owner, pages)`); the
      Interface 1, +D, DivMMC, Multiface, Spectranet and Next hooks still run on every instruction.
- [ ] IDE: the +3e ROMs boot but have no drive; DivIDE; `.hdf` images.
- [ ] Interface 1: a microdrive cartridge API (`.mdr` insert, eject, save) and RS232. The cartridge API is needed
      for the microdrive part of [Spectacol #152](https://github.com/bog-dan-ro/spectacol/issues/152).
- [ ] Spectranet: Ethernet (W5100); only its paging is emulated.
- [ ] TC2068 / TS2068: the AY (ports 0xF5/0xF6) and the two joysticks read through its I/O port (FUSE's
      `joystick_timex_read`).
- [ ] ULAplus (64 colours) on the classic machines (only the Next has it): the Spectrum-compatible part of
      [Spectacol #2](https://github.com/bog-dan-ro/spectacol/issues/2), without taking on its other machine requests.
- [ ] Twister (System 3) fails with the acceleration on, like in FUSE: find which pattern misfires.
- [ ] Standalone DivMMC/esxDOS on classic machines: postponed. Keep it independent of the Next built-in device.
