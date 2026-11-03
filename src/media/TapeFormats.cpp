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
#include "zxkit/core/StateStream.h"
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
    std::size_t position() const noexcept { return m_pos; }
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
    uint64_t remainder = 0;
    Reader reader(rle);
    while (!reader.atEnd()) {
        uint32_t samples = reader.u8();
        if (samples == 0)
            samples = reader.u32();
        if (samples == 0)
            throw FormatException("Zero CSW pulse duration");
        const uint64_t numerator = uint64_t{samples} * 3'500'000 + remainder;
        uint64_t length = numerator / sampleRate;
        remainder = numerator % sampleRate;
        // Two extra transitions join adjacent pieces of an exceptionally long pulse.
        while (length > kMaxStep) {
            pulses.push_back(kMaxStep);
            pulses.push_back(0);
            length -= kMaxStep;
        }
        pulses.push_back(static_cast<uint32_t>(length));
        if (pulses.size() > 16'000'000)
            throw FormatException("Too many CSW pulses");
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
        (void)b.u32(); // Number of encoded pulses (long pulses can expand when resampled).
        const auto payload = b.bytes(b.left());
        if (compression == 1)
            blk.pulses = cswPulses(payload, sampleRate);
        else if (compression == 2)
            blk.pulses = cswPulses(zlibDecompressBounded(payload, 64 * 1024 * 1024), sampleRate);
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
        const auto bodyStart = r.position();
        try {
            auto block = parseTzxBlock(id, r);
            if ((block.isMetadata() || block.type == TapeBlockType::CswRecording) && static_cast<uint8_t>(block.type) == id)
                block.metadata = toVector(raw.subspan(10 + bodyStart, r.position() - bodyStart));
            blocks.push_back(std::move(block));
        } catch (const FormatException &) {
            // A truncated last block: keep what came before it, like a damaged tape
            if (blocks.empty())
                throw;
            break;
        }
    }
    return blocks;
}

std::vector<TapeBlock> parsePzx(std::span<const uint8_t> raw)
{
    if (raw.size() < 10 || !std::ranges::equal(raw.first(4), std::string_view("PZXT")))
        throw FormatException("Invalid PZX header");
    Reader r(raw);
    std::vector<TapeBlock> blocks;
    while (!r.atEnd()) {
        const auto tagBytes = r.bytes(4);
        const std::string_view tag(reinterpret_cast<const char *>(tagBytes.data()), 4);
        Reader body = r.sub(r.u32());
        TapeBlock block;
        block.pauseAfterMs = 0;
        if (tag == "PZXT") {
            if (body.u8() != 1)
                throw FormatException("Unsupported PZX major version");
            (void)body.u8();
            block.type = TapeBlockType::TextDesc;
            const auto info = body.bytes(body.left());
            const auto end = std::ranges::find(info, uint8_t{0});
            block.description.assign(info.begin(), end);
            if (block.description.empty()) block.description = "Tape start";
        } else if (tag == "PULS") {
            block.type = TapeBlockType::PzxPulses;
            while (!body.atEnd()) {
                uint32_t duration = body.u16();
                uint32_t count = 1;
                if (duration > 0x8000) {
                    count = duration & 0x7FFF;
                    duration = body.u16();
                }
                if (duration >= 0x8000)
                    duration = ((duration & 0x7FFF) << 16) | body.u16();
                // Even zero-pulse repeats cancel; odd repeats are one edge.
                if (duration == 0) count &= 1;
                if (count) {
                    block.pulses.push_back(duration);
                    block.pulses.push_back(count);
                }
            }
            block.description = "PZX pulse sequence";
        } else if (tag == "DATA") {
            block.type = TapeBlockType::PzxData;
            const uint32_t count = body.u32();
            block.dataSymbolCount = count & 0x7FFF'FFFF;
            block.value = static_cast<int32_t>(count >> 31);
            block.pilotPulse = body.u16(); // Optional tail pulse.
            const uint8_t p0 = body.u8();
            const uint8_t p1 = body.u8();
            for (const uint8_t pulses : {p0, p1}) {
                TapeSymbol symbol;
                for (uint16_t i = 0; i < pulses; ++i) symbol.pulses.push_back(body.u16());
                block.dataSymbols.push_back(std::move(symbol));
            }
            block.data = toVector(body.bytes((uint64_t{block.dataSymbolCount} + 7) / 8));
            block.usedBits = block.dataSymbolCount % 8 ? block.dataSymbolCount % 8 : 8;
            block.description = "PZX " + describeStandardBlock(block.data);
        } else if (tag == "PAUS") {
            block.type = TapeBlockType::PzxPause;
            const auto duration = body.u32();
            block.value = static_cast<int32_t>(duration >> 31);
            block.pulses = {duration & 0x7FFF'FFFF};
            block.description = "PZX pause";
        } else if (tag == "STOP") {
            block.type = body.u16() == 1 ? TapeBlockType::StopIf48k : TapeBlockType::Pause;
            block.description = "Stop the tape";
        } else if (tag == "BRWS") {
            block.type = TapeBlockType::TextDesc;
            block.description = body.text(body.left());
        } else {
            continue; // Unknown PZX tags must be ignored.
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

std::vector<TapeBlock> parseCsw(std::span<const uint8_t> raw)
{
    constexpr std::string_view signature("Compressed Square Wave\x1A", 23);
    if (raw.size() < 32 || !std::ranges::equal(raw.first(23), signature))
        throw FormatException("Invalid CSW header");
    Reader r(raw.subspan(23));
    const auto major = r.u8();
    (void)r.u8();
    uint32_t rate, expectedCount = 0;
    uint8_t compression, flags;
    if (major == 1) {
        rate = r.u16();
        compression = r.u8();
        flags = r.u8();
        r.skip(3);
    } else if (major == 2) {
        rate = r.u32();
        expectedCount = r.u32();
        compression = r.u8();
        flags = r.u8();
        const auto extra = r.u8();
        r.skip(16 + extra);
    } else {
        throw FormatException("Unsupported CSW major version");
    }
    const auto payload = r.bytes(r.left());
    std::vector<uint8_t> decompressed;
    std::span<const uint8_t> rle = payload;
    if (compression == 2) {
        decompressed = zlibDecompressBounded(payload, 64 * 1024 * 1024);
        rle = decompressed;
    } else if (compression != 1) {
        throw FormatException("Unsupported CSW compression");
    }
    // Count the encoded pulses, before resampling can split long ones.
    Reader check(rle);
    uint64_t count = 0;
    while (!check.atEnd()) {
        if (check.u8() == 0 && check.u32() == 0)
            throw FormatException("Zero CSW pulse duration");
        ++count;
    }
    if (major == 2 && count != expectedCount)
        throw FormatException("CSW pulse count does not match its header");
    TapeBlock level;
    level.type = TapeBlockType::SetSignalLevel;
    level.value = flags & 1;
    level.pauseAfterMs = 0;
    level.description = level.value ? "Signal level: high" : "Signal level: low";
    TapeBlock signal;
    signal.type = TapeBlockType::CswRecording;
    signal.pauseAfterMs = 0;
    signal.pulses = cswPulses(rle, rate);
    signal.description = std::format("CSW recording ({} pulses)", count);
    if (rate <= 0xFF'FFFF && count <= UINT32_MAX) {
        StateWriter body;
        body.write<uint32_t>(static_cast<uint32_t>(10 + payload.size()));
        body.write<uint16_t>(0);
        body.write<uint16_t>(static_cast<uint16_t>(rate));
        body.write<uint8_t>(static_cast<uint8_t>(rate >> 16));
        body.write(compression);
        body.write<uint32_t>(static_cast<uint32_t>(count));
        body.writeBytes(payload);
        signal.metadata = body.takeBuffer();
    }
    return {std::move(level), std::move(signal)};
}

std::vector<uint8_t> writeTap(std::span<const TapeBlock> blocks)
{
    std::vector<uint8_t> out;
    for (const auto &blk : blocks) {
        // Only the blocks the ROM can load fit in a TAP file
        if (blk.isMetadata())
            continue;
        if (!blk.isRomBlock() || blk.data.size() > 0xFFFF)
            throw FormatException("This tape needs TZX export to preserve its signal and control blocks");
        const auto length = static_cast<uint16_t>(blk.data.size());
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.insert(out.end(), blk.data.begin(), blk.data.end());
    }
    return out;
}

std::vector<uint8_t> writeTzx(std::span<const TapeBlock> blocks)
{
    constexpr std::size_t limit = 64 * 1024 * 1024;
    StateWriter out;
    out.writeTag(std::string_view("ZXTape!\x1A\x01\x14", 10));
    const auto pzx = [](const TapeBlock &b) {
        return b.type == TapeBlockType::PzxPulses || b.type == TapeBlockType::PzxData || b.type == TapeBlockType::PzxPause;
    };
    // PZX's absolute polarity needs two extra level blocks in TZX. Remap every control target.
    std::vector<int64_t> indices{0};
    for (const auto &b : blocks) indices.push_back(indices.back() + (pzx(b) ? 3 : 1));
    const auto offset = [&](std::size_t from, int32_t delta) -> int16_t {
        const int64_t target = static_cast<int64_t>(from) + delta;
        if (target < 0 || target > static_cast<int64_t>(blocks.size()))
            throw FormatException("TZX control target is outside the tape");
        const int64_t value = indices[target] - indices[from];
        if (value < -32768 || value > 32767) throw FormatException("TZX control target exceeds 16 bits");
        return static_cast<int16_t>(value);
    };
    const auto text = [](StateWriter &w, std::string_view s) {
        if (s.size() > 255) throw FormatException("TZX text exceeds 255 bytes");
        w.write<uint8_t>(static_cast<uint8_t>(s.size())); w.writeTag(s);
    };
    const auto u24 = [](StateWriter &w, std::size_t n) {
        if (n > 0xFF'FFFF) throw FormatException("TZX data block exceeds 24 bits");
        w.write<uint16_t>(static_cast<uint16_t>(n)); w.write<uint8_t>(static_cast<uint8_t>(n >> 16));
    };
    const auto csw = [&](std::span<const uint32_t> pulses, uint16_t pause) {
        StateWriter rle;
        for (const auto duration : pulses) {
            if (duration == 0) throw FormatException("TZX CSW cannot encode a zero-duration pulse");
            if (duration <= 255) rle.write<uint8_t>(static_cast<uint8_t>(duration));
            else { rle.write<uint8_t>(0); rle.write(duration); }
            if (rle.size() > limit) throw FormatException("TZX waveform exceeds the export size limit");
        }
        const auto compressed = zlibCompress(rle.data());
        out.write<uint8_t>(0x18); out.write<uint32_t>(10 + static_cast<uint32_t>(compressed.size()));
        out.write(pause); u24(out, 3'500'000); out.write<uint8_t>(2);
        out.write<uint32_t>(static_cast<uint32_t>(pulses.size())); out.writeBytes(compressed);
    };
    const auto setLevel = [&](bool high) {
        out.write<uint8_t>(0x2B); out.write<uint32_t>(1); out.write<uint8_t>(high);
    };
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto &b = blocks[index];
        if (pzx(b)) {
            TapeCursor cursor;
            const std::span<const TapeBlock> one(&b, 1);
            std::vector<uint32_t> pulses;
            bool initial = b.value != 0, level = initial, havePulse = false;
            bool final = initial;
            uint64_t duration = 0;
            const auto flush = [&] {
                if (duration > UINT32_MAX) throw FormatException("TZX CSW pulse exceeds 32 bits");
                if (duration) pulses.push_back(static_cast<uint32_t>(duration));
                duration = 0;
            };
            for (uint64_t steps = 0;; ++steps) {
                if (steps > limit) throw FormatException("TZX waveform exceeds the export step limit");
                const auto edge = cursor.next(one);
                if (edge.length) {
                    if (!havePulse) { initial = edge.level; level = edge.level; havePulse = true; }
                    if (edge.level != level) { flush(); level = edge.level; }
                    duration += edge.length;
                }
                final = edge.level;
                if (edge.flags & TapeEdge::Flags::TapeEnd) break;
            }
            flush();
            setLevel(initial); csw(pulses, 0); setLevel(final);
            continue;
        }
        if (!b.metadata.empty() && (b.isMetadata() || b.type == TapeBlockType::CswRecording) && b.type != TapeBlockType::SelectBlock) {
            out.write<uint8_t>(static_cast<uint8_t>(b.type)); out.writeBytes(b.metadata);
            continue;
        }
        out.write<uint8_t>(static_cast<uint8_t>(b.type));
        switch (b.type) {
        case TapeBlockType::StandardData:
            if (b.data.size() > 65535) throw FormatException("Standard TZX data exceeds 16 bits");
            out.write(b.pauseAfterMs); out.write<uint16_t>(static_cast<uint16_t>(b.data.size())); out.writeBytes(b.data); break;
        case TapeBlockType::TurboData:
            out.write(b.pilotPulse); out.write(b.sync1Pulse); out.write(b.sync2Pulse);
            out.write(b.zeroPulse); out.write(b.onePulse); out.write(b.pilotPulses);
            out.write(b.usedBits); out.write(b.pauseAfterMs); u24(out, b.data.size()); out.writeBytes(b.data); break;
        case TapeBlockType::PureTone: out.write(b.pilotPulse); out.write(b.pilotPulses); break;
        case TapeBlockType::PulseSeq:
            if (b.pulses.size() > 255) throw FormatException("TZX pulse sequence exceeds 255 entries");
            out.write<uint8_t>(static_cast<uint8_t>(b.pulses.size()));
            for (auto pulse : b.pulses) {
                if (pulse > 65535) throw FormatException("TZX pulse sequence duration exceeds 16 bits");
                out.write<uint16_t>(static_cast<uint16_t>(pulse));
            }
            break;
        case TapeBlockType::PureData:
            out.write(b.zeroPulse); out.write(b.onePulse); out.write(b.usedBits);
            out.write(b.pauseAfterMs); u24(out, b.data.size()); out.writeBytes(b.data); break;
        case TapeBlockType::DirectRecording:
            out.write(b.sampleTstates); out.write(b.pauseAfterMs); out.write(b.usedBits);
            u24(out, b.data.size()); out.writeBytes(b.data); break;
        case TapeBlockType::CswRecording: {
            // Zero pulses (resampling/long pulses) join runs without changing the signal.
            StateWriter rle;
            std::vector<uint32_t> runs;
            uint64_t duration = 0;
            bool level = false, current = false;
            for (std::size_t i = 0; i < b.pulses.size(); ++i) {
                current = i % 2 != 0;
                if (!b.pulses[i]) continue;
                if (current != level && duration) {
                    if (duration > UINT32_MAX) throw FormatException("CSW pulse exceeds 32 bits");
                    runs.push_back(static_cast<uint32_t>(duration)); duration = 0;
                }
                if (runs.empty() && duration == 0 && current)
                    throw FormatException("CSW starts with a zero pulse; export its original format");
                level = current; duration += b.pulses[i];
            }
            if (duration) {
                if (duration > UINT32_MAX) throw FormatException("CSW pulse exceeds 32 bits");
                runs.push_back(static_cast<uint32_t>(duration));
            }
            for (const auto pulse : runs) {
                if (pulse <= 255) rle.write<uint8_t>(static_cast<uint8_t>(pulse));
                else { rle.write<uint8_t>(0); rle.write(pulse); }
            }
            const auto compressed = zlibCompress(rle.data());
            out.write<uint32_t>(10 + static_cast<uint32_t>(compressed.size())); out.write(b.pauseAfterMs);
            u24(out, 3'500'000); out.write<uint8_t>(2); out.write<uint32_t>(static_cast<uint32_t>(runs.size()));
            out.writeBytes(compressed); break;
        }
        case TapeBlockType::GeneralizedData: {
            StateWriter body;
            body.write(b.pauseAfterMs); body.write<uint32_t>(static_cast<uint32_t>(b.pilotStream.size()));
            const auto maxPulses = [](const auto &symbols) {
                std::size_t count = 0;
                for (const auto &s : symbols) count = std::max(count, s.pulses.size());
                if (count > 255 || symbols.size() > 256) throw FormatException("TZX symbol alphabet is too large");
                return static_cast<uint8_t>(count);
            };
            const auto pilotMax = maxPulses(b.pilotSymbols), dataMax = maxPulses(b.dataSymbols);
            body.write(pilotMax); body.write<uint8_t>(static_cast<uint8_t>(b.pilotSymbols.size()));
            body.write(b.dataSymbolCount); body.write(dataMax); body.write<uint8_t>(static_cast<uint8_t>(b.dataSymbols.size()));
            const auto symbols = [&](const auto &alphabet, uint8_t count) {
                for (const auto &symbol : alphabet) {
                    body.write<uint8_t>(static_cast<uint8_t>(symbol.start));
                    for (std::size_t p = 0; p < count; ++p) body.write<uint16_t>(p < symbol.pulses.size() ? symbol.pulses[p] : 0);
                }
            };
            if (!b.pilotStream.empty()) {
                if (b.pilotSymbols.empty()) throw FormatException("Missing TZX pilot alphabet");
                symbols(b.pilotSymbols, pilotMax);
                for (const auto &run : b.pilotStream) { body.write(run.symbol); body.write(run.repeat); }
            }
            if (b.dataSymbolCount) {
                if (b.dataSymbols.empty()) throw FormatException("Missing TZX data alphabet");
                symbols(b.dataSymbols, dataMax);
                body.writeBytes(b.data);
            }
            out.write<uint32_t>(static_cast<uint32_t>(body.size())); out.writeBytes(body.data()); break;
        }
        case TapeBlockType::Pause: out.write(b.pauseAfterMs); break;
        case TapeBlockType::JumpTo: out.write(offset(index, b.value)); break;
        case TapeBlockType::LoopStart: out.write<uint16_t>(static_cast<uint16_t>(b.value)); break;
        case TapeBlockType::CallSequence:
            if (b.offsets.size() > 65535) throw FormatException("Too many TZX calls");
            out.write<uint16_t>(static_cast<uint16_t>(b.offsets.size()));
            for (auto delta : b.offsets) out.write(offset(index, delta));
            break;
        case TapeBlockType::SelectBlock: {
            if (b.offsets.size() > 255 || b.offsets.size() != b.labels.size()) throw FormatException("Invalid TZX selection");
            StateWriter body; body.write<uint8_t>(static_cast<uint8_t>(b.offsets.size()));
            for (std::size_t i = 0; i < b.offsets.size(); ++i) { body.write(offset(index, b.offsets[i])); text(body, b.labels[i]); }
            if (body.size() > 65535) throw FormatException("TZX selection exceeds 16 bits");
            out.write<uint16_t>(static_cast<uint16_t>(body.size())); out.writeBytes(body.data()); break;
        }
        case TapeBlockType::StopIf48k: out.write<uint32_t>(0); break;
        case TapeBlockType::SetSignalLevel: out.write<uint32_t>(1); out.write<uint8_t>(b.value != 0); break;
        case TapeBlockType::Message: out.write<uint8_t>(0); text(out, b.description); break;
        case TapeBlockType::GroupStart: case TapeBlockType::TextDesc: text(out, b.description); break;
        case TapeBlockType::GroupEnd: case TapeBlockType::LoopEnd: case TapeBlockType::ReturnFromSequence: break;
        default: throw FormatException("Cannot export this tape block without its original metadata");
        }
        if (out.size() > limit) throw FormatException("TZX exceeds the export size limit");
    }
    if (out.size() > limit) throw FormatException("TZX exceeds the export size limit");
    return out.takeBuffer();
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
        case TapeBlockType::PzxPulses:
        case TapeBlockType::PzxData:
        case TapeBlockType::PzxPause:
            step = pzxStep(block);
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

TapeCursor::Step TapeCursor::pzxStep(const TapeBlock &block)
{
    using T = TapeEdge::Transition;
    const bool first = m_phase == Phase::Start;
    if (first) m_phase = Phase::Pulses;
    const T transition = m_byte++ == 0 ? (block.value ? T::High : T::Low) : T::Toggle;
    if (block.type == TapeBlockType::PzxPause) {
        if (first)
            return {.length = block.pulses.empty() ? 0 : block.pulses[0], .transition = transition};
    } else if (block.type == TapeBlockType::PzxPulses) {
        while (m_run + 1 < block.pulses.size()) {
            if (m_repeat++ < block.pulses[m_run + 1])
                return {.length = block.pulses[m_run], .transition = transition};
            m_run += 2;
            m_repeat = 0;
        }
    } else {
        // Each data symbol is kept compact, including its zero-duration transitions.
        if (block.dataSymbols[0].pulses.empty() && block.dataSymbols[1].pulses.empty())
            m_symbol = block.dataSymbolCount;
        while (m_symbol < block.dataSymbolCount) {
            const auto bit = (block.data[m_symbol / 8] >> (7 - m_symbol % 8)) & 1;
            const auto &pulses = block.dataSymbols[bit].pulses;
            if (m_symbolPulse < pulses.size())
                return {.length = pulses[m_symbolPulse++], .transition = transition};
            ++m_symbol;
            m_symbolPulse = 0;
        }
        if (m_phase != Phase::Tail && block.pilotPulse) {
            m_phase = Phase::Tail;
            return {.length = block.pilotPulse, .transition = transition};
        }
    }
    // End the last pulse. An empty block still defines its initial level.
    return {.transition = transition, .blockDone = true};
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
