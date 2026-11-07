# Media tools

The optional `zxmedia` CLI and Linux `zxmount` frontend share `zxkit::MediaArchive`.
They work with image files without starting an emulator. Required test media stays
in this repository; HCDisk supplies a format inventory, not implementation code.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DZXKIT_BUILD_TESTS=ON -DZXKIT_BUILD_TOOLS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`ZXKIT_BUILD_TOOLS` defaults to on for a standalone checkout and off for
`FetchContent`/`add_subdirectory` consumers. Set it off to omit all tools.
`ZXKIT_BUILD_MEDIA_TOOL`, `ZXKIT_BUILD_FUSE_TOOL`, and
`ZXKIT_BUILD_MC_INTEGRATION` select the individual tools. Linux FUSE3 is found
through an optional `find_package` call; a missing FUSE3 dependency omits `zxmount`.
The MC helper and associations install alongside `zxmedia` on Linux when enabled;
MC is only needed at runtime, not on the build machine. Developer profiling tools
live in `tools/profiling` and require `ZXKIT_BUILD_PROFILING_TOOLS=ON`.

## CLI

`man zxmedia` documents every command, option and attribute (the page is
`tools/media/zxmedia.1`, installed to `share/man/man1`; `man -l
tools/media/zxmedia.1` reads it from a checkout).

```sh
build/tools/zxmedia --help
build/tools/zxmedia profiles
build/tools/zxmedia create build/games.tap
build/tools/zxmedia put build/games.tap game.bin files/GAME type=3 param1=32768
build/tools/zxmedia list build/games.tap
build/tools/zxmedia get build/games.tap files/GAME build/game.bin
build/tools/zxmedia attrs build/games.tap files/GAME param1=40000
build/tools/zxmedia rename build/games.tap files/GAME files/DEMO
build/tools/zxmedia rm build/games.tap files/DEMO

build/tools/zxmedia create build/disk.dsk
build/tools/zxmedia put build/disk.dsk program.bin PROGRAM.BIN type=3 param1=32768
build/tools/zxmedia attrs build/disk.dsk PROGRAM.BIN readonly=1
build/tools/zxmedia --filesystem cobra-cpm create build/cobra.img
build/tools/zxmedia --filesystem fat create build/card.img
build/tools/zxmedia mkdir build/card.img programs
build/tools/zxmedia create build/cpm.p3d
```

Existing output files require `--force`. Mutations of an image are explicit
commands. Writers take an exclusive advisory lock, check for external changes,
and replace the image through a temporary file in the same directory. Sidecar
`.zxmedia.lock` files contain no media data and can remain after a process exits.
FUSE holds its lock for the mount's lifetime. Read-only mounts do not create one.
Do not concurrently edit the same image in an emulator.

A raw file extension does not uniquely specify a filesystem or geometry. Existing
HC BASIC and supported CP/M/+3DOS layouts are detected from geometry, catalogue
entries and allocation chains when exactly one profile matches. This also applies
inside DSK/EDSK, TD0 and CQM containers; `.dsk` alone does not imply +3DOS.
HC detection additionally requires a native Spectrum file header. Empty raw disks,
headerless-only HC images and ambiguous catalogues require a profile selected with
`--filesystem` (or `ZXMEDIA_FILESYSTEM` for an MC session). Explicit profiles always
take precedence; detection never depends on the image's directory or filename.
Generic CP/M 8.3 names use uppercase; HC BASIC preserves case and uses
16-bit allocation pointers on both disk sizes. User areas 1–15 appear as `user01`–`user15`.
Generic CP/M lengths are rounded to 128-byte records. +3DOS and HC BASIC headers
carry exact lengths and Spectrum type/parameters. CP/M supports `readonly`,
`system` and `archive`; DEVIL and MGT support `readonly` and `hidden`; FAT supports
all four. Unsupported attributes are errors. Clear `readonly` before modifying or
removing a protected file. D40/D80 images use Didaktik MDOS and its native FAT, directory order and
permission bits. Trimmed TRD images are accepted when every allocated sector is
present; zero-length directory fields with allocated payloads expose that payload.
TR-DOS `type` is the ASCII code of its one-character
extension, for example 67 for `C`.

## Containers and filesystems

| Family | Support |
|---|---|
| TAP/TZX | Create, logical file and block editing, native export |
| PZX | Create/export, standard pilot/DATA file recognition, signal/block editing |
| CSW | Create, RLE signal export at 3.5 MHz, block editing; reads existing RLE/zlib recordings |
| WAV | TAP to unsigned 8-bit mono PCM WAV at 44.1 kHz; no WAV import |
| Floppy containers | Raw IMG, TRD, SCL, MGT, OPD, D40/D80, CPC DSK/EDSK, TD0, CQM |
| Floppy filesystems | CP/M, +3DOS, HC BASIC, CIP-04, TR-DOS, MGT/+D/Disciple, Opus Discovery, Didaktik MDOS, CoBra DEVIL |
| Microdrive | Create, read/write native MDR records and checksums |
| Next | NEX components, FAT12/16/32 SD images, NextZXOS P3D CP/M images |

DSK output uses Extended DSK even when the filename ends in `.dsk`. TD0 output
uses normal compression with uncompressed sector payloads. CQM output uses
literal RLE runs. Existing FAT12/16/32 cards are editable; new cards are 32 MiB
FAT16 superfloppies. New P3D images are 16 MiB and use the NextZXOS XDPB and
512-entry directory. NEX exposes `header.bin`, `banks/000.bin` through
`banks/111.bin`, loading screens and optional `extra.bin`; bank counts/maps are
derived on export. Its standard versions 1.0–1.2 and experimental 1.3 layout are
understood. Selecting 1.3 still requires a compatible loader on the target.

`cip04` uses the stock +3-compatible 180K layout. `cip04-360` is an extended
80-track layout with 2K allocation blocks; the reference utility's 80-track/1K
profile cannot represent that capacity with standard CP/M allocation pointers.
`profiles` lists the available fixed geometry presets; arbitrary CP/M diskdefs
are not yet an input interface.

Tape logical files live under `files/NAME`; duplicate names get numbered aliases.
`basic/NAME.bas` is a writable lossless text view of a BASIC file. `blocks` exposes
individual blocks as standalone TZX files, retaining original relative control
offsets in their contents. Append a block at the next sequential block number.
Removing a block remaps surviving jump/call/selection targets and rejects targets
into the removed range. Raw views of some PZX signal blocks expand to several TZX
blocks; unchanged views can be written back, while replacement must describe one
representable block.

Native filenames that cannot be POSIX paths are exposed through percent escapes;
blank DEVIL names use `%20`. DEVIL retains appended headerless data and its length
tables. Disk changes preserve reserved CP/M tracks; reads and edits reject weak
or unreadable sectors in the filesystem area. EDSK and TD0 preserve extra recorded
tracks; raw/CQM exports refuse to discard them. Deprecated/unsupported TZX bodies,
unknown PZX chunks, and damaged tape tails are preserved by an unchanged native
export; edits that would lose them are rejected. TZX control flow and descriptive
metadata have no general lossless TAP or CSW equivalent.

The archive backend currently keeps an image in memory and clones it for a
transaction. Large SD images therefore require several times the image size in
available memory. Linux is verified here; Windows and macOS use the portable
library/CLI code but need native platform build coverage.

## Conversion and BASIC

```sh
build/tools/zxmedia convert build/game.tap build/game.tzx
build/tools/zxmedia convert build/game.tap build/game.pzx
build/tools/zxmedia convert build/game.pzx build/back.tap
build/tools/zxmedia convert build/game.tap build/game.wav
build/tools/zxmedia --filesystem cobra-cpm convert build/cobra.img build/cobra.dsk
build/tools/zxmedia copy build/game.tap files/GAME build/disk.dsk GAME.BIN type=3 param1=32768

build/tools/zxmedia basic compile source.bas build/program.bin
build/tools/zxmedia --dialect next basic decompile build/program.bin build/listing.bas
build/tools/zxmedia basic array-compile numbers.txt build/numbers.bin
build/tools/zxmedia --array-type string basic array-decompile strings.bin build/strings.txt
```

`convert` changes suitable containers and tape encodings. A PZX can become TAP
when its standard pilot, sync and data timings are recognizable. Arbitrary audio,
protected loaders and control flow cannot become ordinary TAP data. `copy`
transfers item bytes between filesystems; provide destination-native attributes
explicitly. It does not rewrite BASIC loaders or make a program compatible with
a different disk interface.

BASIC dialects are Sinclair 48K, 128K/+3 and NextBASIC. Decompilation emits explicit
`\{PRINT}` token escapes and `\xHH` byte escapes, including hidden numeric
constants, so recompilation preserves the original bytes. Stored variables and
nonstandard tails use `!raw HEX`. Array text starts with comma-separated
dimensions; numeric values follow as whitespace-separated numbers. Character
arrays use one quoted fixed-width string per row. `@HHHHHHHHHH` preserves a
noncanonical five-byte number. This is tokenization and data conversion, not a
compiler to native Z80 code.

## Bootable CP/M images

Supply system tracks for the exact machine/profile and install its loader/system
files. For example:

```sh
build/tools/zxmedia --filesystem cobra-cpm create build/system.dsk --boot-tracks boot-tracks.bin
build/tools/zxmedia --filesystem cobra-cpm put build/system.dsk loader.bin CBOT.SYS
```

System tracks must exactly fill the reserved area. CoBra 80K uses 18,432 bytes.
Its ROM discovers a SYS loader through the directory; install the loader first
when its placement is part of the boot contract. The unit test constructs a fresh
disk using the bundled CoBra system tracks/loader and boots it through the real
ROM. A blank formatted image alone does not contain an operating system.

## Linux FUSE3 and Midnight Commander

```sh
mkdir -p build/mount
build/tools/zxmount build/games.tap build/mount -f
# In another terminal, ordinary reads/writes/copies edit the image.
fusermount3 -u build/mount
```

Use `--filesystem PROFILE` for ambiguous images and `-o ro` for a read-only mount.
Native attributes are decimal text xattrs named `user.zxkit.readonly`,
`user.zxkit.type`, `user.zxkit.param1`, etc. `chmod` maps write permission to the
native readonly attribute where one exists. Supported file operations are read,
write, truncate, create, remove, rename and directory creation/removal where the
filesystem supports directories. Host ownership, timestamps, hard links and
symlinks have no general native mapping. Writes commit on flush/fsync/close;
failed conversions are reported to the caller. The frontend runs single-threaded.

Install `zxmedia` on PATH, then install the helper and add the associations:

```sh
mkdir -p ~/.local/share/mc/extfs.d
cp tools/mc/zxkit ~/.local/share/mc/extfs.d/zxkit
tools/mc/zxkit-mc-setup            # undo with: tools/mc/zxkit-mc-setup --remove
```

`man zxkit-mc-setup` (`tools/mc/zxkit-mc-setup.1`) describes the options.

Without the associations, `cd games.tap/zxkit://` in a panel already opens an image.

MC reads a single extension file and the user's `~/.config/mc/mc.ext.ini` replaces
the system one (`/etc/mc/mc.ext.ini` on Debian, `/usr/share/mc/mc.ext.ini` elsewhere),
so `zxkit-mc-setup` starts from a copy of the system file when the user has none,
and otherwise adds or replaces only its own block, which sits between
`# BEGIN ZxKit` and `# END ZxKit` marker lines. Running it again after an upgrade
updates that block. It refuses a file that is not in the 4.0 format, and `--user-file`,
`--system-file` and `--fragment` override the locations. MC scans the extension
file on first use, so restart it (or press F9, c, e and exit the editor) afterwards.

`tools/mc/zxkit.ini` uses the Midnight Commander 4.0 extension file format,
`mc.ext.ini` with `Version=4.0`, read by MC 4.8.29 and later. Older `mc.ext` files
are not read, and MC reports "The format of the ... mc.ext.ini file has changed with
version 4.0" for a file without the `[mc.ext.ini]` section. That happens when only
an old ZxKit fragment was copied to `~/.config/mc/mc.ext.ini`: move that file away
and run `zxkit-mc-setup`. In this format a backslash in a `Regex` value must be
doubled (`\\.tap`), and `View` needs `%view{ascii}` for MC's internal viewer to show
the command output. MC merges the repeated `[mc.ext.ini]` section, and `[Default]`
applies only when nothing else matches, so appending after it works.

Pressing Enter then opens an image as an archive, and F3 lists its contents. MC
copy-in/copy-out, editing and removal use the same CLI/backend. IMG associations are
separate because unrelated formats also use that extension; delete the
`[ZxKit disk images]` section to leave `.img` files alone.

CMake installs `zxkit-mc-setup` to `bin` and `zxkit.ini` to `share/zxkit/mc`. The
helper goes to `share/zxkit/mc/extfs.d` unless `-DZXKIT_MC_EXTFS_DIR=DIR` (relative
to the prefix) names MC's system helper directory. Installation never changes a
user's MC configuration.

`test_mc_static` checks the fragment against the 4.0 rules (including real glib
parsing when PyGObject is installed), the helper, the extfs listing syntax and
`zxkit-mc-setup`. `test_mc_session` starts a real MC with throwaway HOME and XDG
directories, opens a tape, views it and views a file stored in it, with the fragment
on its own and appended to the system file. CTest reports it as skipped when MC
4.8.29 or later or the Python `pexpect` module is missing.

## Debian packages

`debian/` builds three packages: `libzxkit-dev`, `zxkit-tools` (`zxmedia`) and
`zxkit-mc`. The Midnight Commander files are in their own package, which depends
on `mc (>= 3:4.8.29)` because older MC (Debian bookworm has 4.8.28) cannot read the
4.0 format and `zxkit-tools` should not pull in MC. `zxkit-mc` follows Debian's MC
layout:

| File | Why there |
| --- | --- |
| `/usr/lib/mc/extfs.d/zxkit` | Debian builds MC with `--libexecdir=/usr/lib`, next to MC's own `uzip`, `urar`, ... helpers, so `cd x.tap/zxkit://` works without setup |
| `/usr/share/zxkit/mc/zxkit.ini` | the associations that `zxkit-mc-setup` adds to a user's file |
| `/usr/bin/zxkit-mc-setup` | per-user setup, because `/etc/mc/mc.ext.ini` belongs to the `mc` package and MC has no drop-in directory |

```sh
dpkg-buildpackage -b -us -uc      # binary packages, written to the parent directory
```

A source package (`dpkg-buildpackage -us -uc`) needs the upstream tarball
`../zxkit_1.0.0.orig.tar.xz` because the source format is `3.0 (quilt)`. The unit
tests run during the build, with the MC session test enabled by the
`<!nocheck>` build dependencies `mc`, `python3` and `python3-pexpect`. The
`mc-layout` autopkgtest runs the same session against the installed packages, with
the helper found in `/usr/lib/mc/extfs.d` and the user's file built by
`zxkit-mc-setup` from Debian's `/etc/mc/mc.ext.ini`.

## Optional collection audit

The audit tool never writes to a source image. It extracts each file, reopens a
native export, tests update/rename/add/remove in memory and checks that unrelated
items retain their bytes. It requires explicitly supplied collections; they are
not dependencies of the unit suite.

```sh
cmake -S . -B build -DZXKIT_BUILD_TOOLS=ON -DZXKIT_BUILD_MEDIA_CHECK=ON
cmake --build build --parallel
python3 tools/media/audit_collection.py /path/to/collection --output build/media-audit/collection
build/tools/zxmedia-check trdos /path/to/image.trd .trd
```

ZIPs are unpacked under the output directory; RAR self-extracting archives use
`unrar` when installed, without executing the archives. Reports distinguish failed
checks from unsupported filesystems. The additional Elwro, Timex FDD3000, Kempston,
Rocky Gush, Turbodrive and Watford SPDOS filesystems in the sample repository do
not yet have adapters. Their images are reported as unsupported rather than
interpreted as CP/M by geometry alone.

## References

- [CoBra DEVIL layout and collection](https://www.cobrasov.com/CoBra%20Project/devil.html)
- [CoBra 80-column CP/M and boot layout](https://www.cobrasov.com/CoBra%20Project/cpm80.html)
- [CoBra documentation index](https://www.cobrasov.com/CoBra%20Project/docs.html)
- [NextZXOS P3D creator and XDPB](https://gitlab.com/thesmog358/tbblue/-/blob/master/src/asm/dot_commands/createp3d.asm)
- [NextBASIC token definitions](https://gitlab.com/thesmog358/tbblue/-/blob/master/src/asm/dot_commands/bastoken.def)
- [NEX specification](https://wiki.specnext.dev/NEX_file_format)
- [PZX specification](https://github.com/raxoft/pzxtools/blob/master/docs/pzx_format.txt)
- [MC extfs protocol](https://github.com/MidnightCommander/mc/blob/master/src/vfs/extfs/helpers/README)
- Further tape/disk references are linked in [media.md](media.md).

- [Didaktik MDOS user manual and directory layout](https://www.8bit-wiki.de/Sinclair/ZX_Spectrum/_didaktik/MDOS%20commands.pdf)
- [MDOS FAT packing and terminal values, by MFC's author](https://mts.speccy.cz/manuals/mfc.pdf)
- [MGT image sector ordering](https://sinclair.wiki.zxnet.co.uk/wiki/MGT_format)
- [Klive's disk-container explorer](https://kliveide.net/en/guides/media/disk-explorer/)
- [Additional external disk samples](https://github.com/konkotgit/ZX-Spectrum-FD-Images)
