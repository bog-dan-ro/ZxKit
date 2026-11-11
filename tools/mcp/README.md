# zx-mcp

An optional headless MCP server for ZxKit development and reverse engineering.
It uses MCP++ reflection with Boost.JSON. Multiple independent emulators can
coexist, including 48K and 128K machines with embedded ROMs.

## Build

Requires CMake 3.30+, GCC 16.2 with C++26 reflection, Boost 1.92+ (JSON,
Beast and Asio), and OpenSSL 3+ for verified HTTPS downloads.
The ZxKit library remains C++23. The server is explicitly enabled so ordinary
library builds do not download MCP++ or require a reflection compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DZXKIT_BUILD_TESTS=ON \
    -DZXKIT_BUILD_MCP_TOOL=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

MCP++ is fetched from its public Git repository at a pinned revision.
`FETCHCONTENT_SOURCE_DIR_MCPPP` can select an existing checkout for offline work.
The Boost backend is linked by this consumer; upstream examples/tests are disabled.

## Connect

Configure an MCP client to launch `build/tools/mcp/zx-mcp` using stdio:

```json
{
  "mcpServers": {
    "zxkit": {
      "command": "/absolute/path/to/zxkit/build/tools/mcp/zx-mcp",
      "args": ["--output-dir", "/absolute/path/to/zxkit/build/zx-mcp"]
    }
  }
}
```

The server supports the initialize-based MCP lifecycle (2024-11-05 through
2025-11-25) and MCP++'s native 2026-07-28 discovery/request metadata. The adapter
uses MCP++ for reflected tool schemas, typed argument validation and dispatch.
It converts JSON-text method returns to structured objects and adds PNG image
content for screenshot/graphics tools. Tool failures return `isError: true`
with an actionable message. This is a tools-only stdio server; it does not
advertise resources, subscriptions, asynchronous jobs or cancellation.

`--list-tools` prints the complete generated tool catalog without starting a
protocol session. Every parameter in a tool's input schema is required. Pass
empty strings where the description allows them, including `sessionId: ""`
to use the selected default. Prefer explicit session IDs for multi-machine work.
Use JSON integers for addresses and hexadecimal byte-pair strings for binary data.

## Sessions and execution

Create sessions with `createSession(sessionId, profile)`. `48k`, `128k` and `16k`
work without additional ROM files. Other machine profiles can use `--roms DIR`;
files there also override the corresponding embedded ROMs. Each session owns its
CPU, memory, tape, breakpoints, symbols, trace, rewind and poke-finder state.
`useSession` changes the empty-ID default; `destroySession` releases an emulator.
`cloneSession` copies machine state using SZX, without copying tape/debugger state.
There are at most 16 simultaneous sessions.

The `next` profile uses an SD image containing its firmware. See the Next setup
below. A Next clone shares the original read-only SD image; writable originals
must use independently copied images. Machine selection clears breakpoints so
physical bank watches cannot silently refer to a different machine's banks.

Sessions advance only when an execution tool is called. `runFrames`, stepping,
run-to and audio capture have explicit frame/instruction and wall-clock budgets.
They return paused with actual counts and a stop reason. A timeout preserves
partial execution and cancels pending run control. Wall-clock checks occur
between scanlines or individual instructions. MCP cancellation cannot interrupt
a synchronous tool call; use short budgets for interactive investigation.

## Tool coverage

- Machine profiles and session creation, selection, cloning, status and destruction.
- Registers; logical and physical memory; bank mappings; masked searches; strings;
  comparison across sessions; binary import/export; filling and patching.
- Z80/Z80N assembly and disassembly; symbols and sjasmplus SLD import.
- BASIC and array compilation/decompilation, memory listings and program installation.
- Step, step-over/out, run-to, conditional execute/memory/I/O/register/raster/interrupt
  traps; breakpoint management; bounded trace; rewind; differential poke finding.
- Keyboard chords, held keys, NMI and emulated port reads/writes.
- Media loading/saving from files or supplied bytes; archive listing, extraction,
  editing and conversion; tape blocks, headers, checksums and transport controls.
- PNG screenshots, SCR rendering, row-major 1bpp sprite/tile atlases and stereo WAV capture.

`assemble` accepts newline-separated inline instructions and DB, using ZxKit's
debugger assembler. It is not a source-project assembler: labels, macros, ORG and
linking are not supported. Incomplete byte-array disassembly is marked as truncated.
`renderGraphics` assumes row-major 1bpp bytes; discovering compressed, masked,
interleaved or game-specific sprite/map formats requires investigation.

Logical addresses use the current paging. Physical bank IDs are specific to the
installed machine. Inspect mappings before comparing 128K code or setting
physical watchpoints. Symbol/source mappings are logical; use separate sessions
for different physical source pages. Host memory inspection bypasses emulated
accesses and watchpoints. Port handlers can have hardware side effects.

Relative output paths resolve under `--output-dir` (default `build/zx-mcp` under
the working directory). Existing files require `overwrite: true`. Inputs use
explicit host paths; files up to 64 MiB are accepted, request lines up to 4 MiB,
and inline reads are bounded. No application checkout is needed.

## Inspection details

BASIC decompilation returns `source` for lossless editing, `listing` for readable
visible text, `effectiveListing` with stored numeric values substituted, and
`numericConstants`/`numericMismatchCount` for hidden values that disagree with
visible digits. `trailingHex` preserves variables and nonstandard tails outside
the numbered listing. Complete reads of `basic/*.bas` archive entries also
include this inspection as `basic`; original archive text stays lossless.

`captureIo` records bounded timestamped port accesses and registers while running
frames. `captureAudio` includes up to 8192 output writes as `ioWrites`, with
truncation and total counts, plus clock/sample provenance for correlation.
Host traces use temporary `IoBus::subscribe`/`unsubscribe` registrations and
detach on success or failure; ordinary emulation retains no trace callbacks.

The initialize adapter follows the [MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle).
Tests drive the actual server process. This is not a claim of full conformance
for unadvertised protocol features.

## ZX Spectrum Next

`nextPlatformInfo` reports the default image path and cached state. The setup
flow follows Spectacol's resumable download and streamed extraction behavior,
using Boost.Beast/Asio with OpenSSL instead of Qt networking. It requires no
Spectacol files or checkout.

Call `prepareNextSdCard` with these arguments to download the tested distribution:

```json
{"url":"","entryName":"","output":"","maxMilliseconds":240000,"overwrite":false}
```

The default is the official `24.11` emulator ZIP, containing
`cspect-next-1gb.img`. Files live under `--output-dir/next`; an existing valid
image is reused. This pins a tested version and does not imply it is the latest.
`discoverNextDistribution` finds the current emulator ZIP on the
[official distribution page](https://www.specnext.com/latestdistro/); pass its
returned URL to preparation to choose a newer version. `entryName` can select
another image filename. Existing raw images can bypass downloading entirely.

Downloads have a 512 MiB archive limit and a caller-selected deadline of up to
300000 ms. Interrupted `.part` files retain URL/validator metadata and resume
using Range/If-Range. A full 200 response restarts instead of appending. Redirects
cannot downgrade HTTPS; peer certificates and hostnames are verified. Generic
`downloadFile` additionally accepts an expected SHA-256. Plain HTTP is limited
to loopback for local mirrors/tests. Downloads are synchronous; give the MCP
client a request timeout longer than the tool's budget.

ZIP extraction streams to a separate file, verifies CRC, mounts FAT and checks
the three required `/machines/next` ROM sizes before publication. Failed
preparation preserves the active image; replacing a valid image retains a
timestamped backup. A completed ZIP is cached for retries. Images currently
attached to sessions cannot be replaced.

After preparation, `createSession(sessionId, "next")` opens the default image
read-only. `createNextSession(sessionId, image, readOnly)` selects an explicit
image; an empty image path selects the default. SD storage is attached before
selecting the machine so ROMs can load from the card. `runFrames`, keyboard and
screenshot tools then drive NextZXOS normally.

Next tools inspect/write Next registers, attach/eject SD images and list, read,
write, rename or remove FAT files/directories. Multiple sessions may share a
read-only image. A writable image cannot be shared, even with a read-only session;
use a separate copy. Reset after host filesystem edits before resuming guest
execution, since NextZXOS caches filesystem state.

Offline regression tests use local HTTP and a synthetic FAT image with fake ROM
bytes; they never download firmware or depend on an application. A separate manual
validation downloaded the official 1 GB image and booted to the NextZXOS menu with
simultaneous 48K, 128K and Next sessions.

## Portable agent skill

The [zx-mcp skill](skills/zx-mcp/SKILL.md) provides reusable development and
reverse-engineering workflows for any MCP-capable agent. It uses `SKILL.md`
with name/description frontmatter and Markdown instructions, without
vendor-specific configuration or dependencies. Copy the complete `skills/zx-mcp`
directory into your agent's supported skills directory, or load `SKILL.md`
manually and follow its references.

The skill includes a Python standard-library stdio client for agents that can
run code but have no native MCP connection. Installation places the complete
skill under `share/zxkit/zx-mcp/skills/zx-mcp` alongside the server guide.

## ROM notices

The 48K and two 128K banks are compiled with C++26 `#embed` directly from
`tests/fixtures/roms`. They remain copyright Amstrad and are not relicensed under
ZxKit's GPL. Preserve `tests/fixtures/roms/README.copyright`; installation copies
it as `share/zxkit/zx-mcp/ROM-NOTICES` alongside this guide.
