# Debugger run control and traps

Use `Engine::stepOver()`, `stepOut()` or `runTo(address)` after pausing the engine.
The asynchronous commands resume normal emulation and issue a `PauseEvent` with
`StepComplete` on arrival. `runTo` captures the current physical target bank/page;
step-over additionally requires the caller's stack pointer. Step-out follows nested
CALL/RST and accepted IRQ/NMI handlers, conditional returns and RETN/RETI, and stops
in the caller after the return instruction. Debug operations have no timeout: a
routine that never returns keeps running until the host pauses or another trap fires.
Manually constructed frames/stack pivots are not a general-purpose unwind format.

`Engine::setPaused(true)` cancels pending run control and acknowledges the pause
on the worker thread, including a quick resume/pause sequence. A breakpoint, reset,
machine switch or snapshot load also cancels it. Plain `Emulator` hosts can use
`stepInstruction()`, `stepOver()`, `stepOut()`, `runTo()`, `hasRunControl()` and
`cancelRunControl()` under their own synchronization. `stepInstruction()` reports
`StepComplete` or the actual watch reason; active RZX sessions reject direct
single stepping because bypassing the regular execution loop loses fetch tracking.
Async run commands use the regular RZX execution path.

Breakpoint masks match `(value & mask) == (address & mask)`. Memory/execute traps
can also constrain `bankId` and `page1k`; use mask `0x03ff` and the within-page
offset to follow a physical byte through paging and aliases. Bank IDs belong to
the installed machine; recreate physical watches when changing machine profiles.
`setBreakpointEnabled()` updates the MMU's watched slots; one-shot traps remove
themselves, and every matched breakpoint increments its hit count. Memory and port
watches stop after the instruction containing the access. `peek`/`poke` and the
snapshot/memory inspection APIs bypass watches and emulated I/O handlers.

`NextRegWrite` observes register writes from ports, Z80N instructions and Copper.
`RasterLine` stops at the beginning of the selected zero-based line, after hardware
scanline setup and before its CPU instructions. `Event` accepts `DebugEvent::Interrupt`
or `DebugEvent::Nmi` and stops after acceptance at the handler entry. `PauseEvent`
contains the reason, PC, cycle and watched address/port/register/line/event in `address`.
The first trap in an instruction retains its reason.

`Mmu::banks()` exposes const bank metadata for host inspectors; use `effectiveMapping`
to distinguish effective read/write banks/pages and overlays. Next register inspectors
should use `NextReg::peek`, rather than an emulated port read or `NextReg::read`.

With debugging inactive, memory retains its existing fast path. The per-instruction
debug hook is enabled only for execution traps, tracing or active run control.
Next-register and interrupt observers are installed only when needed. No UI,
network, Qt or consumer-checkout dependency is introduced.

## Assembler source mappings

`SymbolTable::loadSldText()` accepts sjasmplus SLD v1 records:
`source|line[:column[:end]]|definitionFile|definitionLine|page|value|type|data`.
It imports `T` instruction locations and `L` label names, joining module/main/local
name parts. CRLF and the older seven-field shorthand are accepted. Malformed numeric
fields and addresses outside 0–65535 are ignored rather than narrowed into a valid address.

`SymbolTable` indexes logical 16-bit addresses. A host importing banked SLD data must
keep separate tables per source memory page and match them against effective MMU
mappings before annotating addresses. Source file names are metadata: importing SLD
does not execute code or load the referenced source files.
