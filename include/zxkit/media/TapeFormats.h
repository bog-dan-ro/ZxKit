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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zxkit {

/// The tape block kinds. Values below 0xF0 are the TZX block ids.
enum class TapeBlockType : uint8_t {
    StandardData    = 0x10, ///< ROM timings (TAP blocks, TZX 0x10)
    TurboData       = 0x11,
    PureTone        = 0x12,
    PulseSeq        = 0x13,
    PureData        = 0x14,
    DirectRecording = 0x15,
    CswRecording    = 0x18,
    GeneralizedData = 0x19,
    Pause           = 0x20, ///< a 0 ms pause stops the tape
    GroupStart      = 0x21,
    GroupEnd        = 0x22,
    JumpTo          = 0x23,
    LoopStart       = 0x24,
    LoopEnd         = 0x25,
    CallSequence    = 0x26,
    ReturnFromSequence = 0x27,
    SelectBlock     = 0x28,
    StopIf48k       = 0x2A,
    SetSignalLevel  = 0x2B,
    TextDesc        = 0x30,
    Message         = 0x31,
    ArchiveInfo     = 0x32,
    HardwareType    = 0x33,
    CustomInfo      = 0x35,
    Glue            = 0x5A,
    PzxPulses       = 0xF0, ///< `pulses`: (duration, repeat count) pairs; initially low
    PzxData         = 0xF1, ///< two dataSymbols (zero pulses are real edges), bit count, initial level in value
    PzxPause        = 0xF2  ///< one pulse in `pulses`, initial level in value; zero does not stop
};

/// A symbol of a generalized data block (TZX 0x19): how its first pulse starts, and its pulses.
struct TapeSymbol {
    enum class Start : uint8_t {
        Toggle = 0, ///< an edge (the usual)
        Keep   = 1, ///< no edge: the first pulse continues the previous level
        Low    = 2, ///< force the level low
        High   = 3  ///< force the level high
    };
    Start start = Start::Toggle;
    std::vector<uint16_t> pulses; ///< T-states; a zero length ends the symbol early
};

/// `repeat` times the symbol `symbol` (a generalized data block's pilot and sync stream).
struct TapeSymbolRun {
    uint8_t symbol = 0;
    uint16_t repeat = 0;
};

/// One block of a tape. All the pulse lengths are in T-states of the 3.5 MHz ZX Spectrum 48K
/// clock (the TZX unit); the deck scales them to the emulated machine's clock and CPU speed.
///
/// Which fields a block uses depends on its type:
/// - StandardData, TurboData, PureData: the timings (pilot, sync, zero/one bits), `data`, `usedBits`
///   (the bits of the last byte) and `pauseAfterMs`. Standard blocks have the ROM timings.
/// - PureTone: `pilotPulse` x `pilotPulses`.
/// - PulseSeq: `pulses`.
/// - DirectRecording: `data` holds the samples (one bit each, MSB first), `sampleTstates`.
/// - CswRecording: `pulses` (already converted to T-states).
/// - GeneralizedData: `pilotSymbols`, `pilotStream`, `dataSymbols`, `dataSymbolCount` and `data`
///   (the data symbols, `symbolBits()` bits each, MSB first).
/// - Pause: `pauseAfterMs` (0 stops the tape).
/// - JumpTo, LoopStart, SetSignalLevel: `value` (relative jump, repetitions, level).
/// - CallSequence: `offsets` (relative to the call block). SelectBlock: `offsets` and `labels`.
/// - The text blocks: `description`.
struct TapeBlock {
    TapeBlockType type = TapeBlockType::StandardData;
    uint16_t pauseAfterMs = 1000;
    std::string description;
    std::vector<uint8_t> data;

    uint16_t pilotPulse = 2168;
    uint16_t pilotPulses = 0;
    uint16_t sync1Pulse = 667;
    uint16_t sync2Pulse = 735;
    uint16_t zeroPulse = 855;
    uint16_t onePulse = 1710;
    uint8_t usedBits = 8;
    uint16_t sampleTstates = 0;

    std::vector<uint32_t> pulses;
    std::vector<TapeSymbol> pilotSymbols;
    std::vector<TapeSymbolRun> pilotStream;
    std::vector<TapeSymbol> dataSymbols;
    uint32_t dataSymbolCount = 0;

    int32_t value = 0;
    std::vector<int16_t> offsets;
    std::vector<std::string> labels;
    /// Original body of descriptive TZX blocks, retained for lossless metadata export.
    std::vector<uint8_t> metadata;

    /// Carries data the ROM loader can read: a standard (or turbo) block with the ROM timings.
    bool isRomBlock() const noexcept;
    /// Carries no signal and no control: texts, groups, archive info...
    bool isMetadata() const noexcept;
    /// Bits per data symbol of a generalized data block.
    uint8_t symbolBits() const noexcept;

    static TapeBlock standard(std::vector<uint8_t> data, uint16_t pauseAfterMs = 1000);
};

/// One step of the tape signal: the level changes as `transition` says, then holds for `length` T-states.
struct TapeEdge {
    enum class Transition : uint8_t { Toggle, None, Low, High };
    struct Flags {
        static constexpr uint8_t BlockEnd   = 0x01; ///< the last step of a block
        static constexpr uint8_t Stop       = 0x02; ///< stop the tape after this step
        static constexpr uint8_t Stop48k    = 0x04; ///< stop the tape after this step, in 48K mode
        static constexpr uint8_t ShortPulse = 0x08; ///< a data bit's pulse: a 0
        static constexpr uint8_t LongPulse  = 0x10; ///< a data bit's pulse: a 1
        static constexpr uint8_t TapeEnd    = 0x20; ///< the end of the tape
    };

    uint32_t length = 0;
    Transition transition = Transition::Toggle;
    uint8_t flags = 0;
    bool level = false; ///< the level after the transition
};

/// A position in a tape's signal. It walks the blocks (loops, jumps and calls included)
/// and produces the signal one edge at a time. The blocks are passed to every call,
/// so a cursor never refers to a block list that changed.
class TapeCursor {
public:
    /// Moves to the start of block `index` (the signal level goes low).
    void seek(std::size_t index) noexcept;
    std::size_t blockIndex() const noexcept { return m_block; }
    /// Moves to the start of block `index`, keeping the signal level (and the loops and calls).
    void skipTo(std::size_t index) noexcept { advanceBlock(index); }
    /// Moves to the pause at the end of the current data block.
    void skipToTail() noexcept { m_phase = Phase::Tail; }
    /// At the start of its block, before any edge of it.
    bool atBlockStart() const noexcept { return m_phase == Phase::Start; }
    /// In the pause at the end of a data block.
    bool inTail() const noexcept { return m_phase == Phase::Tail; }
    bool level() const noexcept { return m_level; }
    bool atEnd(std::span<const TapeBlock> blocks) const noexcept { return m_block >= blocks.size(); }

    /// The next step of the signal. At the end of the tape it returns a `TapeEnd | Stop` step.
    TapeEdge next(std::span<const TapeBlock> blocks);

private:
    enum class Phase : uint8_t { Start, Pilot, Sync2, Bit1, Bit2, Pulses, Samples, Symbols, Tail };
    struct Step {
        uint32_t length = 0;
        TapeEdge::Transition transition = TapeEdge::Transition::Toggle;
        uint8_t flags = 0;
        bool blockDone = false; ///< this step is the block's last one
        bool nextLow = false;   ///< the block ended with a pause: the next one starts low
        bool noSignal = false;  ///< the block produced nothing: go on with the next one
    };

    Step dataBlockStep(const TapeBlock &block);
    Step pulsesStep(const TapeBlock &block);
    Step samplesStep(const TapeBlock &block);
    Step symbolsStep(const TapeBlock &block);
    Step pzxStep(const TapeBlock &block);
    Step tailStep(const TapeBlock &block);
    bool nextBit(const TapeBlock &block);
    void controlBlock(std::span<const TapeBlock> blocks, const TapeBlock &block, uint8_t &flags);
    void advanceBlock(std::size_t next) noexcept;

    std::size_t m_block = 0;
    Phase m_phase = Phase::Start;
    bool m_level = false;
    bool m_forceLow = true;  ///< the next block's first edge goes low (the tape's start, after a pause)
    uint32_t m_count = 0;    ///< pulses left / index in the pulse list
    std::size_t m_byte = 0;
    uint8_t m_bit = 0;       ///< bits of `m_byte` already played
    uint32_t m_bitPulse = 0;
    uint32_t m_bitPulseZero = 0;
    // generalized data
    std::size_t m_run = 0;
    uint32_t m_repeat = 0;
    uint32_t m_symbol = 0;
    std::size_t m_symbolPulse = 0;
    bool m_inDataSymbols = false;
    // loops and calls
    std::size_t m_loopStart = 0;
    uint32_t m_loopCount = 0;
    std::size_t m_callBlock = 0;
    std::size_t m_callIndex = 0;
    bool m_inCall = false;
};

/// The tape file formats: they parse into blocks.
namespace TapeFormats {
std::vector<TapeBlock> parseTap(std::span<const uint8_t> raw);
std::vector<TapeBlock> parseTzx(std::span<const uint8_t> raw);
std::vector<TapeBlock> parsePzx(std::span<const uint8_t> raw);
std::vector<TapeBlock> parseCsw(std::span<const uint8_t> raw);
std::vector<uint8_t> writeTap(std::span<const TapeBlock> blocks);
std::vector<uint8_t> writeTzx(std::span<const TapeBlock> blocks);
/// "Program: NAME", "Bytes: NAME" for ROM headers, "Data (N bytes)" otherwise.
std::string describeStandardBlock(std::span<const uint8_t> data);
} // namespace TapeFormats

} // namespace zxkit
