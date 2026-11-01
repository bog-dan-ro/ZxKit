# zxkit

Modern C++23/26 ZX Spectrum, Z80 Clones (HC-2000, CoBra 1/2, Pentagon, Scorpion, Timex) & ZX Spectrum Next Emulation Framework.

## Features

- **Modern C++23/26 & Minimal Dependencies**: Standard C++23, `zlib` and [SafeCpp](https://github.com/bog-dan-ro/safecpp) (a single header, fetched automatically). No singletons, no global state and no raw owning or borrowed pointers: as many emulators as you like can run in the same process.
- **Synchronous core, lock-protected sharing**: `zxkit::Emulator` is the whole computer (CPU, memory, ULA, machine, peripherals, media, debugger) and runs only when called, ideal for tests and tools. Share it between threads as `zxkit::Shared<Emulator>` (a SafeCpp `SharedPtr` whose lock lives next to the object): the only way in is `writeLock()` / `readLock()`.
- **Real-time Engine that never sleeps on a lock**: `zxkit::Engine` runs an emulator on its own thread, taking the lock one scanline at a time. The lock is `SpinSharedMutex`, a fair (FIFO ticket) reader/writer spin lock, so the emulation thread is never suspended by the kernel while it runs and a host thread waits at most a scanline (microseconds). Pausing sleeps on an atomic wait, so a paused Engine costs no CPU.
- **Weak callbacks**: frames, audio, pauses (breakpoints, watchpoints, user requests) and errors reach the host through `Weak<std::function<...>>` callbacks, called with no emulator lock held. `CallbackHandle` owns one: destroying it waits for a call in progress, so whatever the callback captured can go right after.
- **Recycled immutable video frames**: `VideoFramePtr` (`Shared<VideoFrame>`) frames are never written once published; the emulator recycles a frame only after every holder released it.
- **Composable software video filters**: A separate `ZX::VideoFilters` library provides 32 variants across HQ, xBRZ, Scale/AdvMAME, SaI/Eagle, TV, dot matrix, bilinear, nearest neighbour, scanline and RGB mask filters. Consumers opt in by linking and calling it; hardware rendering needs only `ZX::Kit`. See the [filter guide and source credits](src/video/filters/README.md).
- **Exception-based errors**: typed exception hierarchy (`zxkit::Exception`, `IoException`, `FormatException`, `RomMissingException`, `PeripheralException`, `DebuggerException`) thrown straight to the caller.
- **High-Performance Z80 + Z80N CPU**: Passes 1,356/1,356 FUSE Z80 compliance tests + full ZX Spectrum Next Z80N instruction set, with a single-branch `m_activeHooksMask` gating M1, RFSH (R7 edge), RETN, and debugger hooks.
- **64×1 KB Page-Table MMU**: Supports 1 KB, 2 KB, 4 KB, 8 KB, and 16 KB banking, independent read/write pages, prioritized `/ROMCS` peripheral overlays, and CP/M hardware banking flips (HC-2000 ports `0x7E` & `0xC5`/`0xC7` A13 pull-high, CoBra 1 8255 + `R7` falling-edge flip-flops, CoBra 2 mode flops loaded by IN).
- **Tapes in real time**: TAP and TZX (every block type, loops and calls included) play on the EAR input at T-state accuracy and at any CPU speed, so turbo and custom loaders (Speedlock and co.) load. Standard blocks load instantly through ROM traps; the tape starts and stops by itself when a loader waits for it, and the loaders' edge loops can be fast-forwarded (FUSE's loader detection and acceleration).
- **Floppy disks**: the +3's uPD765 (`.dsk` / EDSK, weak sectors and the Speedlock protection included) and the Beta 128 with TR-DOS built into the Pentagon and the Scorpion (a WD1793, four drives, `.trd` / `.scl`).
- **Romanian CP/M machines**: the HC-2000 boots CP/M (`PRINT USR 14446`) from `.dsk` / raw `.img` disks through its built-in HC Interface 1 uPD765 floppy controller. The CoBra 1 boots BASIC, OPUS and DEVIL on both RAM configurations, and CP/M from ROM or disk on the 80K model, with its floppy board (an 8272 in non-DMA mode driven by a `Z80Ctc` in IM2, four drives, raw and Teledisk images), its keyboard's sixth column and the 80 characters screen; the CoBra 2 (Turbo Spectrum) runs its boot logo and Spectrum mode.
- **ZX Spectrum Next**: boots NextZXOS straight from an SD card image (`Emulator::insertSdCard`), CSpect style: the ROMs come from the card's FAT file system, and the internal DivMMC (NR 0xB8..0xBB automap) and SPI SD/SDHC card give NextZXOS its storage. `extractZipEntryToFile` streams the official 1GB image out of its distribution zip. Hosts list, read and write the card's files with `Emulator::sdCardVolume()` (FAT12/16/32, long file names).
- **Easily Extendable**: Add custom peripherals or Z80 machines by subclassing `zxkit::Peripheral` or `zxkit::MachineProfile` without modifying framework internals.

## Using it

```cpp
#include <zxkit/zxkit.h>

// Headless: the emulator runs right here
zxkit::Emulator emulator({.machineId = "48k", .romResolver = myRomLoader});
emulator.loadMediaFile("game.tap");
const zxkit::VideoFramePtr frame = emulator.runFrames(100);

// Real time, on the Engine's thread
zxkit::CallbackHandle<zxkit::VideoFrameCallback> onFrame([](const zxkit::VideoFramePtr &frame) {
    const auto pixels = frame.readLock(); // RGBA8888, pixels->width() x pixels->height()
    // ... hand it to the GPU
});
zxkit::Engine engine({.machineId = "next"}, {.callbacks = {.videoFrame = onFrame.weak()}});

auto file = zxkit::MediaFile::read("game.nex");      // the slow part, without the lock
engine.emulator().writeLock()->loadMedia(file);      // waits at most a scanline
engine.emulator().writeLock()->setKeyState(zxkit::ZxKey::Space, true);
```

## Building and testing

Requires CMake 3.25 or newer, a compiler with C++23 support, zlib and threads.
CMake fetches SafeCpp if it is not already available.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DZXKIT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**After every change, building and passing the full unit test suite is
mandatory before committing or reporting completion.** For emulation-core
changes, run the full suite in both Debug and Release (use a separate
`build-release` directory with `-DCMAKE_BUILD_TYPE=Release`). Assertions remain
enabled in the test executables in both configurations. See [AGENTS.md](AGENTS.md)
for the contributor instructions.

The tests include their media, reference images and ROMs under
[`tests/fixtures`](tests/fixtures/spectrum/README.md). No Spectacol checkout or
external test collection is needed. Missing required ROM fixtures fail the
build configuration instead of silently skipping coverage.

The 12 CTest entries cover the software video filters, CPU, machine, tape and storage tests,
Sinclair timing boundaries, the Butler timing suites, incremental ULA raster
rendering and an exact BASIC Border image comparison. Five known 128K Butler
failures remain excluded from the passing suite; their case numbers and the
remaining visual ULA differences are documented in the
[fixture README](tests/fixtures/spectrum/README.md).

The optional collection smoke audit uses Python's standard library and defaults
to bundled fixtures, the built runner and bundled ROMs. It writes its report and
screenshots under `build/spectrum-audit`:

```sh
python3 tests/tools/audit_spectrum_collection.py
```

Smoke completion does not establish a hardware pass. Use `--help` for explicit
collection, runner, ROM-directory and output-directory overrides.

## Checking a machine boots

`build/tests/zxrun` runs a machine headless from a script of key presses and saves screenshots:

```sh
zxrun --machine hc2000 --roms DIR "disk:0:CPM.dsk" wait:150 key:P key:CS+SS key:L type:14446 key:ENTER wait:300 shot:cpm.png
zxrun --machine next --roms DIR --sdcard cspect-next-1gb.img wait:450 key:SPACE wait:250 shot:nextzxos.png
zxrun --machine pentagon --roms DIR "disk:0:game.trd" trdos wait:700 shot:game.png   # TR-DOS runs the disk's "boot"
zxrun --machine cobra1_80k --roms DIR "disk:0:KRYS80_CL_SYSDISK.img" wait:100 key:ENTER wait:200 key:D wait:1200 text:"DIR\n" wait:200 shot:cpm.png   # CoBra: the 80K standard boot, D loads CP/M
```

## Importing with CMake `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(ZxKit
    GIT_REPOSITORY https://github.com/bog-dan-ro/ZxKit.git
    GIT_TAG main)
FetchContent_MakeAvailable(ZxKit)

target_link_libraries(myapp PUBLIC ZX::Kit)
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Third-party test programs, reference
images and ROM fixtures retain their original notices and distribution terms;
see the fixture READMEs and [ROM notice](tests/fixtures/roms/README.copyright).
