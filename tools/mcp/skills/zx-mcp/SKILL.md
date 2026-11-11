---
name: zx-mcp
description: Develop, debug and reverse engineer ZX Spectrum software through zx-mcp. Use for emulator sessions, tape and snapshot inspection, Z80 or BASIC analysis, asset extraction, and comparisons between 48K, 128K and Next machines.
---

# ZX MCP

Use the live emulator to establish behavior, preserve a reference state, and
test changes in an independent session. These instructions work with any agent
that can call MCP tools; no particular LLM, editor or agent framework is required.

## Connect and discover

Use the connected zx-mcp server when available. Discover its tool catalog and
input schemas before composing calls; client-specific prefixes may differ from
the server's tool names. Pass every required property, including empty strings
when the schema permits them. Addresses are JSON integers; binary data uses
hexadecimal byte pairs.

If only the executable is available, read [stdio.md](references/stdio.md) for
the portable Python client and a complete session-isolation example. The server
uses newline-delimited JSON-RPC over stdio. Keep one process alive across calls:
sessions and debugger state belong to that process. `zx-mcp --list-tools` prints
schemas without starting a persistent emulator session.

## Establish the machine and reference

- Inspect `listMachineProfiles` and existing `listSessions` before selecting a
  machine. The 48K/128K ROMs are embedded; other profiles may need external ROMs.
  Next firmware comes from its SD image; see [workflows.md](references/workflows.md).
- Prefer a 128K machine by default. Select 48K when the supplied file specifies
  or requires 48K, or when the task explicitly calls for a 48K machine.
- Give sessions explicit IDs, such as `reference` and `work`. Use explicit
  `sessionId` arguments when comparing machines rather than changing the default.
- Inspect supplied media before loading it. Use `loadMedia` for snapshots or
  programs; tapes may need `autoLoadTape` followed by bounded execution.
- Wait for an observed entry point, loader state or input loop. A fixed frame
  delay alone does not establish that the program is ready.
- Save an SZX checkpoint before modifying a live program. `cloneSession` copies
  machine state, not tape or debugger configuration. Keep baseline and modified
  results associated with their session IDs and media versions.

## Run and inspect deliberately

Execution tools have frame/instruction and wall-clock budgets. Inspect returned
counts, registers and stop reasons after each call; budget exhaustion is not a
breakpoint hit. Machine time advances only during execution calls. Repeated
screenshots or memory reads do not advance emulation.

Read `memoryMap` and `listMemoryBanks` before analyzing paged memory. Logical
addresses depend on the current mapping; physical bank IDs and offsets depend
on the machine. Hardware bank numbers are not necessarily MCP bank IDs. Capture
the relevant mappings with disassembly, memory exports and comparisons.

Use execute breakpoints or write watchpoints to test a specific hypothesis.
Bound instruction traces and I/O captures. Port reads can invoke hardware
handlers; prefer memory inspection or passive capture when observation should
leave device state alone. Remove investigation traps before unrelated runs.

## Select the relevant workflow

Read only the relevant section of [workflows.md](references/workflows.md):

- **Media and BASIC:** tape blocks, checksums, hidden numbers and lossless source.
- **Code and debugging:** inline assembly versus sjasmplus projects, symbols,
  watchpoints and experiments across sessions.
- **Graphics, maps and sound:** discovering data formats and capturing actual
  gameplay events rather than assuming a particular game's layout.
- **Next and port comparison:** SD setup/sharing, registers, memory paging and
  comparisons at equivalent program states.
- **Native display ports:** drawing contracts, direct display writes, sprite
  masks, interrupt/MMU preservation, sound timing and executable packaging.

## Preserve scope and report evidence

Resolve outputs under the task's build/output directory. Keep supplied media,
extracted assets, program-specific scripts and port sources separate from
generic MCP changes unless the user asks to include them. Use explicit inputs
instead of assuming another application's checkout or a private media collection.

Describe what was observed, what was inferred, and which tests used debugger
state seeds. An isolated routine call or seeded completion flag does not
demonstrate an uninterrupted playthrough. Report capture truncation and partial
execution. A useful handoff includes reproducible inputs, session/profile IDs,
relevant addresses with mappings, the experiment and its resulting artifacts.
