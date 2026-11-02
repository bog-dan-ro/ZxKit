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

// The loader detection and acceleration follow FUSE's loader.c and loader_detector.c
// (Copyright (c) 2006-2016 Philip Kendall, Fredrick Meunier and the FUSE contributors).

#include "zxkit/media/TapeDeck.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/bus/Peripheral.h"
#include "zxkit/cpu/Z80Cpu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace zxkit {

namespace {

constexpr uint64_t kTapeClockHz = 3'500'000;

// ── Code patterns ─────────────────────────────────────────────────────────

struct PatternByte {
    std::array<uint8_t, 4> values{};
    std::array<uint8_t, 4> masks{};
    uint8_t alternatives = 0; ///< 0: any byte

    bool matches(uint8_t byte) const noexcept
    {
        if (alternatives == 0)
            return true;
        for (uint8_t i = 0; i < alternatives; ++i) {
            if ((byte & masks[i]) == values[i])
                return true;
        }
        return false;
    }
};

constexpr PatternByte B(uint8_t value) noexcept { return {{value}, {0xFF}, 1}; }
constexpr PatternByte masked(uint8_t value, uint8_t mask) noexcept { return {{value}, {mask}, 1}; }
constexpr PatternByte oneOf(uint8_t a, uint8_t b) noexcept { return {{a, b}, {0xFF, 0xFF}, 2}; }
constexpr PatternByte oneOf(uint8_t a, uint8_t b, uint8_t c) noexcept { return {{a, b, c}, {0xFF, 0xFF, 0xFF}, 3}; }
constexpr PatternByte oneOf(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept
{
    return {{a, b, c, d}, {0xFF, 0xFF, 0xFF, 0xFF}, 4};
}
constexpr PatternByte kAny{};

template <std::size_t... N>
constexpr auto join(const std::array<PatternByte, N> &...parts) noexcept
{
    std::array<PatternByte, (N + ...)> out{};
    std::size_t i = 0;
    ((std::ranges::copy(parts, out.begin() + static_cast<std::ptrdiff_t>(i)), i += N), ...);
    return out;
}

template <typename... Bytes>
constexpr std::array<PatternByte, sizeof...(Bytes)> seq(Bytes... bytes) noexcept
{
    return {bytes...};
}

bool matches(const Mmu &mmu, uint16_t address, std::span<const PatternByte> pattern) noexcept
{
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (!pattern[i].matches(mmu.peek(static_cast<uint16_t>(address + i))))
            return false;
    }
    return true;
}

bool wordMatches(const Mmu &mmu, uint16_t address, uint16_t value) noexcept
{
    return mmu.peek(address) == (value & 0xFF) && mmu.peek(static_cast<uint16_t>(address + 1)) == (value >> 8);
}

// INC B; RET Z; LD A,nn
constexpr auto kRomLoaderPrefix = seq(B(0x04), B(0xC8), B(0x3E), oneOf(0x00, 0x7F, 0xFF));
constexpr auto kRomLoaderInput = seq(B(0xDB), B(0xFE));        // IN A,(FE)
constexpr auto kRomLoaderEdgeTest = seq(B(0x1F));              // RRA
constexpr auto kRomLoaderLevelTest = seq(B(0xA9), B(0xE6), B(0x20), B(0x28)); // XOR C; AND 20; JR Z
constexpr auto kRomLoaderEdgeAction = seq(oneOf(0x00, 0xA7, 0xC8, 0xD0));

constexpr auto kRomLoader = join(kRomLoaderPrefix, kRomLoaderInput, kRomLoaderEdgeTest, kRomLoaderLevelTest, seq(B(0xF4)));
constexpr auto kRomLoaderWithEdgeAction = join(kRomLoaderPrefix, kRomLoaderInput, kRomLoaderEdgeTest,
                                               kRomLoaderEdgeAction, kRomLoaderLevelTest, seq(B(0xF3)));
constexpr auto kMicroprosePrefix = join(kRomLoaderPrefix, seq(B(0x3E), B(0x7F)), kRomLoaderInput, kRomLoaderEdgeTest);
constexpr auto kMicroproseLoader = join(kMicroprosePrefix, kRomLoaderLevelTest, seq(B(0xF2)));
constexpr auto kMicroproseLoaderWithEdgeAction = join(kMicroprosePrefix, kRomLoaderEdgeAction, kRomLoaderLevelTest,
                                                      seq(B(0xF1)));
constexpr auto kSearchLoader = join(kRomLoaderPrefix, kRomLoaderInput, seq(B(0xA9), B(0xE6), B(0x40), B(0x28), B(0xF5)));
constexpr auto kSearchLoaderWithRet = join(kRomLoaderPrefix, kRomLoaderInput,
                                           seq(B(0xA9), B(0xE6), B(0x40), B(0xD8), B(0x00), B(0x28), B(0xF3)));
constexpr auto kAlkatrazSuffix = join(kRomLoaderInput, kRomLoaderEdgeTest, seq(B(0xC8)), kRomLoaderLevelTest,
                                      seq(oneOf(0xF1, 0xF3)));
constexpr auto kAlkatrazLoader = join(seq(B(0x03), B(0xC3), kAny, kAny), kAlkatrazSuffix);
constexpr auto kAlkatrazVariantLoader = join(seq(B(0x04), B(0x20), B(0x01), B(0xC9)), kAlkatrazSuffix);
constexpr auto kSoftwareProjectsLoader = join(seq(B(0x47), B(0x08), B(0x3E), B(0x7F)), kRomLoaderInput,
                                              seq(B(0xA9), B(0xE6), B(0x40), B(0x20), B(0x04), B(0x05), B(0x20), B(0xF4)));
constexpr auto kDigitalIntegrationLoader = join(seq(kAny, kAny, B(0x05), B(0xC8)), kRomLoaderInput,
                                                seq(B(0xA9), B(0xE6), B(0x40), B(0xCA), kAny, kAny));
// LD L,0; INC L; IN A,(FE); AND H; JP Z,<INC L>
constexpr auto kGremlinRisingLoader = join(seq(B(0x2E), B(0x00), B(0x2C)), kRomLoaderInput,
                                           seq(B(0xA4), B(0xCA), kAny, kAny));
// LD A,8; OUT (FE),A; INC L; IN A,(FE); AND H; JP NZ,<INC L>
constexpr auto kGremlinFallingLoader = join(seq(B(0x3E), B(0x08), B(0xD3), B(0xFE), B(0x2C)), kRomLoaderInput,
                                            seq(B(0xA4), B(0xC2), kAny, kAny));
// EXX; LD DE,nn; LD HL,nn; LD B,5; CALL nn; RET NC; EXX
constexpr auto kModifiedRomPrefix = seq(B(0xD9), B(0x11), kAny, kAny, B(0x21), kAny, kAny, B(0x06), B(0x05), B(0xCD),
                                        kAny, kAny, B(0xD0), B(0xD9));
constexpr auto kModifiedRomEdge2Prefix = seq(B(0xD9), B(0xA7));
// INC D; RET Z; LD A,7F; IN A,(FE); RRA; NOP; XOR E; AND 20; JR Z,<INC D>
constexpr auto kMovieloadLoader = join(seq(B(0x14), B(0xC8), B(0x3E), B(0x7F)), kRomLoaderInput,
                                       seq(B(0x1F), B(0x00), B(0xAB), B(0xE6), B(0x20), B(0x28), B(0xF3)));
// INC B; RET Z; IN A,(FE); ADD A,A; JP P/M,<INC B>
constexpr auto kSignFlagLoader = join(seq(B(0x04), B(0xC8)), kRomLoaderInput,
                                      seq(B(0x87), masked(0xF2, 0xF7), kAny, kAny));

enum class Mode : uint8_t { None, Increasing, Decreasing, SoftwareProjects, GremlinRising, GremlinFalling };

struct AccelerationPattern {
    std::span<const PatternByte> bytes;
    Mode mode;
};

const std::array kAccelerationPatterns{
    AccelerationPattern{kRomLoader, Mode::Increasing},
    AccelerationPattern{kRomLoaderWithEdgeAction, Mode::Increasing},
    AccelerationPattern{kMicroproseLoader, Mode::Increasing},
    AccelerationPattern{kMicroproseLoaderWithEdgeAction, Mode::Increasing},
    AccelerationPattern{kSearchLoader, Mode::Increasing},
    AccelerationPattern{kSearchLoaderWithRet, Mode::Increasing},
    AccelerationPattern{kAlkatrazLoader, Mode::Increasing},
    AccelerationPattern{kAlkatrazVariantLoader, Mode::Increasing},
    AccelerationPattern{kSoftwareProjectsLoader, Mode::SoftwareProjects},
};

// `start`: where the loop's code starts; `pc`: the CPU's PC (after the IN)
Mode accelerationAt(const Mmu &mmu, uint16_t start, uint16_t pc) noexcept
{
    const uint8_t first = mmu.peek(start);
    // These first bytes start other loaders, never a Digital Integration one
    if (first != 0x03 && first != 0x04 && first != 0x47 && matches(mmu, start, kDigitalIntegrationLoader)
        && wordMatches(mmu, static_cast<uint16_t>(start + 10), static_cast<uint16_t>(pc - 4)))
        return Mode::Decreasing;
    for (const auto &pattern : kAccelerationPatterns) {
        if (matches(mmu, start, pattern.bytes))
            return pattern.mode;
    }
    return Mode::None;
}

Mode gremlinAcceleration(const Mmu &mmu, uint16_t pc) noexcept
{
    if (matches(mmu, static_cast<uint16_t>(pc - 5), kGremlinRisingLoader)
        && wordMatches(mmu, static_cast<uint16_t>(pc + 2), static_cast<uint16_t>(pc - 3)))
        return Mode::GremlinRising;
    if (matches(mmu, static_cast<uint16_t>(pc - 7), kGremlinFallingLoader)
        && wordMatches(mmu, static_cast<uint16_t>(pc + 2), static_cast<uint16_t>(pc - 3)))
        return Mode::GremlinFalling;
    return Mode::None;
}

// Technician Ted replaces the ROM's EDGE1 delay with useful work: the loop is a loader,
// but accelerating it would corrupt the load
bool isModifiedRomLoader(const Mmu &mmu, uint16_t pc) noexcept
{
    return matches(mmu, static_cast<uint16_t>(pc - 40), kModifiedRomPrefix)
           && wordMatches(mmu, static_cast<uint16_t>(pc - 30), static_cast<uint16_t>(pc - 24))
           && matches(mmu, static_cast<uint16_t>(pc - 8), kModifiedRomEdge2Prefix)
           && accelerationAt(mmu, static_cast<uint16_t>(pc - 6), pc) == Mode::Increasing;
}

Mode accelerationAtIn(const Mmu &mmu, uint16_t pc) noexcept
{
    if (isModifiedRomLoader(mmu, pc))
        return Mode::None;
    if (const Mode mode = gremlinAcceleration(mmu, pc); mode != Mode::None)
        return mode;
    if (const Mode mode = accelerationAt(mmu, static_cast<uint16_t>(pc - 6), pc); mode != Mode::None)
        return mode;
    // Microprose puts another LD A,7F before the IN
    return accelerationAt(mmu, static_cast<uint16_t>(pc - 8), pc);
}

bool isMovieloadLoader(const Mmu &mmu, uint16_t pc) noexcept
{
    return matches(mmu, static_cast<uint16_t>(pc - 6), kMovieloadLoader);
}

bool isSignFlagLoader(const Mmu &mmu, uint16_t pc) noexcept
{
    return matches(mmu, static_cast<uint16_t>(pc - 4), kSignFlagLoader)
           && wordMatches(mmu, static_cast<uint16_t>(pc + 2), static_cast<uint16_t>(pc - 4));
}

bool isLoaderLoop(const Mmu &mmu, uint16_t pc) noexcept
{
    return isModifiedRomLoader(mmu, pc) || accelerationAtIn(mmu, pc) != Mode::None || isSignFlagLoader(mmu, pc);
}

// CPL; AND 1F: a keyboard scan keeps only the key bits
bool isKeyboardScan(const Mmu &mmu, uint16_t pc) noexcept
{
    return mmu.peek(pc) == 0x2F && mmu.peek(static_cast<uint16_t>(pc + 1)) == 0xE6
           && mmu.peek(static_cast<uint16_t>(pc + 2)) == 0x1F;
}

// A loader tests EAR with AND 40, or rotates it to bit 5 first (RRA ... AND 20)
bool readUsesEar(const Mmu &mmu, uint16_t pc) noexcept
{
    const auto at = [&mmu, pc](int offset) { return mmu.peek(static_cast<uint16_t>(pc + offset)); };
    if (at(0) == 0x1F) {
        for (int i = 1; i < 6; ++i) {
            if (at(i) == 0xE6 && at(i + 1) == 0x20)
                return true;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (at(i) == 0xE6 && at(i + 1) == 0x40)
            return true;
    }
    return false;
}

bool isLoaderRead(const Mmu &mmu, uint16_t pc) noexcept
{
    return isLoaderLoop(mmu, pc) || isMovieloadLoader(mmu, pc) || readUsesEar(mmu, pc);
}

// Loaders are detected after a few reads, so the playback starts between two samples
constexpr int kDetectionReads = 10;
constexpr int kMovieloadDetectionReads = 128;
constexpr int kStopNonEarReads = 10;
// Some games run BASIC and scan the keyboard for seconds between two ROM loads
constexpr uint32_t kStopGraceFrames = 250;

constexpr int kSoftwareProjectsShortIterations = 10;
constexpr int kSoftwareProjectsLongIterations = 20;
constexpr int kGremlinShortIterations = 24;
constexpr int kGremlinLongIterations = 48;

// ── ROM routines ──────────────────────────────────────────────────────────

// The end of LD-BYTES' set up, just before its RET NZ (0x056B, or 0x0111 in the TC2068 EXROM):
// IN A,(FE); RRA; AND 20; OR 02; LD C,A; CP A; RET NZ
constexpr std::array<uint8_t, 10> kLdBytesSignature{0xDB, 0xFE, 0x1F, 0xE6, 0x20, 0xF6, 0x02, 0x4F, 0xBF, 0xC0};
// SA-BYTES up to its EX AF,AF' (0x04D0, or 0x0076 in the TC2068 EXROM):
// PUSH HL; LD HL,1F80; BIT 7,A; JR Z,+3; LD HL,0C98; EX AF,AF'
constexpr std::array<uint8_t, 12> kSaBytesSignature{0xE5, 0x21, 0x80, 0x1F, 0xCB, 0x7F, 0x28, 0x03, 0x21, 0x98, 0x0C, 0x08};

bool codeMatches(const Mmu &mmu, uint16_t address, std::span<const uint8_t> code) noexcept
{
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (mmu.peek(static_cast<uint16_t>(address + i)) != code[i])
            return false;
    }
    return true;
}

uint8_t sz53p(uint8_t value) noexcept
{
    uint8_t f = value & (Z80Flags::S | Z80Flags::F3 | Z80Flags::F5);
    if (value == 0)
        f |= Z80Flags::Z;
    if ((std::popcount(value) & 1) == 0)
        f |= Z80Flags::PV;
    return f;
}

// The flags of CP 1 (the ROM's parity check: carry when A is 0)
uint8_t cpOneFlags(uint8_t a) noexcept
{
    const auto result = static_cast<uint16_t>(a - 1);
    uint8_t f = Z80Flags::N | (result & Z80Flags::S);
    if (result & 0x100)
        f |= Z80Flags::C;
    else if (result == 0)
        f |= Z80Flags::Z;
    if ((a & 0x0F) == 0)
        f |= Z80Flags::H;
    if (a == 0x80)
        f |= Z80Flags::PV;
    return f;
}

void returnFromRoutine(Z80Registers &regs, const Mmu &mmu) noexcept
{
    const uint8_t lo = mmu.peek(regs.SP++);
    const uint8_t hi = mmu.peek(regs.SP++);
    regs.PC = static_cast<uint16_t>((hi << 8) | lo);
}

} // namespace

// ── The tape ──────────────────────────────────────────────────────────────

void TapeDeck::clear()
{
    load({});
}

void TapeDeck::load(std::vector<TapeBlock> blocks)
{
    stop();
    m_blocks = std::move(blocks);
    seekBlock(0);
}

void TapeDeck::seekBlock(std::size_t index) noexcept
{
    m_cursor.seek(std::min(index, m_blocks.size()));
    m_level = false;
    m_stopPending = false;
    m_pausedRemaining = 0;
    m_scaleRemainder = 0;
    m_timelineStarted = false;
}

// ── Playback ──────────────────────────────────────────────────────────────

void TapeDeck::setPlaying(bool playing) noexcept
{
    if (playing)
        play(false);
    else
        stop();
}

void TapeDeck::setOptions(const TapeOptions &options) noexcept
{
    m_options = options;
    if (!m_options.accelerateLoaders)
        m_acceleration = Acceleration::None;
}

void TapeDeck::play(bool autoPlay) noexcept
{
    if (m_playing || m_cursor.atEnd(m_blocks))
        return;
    m_playing = true;
    m_autoPlaying = autoPlay;
    m_stopPending = false;
    m_micOffPending = false;
    m_timelineStarted = false; // the next update starts the clock
    m_level = m_cursor.level();
    m_successiveReads = 0;
    m_nonLoaderFrames = 0;
    m_acceleration = Acceleration::None;
    m_lengthKnown1 = m_lengthKnown2 = false;
}

void TapeDeck::stop() noexcept
{
    if (!m_playing)
        return;
    m_playing = false;
    m_autoPlaying = false;
    m_stopPending = false;
    // Resume in the middle of the pulse; it's in CPU T-states at `m_edgeSpeed`
    m_pausedRemaining = (m_timelineStarted && m_nextEdgeAt > m_lastNow) ? m_nextEdgeAt - m_lastNow : 0;
    m_timelineStarted = false;
    m_successiveReads = 0;
    m_acceleration = Acceleration::None;
    // The last level stays on the EAR input for a frame: some loaders (Alkatraz) check it
    // right after loading, as copy protection
    m_micOffPending = true;
    m_micOffAt = m_lastNow + uint64_t{70'000} * m_edgeSpeed;
}

void TapeDeck::machineChanged() noexcept
{
    stop();
    m_micOffPending = false;
    m_level = false;
    m_pausedRemaining = 0;
    m_scaleRemainder = 0;
    m_lastNow = 0;
    m_edgeSpeed = 1;
    m_lastReadValid = false;
    m_successiveReads = 0;
    m_nonLoaderFrames = 0;
}

void TapeDeck::onFrameStart(bool is48kMode, bool rzxActive) noexcept
{
    m_is48kMode = is48kMode;
    m_rzxActive = rzxActive;
    if (m_nonLoaderFrames < kStopGraceFrames)
        ++m_nonLoaderFrames;
}

uint64_t TapeDeck::pulseToCpu(uint32_t tapeTstates, MachineContext &ctx) noexcept
{
    // TZX lengths are 3.5 MHz T-states: scale them to the machine's clock and CPU speed
    const uint64_t hz = uint64_t{ctx.clock.timings().baseClockHz} * ctx.clock.speedMultiplier();
    const uint64_t scaled = uint64_t{tapeTstates} * hz + m_scaleRemainder;
    m_scaleRemainder = scaled % kTapeClockHz;
    return scaled / kTapeClockHz;
}

void TapeDeck::setLevel(bool level, uint64_t at, MachineContext &ctx) noexcept
{
    m_level = level;
    const bool sound = level && m_options.loadingSound;
    if (sound == m_soundLevel)
        return;
    m_soundLevel = sound;
    // `at` may be a little in the past: place the edge where it belongs in the frame
    const uint64_t now = ctx.clock.totalTstates();
    const uint64_t behind = (now > at ? now - at : 0) / ctx.clock.speedMultiplier();
    const uint32_t frameTs = ctx.clock.frameBaseTstates();
    const uint32_t ts = behind >= frameTs ? 0 : frameTs - static_cast<uint32_t>(behind);
    ctx.audioMixer.writeTapeEar(ts, sound);
}

void TapeDeck::startTimeline(MachineContext &ctx)
{
    const uint64_t now = ctx.clock.totalTstates();
    const uint32_t speed = ctx.clock.speedMultiplier();
    // What was left of the pulse when the tape stopped, at the speed of then
    const uint64_t remaining = m_pausedRemaining * speed / std::max<uint32_t>(m_edgeSpeed, 1);
    m_pausedRemaining = 0;
    m_nextEdgeAt = now + remaining;
    m_edgeSpeed = speed;
    m_lastNow = now;
    m_timelineStarted = true;
}

void TapeDeck::advance(MachineContext &ctx)
{
    const uint64_t now = ctx.clock.totalTstates();
    const uint64_t maxGap = uint64_t{ctx.clock.scaledTstatesPerFrame()} * 4;
    // The clock jumped (a reset or a snapshot): keep the pulses' lengths, not their times
    if (now < m_lastNow || now - m_lastNow > maxGap) {
        if (m_timelineStarted) {
            const uint64_t left = m_nextEdgeAt > m_lastNow ? m_nextEdgeAt - m_lastNow : 0;
            m_nextEdgeAt = now + left;
        }
        m_micOffAt = now;
        m_lastNow = now;
    }

    if (m_micOffPending && !m_playing && now >= m_micOffAt) {
        m_micOffPending = false;
        setLevel(false, m_micOffAt, ctx);
    }

    if (m_playing) {
        if (!m_timelineStarted) {
            startTimeline(ctx);
        } else if (const uint32_t speed = ctx.clock.speedMultiplier(); speed != m_edgeSpeed) {
            // The CPU speed changed: the pulse goes on at the new speed
            const uint64_t left = m_nextEdgeAt > now ? m_nextEdgeAt - now : 0;
            m_nextEdgeAt = now + left * speed / m_edgeSpeed;
            m_edgeSpeed = speed;
        }
        while (m_playing && m_nextEdgeAt <= now)
            nextEdge(ctx, m_nextEdgeAt, false);
    }

    // The loading sound was switched on or off
    if (m_soundLevel != (m_level && m_options.loadingSound))
        setLevel(m_level, now, ctx);
    m_lastNow = now;
}

void TapeDeck::nextEdge(MachineContext &ctx, uint64_t at, bool fromAcceleration)
{
    if (m_stopPending) {
        stop();
        m_pausedRemaining = 0;
        return;
    }

    const TapeEdge edge = m_cursor.next(m_blocks);
    setLevel(edge.level, at, ctx);

    // A stop takes effect after its edge played
    if ((edge.flags & TapeEdge::Flags::Stop) || ((edge.flags & TapeEdge::Flags::Stop48k) && m_is48kMode))
        m_stopPending = true;

    m_nextEdgeAt = at + pulseToCpu(edge.length, ctx);
    m_edgeSpeed = ctx.clock.speedMultiplier();

    // A tape the loader detection started stops before a standard block: the ROM trap loads it
    if (!m_stopPending && (edge.flags & TapeEdge::Flags::BlockEnd) && m_autoPlaying && m_options.romTraps
        && !m_rzxActive && nextDataBlockIsRom()) {
        m_lastNow = std::max(m_lastNow, at);
        stop();
        m_pausedRemaining = 0;
        return;
    }

    // What the pulse that starts now is, for the acceleration
    if (edge.flags & TapeEdge::Flags::ShortPulse) {
        m_lengthKnown2 = true;
        m_lengthLong2 = false;
    } else if (edge.flags & TapeEdge::Flags::LongPulse) {
        m_lengthKnown2 = true;
        m_lengthLong2 = true;
    } else {
        m_lengthKnown2 = false;
    }
    // An edge that came on time (not accelerated) may have been seen by the loader's loop
    // already: accelerating the next one would skip an edge
    if (!fromAcceleration)
        m_lengthKnown1 = false;
}

bool TapeDeck::nextDataBlockIsRom() const noexcept
{
    if (!m_cursor.atBlockStart())
        return false;
    std::size_t index = m_cursor.blockIndex();
    while (index < m_blocks.size() && m_blocks[index].isMetadata())
        ++index;
    return index < m_blocks.size() && m_blocks[index].isRomBlock();
}

void TapeDeck::skipMetadata() noexcept
{
    std::size_t index = m_cursor.blockIndex();
    if (!m_cursor.atBlockStart())
        return;
    while (index < m_blocks.size() && m_blocks[index].isMetadata())
        ++index;
    if (index != m_cursor.blockIndex())
        m_cursor.skipTo(index);
}

// ── The machine reads the EAR input ───────────────────────────────────────

bool TapeDeck::readEar(MachineContext &ctx)
{
    update(ctx);
    if (!m_blocks.empty() && (m_options.autoPlay || m_options.accelerateLoaders))
        detectLoader(ctx);
    return m_level;
}

void TapeDeck::detectLoader(MachineContext &ctx)
{
    const uint64_t now = ctx.clock.totalTstates();
    const Z80Registers &regs = ctx.cpu.registers();
    const uint8_t b = regs.B();

    uint32_t tstatesDiff = std::numeric_limits<uint32_t>::max();
    if (m_lastReadValid && now >= m_lastReadAt)
        tstatesDiff = static_cast<uint32_t>(std::min<uint64_t>((now - m_lastReadAt) / ctx.clock.speedMultiplier(),
                                                                std::numeric_limits<uint32_t>::max()));
    const auto bDiff = static_cast<uint8_t>(b - m_lastB);
    m_lastReadAt = now;
    m_lastB = b;
    m_lastReadValid = true;

    if (!m_options.autoPlay)
        m_successiveReads = 0;
    else if (m_playing)
        detectWhilePlaying(ctx, tstatesDiff, bDiff);
    else
        detectWhileStopped(ctx, tstatesDiff, bDiff);

    if (m_options.accelerateLoaders && m_playing && !m_rzxActive)
        checkForAcceleration(ctx);
}

void TapeDeck::detectWhilePlaying(MachineContext &ctx, uint32_t tstatesDiff, uint8_t bDiff)
{
    const uint16_t pc = ctx.cpu.registers().PC;
    const bool keyboard = isKeyboardScan(ctx.mmu, pc);
    const bool counterLike = tstatesDiff <= 1000 && (bDiff == 1 || bDiff == 0 || bDiff == 0xFF);
    if (isLoaderRead(ctx.mmu, pc) || (!keyboard && counterLike)) {
        m_successiveReads = 0;
        m_nonLoaderFrames = 0;
        return;
    }
    // Something else reads the port: the loading is over. Keyboard scans get a grace
    // period, some games run BASIC between two loads.
    if (m_successiveReads < kStopNonEarReads)
        ++m_successiveReads;
    if (m_successiveReads >= kStopNonEarReads && (!keyboard || m_nonLoaderFrames >= kStopGraceFrames))
        stop();
}

void TapeDeck::detectWhileStopped(MachineContext &ctx, uint32_t tstatesDiff, uint8_t bDiff)
{
    const uint16_t pc = ctx.cpu.registers().PC;
    if (isMovieloadLoader(ctx.mmu, pc) && tstatesDiff <= 500)
        startAfterReads(kMovieloadDetectionReads); // Movieload samples the silence for a while first
    else if (isLoaderLoop(ctx.mmu, pc))
        startAfterReads(kDetectionReads);
    else if (readUsesEar(ctx.mmu, pc) && tstatesDiff <= 500 && (bDiff == 1 || bDiff == 0xFF))
        startAfterReads(kDetectionReads);
    else
        m_successiveReads = 0;
}

void TapeDeck::startAfterReads(int reads) noexcept
{
    if (++m_successiveReads >= reads)
        play(true);
}

void TapeDeck::checkForAcceleration(MachineContext &ctx)
{
    const uint16_t pc = ctx.cpu.registers().PC;
    if (m_acceleration != Acceleration::None && pc != m_accelerationPc)
        m_acceleration = Acceleration::None;
    if (m_acceleration == Acceleration::None) {
        m_acceleration = static_cast<Acceleration>(accelerationAtIn(ctx.mmu, pc));
        m_accelerationPc = pc;
    }
    if (m_acceleration == Acceleration::None)
        return;

    if (m_lengthKnown1) {
        // Skip the rest of the loop: the edge comes now
        accelerate(ctx, m_lengthLong1);
        const uint64_t now = ctx.clock.totalTstates();
        nextEdge(ctx, now, true);
        m_successiveReads = 0;
    }
    m_lengthKnown1 = m_lengthKnown2;
    m_lengthLong1 = m_lengthLong2;
}

void TapeDeck::accelerate(MachineContext &ctx, bool longPulse)
{
    Z80Registers &regs = ctx.cpu.registers();
    switch (m_acceleration) {
    case Acceleration::SoftwareProjects:
        // The loader turns its iterations into a length subtracting B from A'
        regs.setB(static_cast<uint8_t>((regs.AF_ >> 8)
                                       - (longPulse ? kSoftwareProjectsLongIterations : kSoftwareProjectsShortIterations)));
        regs.PC = static_cast<uint16_t>(m_accelerationPc + 9);
        break;
    case Acceleration::GremlinRising:
    case Acceleration::GremlinFalling: {
        // INC L already ran once
        const int iterations = longPulse ? kGremlinLongIterations : kGremlinShortIterations;
        regs.setL(static_cast<uint8_t>(regs.L() + iterations - 1));
        if (m_acceleration == Acceleration::GremlinFalling)
            regs.setA(regs.L()); // the routine returns the rising + falling count in A
        regs.PC = static_cast<uint16_t>(m_accelerationPc + 4);
        break;
    }
    case Acceleration::Increasing:
    case Acceleration::Decreasing: {
        // B tells the pulse's length, bit 5 of C the level; then the edge loop's RET
        const bool high = longPulse != (m_acceleration == Acceleration::Decreasing);
        regs.setB(high ? 0xFE : 0x00);
        regs.setC(static_cast<uint8_t>((regs.C() & ~0x20) | (m_level ? 0x00 : 0x20)));
        regs.setF(regs.F() | Z80Flags::C);
        returnFromRoutine(regs, ctx.mmu);
        break;
    }
    case Acceleration::None:
        break;
    }
}

// ── ROM traps ─────────────────────────────────────────────────────────────

bool TapeDeck::handleRomTrap(MachineContext &ctx, bool isLoad)
{
    if (!m_options.romTraps || m_playing || m_rzxActive)
        return false;

    Z80Registers &regs = ctx.cpu.registers();
    const Mmu &mmu = ctx.mmu;

    if (!isLoad) {
        // SA-BYTES: A = flag, IX = start, DE = length. The block goes at the end of the tape.
        if (!codeMatches(mmu, static_cast<uint16_t>(regs.PC - kSaBytesSignature.size()), kSaBytesSignature))
            return false;
        std::vector<uint8_t> data;
        data.reserve(std::size_t{regs.DE} + 2);
        uint8_t parity = regs.A();
        data.push_back(parity);
        for (uint16_t i = 0; i < regs.DE; ++i) {
            const uint8_t byte = mmu.peek(static_cast<uint16_t>(regs.IX + i));
            data.push_back(byte);
            parity ^= byte;
        }
        data.push_back(parity);
        const bool atEnd = m_cursor.atEnd(m_blocks);
        m_blocks.push_back(TapeBlock::standard(std::move(data)));
        if (atEnd)
            m_cursor.skipTo(m_blocks.size()); // stays at the end, after the new block
        returnFromRoutine(regs, mmu);
        return true;
    }

    // LD-BYTES: A' = flag, F' carry = load (or verify), IX = start, DE = length
    if (!codeMatches(mmu, static_cast<uint16_t>(regs.PC - kLdBytesSignature.size()), kLdBytesSignature))
        return false;

    // The pause after the last block the trap loaded
    if (m_cursor.inTail()) {
        const TapeEdge edge = m_cursor.next(m_blocks);
        m_level = edge.level;
    }
    skipMetadata();
    if (m_cursor.atEnd(m_blocks))
        return false; // no tape: the ROM waits, like with an empty deck

    const std::size_t index = m_cursor.blockIndex();
    const TapeBlock &block = m_blocks[index];
    const std::size_t length = block.data.size();

    // Custom blocks, and blocks the ROM would read partially, play in real time
    std::size_t nextIndex = index + 1;
    while (nextIndex < m_blocks.size() && m_blocks[nextIndex].isMetadata())
        ++nextIndex;
    const bool nextIsRom = nextIndex < m_blocks.size() && m_blocks[nextIndex].isRomBlock();
    const std::size_t wanted = std::size_t{regs.DE} + 2;
    if (!block.isRomBlock() || !m_cursor.atBlockStart() || length > wanted || (length < wanted && !nextIsRom)) {
        play(true);
        return false;
    }

    // What LD-BYTES leaves in the registers (after FUSE's tape_trap_load_block)
    const std::span<const uint8_t> payload = std::span(block.data).subspan(1);
    const bool verify = (regs.AF_ & Z80Flags::C) == 0;
    const uint8_t requestedFlag = static_cast<uint8_t>(regs.AF_ >> 8);
    std::size_t toRead = std::min<std::size_t>(payload.size(), regs.DE);
    std::size_t processed = 0;
    uint8_t parity = block.data[0];

    const auto finish = [&] {
        regs.setC(0x01);
        regs.setH(parity);
        regs.DE = static_cast<uint16_t>(regs.DE - processed);
        regs.IX = static_cast<uint16_t>(regs.IX + processed);
    };
    const auto fail = [&] {
        regs.setF(regs.F() & ~Z80Flags::C);
        finish();
    };

    regs.setA(0);
    regs.setL(parity);
    if (regs.DE == 0) {
        // The ROM reads the flag byte, and nothing else
        regs.setB(0xB0);
        regs.setA(parity);
        regs.setF(cpOneFlags(parity));
        finish();
    } else {
        regs.AF_ = 0x0145;
        regs.setA(static_cast<uint8_t>(requestedFlag ^ parity));
        regs.setF(sz53p(regs.A()));
        if (regs.A() != 0) {
            fail(); // the wrong block
        } else {
            if (toRead > 0)
                regs.setL(payload[toRead - 1]);
            bool ok = true;
            for (processed = 0; processed < toRead; ++processed) {
                const uint8_t byte = payload[processed];
                parity ^= byte;
                const auto address = static_cast<uint16_t>(regs.IX + processed);
                if (verify) {
                    if (mmu.peek(address) != byte) {
                        regs.setL(byte);
                        ok = false;
                        break;
                    }
                } else {
                    ctx.mmu.poke(address, byte);
                }
            }
            if (!ok) {
                fail();
            } else if (regs.DE == processed && toRead < payload.size()) {
                // All the bytes read, the parity byte follows
                parity ^= payload[toRead];
                regs.setA(parity);
                regs.setF(cpOneFlags(parity));
                regs.setB(0xB0);
                finish();
            } else {
                // The block ended early: the ROM fails reading the next byte's first bit
                regs.setB(0);
                regs.setL(0x01);
                regs.setF(static_cast<uint8_t>((regs.F() & Z80Flags::C) | Z80Flags::Z | Z80Flags::H));
                fail();
            }
        }
    }

    // The tape goes on after the block: its pause if a custom loader follows, else the next block
    if (nextIsRom || index + 1 >= m_blocks.size())
        m_cursor.skipTo(index + 1);
    else
        m_cursor.skipToTail();
    m_level = false;
    returnFromRoutine(regs, mmu);
    return true;
}

} // namespace zxkit
