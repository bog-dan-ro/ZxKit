// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Support.h"
#include <mcp++/annotations.hpp>
#include <zxkit/core/Emulator.h>
#include <exception>
#include <optional>
#include <map>
#include <memory>

namespace zxkit::mcp_tool {
class Tools
{
public:
    Tools(std::filesystem::path outputDirectory, std::filesystem::path romDirectory);

    [[= mcp::tool_t{ "List machine profiles, with embedded-ROM readiness. Other profiles require "
                     "--roms DIR." }]] std::string listMachineProfiles();
    [[= mcp::tool_t{ "Create an independent paused emulator. IDs must be unique. Embedded "
                     "profiles: 48k, 128k." }]] std::string createSession(std::string sessionId,
                                                                          std::string profile);
    [[= mcp::tool_t{ "List all emulators and the selected default. Sessions advance only during "
                     "execution tools." }]] std::string listSessions();
    [[= mcp::tool_t{ "Select the default emulator. Every session tool accepts an explicit ID; "
                     "empty ID uses this default." }]] std::string
            useSession(std::string sessionId);
    [[= mcp::tool_t{ "Destroy one emulator and its private debugger/media state." }]] std::string
            destroySession(std::string sessionId);
    [[= mcp::tool_t{ "Clone machine state through an SZX snapshot. Tape and debugger configuration "
                     "are not copied." }]] std::string cloneSession(std::string sessionId,
                                                                    std::string newSessionId);
    [[= mcp::tool_t{
            "Inspect a session's machine, CPU, tape, cycles and last stop reason." }]] std::string
            sessionStatus(std::string sessionId);
    [[= mcp::tool_t{
            "Change the machine profile and clear breakpoints. Extra ROMs are resolved through "
            "--roms DIR." }]] std::string selectMachine(std::string sessionId, std::string profile);
    [[= mcp::tool_t{ "Reset an emulator; hard=true reloads ROMs and clears RAM according to the "
                     "machine." }]] std::string resetSession(std::string sessionId, bool hard);
    [[= mcp::tool_t{ "Get all Z80 registers and interrupt state." }]] std::string
            getRegisters(std::string sessionId);
    [[= mcp::tool_t{ "Set a Z80 register by name: AF/BC/DE/HL, alternate AF_/BC_/DE_/HL_, "
                     "IX/IY/SP/PC/WZ, I/R/IM/IFF1/IFF2, or A/F/B/C/D/E/H/L." }]] std::string
            setRegister(std::string sessionId, std::string name, int value);
    [[= mcp::tool_t{ "Side-effect-free logical memory read. Addresses are decimal JSON integers; "
                     "bytes are hexadecimal." }]] std::string peekMemory(std::string sessionId,
                                                                         int address, int length);
    [[= mcp::tool_t{ "Patch logical memory with hexadecimal byte pairs. Crossing 65536 is "
                     "rejected. ROM patching requires allowRomWrite=true." }]] std::string
            pokeMemory(std::string sessionId, int address, std::string hexData, bool allowRomWrite);
    [[= mcp::tool_t{ "Fill a logical RAM range with one byte; ROM is protected." }]] std::string
            fillMemory(std::string sessionId, int address, int length, int value);
    [[= mcp::tool_t{ "Search logical memory for hex bytes. Empty mask matches exactly; otherwise "
                     "(byte & mask)==(pattern & mask). Reports truncation." }]] std::string
            searchMemory(std::string sessionId, int address, int length, std::string hexPattern,
                         std::string hexMask, int maxResults);
    [[= mcp::tool_t{ "Find printable ASCII strings in a logical memory range, with their "
                     "addresses." }]] std::string findStrings(std::string sessionId, int address,
                                                              int length, int minimumLength,
                                                              int maxResults);
    [[= mcp::tool_t{ "Compare logical memory between two independent sessions. Returns differing "
                     "runs and a truncation flag." }]] std::string
            compareMemory(std::string sessionId, std::string otherSessionId, int address,
                          int length, int maxResults);
    [[= mcp::tool_t{ "List physical RAM/ROM banks, IDs, sizes and write protection. IDs are "
                     "machine-specific." }]] std::string listMemoryBanks(std::string sessionId);
    [[= mcp::tool_t{ "Inspect the effective read/write bank/page mappings of all 64 logical 1 KiB "
                     "slots." }]] std::string memoryMap(std::string sessionId);
    [[= mcp::tool_t{ "Read physical bank bytes, including paged-out 128K memory. Maximum 65536 "
                     "bytes per request." }]] std::string
            readMemoryBank(std::string sessionId, int bankId, int offset, int length);
    [[= mcp::tool_t{ "Patch a physical bank, including paged-out RAM. ROM writes require explicit "
                     "opt-in." }]] std::string writeMemoryBank(std::string sessionId, int bankId,
                                                               int offset, std::string hexData,
                                                               bool allowRomWrite);
    [[= mcp::tool_t{ "Export logical bytes to a binary file. Relative output paths resolve under "
                     "--output-dir." }]] std::string exportMemory(std::string sessionId,
                                                                  int address, int length,
                                                                  std::string path, bool overwrite);
    [[= mcp::tool_t{ "Import a binary file into logical RAM without changing CPU state. The "
                     "complete range is validated first." }]] std::string
            importMemory(std::string sessionId, int address, std::string path);
    [[= mcp::tool_t{ "Disassemble Z80/Z80N instructions from logical memory, with bytes, symbols "
                     "and source locations." }]] std::string
            disassembleMemory(std::string sessionId, int address, int count);
    [[= mcp::tool_t{ "Disassemble caller-provided hex bytes at an origin. Incomplete trailing "
                     "instructions are marked; no emulator is changed." }]] std::string
            disassembleData(std::string hexData, int origin, bool z80n);
    [[= mcp::tool_t{
            "Assemble newline-separated Z80/Z80N instructions at an origin. Supports inline "
            "instructions and DB; no labels/macros/directives. Returns bytes." }]] std::string
            assemble(std::string source, int origin, bool z80n);
    [[= mcp::tool_t{ "Assemble and patch logical RAM atomically after all instructions validate. "
                     "No labels/macros/directives." }]] std::string
            assembleToMemory(std::string sessionId, std::string source, int address);
    [[= mcp::tool_t{ "Compile numbered Spectrum BASIC text into lossless tokenized hex. Dialect: "
                     "48, 128, next." }]] std::string compileBasic(std::string source,
                                                                   std::string dialect);
    [[= mcp::tool_t{ "Decompile supplied tokenized BASIC hex, preserving control codes and stored "
                     "variables. Dialect: 48, 128, next." }]] std::string
            decompileBasic(std::string hexData, std::string dialect);
    [[= mcp::tool_t{ "Compile numbered BASIC to logical RAM. Does not update BASIC system "
                     "variables; use setBasicProgram to install a runnable program." }]] std::string
            compileBasicToMemory(std::string sessionId, std::string source, int address,
                                 std::string dialect);
    [[= mcp::tool_t{ "List tokenized BASIC from a logical memory range. Use getBasicProgram for "
                     "the current interpreter program." }]] std::string
            decompileBasicMemory(std::string sessionId, int address, int length,
                                 std::string dialect);
    [[= mcp::tool_t{ "Read PROG/VARS/E_LINE and decompile the interpreter's current BASIC program "
                     "and variables." }]] std::string getBasicProgram(std::string sessionId);
    [[= mcp::tool_t{ "Install BASIC at PROG in a booted 48K-compatible interpreter and reset "
                     "variable/workspace pointers. Clears old variables." }]] std::string
            setBasicProgram(std::string sessionId, std::string source);
    [[= mcp::tool_t{ "Compile Spectrum numeric or character array text. Dimensions first, then "
                     "values; returns hex." }]] std::string compileBasicArray(std::string source,
                                                                              bool characterArray);
    [[= mcp::tool_t{
            "Decompile Spectrum numeric or character array bytes to editable text." }]] std::string
            decompileBasicArray(std::string hexData, bool characterArray);
    [[= mcp::tool_t{ "Run up to frameCount frames or a breakpoint/time budget. maxMilliseconds: "
                     "1..30000; frameCount: 1..10000. Always returns paused." }]] std::string
            runFrames(std::string sessionId, int frameCount, int maxMilliseconds);
    [[= mcp::tool_t{ "Single-step up to count instructions, stopping on a watchpoint. count: "
                     "1..100000, maxMilliseconds: 1..30000." }]] std::string
            stepInstructions(std::string sessionId, int count, int maxMilliseconds);
    [[= mcp::tool_t{ "Run to a logical address with its current physical bank mapping. Budget "
                     "exhaustion cancels this run control." }]] std::string
            runToAddress(std::string sessionId, int address, int maxFrames, int maxMilliseconds);
    [[= mcp::tool_t{ "Step over a CALL/RST/repeated block instruction, with frame and wall-clock "
                     "bounds." }]] std::string stepOver(std::string sessionId, int maxFrames,
                                                        int maxMilliseconds);
    [[= mcp::tool_t{ "Step out of a routine, accounting for nested calls and interrupts, with "
                     "frame and wall-clock bounds." }]] std::string
            stepOut(std::string sessionId, int maxFrames, int maxMilliseconds);
    [[= mcp::tool_t{ "Add execute/read/write/in/out/nextreg/raster/event breakpoint. "
                     "bankId/page1k=-1 means any. condition is empty or REG==VALUE / != / < / > / "
                     "<= / >=, decimal or 0x hex." }]] std::string
            addBreakpoint(std::string sessionId, std::string type, int address, int mask,
                          int bankId, int page1k, bool temporary, std::string condition);
    [[= mcp::tool_t{ "List session breakpoints, types, hit counts, physical constraints and "
                     "conditions." }]] std::string listBreakpoints(std::string sessionId);
    [[= mcp::tool_t{ "Remove a breakpoint by ID." }]] std::string
            removeBreakpoint(std::string sessionId, int breakpointId);
    [[= mcp::tool_t{ "Enable or disable a breakpoint by ID." }]] std::string
            enableBreakpoint(std::string sessionId, int breakpointId, bool enabled);
    [[= mcp::tool_t{ "Remove all breakpoints and watchpoints in one session." }]] std::string
            clearBreakpoints(std::string sessionId);
    [[= mcp::tool_t{ "Clear the previous trace and enable bounded instruction tracing, keeping at "
                     "most capacity entries "
                     "(1..100000)." }]] std::string configureTrace(std::string sessionId,
                                                                   bool enabled, int capacity);
    [[= mcp::tool_t{ "Read the latest count instruction trace entries with registers and cycle "
                     "stamps." }]] std::string readTrace(std::string sessionId, int count);
    [[= mcp::tool_t{ "Add a logical-address symbol. Symbols annotate disassembly; physical paging "
                     "is reported separately." }]] std::string
            setSymbol(std::string sessionId, int address, std::string name);
    [[= mcp::tool_t{ "Import sjasmplus SLD source mappings supplied as text. Addresses are "
                     "logical; banked source tables require separate sessions." }]] std::string
            loadSymbols(std::string sessionId, std::string sldText);
    [[= mcp::tool_t{ "Enable frame rewind with bounded capacity (1..300) and the library's memory "
                     "budget." }]] std::string configureRewind(std::string sessionId, bool enabled,
                                                               int capacity);
    [[= mcp::tool_t{
            "Restore an earlier captured frame, preserving this session's identity." }]] std::string
            rewindFrames(std::string sessionId, int count);
    [[= mcp::tool_t{ "Reset differential poke-finder candidates to writable RAM." }]] std::string
            resetPokeFinder(std::string sessionId);
    [[= mcp::tool_t{ "Filter poke candidates by exact byte value, incremented or decremented. "
                     "mode: value/incremented/decremented. Reports truncation." }]] std::string
            findPokes(std::string sessionId, std::string mode, int value, int maxResults);
    [[= mcp::tool_t{ "Read an emulated port. Hardware handlers run and may have side effects; this "
                     "is not a side-effect-free memory peek." }]] std::string
            readPort(std::string sessionId, int port);
    [[= mcp::tool_t{ "Write an emulated port, e.g. border, AY or 128K memory paging. Hardware "
                     "state changes immediately." }]] std::string writePort(std::string sessionId,
                                                                            int port, int value);
    [[= mcp::tool_t{ "List valid keyboard names for setKey/tapKeys." }]] std::string listKeys();
    [[= mcp::tool_t{
            "Hold or release one keyboard key by name, e.g. SPACE, ENTER, CS, SS, A, 0." }]] std::
            string setKey(std::string sessionId, std::string key, bool pressed);
    [[= mcp::tool_t{ "Press a key chord such as CS+8 for holdFrames, then release and run "
                     "releaseFrames. Stops early on debugger traps." }]] std::string
            tapKeys(std::string sessionId, std::string keys, int holdFrames, int releaseFrames);
    [[= mcp::tool_t{ "Trigger a non-maskable interrupt in one session." }]] std::string
            triggerNmi(std::string sessionId);
    [[= mcp::tool_t{ "Load a tape, snapshot, disk, screen or recording from a host path into one "
                     "emulator." }]] std::string loadMedia(std::string sessionId, std::string path);
    [[= mcp::tool_t{
            "Load caller-provided media hex; extension includes '.', e.g. .tap or .szx." }]] std::
            string loadMediaData(std::string sessionId, std::string hexData, std::string extension);
    [[= mcp::tool_t{
            "Save current media/state by filename extension (.szx/.z80/.sna/.tap/.tzx/.png/.nex). "
            "Relative paths use --output-dir." }]] std::string
            saveMedia(std::string sessionId, std::string path, bool overwrite);
    [[= mcp::tool_t{
            "List files and editable BASIC listings inside a tape/disk/archive. filesystem may be "
            "empty, or a disk profile from listMediaProfiles." }]] std::string
            inspectMedia(std::string path, std::string filesystem);
    [[= mcp::tool_t{ "List disk filesystem/geometry profiles accepted by inspectMedia and media "
                     "archive tools." }]] std::string listMediaProfiles();
    [[= mcp::tool_t{
            "Read a tape/disk/archive entry as hex or text. Offset/length support paginated "
            "inspection; length=0 means remaining, up to 65536 bytes." }]] std::string
            readMediaEntry(std::string path, std::string entry, std::string filesystem, int offset,
                           int length, bool asText);
    [[= mcp::tool_t{ "Extract a complete archive entry to a host file, preserving the original "
                     "media." }]] std::string extractMediaEntry(std::string path, std::string entry,
                                                                std::string filesystem,
                                                                std::string output, bool overwrite);
    [[= mcp::tool_t{ "Convert media containers (including TAP to WAV) to a new output; filesystem "
                     "may be empty." }]] std::string convertMedia(std::string path,
                                                                  std::string filesystem,
                                                                  std::string output,
                                                                  bool overwrite);
    [[= mcp::tool_t{ "Create a tape/disk/archive image. Extension and filesystem profile determine "
                     "the format." }]] std::string
            createMedia(std::string output, std::string filesystem, bool overwrite);
    [[= mcp::tool_t{ "Add/replace an archive entry from hex and write a separate output. "
                     "attributesJson is an object, e.g. {\"type\":3,\"param1\":32768} for a tape "
                     "CODE file." }]] std::string putMediaEntry(std::string path, std::string entry,
                                                                std::string filesystem,
                                                                std::string hexData,
                                                                std::string attributesJson,
                                                                std::string output, bool overwrite);
    [[= mcp::tool_t{ "Remove an archive entry and write a separate output image." }]] std::string
            removeMediaEntry(std::string path, std::string entry, std::string filesystem,
                             std::string output, bool overwrite);
    [[= mcp::tool_t{ "Inspect the loaded tape position, playback options and checksum validity of "
                     "every data block." }]] std::string inspectTape(std::string sessionId);
    [[= mcp::tool_t{ "Read one loaded tape block as hex, optionally removing its Spectrum flag and "
                     "checksum bytes." }]] std::string
            readTapeBlock(std::string sessionId, int blockIndex, bool payloadOnly);
    [[= mcp::tool_t{
            "Seek to a loaded tape block; index equal to block count selects end of tape." }]] std::
            string seekTape(std::string sessionId, int blockIndex);
    [[= mcp::tool_t{ "Press play or stop on the loaded tape deck." }]] std::string
            playTape(std::string sessionId, bool playing);
    [[= mcp::tool_t{ "Configure tape ROM traps, loader acceleration, automatic play and loading "
                     "sound." }]] std::string configureTape(std::string sessionId, bool romTraps,
                                                            bool accelerateLoaders, bool autoPlay,
                                                            bool loadingSound);
    [[= mcp::tool_t{ "Reset and schedule ROM keyboard loading from a tape block. Follow with "
                     "bounded runFrames calls." }]] std::string autoLoadTape(std::string sessionId,
                                                                             int blockIndex);
    [[= mcp::tool_t{ "Capture the current screen as PNG; refreshes rendering without advancing the "
                     "CPU." }]] std::string captureScreen(std::string sessionId, std::string path,
                                                          bool overwrite);
    [[= mcp::tool_t{ "Render caller-provided 6912-byte Spectrum SCR data to PNG." }]] std::string
            renderScreen(std::string hexData, std::string path, bool overwrite);
    [[= mcp::tool_t{
            "Decode row-major 1bpp graphics from memory to a PNG atlas. width must be a multiple "
            "of 8; stride=0 means width/8. No game-specific format is assumed." }]] std::string
            renderGraphics(std::string sessionId, int address, int width, int height, int count,
                           int stride, int columns, std::string path, bool overwrite);
    [[= mcp::tool_t{
            "Capture emulated stereo audio to PCM16 WAV while advancing bounded frames. Includes "
            "beeper and AY. Returns actual frame/sample counts and stop reason." }]] std::string
            captureAudio(std::string sessionId, int frameCount, int maxMilliseconds,
                         std::string path, bool overwrite);

    [[= mcp::tool_t{
            "Capture timestamped emulated I/O accesses while advancing bounded frames. "
            "Matches (port & mask)==(value & mask). maxEvents: 1..65536. Reports total "
            "matches and truncation. Trace subscriptions are removed after the call." }]] std::
            string captureIo(std::string sessionId, int frameCount, int maxMilliseconds, int mask,
                             int value, bool includeReads, bool includeWrites, int maxEvents);

    [[= mcp::tool_t{ "ZX Spectrum Next setup: cached image readiness, official distribution URLs, "
                     "required ROMs and the default SD path. No network access." }]] std::string
            nextPlatformInfo();
    [[= mcp::tool_t{ "Discover the emulator ZIP link from the official distro page (empty pageUrl "
                     "uses specnext.com/latestdistro). HTTPS is verified; loopback HTTP supports "
                     "local mirrors." }]] std::string discoverNextDistribution(std::string pageUrl,
                                                                               int maxMilliseconds);
    [[= mcp::tool_t{
            "Stream a resumable HTTPS download to a file, with deadlines, redirects and "
            "optional SHA-256 validation. HTTP is restricted to loopback. Partial files survive "
            "interruption; overwrite restarts and backs up any previous completed file." }]] std::
            string downloadFile(std::string url, std::string output, std::string expectedSha256,
                                int maxMilliseconds, bool overwrite);
    [[= mcp::tool_t{
            "Download/cache a Next emulator ZIP, stream-extract and validate its SD image. "
            "Empty URL uses Spectacol's tested 24.11 source; empty output uses "
            "next/cspect-next-1gb.img. "
            "Empty entryName uses cspect-next-1gb.img. Existing valid images are reused unless "
            "overwrite=true." }]] std::string
            prepareNextSdCard(std::string url, std::string entryName, std::string output,
                              int maxMilliseconds, bool overwrite);
    [[= mcp::tool_t{ "Create a ZX Spectrum Next session using firmware on a FAT SD image. Empty "
                     "image uses the prepared default. Use readOnly=true to share a reference "
                     "image safely." }]] std::string
            createNextSession(std::string sessionId, std::string image, bool readOnly);
    [[= mcp::tool_t{
            "Attach an existing raw SD image. Reset the session after attachment to reload "
            "firmware. A writable image cannot be shared with another session." }]] std::string
            insertSdCard(std::string sessionId, std::string image, bool readOnly);
    [[= mcp::tool_t{ "Eject the session's SD image." }]] std::string
            ejectSdCard(std::string sessionId);
    [[= mcp::tool_t{ "Side-effect-free inspection of all 256 Next registers." }]] std::string
            readNextRegisters(std::string sessionId);
    [[= mcp::tool_t{ "Write a Next register with normal hardware side effects." }]] std::string
            writeNextRegister(std::string sessionId, int index, int value);
    [[= mcp::tool_t{
            "List files and directories inside the attached FAT SD image. Paths use '/'." }]] std::
            string listSdFiles(std::string sessionId, std::string path);
    [[= mcp::tool_t{
            "Read an attached SD file as bounded hex/text. offset/length paginate; length=0 "
            "reads the remaining bytes up to 65536. Reports truncation." }]] std::string
            readSdFile(std::string sessionId, std::string path, int offset, int length,
                       bool asText);
    [[= mcp::tool_t{
            "Write hex data to an SD file. Its parent must exist and the image must be "
            "writable. Reset after host edits to invalidate the guest's filesystem cache." }]] std::
            string writeSdFile(std::string sessionId, std::string path, std::string hexData);
    [[= mcp::tool_t{ "Create an SD directory and missing parents. Requires a writable image; reset "
                     "after edits." }]] std::string makeSdDirectory(std::string sessionId,
                                                                    std::string path);
    [[= mcp::tool_t{ "Remove an SD file or empty directory. Requires a writable image; reset after "
                     "edits." }]] std::string removeSdFile(std::string sessionId, std::string path);
    [[= mcp::tool_t{ "Rename/move an SD file or directory. Requires a writable image; reset after "
                     "edits." }]] std::string renameSdFile(std::string sessionId, std::string path,
                                                           std::string destination);

private:
    struct Session
    {
        std::string id;
        std::unique_ptr<Emulator> emulator;
        std::optional<PauseEvent> lastPause;
        std::map<uint32_t, std::string> conditions;
    };
    std::map<std::string, Session, std::less<>> m_sessions;
    std::string m_current;
    std::filesystem::path m_outputDirectory;
    std::filesystem::path m_romDirectory;
    Session &session(std::string_view id);
    EmulatorConfig config(std::string profile) const;
    Json status(Session &current) const;
    Json execute(Session &current, int frames, int milliseconds, OptionalRef<Bytes> audio = {});
    std::filesystem::path outputPath(std::string_view path) const;
    void checkSdSharing(const std::filesystem::path &image, bool readOnly,
                        std::string_view exceptSession = {}) const;
    template <typename Function> std::string result(Function function)
    {
        try {
            return boost::json::serialize(Json(function()));
        } catch (const std::exception &error) {
            return boost::json::serialize(Object{ { "error", error.what() } });
        }
    }
};
} // namespace zxkit::mcp_tool
