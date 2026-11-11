// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"
#include "BasicListing.h"

#include <zxkit/debug/Assembler.h>
#include <zxkit/media/BasicCodec.h>
#include <zxkit/media/MediaArchive.h>
#include <zxkit/media/MediaPreview.h>
#include <zxkit/media/ScreenshotFormat.h>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <charconv>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
Object registersJson(const Z80Registers &registers)
{
    return { { "AF", registers.AF },
             { "BC", registers.BC },
             { "DE", registers.DE },
             { "HL", registers.HL },
             { "AF_", registers.AF_ },
             { "BC_", registers.BC_ },
             { "DE_", registers.DE_ },
             { "HL_", registers.HL_ },
             { "IX", registers.IX },
             { "IY", registers.IY },
             { "SP", registers.SP },
             { "PC", registers.PC },
             { "WZ", registers.WZ },
             { "I", registers.I },
             { "R", registers.R | registers.R7 },
             { "IM", registers.IM },
             { "IFF1", registers.IFF1 },
             { "IFF2", registers.IFF2 },
             { "halted", registers.halted },
             { "eiDelayed", registers.eiDelayed } };
}

uint16_t word(Emulator &emulator, uint16_t address)
{
    return uint16_t(emulator.mmu().peek(address) | (emulator.mmu().peek(address + 1) << 8));
}

void setWord(Emulator &emulator, uint16_t address, uint16_t value)
{
    emulator.writeMemory(address, { uint8_t(value), uint8_t(value >> 8) });
}

BasicDialect basicDialect(std::string_view dialect)
{
    if (dialect == "48"sv)
        return BasicDialect::Spectrum48;
    if (dialect == "128"sv)
        return BasicDialect::Spectrum128;
    if (dialect == "next"sv)
        return BasicDialect::Next;
    throw std::invalid_argument("BASIC dialect must be 48, 128 or next"s);
}

Bytes assembled(std::string_view source, int origin, bool z80n)
{
    checked(origin, 0, 65535, "origin"sv);
    Bytes bytes;
    std::istringstream stream{ std::string(source) };
    std::string line;
    int lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        auto encoded = Assembler::assemble(line, uint16_t(origin + bytes.size()), { z80n, 10 });
        if (!encoded)
            throw std::invalid_argument("Assembly line "s + std::to_string(lineNumber) + ": "s
                                        + encoded.error());
        if (bytes.size() + encoded->size() > std::size_t(65536 - origin))
            throw std::invalid_argument("Assembly crosses the 16-bit address space"s);
        bytes.insert(bytes.end(), encoded->begin(), encoded->end());
    }
    return bytes;
}

Json instructionJson(const DisasmInstruction &instruction)
{
    return Object{ { "address", instruction.address },
                   { "length", instruction.length },
                   { "hex", hex(std::span(instruction.bytes).first(instruction.length)) },
                   { "mnemonic", instruction.mnemonic } };
}

const std::map<std::string, ZxKey, std::less<>> &keyNames()
{
    static const std::map<std::string, ZxKey, std::less<>> names = {
        { "CS"s, ZxKey::CapsShift }, { "SS"s, ZxKey::SymbolShift },
        { "SPACE"s, ZxKey::Space },  { "ENTER"s, ZxKey::Enter },
        { "0"s, ZxKey::Num0 },       { "1"s, ZxKey::Num1 },
        { "2"s, ZxKey::Num2 },       { "3"s, ZxKey::Num3 },
        { "4"s, ZxKey::Num4 },       { "5"s, ZxKey::Num5 },
        { "6"s, ZxKey::Num6 },       { "7"s, ZxKey::Num7 },
        { "8"s, ZxKey::Num8 },       { "9"s, ZxKey::Num9 },
        { "A"s, ZxKey::A },          { "B"s, ZxKey::B },
        { "C"s, ZxKey::C },          { "D"s, ZxKey::D },
        { "E"s, ZxKey::E },          { "F"s, ZxKey::F },
        { "G"s, ZxKey::G },          { "H"s, ZxKey::H },
        { "I"s, ZxKey::I },          { "J"s, ZxKey::J },
        { "K"s, ZxKey::K },          { "L"s, ZxKey::L },
        { "M"s, ZxKey::M },          { "N"s, ZxKey::N },
        { "O"s, ZxKey::O },          { "P"s, ZxKey::P },
        { "Q"s, ZxKey::Q },          { "R"s, ZxKey::R },
        { "S"s, ZxKey::S },          { "T"s, ZxKey::T },
        { "U"s, ZxKey::U },          { "V"s, ZxKey::V },
        { "W"s, ZxKey::W },          { "X"s, ZxKey::X },
        { "Y"s, ZxKey::Y },          { "Z"s, ZxKey::Z },
        { "CTRL"s, ZxKey::Ctrl },    { "ESC"s, ZxKey::Esc },
        { "LF"s, ZxKey::LineFeed },  { "NOSCROLL"s, ZxKey::NoScroll },
        { "F1"s, ZxKey::F1 },        { "F2"s, ZxKey::F2 },
        { "F3"s, ZxKey::F3 },        { "F4"s, ZxKey::F4 }
    };
    return names;
}

int registerValue(const Z80Registers &registers, std::string name)
{
    name = upper(std::move(name));
    auto values = registersJson(registers);
    if (auto value = values.if_contains(name); value && (value->is_int64() || value->is_uint64()))
        return boost::json::value_to<int>(*value);
    if (name == "A"sv)
        return registers.A();
    if (name == "F"sv)
        return registers.F();
    if (name == "B"sv)
        return registers.B();
    if (name == "C"sv)
        return registers.C();
    if (name == "D"sv)
        return registers.D();
    if (name == "E"sv)
        return registers.E();
    if (name == "H"sv)
        return registers.H();
    if (name == "L"sv)
        return registers.L();
    throw std::invalid_argument("Unknown register name"s);
}

std::function<bool(const Z80Registers &)> breakpointCondition(std::string text)
{
    if (text.empty())
        return {};
    std::erase_if(text, [](unsigned char character) { return std::isspace(character); });
    auto position = text.find_first_of("=!<>");
    if (position == std::string::npos)
        throw std::invalid_argument("Condition must use REG==VALUE, !=, <, >, <= or >="s);
    auto name = text.substr(0, position);
    registerValue(Z80Registers{}, name);
    auto end = text.find_first_not_of("=!<>", position);
    auto operation = text.substr(position, end - position);
    if (end == std::string::npos
        || (operation != "=="sv && operation != "!="sv && operation != "<"sv && operation != ">"sv
            && operation != "<="sv && operation != ">="sv))
        throw std::invalid_argument("Invalid comparison in condition"s);
    std::string_view number(text.data() + end, text.size() - end);
    int base = 10;
    if (number.starts_with("0x"sv)) {
        number.remove_prefix(2);
        base = 16;
    }
    int expected = 0;
    auto parsed = std::from_chars(number.data(), number.data() + number.size(), expected, base);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
        throw std::invalid_argument("Invalid integer in breakpoint condition"s);
    checked(expected, 0, 65535, "condition value"sv);
    return [name, operation, expected](const Z80Registers &registers) {
        int actual = registerValue(registers, name);
        if (operation == "=="sv)
            return actual == expected;
        if (operation == "!="sv)
            return actual != expected;
        if (operation == "<"sv)
            return actual < expected;
        if (operation == ">"sv)
            return actual > expected;
        if (operation == "<="sv)
            return actual <= expected;
        return actual >= expected;
    };
}

void requireWritable(Emulator &emulator, int address, std::size_t length)
{
    memoryRange(address, int(length));
    for (std::size_t offset = 0; offset < length; ++offset) {
        if (!emulator.mmu().effectiveMapping((address + offset) / 1024).writable)
            throw std::invalid_argument("Range includes non-writable memory; no bytes changed"s);
    }
}

MediaArchive archive(const std::string &path, const std::string &filesystem)
{
    auto media =
            MediaFile::fromBytes(readFile(path), std::filesystem::path(path).extension().string());
    return MediaArchive::open(media.data, media.extension, { filesystem });
}

Object fileJson(const std::filesystem::path &path, std::size_t length)
{
    return { { "path", path.string() }, { "bytes", length } };
}

constexpr std::array breakpointNames = { "execute"sv, "read"sv,    "write"sv,  "in"sv,
                                         "out"sv,     "nextreg"sv, "raster"sv, "event"sv };
constexpr std::array pauseNames = { "user"sv,   "execute"sv, "memory"sv, "port"sv,  "nextreg"sv,
                                    "raster"sv, "event"sv,   "step"sv,   "magic"sv, "error"sv };
class ScopedIoTrace
{
public:
    ScopedIoTrace(Emulator &emulator, int mask, int value, bool reads, bool writes, int capacity)
        : m_bus(emulator.ioBus())
    {
        m_subscription = m_bus.subscribe([this, &emulator, mask, value, reads, writes,
                                          capacity](uint16_t port, uint8_t data, bool isWrite) {
            if ((port & mask) != (value & mask) || (isWrite ? !writes : !reads))
                return;
            ++m_count;
            if (m_events.size() < std::size_t(capacity))
                m_events.push_back(Object{ { "cycle", emulator.clock().totalTstates() },
                                           { "pc", emulator.registers().PC },
                                           { "port", port },
                                           { "value", data },
                                           { "write", isWrite },
                                           { "bc", emulator.registers().BC },
                                           { "de", emulator.registers().DE } });
        });
    }
    ~ScopedIoTrace()
    {
        m_bus.unsubscribe(m_subscription);
    }
    ScopedIoTrace(const ScopedIoTrace &) = delete;
    ScopedIoTrace &operator=(const ScopedIoTrace &) = delete;

    Json summary() const
    {
        return Object{ { "events", m_events },
                       { "totalEvents", m_count },
                       { "truncated", m_count > m_events.size() } };
    }

private:
    IoBus &m_bus;
    uint64_t m_subscription = 0;
    uint64_t m_count = 0;
    Array m_events;
};
} // namespace

Tools::Tools(std::filesystem::path outputDirectory, std::filesystem::path romDirectory)
    : m_outputDirectory(std::filesystem::absolute(outputDirectory)),
      m_romDirectory(std::move(romDirectory))
{
}

Tools::Session &Tools::session(std::string_view id)
{
    auto found = m_sessions.find(id.empty() ? std::string_view(m_current) : id);
    if (found == m_sessions.end())
        throw std::invalid_argument("Unknown session; call createSession/listSessions first"s);
    return found->second;
}

EmulatorConfig Tools::config(std::string profile) const
{
    EmulatorConfig configuration;
    configuration.machineId = std::move(profile);
    configuration.romResolver = [directory = m_romDirectory](std::string_view name,
                                                             std::size_t expectedSize) {
        auto bytes = embeddedRom(name);
        if (!directory.empty() && std::filesystem::is_regular_file(directory / name))
            bytes = readFile(directory / name);
        if (!bytes.empty() && bytes.size() != expectedSize)
            throw std::invalid_argument("Incorrect ROM size for "s + std::string(name));
        return bytes;
    };
    return configuration;
}

Json Tools::status(Session &current) const
{
    auto &emulator = *current.emulator;
    Object output{ { "sessionId", current.id },
                   { "profile", emulator.machine().id() },
                   { "paused", true },
                   { "registers", registersJson(emulator.registers()) },
                   { "cycles", emulator.clock().totalTstates() },
                   { "frame", emulator.clock().frameCount() },
                   { "tapeBlock", emulator.tapeDeck().currentBlockIndex() },
                   { "tapePlaying", emulator.tapeDeck().isPlaying() } };
    if (emulator.sdCard().isInserted()) {
        output["sdImage"] = emulator.sdCard().imagePath().string();
        output["sdReadOnly"] = emulator.sdCard().isReadOnly();
    }
    if (current.lastPause) {
        auto &pause = *current.lastPause;
        output["lastPause"] = Object{ { "reason", pauseNames.at(unsigned(pause.reason)) },
                                      { "address", pause.address },
                                      { "pc", pause.pc },
                                      { "cycle", pause.cycle } };
    }
    return output;
}

std::filesystem::path Tools::outputPath(std::string_view path) const
{
    if (path.empty())
        throw std::invalid_argument("Output path must not be empty"s);
    std::filesystem::path requested(path);
    return requested.is_absolute() ? requested : m_outputDirectory / requested;
}

Json Tools::execute(Session &current, int frames, int milliseconds, OptionalRef<Bytes> audio)
{
    checked(frames, 1, 10000, "frame count"sv);
    checked(milliseconds, 1, 30000, "maxMilliseconds"sv);
    auto &emulator = *current.emulator;
    current.lastPause.reset();
    emulator.takePauseEvent();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    struct RunControlGuard
    {
        Emulator &emulator;
        ~RunControlGuard()
        {
            emulator.cancelRunControl();
        }
    } guard{ emulator };
    int completed = 0;
    std::string reason = "frameBudget"s;
    while (completed < frames) {
        if (std::chrono::steady_clock::now() >= deadline) {
            reason = "timeBudget"s;
            break;
        }
        auto run = emulator.runScanline();
        if (run == RunStatus::Stopped) {
            current.lastPause = emulator.takePauseEvent();
            reason = "breakpoint"s;
            break;
        }
        if (run == RunStatus::FrameDone) {
            ++completed;
            if (audio) {
                for (int16_t sample : emulator.audioSamples()) {
                    audio->get().push_back(uint8_t(sample));
                    audio->get().push_back(uint8_t(uint16_t(sample) >> 8));
                }
            }
        }
    }
    auto output = status(current).as_object();
    output["completedFrames"] = completed;
    output["stopReason"] = reason;
    return output;
}

std::string Tools::listMachineProfiles()
{
    return result([&] {
        MachineRegistry registry;
        Array profiles;
        for (auto &id : registry.availableIds()) {
            auto machine = registry.create(id);
            profiles.push_back(
                    Object{ { "id", id },
                            { "name", machine->name() },
                            { "embeddedRoms", id == "48k"sv || id == "16k"sv || id == "128k"sv },
                            { "z80n", machine->supportsZ80n() },
                            { "requiresSdCard", id == "next"sv } });
        }
        return Object{ { "profiles", profiles } };
    });
}

std::string Tools::createSession(std::string sessionId, std::string profile)
{
    if (profile == "next"sv)
        return createNextSession(std::move(sessionId), {}, true);
    return result([&] {
        if (sessionId.empty() || sessionId.size() > 128 || m_sessions.contains(sessionId))
            throw std::invalid_argument(
                    "Session ID must be unique, nonempty and at most 128 bytes"s);
        if (m_sessions.size() >= 16)
            throw std::invalid_argument(
                    "Maximum 16 simultaneous sessions; destroy an unused session"s);
        Session created{
            sessionId, std::make_unique<Emulator>(config(std::move(profile))), {}, {}
        };
        auto [iterator, inserted] = m_sessions.emplace(sessionId, std::move(created));
        if (m_current.empty())
            m_current = sessionId;
        return status(iterator->second);
    });
}

std::string Tools::listSessions()
{
    return result([&] {
        Array sessions;
        for (auto &[id, current] : m_sessions)
            sessions.push_back(status(current));
        return Object{ { "currentSessionId", m_current }, { "sessions", sessions } };
    });
}

std::string Tools::useSession(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        m_current = current.id;
        return status(current);
    });
}

std::string Tools::destroySession(std::string sessionId)
{
    return result([&] {
        auto id = session(sessionId).id;
        m_sessions.erase(id);
        if (m_current == id)
            m_current = m_sessions.empty() ? std::string{} : m_sessions.begin()->first;
        return Object{ { "destroyed", id }, { "currentSessionId", m_current } };
    });
}

std::string Tools::cloneSession(std::string sessionId, std::string newSessionId)
{
    return result([&] {
        auto &original = session(sessionId);
        if (newSessionId.empty() || newSessionId.size() > 128 || m_sessions.contains(newSessionId)
            || m_sessions.size() >= 16)
            throw std::invalid_argument("Invalid/duplicate clone ID or session limit reached"s);
        const bool next = original.emulator->machine().id() == "next"sv;
        if (next
            && (!original.emulator->sdCard().isInserted()
                || !original.emulator->sdCard().isReadOnly()))
            throw std::invalid_argument(
                    "Cloning a Next session requires a read-only SD image; use an independent image for writable sessions"s);
        auto emulator = std::make_unique<Emulator>(
                config(next ? "48k"s : std::string(original.emulator->machine().id())));
        if (next) {
            emulator->insertSdCard(original.emulator->sdCard().imagePath(), true);
            emulator->selectMachine("next"sv);
        }
        emulator->loadSnapshot(original.emulator->saveSnapshot(".szx"sv), ".szx"sv);
        auto [iterator, inserted] = m_sessions.emplace(
                newSessionId, Session{ newSessionId, std::move(emulator), {}, {} });
        return status(iterator->second);
    });
}

std::string Tools::sessionStatus(std::string sessionId)
{
    return result([&] { return status(session(sessionId)); });
}

std::string Tools::selectMachine(std::string sessionId, std::string profile)
{
    return result([&] {
        auto &current = session(sessionId);
        current.emulator->selectMachine(profile);
        current.emulator->clearBreakpoints();
        current.lastPause.reset();
        current.conditions.clear();
        return status(current);
    });
}

std::string Tools::resetSession(std::string sessionId, bool hard)
{
    return result([&] {
        auto &current = session(sessionId);
        current.emulator->reset(hard ? ResetKind::Hard : ResetKind::Soft);
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::getRegisters(std::string sessionId)
{
    return result([&] { return registersJson(session(sessionId).emulator->registers()); });
}

std::string Tools::setRegister(std::string sessionId, std::string name, int value)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        auto registers = emulator.registers();
        name = upper(std::move(name));
        checked(value, 0, 65535, "value"sv);
        if (name == "AF"sv)
            registers.AF = value;
        else if (name == "BC"sv)
            registers.BC = value;
        else if (name == "DE"sv)
            registers.DE = value;
        else if (name == "HL"sv)
            registers.HL = value;
        else if (name == "AF_"sv)
            registers.AF_ = value;
        else if (name == "BC_"sv)
            registers.BC_ = value;
        else if (name == "DE_"sv)
            registers.DE_ = value;
        else if (name == "HL_"sv)
            registers.HL_ = value;
        else if (name == "IX"sv)
            registers.IX = value;
        else if (name == "IY"sv)
            registers.IY = value;
        else if (name == "SP"sv)
            registers.SP = value;
        else if (name == "PC"sv)
            registers.PC = value;
        else if (name == "WZ"sv)
            registers.WZ = value;
        else {
            checked(value, 0, name == "IM"sv ? 2 : name.starts_with("IFF"sv) ? 1 : 255, "value"sv);
            if (name == "I"sv)
                registers.I = value;
            else if (name == "R"sv) {
                registers.R = value & 127;
                registers.R7 = value & 128;
            } else if (name == "IM"sv)
                registers.IM = value;
            else if (name == "IFF1"sv)
                registers.IFF1 = value;
            else if (name == "IFF2"sv)
                registers.IFF2 = value;
            else if (name == "A"sv)
                registers.setA(value);
            else if (name == "F"sv)
                registers.setF(value);
            else if (name == "B"sv)
                registers.setB(value);
            else if (name == "C"sv)
                registers.setC(value);
            else if (name == "D"sv)
                registers.setD(value);
            else if (name == "E"sv)
                registers.setE(value);
            else if (name == "H"sv)
                registers.setH(value);
            else if (name == "L"sv)
                registers.setL(value);
            else
                throw std::invalid_argument("Unknown register name"s);
        }
        emulator.setRegisters(registers);
        return registersJson(emulator.registers());
    });
}

std::string Tools::peekMemory(std::string sessionId, int address, int length)
{
    return result([&] {
        memoryRange(address, length);
        return Object{ { "address", address },
                       { "length", length },
                       { "hex", hex(session(sessionId).emulator->readMemory(address, length)) } };
    });
}

std::string Tools::pokeMemory(std::string sessionId, int address, std::string hexData,
                              bool allowRomWrite)
{
    return result([&] {
        auto bytes = unhex(hexData);
        memoryRange(address, int(bytes.size()));
        auto &emulator = *session(sessionId).emulator;
        if (!allowRomWrite)
            requireWritable(emulator, address, bytes.size());
        emulator.writeMemory(address, bytes, allowRomWrite);
        return Object{ { "address", address }, { "written", bytes.size() } };
    });
}

std::string Tools::fillMemory(std::string sessionId, int address, int length, int value)
{
    return result([&] {
        memoryRange(address, length);
        checked(value, 0, 255, "value"sv);
        auto &emulator = *session(sessionId).emulator;
        requireWritable(emulator, address, length);
        emulator.writeMemory(address, Bytes(length, uint8_t(value)));
        return Object{ { "written", length } };
    });
}

std::string Tools::searchMemory(std::string sessionId, int address, int length,
                                std::string hexPattern, std::string hexMask, int maxResults)
{
    return result([&] {
        memoryRange(address, length);
        checked(maxResults, 1, 4096, "maxResults"sv);
        auto pattern = unhex(hexPattern);
        auto mask = hexMask.empty() ? Bytes(pattern.size(), 255) : unhex(hexMask);
        if (pattern.empty() || pattern.size() != mask.size())
            throw std::invalid_argument("Pattern must be nonempty; mask length must match"s);
        auto bytes = session(sessionId).emulator->readMemory(address, length);
        Array matches;
        bool truncated = false;
        for (std::size_t offset = 0; offset + pattern.size() <= bytes.size(); ++offset) {
            bool match = true;
            for (std::size_t index = 0; index < pattern.size() && match; ++index)
                match = (bytes[offset + index] & mask[index]) == (pattern[index] & mask[index]);
            if (match) {
                if (matches.size() == std::size_t(maxResults)) {
                    truncated = true;
                    break;
                }
                matches.push_back(address + offset);
            }
        }
        return Object{ { "addresses", matches }, { "truncated", truncated } };
    });
}

std::string Tools::findStrings(std::string sessionId, int address, int length, int minimumLength,
                               int maxResults)
{
    return result([&] {
        memoryRange(address, length);
        checked(minimumLength, 1, 65536, "minimumLength"sv);
        checked(maxResults, 1, 4096, "maxResults"sv);
        auto bytes = session(sessionId).emulator->readMemory(address, length);
        Array strings;
        bool truncated = false;
        for (std::size_t offset = 0; offset < bytes.size();) {
            auto start = offset;
            while (offset < bytes.size() && bytes[offset] >= 32 && bytes[offset] < 127)
                ++offset;
            if (offset - start >= std::size_t(minimumLength)) {
                if (strings.size() == std::size_t(maxResults)) {
                    truncated = true;
                    break;
                }
                strings.push_back(Object{
                        { "address", address + start },
                        { "text", std::string(bytes.begin() + start, bytes.begin() + offset) } });
            }
            if (offset < bytes.size())
                ++offset;
        }
        return Object{ { "strings", strings }, { "truncated", truncated } };
    });
}

std::string Tools::compareMemory(std::string sessionId, std::string otherSessionId, int address,
                                 int length, int maxResults)
{
    return result([&] {
        memoryRange(address, length);
        checked(maxResults, 1, 4096, "maxResults"sv);
        auto first = session(sessionId).emulator->readMemory(address, length);
        auto second = session(otherSessionId).emulator->readMemory(address, length);
        Array ranges;
        bool truncated = false;
        for (std::size_t offset = 0; offset < first.size();) {
            if (first[offset] == second[offset]) {
                ++offset;
                continue;
            }
            auto start = offset;
            while (offset < first.size() && first[offset] != second[offset])
                ++offset;
            if (ranges.size() == std::size_t(maxResults)) {
                truncated = true;
                break;
            }
            ranges.push_back(
                    Object{ { "address", address + start },
                            { "length", offset - start },
                            { "first", hex(std::span(first).subspan(start, offset - start)) },
                            { "second", hex(std::span(second).subspan(start, offset - start)) } });
        }
        return Object{ { "differences", ranges }, { "truncated", truncated } };
    });
}

std::string Tools::listMemoryBanks(std::string sessionId)
{
    return result([&] {
        Array banks;
        for (auto &bank : session(sessionId).emulator->mmu().banks())
            banks.push_back(Object{ { "id", bank.id() },
                                    { "name", bank.name() },
                                    { "size", bank.size() },
                                    { "writable", bank.isWritable() } });
        return Object{ { "banks", banks } };
    });
}

std::string Tools::memoryMap(std::string sessionId)
{
    return result([&] {
        auto &mmu = session(sessionId).emulator->mmu();
        Array slots;
        for (std::size_t slot = 0; slot < Mmu::kSlotCount; ++slot) {
            auto mapping = mmu.effectiveMapping(slot);
            slots.push_back(Object{ { "address", slot * 1024 },
                                    { "bankId", mapping.bankId },
                                    { "page1k", mapping.readPage1k },
                                    { "writeBankId", mapping.writeBankId },
                                    { "writePage1k", mapping.writePage1k },
                                    { "writable", mapping.writable },
                                    { "contended", mapping.contended },
                                    { "overlay", mmu.isOverlayActiveAt(slot * 1024) } });
        }
        return Object{ { "slots", slots } };
    });
}

std::string Tools::readMemoryBank(std::string sessionId, int bankId, int offset, int length)
{
    return result([&] {
        checked(bankId, 0, 65535, "bankId"sv);
        auto bytes = session(sessionId).emulator->mmu().bank(bankId).bytes();
        checked(offset, 0, int(bytes.size()), "offset"sv);
        checked(length, 0, int(std::min<std::size_t>(65536, bytes.size() - offset)), "length"sv);
        return Object{ { "bankId", bankId },
                       { "offset", offset },
                       { "hex", hex(bytes.subspan(offset, length)) } };
    });
}

std::string Tools::writeMemoryBank(std::string sessionId, int bankId, int offset,
                                   std::string hexData, bool allowRomWrite)
{
    return result([&] {
        checked(bankId, 0, 65535, "bankId"sv);
        auto &bank = session(sessionId).emulator->mmu().bank(bankId);
        auto bytes = unhex(hexData);
        checked(offset, 0, int(bank.size()), "offset"sv);
        checked(int(bytes.size()), 0, int(bank.size()) - offset, "data length"sv);
        if (!bank.isWritable() && !allowRomWrite)
            throw std::invalid_argument("Bank is read-only; allowRomWrite is required"s);
        bank.loadData(offset, bytes);
        return Object{ { "written", bytes.size() } };
    });
}

std::string Tools::exportMemory(std::string sessionId, int address, int length, std::string path,
                                bool overwrite)
{
    return result([&] {
        memoryRange(address, length);
        auto bytes = session(sessionId).emulator->readMemory(address, length);
        auto output = outputPath(path);
        writeFile(output, bytes, overwrite);
        return fileJson(output, bytes.size());
    });
}

std::string Tools::importMemory(std::string sessionId, int address, std::string path)
{
    return result([&] {
        auto bytes = readFile(path);
        auto &emulator = *session(sessionId).emulator;
        requireWritable(emulator, address, bytes.size());
        emulator.writeMemory(address, bytes);
        return Object{ { "written", bytes.size() } };
    });
}

std::string Tools::disassembleMemory(std::string sessionId, int address, int count)
{
    return result([&] {
        checked(address, 0, 65535, "address"sv);
        checked(count, 1, 4096, "count"sv);
        auto &emulator = *session(sessionId).emulator;
        Array instructions;
        for (auto &instruction : emulator.disassemble(address, count)) {
            auto item = instructionJson(instruction).as_object();
            auto &symbols = emulator.debugger().symbolTable();
            if (auto symbol = symbols.findSymbol(instruction.address))
                item["symbol"] = *symbol;
            if (auto source = symbols.findSourceLocation(instruction.address))
                item["source"] = Object{ { "file", source->file },
                                         { "line", source->line },
                                         { "page", source->page } };
            instructions.push_back(item);
        }
        return Object{ { "instructions", instructions } };
    });
}

std::string Tools::disassembleData(std::string hexData, int origin, bool z80n)
{
    return result([&] {
        auto bytes = unhex(hexData);
        memoryRange(origin, int(bytes.size()));
        checked(int(bytes.size()), 0, 16384, "data length"sv);
        Array instructions;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto instruction = Disassembler::disassembleOne(
                    [&](uint16_t address) {
                        int index = int(address) - origin;
                        return index >= 0 && std::size_t(index) < bytes.size() ? bytes[index]
                                                                               : uint8_t(0);
                    },
                    uint16_t(origin + offset), z80n);
            auto item = instructionJson(instruction).as_object();
            item["truncated"] = offset + instruction.length > bytes.size();
            instructions.push_back(item);
            offset += instruction.length;
        }
        return Object{ { "instructions", instructions } };
    });
}

std::string Tools::assemble(std::string source, int origin, bool z80n)
{
    return result([&] {
        auto bytes = assembled(source, origin, z80n);
        return Object{ { "origin", origin }, { "hex", hex(bytes) }, { "length", bytes.size() } };
    });
}

std::string Tools::assembleToMemory(std::string sessionId, std::string source, int address)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        auto bytes = assembled(source, address, emulator.machine().supportsZ80n());
        requireWritable(emulator, address, bytes.size());
        emulator.writeMemory(address, bytes);
        return Object{ { "address", address }, { "hex", hex(bytes) }, { "written", bytes.size() } };
    });
}

std::string Tools::compileBasic(std::string source, std::string dialect)
{
    return result([&] {
        auto bytes = BasicCodec::compile(source, basicDialect(dialect));
        return Object{ { "hex", hex(bytes) }, { "length", bytes.size() } };
    });
}

std::string Tools::decompileBasic(std::string hexData, std::string dialect)
{
    return result([&] { return basicListing(unhex(hexData), basicDialect(dialect)); });
}

std::string Tools::compileBasicToMemory(std::string sessionId, std::string source, int address,
                                        std::string dialect)
{
    return result([&] {
        auto bytes = BasicCodec::compile(source, basicDialect(dialect));
        auto &emulator = *session(sessionId).emulator;
        requireWritable(emulator, address, bytes.size());
        emulator.writeMemory(address, bytes);
        return Object{ { "address", address }, { "written", bytes.size() } };
    });
}

std::string Tools::decompileBasicMemory(std::string sessionId, int address, int length,
                                        std::string dialect)
{
    return result([&] {
        memoryRange(address, length);
        return basicListing(session(sessionId).emulator->readMemory(address, length),
                            basicDialect(dialect));
    });
}

std::string Tools::getBasicProgram(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        auto start = word(emulator, 23635);
        auto variables = word(emulator, 23627);
        auto end = word(emulator, 23641);
        if (start < 23755 || variables < start || end < variables)
            throw std::invalid_argument(
                    "BASIC pointers are invalid; boot into a compatible BASIC interpreter first"s);
        auto listing =
                basicListing(emulator.readMemory(start, end - start), BasicDialect::Spectrum48);
        listing["PROG"] = start;
        listing["VARS"] = variables;
        listing["E_LINE"] = end;
        return listing;
    });
}

std::string Tools::setBasicProgram(std::string sessionId, std::string source)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        auto start = word(emulator, 23635);
        auto ramTop = word(emulator, 23730);
        auto bytes = BasicCodec::compile(source);
        auto variables = std::size_t(start) + bytes.size();
        if (start < 23755 || ramTop <= start || variables + 128 >= ramTop)
            throw std::invalid_argument(
                    "BASIC is not initialized or program exceeds RAMTOP workspace"s);
        requireWritable(emulator, start, bytes.size() + 5);
        bytes.insert(bytes.end(), { 0x80, 0x0d, 0x80, 0, 0 });
        emulator.writeMemory(start, bytes);
        setWord(emulator, 23627, variables);
        setWord(emulator, 23641, variables + 1);
        setWord(emulator, 23643, variables + 1);
        setWord(emulator, 23645, variables + 1);
        setWord(emulator, 23647, 0);
        for (uint16_t pointer : { 23649, 23651, 23653 })
            setWord(emulator, pointer, variables + 3);
        return Object{ { "PROG", start }, { "VARS", variables }, { "written", bytes.size() } };
    });
}

std::string Tools::compileBasicArray(std::string source, bool characterArray)
{
    return result([&] {
        return Object{ { "hex", hex(BasicCodec::compileArray(source, characterArray)) } };
    });
}

std::string Tools::decompileBasicArray(std::string hexData, bool characterArray)
{
    return result([&] {
        return Object{ { "source", BasicCodec::decompileArray(unhex(hexData), characterArray) } };
    });
}
std::string Tools::runFrames(std::string sessionId, int frameCount, int maxMilliseconds)
{
    return result([&] { return execute(session(sessionId), frameCount, maxMilliseconds); });
}

std::string Tools::stepInstructions(std::string sessionId, int count, int maxMilliseconds)
{
    return result([&] {
        checked(count, 1, 100000, "count"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        auto &current = session(sessionId);
        auto &emulator = *current.emulator;
        current.lastPause.reset();
        emulator.takePauseEvent();
        auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMilliseconds);
        int completed = 0;
        std::string reason = "instructionBudget"s;
        while (completed < count) {
            if (std::chrono::steady_clock::now() >= deadline) {
                reason = "timeBudget"s;
                break;
            }
            emulator.stepInstruction();
            ++completed;
            current.lastPause = emulator.takePauseEvent();
            if (current.lastPause && current.lastPause->reason != PauseReason::StepComplete) {
                reason = "breakpoint"s;
                break;
            }
        }
        auto output = status(current).as_object();
        output["completedInstructions"] = completed;
        output["stopReason"] = reason;
        return output;
    });
}

std::string Tools::runToAddress(std::string sessionId, int address, int maxFrames,
                                int maxMilliseconds)
{
    return result([&] {
        checked(address, 0, 65535, "address"sv);
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        auto &current = session(sessionId);
        if (!current.emulator->runTo(address))
            return status(current);
        return execute(current, maxFrames, maxMilliseconds);
    });
}

std::string Tools::stepOver(std::string sessionId, int maxFrames, int maxMilliseconds)
{
    return result([&] {
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        auto &current = session(sessionId);
        if (!current.emulator->stepOver()) {
            current.lastPause = current.emulator->takePauseEvent();
            return status(current);
        }
        return execute(current, maxFrames, maxMilliseconds);
    });
}

std::string Tools::stepOut(std::string sessionId, int maxFrames, int maxMilliseconds)
{
    return result([&] {
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        auto &current = session(sessionId);
        if (!current.emulator->stepOut())
            return status(current);
        return execute(current, maxFrames, maxMilliseconds);
    });
}

std::string Tools::addBreakpoint(std::string sessionId, std::string type, int address, int mask,
                                 int bankId, int page1k, bool temporary, std::string condition)
{
    return result([&] {
        auto found = std::ranges::find(breakpointNames, type);
        if (found == breakpointNames.end())
            throw std::invalid_argument(
                    "Breakpoint type must be execute/read/write/in/out/nextreg/raster/event"s);
        checked(address, 0, 65535, "address"sv);
        checked(mask, 0, 65535, "mask"sv);
        checked(bankId, -1, 65535, "bankId"sv);
        checked(page1k, -1, 65535, "page1k"sv);
        auto &current = session(sessionId);
        if (current.emulator->debugger().breakpoints().breakpoints().size() >= 1024)
            throw std::invalid_argument("Maximum 1024 breakpoints per session"s);
        Breakpoint breakpoint;
        breakpoint.type = BreakpointType(found - breakpointNames.begin());
        breakpoint.address = address;
        breakpoint.mask = mask;
        breakpoint.temporary = temporary;
        if (bankId >= 0) {
            auto &bank = current.emulator->mmu().bank(bankId);
            if (page1k >= 0)
                checked(page1k, 0, int(bank.count1kPages()) - 1, "page1k"sv);
            breakpoint.bankId = bankId;
        }
        if (page1k >= 0)
            breakpoint.page1k = page1k;
        if (type == "nextreg"sv)
            checked(address, 0, 255, "Next register"sv);
        if (type == "raster"sv)
            checked(address, 0, current.emulator->machine().timings().linesPerFrame - 1,
                    "raster line"sv);
        if (type == "event"sv)
            checked(address, 0, 1, "event (0=IRQ, 1=NMI)"sv);
        breakpoint.condition = breakpointCondition(condition);
        auto id = current.emulator->addBreakpoint(std::move(breakpoint));
        current.conditions[id] = condition;
        return Object{ { "breakpointId", id } };
    });
}

std::string Tools::listBreakpoints(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        Array entries;
        for (auto &breakpoint : current.emulator->debugger().breakpoints().breakpoints()) {
            Object item{ { "id", breakpoint.id },
                         { "type", breakpointNames.at(unsigned(breakpoint.type)) },
                         { "address", breakpoint.address },
                         { "mask", breakpoint.mask },
                         { "enabled", breakpoint.enabled },
                         { "temporary", breakpoint.temporary },
                         { "hitCount", breakpoint.hitCount },
                         { "condition", current.conditions[breakpoint.id] } };
            item["bankId"] = breakpoint.bankId ? Json(*breakpoint.bankId) : Json(nullptr);
            item["page1k"] = breakpoint.page1k ? Json(*breakpoint.page1k) : Json(nullptr);
            entries.push_back(item);
        }
        return Object{ { "breakpoints", entries } };
    });
}

std::string Tools::removeBreakpoint(std::string sessionId, int breakpointId)
{
    return result([&] {
        checked(breakpointId, 0, std::numeric_limits<int>::max(), "breakpointId"sv);
        auto &current = session(sessionId);
        bool removed = current.emulator->removeBreakpoint(breakpointId);
        current.conditions.erase(breakpointId);
        return Object{ { "removed", removed } };
    });
}

std::string Tools::enableBreakpoint(std::string sessionId, int breakpointId, bool enabled)
{
    return result([&] {
        checked(breakpointId, 0, std::numeric_limits<int>::max(), "breakpointId"sv);
        if (!session(sessionId).emulator->setBreakpointEnabled(breakpointId, enabled))
            throw std::invalid_argument("Unknown breakpoint ID"s);
        return Object{ { "enabled", enabled } };
    });
}

std::string Tools::clearBreakpoints(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        current.emulator->clearBreakpoints();
        current.conditions.clear();
        return Object{ { "cleared", true } };
    });
}

std::string Tools::configureTrace(std::string sessionId, bool enabled, int capacity)
{
    return result([&] {
        checked(capacity, 1, 100000, "capacity"sv);
        auto &debugger = session(sessionId).emulator->debugger();
        debugger.setTraceEnabled(false);
        if (enabled)
            debugger.setTraceEnabled(true, capacity);
        return Object{ { "enabled", enabled }, { "capacity", capacity } };
    });
}

std::string Tools::readTrace(std::string sessionId, int count)
{
    return result([&] {
        checked(count, 1, 100000, "count"sv);
        auto &trace = session(sessionId).emulator->debugger().traceLog();
        Array entries;
        auto skip = trace.size() > std::size_t(count) ? trace.size() - count : 0;
        for (std::size_t index = skip; index < trace.size(); ++index) {
            auto &entry = trace[index];
            entries.push_back(Object{ { "cycle", entry.cycle },
                                      { "PC", entry.pc },
                                      { "AF", entry.af },
                                      { "BC", entry.bc },
                                      { "DE", entry.de },
                                      { "HL", entry.hl },
                                      { "SP", entry.sp } });
        }
        return Object{ { "entries", entries },
                       { "retained", trace.size() },
                       { "truncated", skip != 0 } };
    });
}

std::string Tools::setSymbol(std::string sessionId, int address, std::string name)
{
    return result([&] {
        checked(address, 0, 65535, "address"sv);
        if (name.empty() || name.size() > 256)
            throw std::invalid_argument("Symbol name must be 1..256 bytes"s);
        session(sessionId).emulator->debugger().setSymbol(address, name);
        return Object{ { "address", address }, { "name", name } };
    });
}

std::string Tools::loadSymbols(std::string sessionId, std::string sldText)
{
    return result([&] {
        session(sessionId).emulator->debugger().symbolTable().loadSldText(sldText);
        return Object{ { "loaded", true } };
    });
}

std::string Tools::configureRewind(std::string sessionId, bool enabled, int capacity)
{
    return result([&] {
        checked(capacity, 1, 300, "capacity"sv);
        session(sessionId).emulator->setRewindEnabled(enabled, capacity);
        return Object{ { "enabled", enabled }, { "capacity", capacity } };
    });
}

std::string Tools::rewindFrames(std::string sessionId, int count)
{
    return result([&] {
        auto &current = session(sessionId);
        checked(count, 1, int(current.emulator->debugger().rewindBuffer().size()), "count"sv);
        current.emulator->rewind(count);
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::resetPokeFinder(std::string sessionId)
{
    return result([&] {
        return Object{ { "candidates", session(sessionId).emulator->pokeFinderReset() } };
    });
}

std::string Tools::findPokes(std::string sessionId, std::string mode, int value, int maxResults)
{
    return result([&] {
        checked(value, 0, 255, "value"sv);
        checked(maxResults, 1, 4096, "maxResults"sv);
        auto &emulator = *session(sessionId).emulator;
        auto &finder = emulator.debugger().pokeFinder();
        if (mode == "value"sv)
            finder.search(emulator.mmu(), value);
        else if (mode == "incremented"sv)
            finder.searchIncremented(emulator.mmu());
        else if (mode == "decremented"sv)
            finder.searchDecremented(emulator.mmu());
        else
            throw std::invalid_argument("mode must be value, incremented or decremented"s);
        Array candidates;
        for (auto &candidate : finder.results(maxResults))
            candidates.push_back(Object{ { "page16k", candidate.page16k },
                                         { "offset", candidate.offset },
                                         { "value", candidate.currentValue } });
        return Object{ { "candidates", candidates },
                       { "total", finder.count() },
                       { "truncated", finder.count() > candidates.size() } };
    });
}

std::string Tools::readPort(std::string sessionId, int port)
{
    return result([&] {
        checked(port, 0, 65535, "port"sv);
        return Object{ { "port", port },
                       { "value", session(sessionId).emulator->ioBus().peekPort(port) } };
    });
}

std::string Tools::writePort(std::string sessionId, int port, int value)
{
    return result([&] {
        checked(port, 0, 65535, "port"sv);
        checked(value, 0, 255, "value"sv);
        session(sessionId).emulator->ioBus().write(port, value);
        return Object{ { "port", port }, { "value", value } };
    });
}

std::string Tools::listKeys()
{
    return result([&] {
        Array names;
        for (auto &[name, key] : keyNames())
            names.emplace_back(name);
        return Object{ { "keys", names } };
    });
}

std::string Tools::setKey(std::string sessionId, std::string key, bool pressed)
{
    return result([&] {
        auto found = keyNames().find(upper(key));
        if (found == keyNames().end())
            throw std::invalid_argument("Unknown key; call listKeys"s);
        session(sessionId).emulator->setKeyState(found->second, pressed);
        return Object{ { "key", found->first }, { "pressed", pressed } };
    });
}

std::string Tools::tapKeys(std::string sessionId, std::string keys, int holdFrames,
                           int releaseFrames)
{
    return result([&] {
        checked(holdFrames, 1, 100, "holdFrames"sv);
        checked(releaseFrames, 1, 100, "releaseFrames"sv);
        std::vector<ZxKey> chord;
        std::istringstream stream(upper(keys));
        std::string name;
        while (std::getline(stream, name, '+')) {
            auto found = keyNames().find(name);
            if (found == keyNames().end())
                throw std::invalid_argument("Unknown chord key; call listKeys"s);
            chord.push_back(found->second);
        }
        if (chord.empty())
            throw std::invalid_argument("Key chord is empty"s);
        auto &current = session(sessionId);
        for (auto key : chord)
            current.emulator->setKeyState(key, true);
        Json held;
        try {
            held = execute(current, holdFrames, 1000);
        } catch (...) {
            for (auto key : chord)
                current.emulator->setKeyState(key, false);
            throw;
        }
        for (auto key : chord)
            current.emulator->setKeyState(key, false);
        if (held.at("stopReason").as_string() != "frameBudget"sv)
            return held;
        return execute(current, releaseFrames, 1000);
    });
}

std::string Tools::triggerNmi(std::string sessionId)
{
    return result([&] {
        session(sessionId).emulator->triggerNmi();
        return Object{ { "triggered", true } };
    });
}

std::string Tools::loadMedia(std::string sessionId, std::string path)
{
    return result([&] {
        auto &current = session(sessionId);
        current.emulator->loadMedia(MediaFile::fromBytes(
                readFile(path), std::filesystem::path(path).extension().string()));
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::loadMediaData(std::string sessionId, std::string hexData, std::string extension)
{
    return result([&] {
        auto &current = session(sessionId);
        current.emulator->loadMedia(MediaFile::fromBytes(unhex(hexData), extension));
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::saveMedia(std::string sessionId, std::string path, bool overwrite)
{
    return result([&] {
        auto output = outputPath(path);
        auto bytes = session(sessionId).emulator->saveMedia(output.extension().string());
        writeFile(output, bytes, overwrite);
        return fileJson(output, bytes.size());
    });
}

std::string Tools::inspectMedia(std::string path, std::string filesystem)
{
    return result([&] {
        auto media = archive(path, filesystem);
        Array entries;
        for (auto &entry : media.list()) {
            Object attributes;
            for (auto &[name, value] : entry.attributes)
                attributes[name] = value;
            entries.push_back(Object{ { "name", entry.name },
                                      { "size", entry.size },
                                      { "directory", entry.directory },
                                      { "attributes", attributes } });
        }
        return Object{ { "path", path }, { "entries", entries } };
    });
}

std::string Tools::listMediaProfiles()
{
    return result([&] {
        Array profiles;
        for (auto &profile : MediaArchive::profiles())
            profiles.emplace_back(profile);
        return Object{ { "profiles", profiles } };
    });
}

std::string Tools::readMediaEntry(std::string path, std::string entry, std::string filesystem,
                                  int offset, int length, bool asText)
{
    return result([&] {
        auto bytes = archive(path, filesystem).read(entry);
        checked(offset, 0, int(bytes.size()), "offset"sv);
        int remaining = int(bytes.size()) - offset;
        checked(length, 0, std::min(remaining, 65536), "length"sv);
        if (!length)
            length = std::min(remaining, 65536);
        auto part = std::span(bytes).subspan(offset, length);
        Object output{ { "entry", entry },
                       { "offset", offset },
                       { "length", length },
                       { "totalBytes", bytes.size() },
                       { "truncated", length < remaining } };
        if (asText) {
            output["text"] = std::string(part.begin(), part.end());
            if (offset == 0 && length == int(bytes.size()) && entry.starts_with("basic/"sv)
                && entry.ends_with(".bas"sv))
                output["basic"] = basicListing(BasicCodec::compile(output["text"].as_string()),
                                               BasicDialect::Spectrum48);
        } else
            output["hex"] = hex(part);
        return output;
    });
}

std::string Tools::extractMediaEntry(std::string path, std::string entry, std::string filesystem,
                                     std::string output, bool overwrite)
{
    return result([&] {
        auto bytes = archive(path, filesystem).read(entry);
        auto destination = outputPath(output);
        writeFile(destination, bytes, overwrite);
        return fileJson(destination, bytes.size());
    });
}

std::string Tools::convertMedia(std::string path, std::string filesystem, std::string output,
                                bool overwrite)
{
    return result([&] {
        auto destination = outputPath(output);
        auto bytes = archive(path, filesystem).encode(destination.extension().string());
        writeFile(destination, bytes, overwrite);
        return fileJson(destination, bytes.size());
    });
}

std::string Tools::createMedia(std::string output, std::string filesystem, bool overwrite)
{
    return result([&] {
        auto destination = outputPath(output);
        auto bytes =
                MediaArchive::create(destination.extension().string(), { filesystem }).encode();
        writeFile(destination, bytes, overwrite);
        return fileJson(destination, bytes.size());
    });
}

std::string Tools::putMediaEntry(std::string path, std::string entry, std::string filesystem,
                                 std::string hexData, std::string attributesJson,
                                 std::string output, bool overwrite)
{
    return result([&] {
        MediaAttributes attributes;
        auto parsed = boost::json::parse(attributesJson);
        if (!parsed.is_object())
            throw std::invalid_argument("attributesJson must be a JSON object"s);
        for (auto &[key, value] : parsed.as_object()) {
            if ((!value.is_int64() || value.as_int64() < 0) && !value.is_uint64())
                throw std::invalid_argument("Media attributes must be nonnegative integers"s);
            attributes[std::string(key)] =
                    value.is_uint64() ? value.as_uint64() : uint64_t(value.as_int64());
        }
        auto media = archive(path, filesystem);
        media.put(entry, unhex(hexData), attributes);
        auto destination = outputPath(output);
        auto bytes = media.encode(destination.extension().string());
        writeFile(destination, bytes, overwrite);
        return fileJson(destination, bytes.size());
    });
}

std::string Tools::removeMediaEntry(std::string path, std::string entry, std::string filesystem,
                                    std::string output, bool overwrite)
{
    return result([&] {
        auto media = archive(path, filesystem);
        media.remove(entry);
        auto destination = outputPath(output);
        auto bytes = media.encode(destination.extension().string());
        writeFile(destination, bytes, overwrite);
        return fileJson(destination, bytes.size());
    });
}

std::string Tools::inspectTape(std::string sessionId)
{
    return result([&] {
        auto &tape = session(sessionId).emulator->tapeDeck();
        Array blocks;
        for (std::size_t index = 0; index < tape.blocks().size(); ++index) {
            auto &block = tape.blocks()[index];
            Object item{ { "index", index },
                         { "type", unsigned(block.type) },
                         { "bytes", block.data.size() },
                         { "description", block.description },
                         { "pauseMs", block.pauseAfterMs },
                         { "romBlock", block.isRomBlock() } };
            if (block.isRomBlock() && !block.data.empty()) {
                uint8_t checksum = 0;
                for (auto byte : block.data)
                    checksum ^= byte;
                item["checksumValid"] = checksum == 0;
                item["flag"] = block.data.front();
                if (block.data.size() == 19 && block.data.front() == 0) {
                    auto &data = block.data;
                    item["header"] =
                            Object{ { "type", data[1] },
                                    { "name", std::string(data.begin() + 2, data.begin() + 12) },
                                    { "length", data[12] | (data[13] << 8) },
                                    { "param1", data[14] | (data[15] << 8) },
                                    { "param2", data[16] | (data[17] << 8) } };
                }
            }
            blocks.push_back(item);
        }
        auto &options = tape.options();
        return Object{ { "blockIndex", tape.currentBlockIndex() },
                       { "playing", tape.isPlaying() },
                       { "blocks", blocks },
                       { "options",
                         Object{ { "romTraps", options.romTraps },
                                 { "accelerateLoaders", options.accelerateLoaders },
                                 { "autoPlay", options.autoPlay },
                                 { "loadingSound", options.loadingSound } } } };
    });
}

std::string Tools::readTapeBlock(std::string sessionId, int blockIndex, bool payloadOnly)
{
    return result([&] {
        auto &blocks = session(sessionId).emulator->tapeDeck().blocks();
        checked(blockIndex, 0, int(blocks.size()) - 1, "blockIndex"sv);
        auto &block = blocks[blockIndex];
        auto bytes = std::span(block.data);
        if (payloadOnly) {
            if (!block.isRomBlock() || bytes.size() < 2)
                throw std::invalid_argument(
                        "Only ROM-compatible blocks have removable flag/checksum bytes"s);
            bytes = bytes.subspan(1, bytes.size() - 2);
        }
        if (bytes.size() > 65536)
            throw std::invalid_argument(
                    "Block exceeds inline limit; use archive entry extraction"s);
        return Object{ { "index", blockIndex }, { "hex", hex(bytes) } };
    });
}

std::string Tools::seekTape(std::string sessionId, int blockIndex)
{
    return result([&] {
        auto &tape = session(sessionId).emulator->tapeDeck();
        checked(blockIndex, 0, int(tape.blocks().size()), "blockIndex"sv);
        tape.seekBlock(blockIndex);
        return Object{ { "blockIndex", tape.currentBlockIndex() } };
    });
}

std::string Tools::playTape(std::string sessionId, bool playing)
{
    return result([&] {
        session(sessionId).emulator->tapeDeck().setPlaying(playing);
        return Object{ { "playing", playing } };
    });
}

std::string Tools::configureTape(std::string sessionId, bool romTraps, bool accelerateLoaders,
                                 bool autoPlay, bool loadingSound)
{
    return result([&] {
        auto &tape = session(sessionId).emulator->tapeDeck();
        auto options = tape.options();
        options.romTraps = romTraps;
        options.accelerateLoaders = accelerateLoaders;
        options.autoPlay = autoPlay;
        options.loadingSound = loadingSound;
        tape.setOptions(options);
        return Object{ { "configured", true } };
    });
}

std::string Tools::autoLoadTape(std::string sessionId, int blockIndex)
{
    return result([&] {
        auto &current = session(sessionId);
        checked(blockIndex, 0, int(current.emulator->tapeDeck().blocks().size()) - 1,
                "blockIndex"sv);
        if (!current.emulator->autoLoadTape(blockIndex))
            throw std::invalid_argument("Autoload is unsupported by this machine"s);
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::captureScreen(std::string sessionId, std::string path, bool overwrite)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        emulator.refreshFrame();
        auto bytes = emulator.screenshotPng();
        auto output = outputPath(path);
        writeFile(output, bytes, overwrite);
        return fileJson(output, bytes.size());
    });
}

std::string Tools::renderScreen(std::string hexData, std::string path, bool overwrite)
{
    return result([&] {
        auto data = unhex(hexData);
        if (data.size() != 6912)
            throw std::invalid_argument("SCR data must contain exactly 6912 bytes"s);
        auto bytes = encodePng(renderScrToVideoFrame(data));
        auto output = outputPath(path);
        writeFile(output, bytes, overwrite);
        return fileJson(output, bytes.size());
    });
}

std::string Tools::renderGraphics(std::string sessionId, int address, int width, int height,
                                  int count, int stride, int columns, std::string path,
                                  bool overwrite)
{
    return result([&] {
        checked(width, 8, 256, "width"sv);
        checked(height, 1, 256, "height"sv);
        checked(count, 1, 1024, "count"sv);
        checked(columns, 1, count, "columns"sv);
        if (width % 8)
            throw std::invalid_argument("width must be a multiple of eight pixels"s);
        if (!stride)
            stride = width / 8;
        checked(stride, width / 8, 256, "stride"sv);
        memoryRange(address, stride * height * count);
        int rows = (count + columns - 1) / columns;
        if (width * columns > 4096 || height * rows > 4096)
            throw std::invalid_argument("Atlas dimensions exceed 4096 pixels"s);
        auto bytes = session(sessionId).emulator->readMemory(address, stride * height * count);
        VideoFrame frame(width * columns, height * rows);
        std::ranges::fill(frame.pixels(), packRgba(0, 0, 0));
        for (int tile = 0; tile < count; ++tile)
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    bool set =
                            bytes[tile * stride * height + y * stride + x / 8] & (0x80 >> (x % 8));
                    frame.putPixel((tile % columns) * width + x, (tile / columns) * height + y,
                                   set ? packRgba(255, 255, 255) : packRgba(0, 0, 0));
                }
        auto png = encodePng(frame);
        auto output = outputPath(path);
        writeFile(output, png, overwrite);
        auto info = fileJson(output, png.size());
        info["sourceAddress"] = address;
        info["sourceBytes"] = bytes.size();
        info["width"] = frame.width();
        info["height"] = frame.height();
        return info;
    });
}

std::string Tools::captureAudio(std::string sessionId, int frameCount, int maxMilliseconds,
                                std::string path, bool overwrite)
{
    return result([&] {
        checked(frameCount, 1, 1500, "frameCount"sv);
        auto output = outputPath(path);
        if (!overwrite && std::filesystem::exists(output))
            throw std::invalid_argument("Output exists; set overwrite=true"s);
        auto &current = session(sessionId);
        Bytes samples;
        ScopedIoTrace trace(*current.emulator, 0, 0, false, true, 8192);
        auto startCycle = current.emulator->clock().totalTstates();
        auto startFrameTstates = current.emulator->clock().frameTstates();
        auto info = execute(current, frameCount, maxMilliseconds, std::ref(samples)).as_object();
        auto rate = current.emulator->audioSampleRate();
        Bytes wav;
        auto text = [&](std::string_view value) {
            wav.insert(wav.end(), value.begin(), value.end());
        };
        auto integer = [&](uint32_t value, int count) {
            for (int index = 0; index < count; ++index)
                wav.push_back(uint8_t(value >> (8 * index)));
        };
        text("RIFF"sv);
        integer(uint32_t(samples.size()) + 36, 4);
        text("WAVEfmt "sv);
        integer(16, 4);
        integer(1, 2);
        integer(2, 2);
        integer(rate, 4);
        integer(rate * 4, 4);
        integer(4, 2);
        integer(16, 2);
        text("data"sv);
        integer(uint32_t(samples.size()), 4);
        wav.insert(wav.end(), samples.begin(), samples.end());
        writeFile(output, wav, overwrite);
        info["path"] = output.string();
        info["sampleFrames"] = samples.size() / 4;
        info["sampleRate"] = rate;
        info["startCycle"] = startCycle;
        info["startFrameTstates"] = startFrameTstates;
        info["clockHz"] = current.emulator->clock().timings().baseClockHz;
        info["ioWrites"] = trace.summary();
        return info;
    });
}
std::string Tools::captureIo(std::string sessionId, int frameCount, int maxMilliseconds, int mask,
                             int value, bool includeReads, bool includeWrites, int maxEvents)
{
    return result([&] {
        checked(mask, 0, 65535, "mask"sv);
        checked(value, 0, 65535, "value"sv);
        checked(maxEvents, 1, 65536, "maxEvents"sv);
        if (!includeReads && !includeWrites)
            throw std::invalid_argument("Enable reads, writes or both"s);
        auto &current = session(sessionId);
        ScopedIoTrace trace(*current.emulator, mask, value, includeReads, includeWrites, maxEvents);
        auto output = execute(current, frameCount, maxMilliseconds).as_object();
        output["io"] = trace.summary();
        return output;
    });
}
} // namespace zxkit::mcp_tool
