/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "zxkit/media/TapeFormats.h"

#include "zxkit/core/Exception.h"
#include "zxkit/media/Compression.h"

#include <algorithm>
#include <bit>
#include <format>
#include <limits>

namespace zxkit {

namespace {

constexpr uint32_t kTstatesPerMs = 3500;
// Longest step the cursor returns: longer ones (huge pauses, long direct recording runs) are split
constexpr uint32_t kMaxStep = 0x7FFF'FFFF;

// The ROM loader's timings
constexpr uint16_t kRomPilot = 2168;
constexpr uint16_t kRomSync1 = 667;
constexpr uint16_t kRomSync2 = 735;
constexpr uint16_t kRomZero = 855;
constexpr uint16_t kRomOne = 1710;
constexpr uint16_t kRomHeaderPilots = 8063;
constexpr uint16_t kRomDataPilots = 3223;

// Bounds checked little endian reader: reading past the end throws
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) noexcept
        : m_data(data)
    {}

    std::size_t left() const noexcept { return m_data.size() - m_pos; }
    bool atEnd() const noexcept { return m_pos >= m_data.size(); }

    uint8_t u8()
    {
        need(1);
        return m_data[m_pos++];
    }
    uint16_t u16()
    {
        need(2);
        const auto v = static_cast<uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }
    int16_t s16() { return static_cast<int16_t>(u16()); }
    uint32_t u24()
    {
        need(3);
        const uint32_t v = m_data[m_pos] | (m_data[m_pos + 1] << 8) | (static_cast<uint32_t>(m_data[m_pos + 2]) << 16);
        m_pos += 3;
        return v;
    }
    uint32_t u32()
    {
        need(4);
        const uint32_t v = m_data[m_pos] | (m_data[m_pos + 1] << 8) | (static_cast<uint32_t>(m_data[m_pos + 2]) << 16)
                           | (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
        m_pos += 4;
        return v;
    }
    std::span<const uint8_t> bytes(std::size_t n)
    {
        need(n);
        const auto s = m_data.subspan(m_pos, n);
        m_pos += n;
        return s;
    }
    std::string text(std::size_t n)
    {
        const auto s = bytes(n);
        std::string out(s.begin(), s.end());
        // TZX texts separate their lines with CR
        std::ranges::replace(out, '\r', '\n');
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\0'))
            out.pop_back();
        return out;
    }
    void skip(std::size_t n) { (void)bytes(n); }
    /// The next `n` bytes, as a reader of their own (blocks with a length field).
    Reader sub(std::size_t n) { return Reader(bytes(n)); }

private:
    void need(std::size_t n) const
    {
        if (left() < n)
            throw FormatException("Truncated tape block");
    }

    std::span<const uint8_t> m_data;
    std::size_t m_pos = 0;
};

std::vector<uint8_t> toVector(std::span<const uint8_t> s)
{
    return {s.begin(), s.end()};
}

TapeSymbol readSymbol(Reader &r, std::size_t maxPulses)
{
    TapeSymbol sym;
    sym.start = static_cast<TapeSymbol::Start>(r.u8() & 0x03);
    sym.pulses.reserve(maxPulses);
    for (std::size_t i = 0; i < maxPulses; ++i)
        sym.pulses.push_back(r.u16());
    // A zero length ends the symbol: a zero first pulse only sets the level
    if (const auto zero = std::ranges::find(sym.pulses, uint16_t{0}); zero != sym.pulses.end())
        sym.pulses.erase(zero == sym.pulses.begin() ? zero + 1 : zero, sym.pulses.end());
    return sym;
}

// The bits a symbol of an alphabet of `symbols` symbols takes
uint8_t bitsFor(std::size_t symbols) noexcept
{
    return symbols <= 1 ? 0 : static_cast<uint8_t>(std::bit_width(symbols - 1));
}

// CSW pulses (in samples at `sampleRate`) to T-states, without accumulating rounding errors
std::vector<uint32_t> cswPulses(std::span<const uint8_t> rle, uint32_t sampleRate)
{
    if (sampleRate == 0)
        throw FormatException("CSW recording with a zero sample rate");
    std::vector<uint32_t> pulses;
    pulses.reserve(rle.size());
    uint64_t samples = 0;
    uint64_t tstates = 0;
    for (std::size_t i = 0; i < rle.size();) {
        uint32_t length = rle[i++];
        if (length == 0) {
            if (i + 4 > rle.size())
                break;
            length = rle[i] | (rle[i + 1] << 8) | (static_cast<uint32_t>(rle[i + 2]) << 16)
                     | (static_cast<uint32_t>(rle[i + 3]) << 24);
            i += 4;
        }
        samples += length;
        const uint64_t end = samples * 3'500'000 / sampleRate;
        pulses.push_back(static_cast<uint32_t>(std::min<uint64_t>(end - tstates, kMaxStep)));
        tstates = end;
    }
    return pulses;
}

std::string archiveDescription(Reader &r)
{
    const uint8_t count = r.u8();
    std::string title;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t id = r.u8();
        const uint8_t length = r.u8();
        std::string text = r.text(length);
        if (id == 0 && title.empty())
            title = std::move(text);
    }
    return title.empty() ? std::string("Archive info") : std::format("Archive info: {}", title);
}

// One TZX block, its id already read
TapeBlock parseTzxBlock(uint8_t id, Reader &r)
{
    TapeBlock blk;
    blk.type = static_cast<TapeBlockType>(id);
    blk.pauseAfterMs = 0;

    switch (id) {
    case 0x10: { // Standard speed data
        const uint16_t pause = r.u16();
        const uint16_t length = r.u16();
        return TapeBlock::standard(toVector(r.bytes(length)), pause);
    }
    case 0x11: { // Turbo speed data
        blk.pilotPulse = r.u16();
        blk.sync1Pulse = r.u16();
        blk.sync2Pulse = r.u16();
        blk.zeroPulse = r.u16();
        blk.onePulse = r.u16();
        blk.pilotPulses = r.u16();
        blk.usedBits = r.u8();
        blk.pauseAfterMs = r.u16();
        const uint32_t length = r.u24();
        blk.data = toVector(r.bytes(length));
        blk.description = TapeFormats::describeStandardBlock(blk.data);
        if (!blk.isRomBlock())
            blk.description = "Turbo " + blk.description;
        return blk;
    }
    case 0x12: // Pure tone
        blk.pilotPulse = r.u16();
        blk.pilotPulses = r.u16();
        blk.description = std::format("Pure tone ({} pulses)", blk.pilotPulses);
        return blk;
    case 0x13: { // Pulse sequence
        const uint8_t count = r.u8();
        for (uint8_t i = 0; i < count; ++i)
            blk.pulses.push_back(r.u16());
        blk.description = std::format("Pulse sequence ({} pulses)", count);
        return blk;
    }
    case 0x14: { // Pure data
        blk.zeroPulse = r.u16();
        blk.onePulse = r.u16();
        blk.usedBits = r.u8();
        blk.pauseAfterMs = r.u16();
        const uint32_t length = r.u24();
        blk.data = toVector(r.bytes(length));
        blk.description = std::format("Pure data ({} bytes)", blk.data.size());
        return blk;
    }
    case 0x15: { // Direct recording
        blk.sampleTstates = r.u16();
        blk.pauseAfterMs = r.u16();
        blk.usedBits = r.u8();
        const uint32_t length = r.u24();
        blk.data = toVector(r.bytes(length));
        blk.description = std::format("Direct recording ({} ms)",
                                      uint64_t{blk.data.size()} * 8 * blk.sampleTstates / kTstatesPerMs);
        return blk;
    }
    case 0x18: { // CSW recording
        Reader b = r.sub(r.u32());
        blk.pauseAfterMs = b.u16();
        const uint32_t sampleRate = b.u24();
        const uint8_t compression = b.u8();
        const uint32_t storedPulses = b.u32();
        const auto payload = b.bytes(b.left());
        if (compression == 1)
            blk.pulses = cswPulses(payload, sampleRate);
        else if (compression == 2)
            blk.pulses = cswPulses(zlibDecompress(payload, std::max<std::size_t>(storedPulses, 4096)), sampleRate);
        else
            throw FormatException(std::format("Unknown CSW compression {}", compression));
        blk.description = std::format("CSW recording ({} pulses)", blk.pulses.size());
        return blk;
    }
    case 0x19: { // Generalized data
        Reader b = r.sub(r.u32());
        blk.pauseAfterMs = b.u16();
        const uint32_t pilotCount = b.u32();
        const uint8_t pilotMaxPulses = b.u8();
        const uint8_t pilotAlphabet = b.u8();
        const uint32_t dataCount = b.u32();
        const uint8_t dataMaxPulses = b.u8();
        const uint8_t dataAlphabet = b.u8();
        if (pilotCount > 0) {
            const std::size_t symbols = pilotAlphabet ? pilotAlphabet : 256;
            for (std::size_t i = 0; i < symbols; ++i)
                blk.pilotSymbols.push_back(readSymbol(b, pilotMaxPulses));
            blk.pilotStream.reserve(pilotCount);
            for (uint32_t i = 0; i < pilotCount; ++i) {
                TapeSymbolRun run;
                run.symbol = b.u8();
                run.repeat = b.u16();
                blk.pilotStream.push_back(run);
            }
        }
        if (dataCount > 0) {
            const std::size_t symbols = dataAlphabet ? dataAlphabet : 256;
            for (std::size_t i = 0; i < symbols; ++i)
                blk.dataSymbols.push_back(readSymbol(b, dataMaxPulses));
            const uint8_t bits = blk.symbolBits();
            const uint64_t bytes = (uint64_t{dataCount} * bits + 7) / 8;
            blk.data = toVector(b.bytes(static_cast<std::size_t>(std::min<uint64_t>(bytes, b.left()))));
            // No more symbols than the data stream holds
            blk.dataSymbolCount = bits == 0 ? dataCount
                                            : static_cast<uint32_t>(std::min<uint64_t>(dataCount, blk.data.size() * 8 / bits));
        }
        blk.description = std::format("Generalized data ({} bytes)", blk.data.size());
        return blk;
    }
    case 0x20: // Pause or stop the tape
        blk.pauseAfterMs = r.u16();
        blk.description = blk.pauseAfterMs ? std::format("Pause ({} ms)", blk.pauseAfterMs) : std::string("Stop the tape");
        return blk;
    case 0x21: // Group start
        blk.description = std::format("Group: {}", r.text(r.u8()));
        return blk;
    case 0x22:
        blk.description = "Group end";
        return blk;
    case 0x23: // Jump to block
        blk.value = r.s16();
        blk.description = std::format("Jump {:+}", blk.value);
        return blk;
    case 0x24: // Loop start
        blk.value = r.u16();
        blk.description = std::format("Loop ({} times)", blk.value);
        return blk;
    case 0x25:
        blk.description = "Loop end";
        return blk;
    case 0x26: { // Call sequence
        const uint16_t count = r.u16();
        for (uint16_t i = 0; i < count; ++i)
            blk.offsets.push_back(r.s16());
        blk.description = "Call sequence";
        return blk;
    }
    case 0x27:
        blk.description = "Return from sequence";
        return blk;
    case 0x28: { // Select block: like libspectrum, trust the entries rather than the length
        r.skip(2);
        const uint8_t count = r.u8();
        for (uint8_t i = 0; i < count; ++i) {
            blk.offsets.push_back(r.s16());
            const uint8_t length = r.u8();
            blk.labels.push_back(r.text(length));
        }
        blk.description = "Select block";
        return blk;
    }
    case 0x2A: // Stop the tape if in 48K mode. Its length is always 0: some files store a wrong one
        r.skip(4);
        blk.description = "Stop the tape (48K)";
        return blk;
    case 0x2B: // Set signal level, length 1
        r.skip(4);
        blk.value = r.u8() ? 1 : 0;
        blk.description = blk.value ? "Signal level: high" : "Signal level: low";
        return blk;
    case 0x30: // Text description
        blk.description = r.text(r.u8());
        return blk;
    case 0x31: { // Message
        r.skip(1); // display time
        blk.description = r.text(r.u8());
        return blk;
    }
    case 0x32: // Archive info: like libspectrum, trust the entries rather than the length
        r.skip(2);
        blk.description = archiveDescription(r);
        return blk;
    case 0x33: // Hardware type
        r.skip(std::size_t{r.u8()} * 3);
        blk.description = "Hardware type";
        return blk;
    case 0x35: { // Custom info
        std::string id = r.text(16);
        r.skip(r.u32());
        blk.description = std::format("Custom info: {}", id);
        return blk;
    }
    case 0x5A: // Glue (concatenated TZX files)
        r.skip(9);
        blk.description = "Glue";
        return blk;
    case 0x34: // Emulation info (deprecated)
        r.skip(8);
        break;
    case 0x40: // Snapshot (deprecated)
        r.skip(1);
        r.skip(r.u24());
        break;
    default: // Unknown and unsupported blocks: from TZX 1.10 on they all start with their length
        r.skip(r.u32());
        break;
    }
    // A block that doesn't play: kept as a description, so the block numbers match the file
    blk.type = TapeBlockType::TextDesc;
    blk.description = std::format("Unsupported block 0x{:02X}", id);
    return blk;
}

} // namespace

// ── TapeBlock ─────────────────────────────────────────────────────────────

bool TapeBlock::isRomBlock() const noexcept
{
    if (data.empty())
        return false;
    if (type == TapeBlockType::StandardData)
        return true;
    return type == TapeBlockType::TurboData && pilotPulse == kRomPilot && sync1Pulse == kRomSync1
           && sync2Pulse == kRomSync2 && zeroPulse == kRomZero && onePulse == kRomOne && usedBits == 8
           && pilotPulses >= 256;
}

bool TapeBlock::isMetadata() const noexcept
{
    switch (type) {
    case TapeBlockType::GroupStart:
    case TapeBlockType::GroupEnd:
    case TapeBlockType::SelectBlock:
    case TapeBlockType::TextDesc:
    case TapeBlockType::Message:
    case TapeBlockType::ArchiveInfo:
    case TapeBlockType::HardwareType:
    case TapeBlockType::CustomInfo:
    case TapeBlockType::Glue:
        return true;
    default:
        return false;
    }
}

uint8_t TapeBlock::symbolBits() const noexcept
{
    return bitsFor(dataSymbols.size());
}

TapeBlock TapeBlock::standard(std::vector<uint8_t> data, uint16_t pauseAfterMs)
{
    TapeBlock blk;
    blk.type = TapeBlockType::StandardData;
    blk.pauseAfterMs = pauseAfterMs;
    blk.pilotPulses = (!data.empty() && data[0] < 0x80) ? kRomHeaderPilots : kRomDataPilots;
    blk.description = TapeFormats::describeStandardBlock(data);
    blk.data = std::move(data);
    return blk;
}

// ── Formats ───────────────────────────────────────────────────────────────

namespace TapeFormats {

std::vector<TapeBlock> parseTap(std::span<const uint8_t> raw)
{
    std::vector<TapeBlock> blocks;
    Reader r(raw);
    while (r.left() >= 2) {
        const uint16_t length = r.u16();
        blocks.push_back(TapeBlock::standard(toVector(r.bytes(length))));
    }
    return blocks;
}

std::vector<TapeBlock> parseTzx(std::span<const uint8_t> raw)
{
    if (raw.size() < 10 || std::ranges::equal(raw.first(8), std::string_view("ZXTape!\x1A")) == false)
        throw FormatException("Invalid TZX header signature");
    // Like libspectrum, ignore the version: the blocks say what they are

    std::vector<TapeBlock> blocks;
    Reader r(raw.subspan(10));
    while (!r.atEnd()) {
        const uint8_t id = r.u8();
        try {
            blocks.push_back(parseTzxBlock(id, r));
        } catch (const FormatException &) {
            // A truncated last block: keep what came before it, like a damaged tape
            if (blocks.empty())
                throw;
            break;
        }
    }
    return blocks;
}

std::vector<uint8_t> writeTap(std::span<const TapeBlock> blocks)
{
    std::vector<uint8_t> out;
    for (const auto &blk : blocks) {
        // Only the blocks the ROM can load fit in a TAP file
        if (!blk.isRomBlock() || blk.data.size() > 0xFFFF)
            continue;
        const auto length = static_cast<uint16_t>(blk.data.size());
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.insert(out.end(), blk.data.begin(), blk.data.end());
    }
    return out;
}

std::string describeStandardBlock(std::span<const uint8_t> data)
{
    if (data.empty())
        return "Empty block";
    if (data[0] == 0x00 && data.size() == 19) {
        std::string title(data.begin() + 2, data.begin() + 12);
        while (!title.empty() && title.back() == ' ')
            title.pop_back();
        switch (data[1]) {
        case 0: return std::format("Program: {}", title);
        case 1: return std::format("Number array: {}", title);
        case 2: return std::format("Char array: {}", title);
        case 3: return std::format("Bytes: {}", title);
        default: break;
        }
    }
    return std::format("Data ({} bytes)", data.size() >= 2 ? data.size() - 2 : data.size());
}

} // namespace TapeFormats

// ── TapeCursor ────────────────────────────────────────────────────────────

void TapeCursor::seek(std::size_t index) noexcept
{
    advanceBlock(index);
    // A tape starts low: its first edge doesn't go high
    m_level = false;
    m_forceLow = true;
    m_loopCount = 0;
    m_inCall = false;
}

void TapeCursor::advanceBlock(std::size_t next) noexcept
{
    m_block = next;
    m_phase = Phase::Start;
    m_count = 0;
    m_byte = 0;
    m_bit = 0;
    m_bitPulse = 0;
    m_run = 0;
    m_repeat = 0;
    m_symbol = 0;
    m_symbolPulse = 0;
    m_inDataSymbols = false;
}

TapeEdge TapeCursor::next(std::span<const TapeBlock> blocks)
{
    using T = TapeEdge::Transition;
    uint8_t flags = 0;
    // Jumps and loops without any signal in between must not hang the emulation
    std::size_t controlSteps = 0;
    const std::size_t maxControlSteps = blocks.size() * 4 + 64;
    const bool startedAtEnd = m_block >= blocks.size();

    for (;;) {
        if (m_block >= blocks.size() || controlSteps > maxControlSteps) {
            m_block = blocks.size();
            // Reaching the end (after blocks without signal) ends the last pulse, like libspectrum
            const T transition = startedAtEnd ? T::None : T::Toggle;
            if (transition == T::Toggle)
                m_level = !m_level;
            return {.length = 0, .transition = transition,
                    .flags = static_cast<uint8_t>(flags | TapeEdge::Flags::Stop | TapeEdge::Flags::TapeEnd),
                    .level = m_level};
        }

        const TapeBlock &block = blocks[m_block];
        Step step;
        switch (block.type) {
        case TapeBlockType::StandardData:
        case TapeBlockType::TurboData:
        case TapeBlockType::PureData:
            step = dataBlockStep(block);
            break;
        case TapeBlockType::PureTone:
        case TapeBlockType::PulseSeq:
        case TapeBlockType::CswRecording:
            step = pulsesStep(block);
            break;
        case TapeBlockType::DirectRecording:
            step = samplesStep(block);
            break;
        case TapeBlockType::GeneralizedData:
            step = symbolsStep(block);
            break;
        case TapeBlockType::Pause:
            // A pause block is a low level (libspectrum); a 0 ms one stops the tape
            if (block.pauseAfterMs == 0)
                step = {.length = 0, .transition = T::None, .flags = TapeEdge::Flags::Stop, .blockDone = true};
            else
                step = {.length = uint32_t{block.pauseAfterMs} * kTstatesPerMs, .transition = T::Low,
                        .blockDone = true, .nextLow = true};
            break;
        case TapeBlockType::StopIf48k:
            step = {.length = 0, .transition = T::None, .flags = TapeEdge::Flags::Stop48k, .blockDone = true};
            break;
        case TapeBlockType::SetSignalLevel:
            step = {.length = 0, .transition = block.value ? T::High : T::Low, .blockDone = true};
            break;
        default:
            ++controlSteps;
            controlBlock(blocks, block, flags);
            continue;
        }

        if (step.noSignal) {
            ++controlSteps;
            flags |= TapeEdge::Flags::BlockEnd;
            advanceBlock(m_block + 1);
            continue;
        }

        // After a pause the next block starts low, unless it says otherwise
        T transition = step.transition;
        if (m_forceLow && transition == T::Toggle)
            transition = T::Low;
        if (transition != T::None || step.length > 0)
            m_forceLow = false;

        flags |= step.flags;
        if (step.blockDone) {
            flags |= TapeEdge::Flags::BlockEnd;
            if (step.nextLow)
                m_forceLow = true;
            advanceBlock(m_block + 1);
            if (m_block >= blocks.size()) {
                flags |= TapeEdge::Flags::Stop | TapeEdge::Flags::TapeEnd;
                // The last edge of the tape ends its last pulse
                if (step.length == 0 && transition == T::None)
                    transition = T::Toggle;
            }
        }

        switch (transition) {
        case T::Toggle: m_level = !m_level; break;
        case T::Low: m_level = false; break;
        case T::High: m_level = true; break;
        case T::None: break;
        }
        return {.length = step.length, .transition = transition, .flags = flags, .level = m_level};
    }
}

void TapeCursor::controlBlock(std::span<const TapeBlock> blocks, const TapeBlock &block, uint8_t &flags)
{
    const auto relative = [this, &blocks](int32_t offset) -> std::size_t {
        const int64_t target = static_cast<int64_t>(m_block) + offset;
        return static_cast<std::size_t>(std::clamp<int64_t>(target, 0, static_cast<int64_t>(blocks.size())));
    };
    flags |= TapeEdge::Flags::BlockEnd;
    std::size_t next = m_block + 1;
    switch (block.type) {
    case TapeBlockType::JumpTo:
        if (block.value != 0)
            next = relative(block.value);
        break;
    case TapeBlockType::LoopStart:
        m_loopStart = m_block + 1;
        m_loopCount = static_cast<uint32_t>(block.value);
        break;
    case TapeBlockType::LoopEnd:
        if (m_loopCount > 1) {
            --m_loopCount;
            next = m_loopStart;
        } else {
            m_loopCount = 0;
        }
        break;
    case TapeBlockType::CallSequence:
        if (!block.offsets.empty()) {
            m_callBlock = m_block;
            m_callIndex = 0;
            m_inCall = true;
            next = relative(block.offsets.front());
        }
        break;
    case TapeBlockType::ReturnFromSequence:
        if (m_inCall && m_callBlock < blocks.size()) {
            const auto &offsets = blocks[m_callBlock].offsets;
            if (++m_callIndex < offsets.size()) {
                m_block = m_callBlock;
                next = relative(offsets[m_callIndex]);
            } else {
                m_inCall = false;
                next = m_callBlock + 1;
            }
        }
        break;
    default: // descriptions, groups...: nothing to play
        break;
    }
    advanceBlock(next);
}

bool TapeCursor::nextBit(const TapeBlock &block)
{
    // Moves to the next data bit (or the first one, from the block's start)
    if (m_phase == Phase::Start) {
        m_byte = 0;
        m_bit = 0;
        if (block.data.empty())
            return false;
    } else {
        const bool lastByte = m_byte + 1 == block.data.size();
        const uint8_t bits = (lastByte && block.usedBits >= 1 && block.usedBits <= 8) ? block.usedBits : 8;
        if (++m_bit >= bits) {
            m_bit = 0;
            if (++m_byte >= block.data.size())
                return false;
        }
    }
    const bool one = (block.data[m_byte] << m_bit) & 0x80;
    m_bitPulse = one ? block.onePulse : block.zeroPulse;
    return true;
}

TapeCursor::Step TapeCursor::dataBlockStep(const TapeBlock &block)
{
    const auto bitStep = [this] {
        const uint8_t kind = m_bitPulse == 0 ? 0
                             : (m_bitPulse == m_bitPulseZero) ? TapeEdge::Flags::ShortPulse
                                                              : TapeEdge::Flags::LongPulse;
        return Step{.length = m_bitPulse, .flags = kind};
    };

    switch (m_phase) {
    case Phase::Start:
        m_bitPulseZero = block.zeroPulse;
        if (block.type == TapeBlockType::PureData) {
            const bool hasData = nextBit(block);
            m_phase = hasData ? Phase::Bit2 : Phase::Tail;
            if (!hasData)
                return tailStep(block);
            return bitStep();
        }
        m_count = block.pilotPulses;
        m_phase = Phase::Pilot;
        [[fallthrough]];
    case Phase::Pilot:
        if (m_count > 0) {
            --m_count;
            return {.length = block.pilotPulse};
        }
        m_phase = Phase::Sync2;
        return {.length = block.sync1Pulse};
    case Phase::Sync2: {
        m_phase = Phase::Start;
        const bool hasData = nextBit(block);
        m_phase = hasData ? Phase::Bit1 : Phase::Tail;
        return {.length = block.sync2Pulse};
    }
    case Phase::Bit1:
        m_phase = Phase::Bit2;
        return bitStep();
    case Phase::Bit2: {
        const Step step = bitStep();
        m_phase = nextBit(block) ? Phase::Bit1 : Phase::Tail;
        return step;
    }
    case Phase::Tail:
    default:
        return tailStep(block);
    }
}

TapeCursor::Step TapeCursor::tailStep(const TapeBlock &block)
{
    // The pause after a block: an edge ends the last pulse, then the next block starts low
    if (block.pauseAfterMs == 0)
        return {.length = 0, .transition = TapeEdge::Transition::None, .blockDone = true};
    const uint32_t length = uint32_t{block.pauseAfterMs} * kTstatesPerMs;
    const auto transition = block.type == TapeBlockType::CswRecording ? TapeEdge::Transition::Low
                                                                       : TapeEdge::Transition::Toggle;
    return {.length = length, .transition = transition, .blockDone = true, .nextLow = true};
}

TapeCursor::Step TapeCursor::pulsesStep(const TapeBlock &block)
{
    if (m_phase == Phase::Tail)
        return tailStep(block);

    const bool tone = block.type == TapeBlockType::PureTone;
    const std::size_t total = tone ? block.pilotPulses : block.pulses.size();
    if (m_phase == Phase::Start) {
        m_phase = Phase::Pulses;
        m_count = 0;
        if (total == 0)
            return block.type == TapeBlockType::CswRecording ? tailStep(block) : Step{.noSignal = true};
    }

    const uint32_t length = tone ? block.pilotPulse : block.pulses[m_count];
    // A CSW recording's first pulse keeps the level the tape had
    const auto transition = (block.type == TapeBlockType::CswRecording && m_count == 0) ? TapeEdge::Transition::None
                                                                                         : TapeEdge::Transition::Toggle;
    if (++m_count < total)
        return {.length = length, .transition = transition};
    if (block.type == TapeBlockType::CswRecording) {
        m_phase = Phase::Tail;
        return {.length = length, .transition = transition};
    }
    return {.length = length, .transition = transition, .blockDone = true};
}

TapeCursor::Step TapeCursor::samplesStep(const TapeBlock &block)
{
    if (m_phase == Phase::Start) {
        m_phase = Phase::Samples;
        m_byte = 0;
        m_bit = 0;
        if (block.data.empty() || block.sampleTstates == 0) {
            m_phase = Phase::Tail;
            return tailStep(block);
        }
    }
    if (m_phase == Phase::Tail)
        return tailStep(block);

    // One step per run of equal samples, at the run's level
    const auto bitsIn = [&block](std::size_t byte) -> uint8_t {
        const bool last = byte + 1 == block.data.size();
        return (last && block.usedBits >= 1 && block.usedBits <= 8) ? block.usedBits : 8;
    };
    const auto sample = [&block](std::size_t byte, uint8_t bit) { return ((block.data[byte] << bit) & 0x80) != 0; };

    const bool level = sample(m_byte, m_bit);
    const uint64_t maxSamples = kMaxStep / block.sampleTstates;
    uint64_t run = 0;
    while (m_byte < block.data.size() && sample(m_byte, m_bit) == level && run < maxSamples) {
        ++run;
        if (++m_bit >= bitsIn(m_byte)) {
            m_bit = 0;
            ++m_byte;
        }
    }
    if (m_byte >= block.data.size())
        m_phase = Phase::Tail;
    return {.length = static_cast<uint32_t>(run * block.sampleTstates),
            .transition = level ? TapeEdge::Transition::High : TapeEdge::Transition::Low};
}

TapeCursor::Step TapeCursor::symbolsStep(const TapeBlock &block)
{
    if (m_phase == Phase::Start) {
        m_phase = Phase::Symbols;
        m_run = 0;
        m_repeat = 0;
        m_symbol = 0;
        m_symbolPulse = 0;
        m_inDataSymbols = false;
    }
    if (m_phase == Phase::Tail)
        return tailStep(block);

    const uint8_t bits = block.symbolBits();
    const auto dataSymbol = [&block, bits](uint32_t index) -> std::size_t {
        if (bits == 0)
            return 0;
        uint32_t value = 0;
        const uint64_t first = uint64_t{index} * bits;
        for (uint8_t i = 0; i < bits; ++i) {
            const uint64_t bit = first + i;
            const std::size_t byte = static_cast<std::size_t>(bit / 8);
            const bool set = byte < block.data.size() && ((block.data[byte] << (bit % 8)) & 0x80);
            value = (value << 1) | (set ? 1u : 0u);
        }
        return value;
    };
    // One pulse of a symbol; returns false when the symbol has no more pulses
    const auto symbolPulse = [this](const TapeSymbol &symbol, Step &step) {
        if (m_symbolPulse >= symbol.pulses.size())
            return false;
        const uint16_t length = symbol.pulses[m_symbolPulse];
        if (length == 0 && m_symbolPulse > 0)
            return false;
        step.length = length;
        step.transition = TapeEdge::Transition::Toggle;
        if (m_symbolPulse == 0) {
            switch (symbol.start) {
            case TapeSymbol::Start::Toggle: break;
            case TapeSymbol::Start::Keep: step.transition = TapeEdge::Transition::None; break;
            case TapeSymbol::Start::Low: step.transition = TapeEdge::Transition::Low; break;
            case TapeSymbol::Start::High: step.transition = TapeEdge::Transition::High; break;
            }
        }
        // A zero length first pulse only sets the level
        m_symbolPulse = length == 0 ? symbol.pulses.size() : m_symbolPulse + 1;
        return true;
    };

    Step step;
    while (!m_inDataSymbols) {
        if (m_run >= block.pilotStream.size()) {
            m_inDataSymbols = true;
            break;
        }
        const TapeSymbolRun &run = block.pilotStream[m_run];
        if (m_repeat >= run.repeat || run.symbol >= block.pilotSymbols.size()) {
            ++m_run;
            m_repeat = 0;
            m_symbolPulse = 0;
            continue;
        }
        if (symbolPulse(block.pilotSymbols[run.symbol], step))
            return step;
        ++m_repeat;
        m_symbolPulse = 0;
    }
    while (m_symbol < block.dataSymbolCount) {
        const std::size_t index = dataSymbol(m_symbol);
        if (index < block.dataSymbols.size() && symbolPulse(block.dataSymbols[index], step))
            return step;
        ++m_symbol;
        m_symbolPulse = 0;
    }
    m_phase = Phase::Tail;
    return tailStep(block);
}

} // namespace zxkit
