# Host ROM overrides

`EmulatorConfig::romResolver(name, expectedSize)` still supplies ordinary ROMs.
The optional `machineRomResolver(machineId, name, expectedSize)` supplies per-machine
custom ROMs. Its nonempty result takes precedence over the ordinary resolver and,
for the Next, `/machines/next` firmware on the SD card. An empty result retains the
normal fallback. Every nonempty override must have the requested byte count;
invalid sizes fail with `RomMissingException`.

The machine ID follows the installed machine, including automatic switches while
loading snapshots. ROM requests happen on construction, hard reset, machine
selection and peripheral attachment. Soft resets keep the current ROM bytes.
Overrides apply to named slots, not concatenated multi-slot files. Next slots are
64 KiB (`enNextZX.rom`), 8 KiB (`enNxtmmc.rom`, optional `enNextMf.rom`) and
32 KiB (`enAltZX.rom`). A custom Next ROM does not replace the SD storage/OS. With scoped overrides enabled,
Next ROMs reload at hard reset; removing an optional Multiface override also clears its
bytes when the SD image has no replacement.

Resolvers run while the emulator is locked. Preload and validate host files before
taking that lock; return cached bytes from the callback. Never acquire the emulator
lock recursively. Own callback state at least as long as the emulator and synchronize
host updates if its Engine can invoke the callback concurrently.

The standalone foundation tests cover machine identity, shared ROM filenames across
16K/48K models, snapshot-driven switching, hard reset, fallback and size rejection.
The ordinary resolver remains source-compatible and works without a consumer app.
