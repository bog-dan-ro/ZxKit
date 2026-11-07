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

#include "zxkit/media/TapeFormats.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zxkit {

struct MachineContext;

struct TapeOptions
{
    /// Loads (and saves) the standard blocks instantly, trapping the ROM's LD-BYTES and
    /// SA-BYTES routines. Other blocks always play in real time.
    bool romTraps = true;
    /// Starts the tape when the software waits for it (a loader polling the EAR input),
    /// and stops it when the loading is over.
    bool autoPlay = true;
    /// Fast-forwards the edge searching loops of the ROM loader and of the known custom loaders.
    bool accelerateLoaders = true;
    /// The `Engine` runs as fast as it can while the tape plays.
    bool turboWhilePlaying = false;
    /// The tape signal plays through the speaker.
    bool loadingSound = true;
};

/// The cassette deck: the tape (TAP, TZX) and its real-time playback on the EAR input.
///
/// The signal is generated on the emulated timeline at T-state accuracy, at any CPU speed:
/// the machine asks for the EAR level when the CPU reads it (`readEar()`). The ROM traps
/// load the standard blocks instantly; everything else (turbo blocks, custom loaders)
/// plays in real time, and the loaders' edge searching loops can be accelerated.
class TapeDeck
{
public:
    // ── The tape ──────────────────────────────────────────────────────────
    void clear();
    /// Replaces the tape (the deck stops at its start).
    void load(std::vector<TapeBlock> blocks);
    void loadTap(std::span<const uint8_t> raw)
    {
        load(TapeFormats::parseTap(raw));
    }
    void loadTzx(std::span<const uint8_t> raw)
    {
        load(TapeFormats::parseTzx(raw));
    }
    void loadPzx(std::span<const uint8_t> raw)
    {
        load(TapeFormats::parsePzx(raw));
    }
    void loadCsw(std::span<const uint8_t> raw)
    {
        load(TapeFormats::parseCsw(raw));
    }
    std::vector<uint8_t> saveTap() const
    {
        return TapeFormats::writeTap(m_blocks);
    }
    std::vector<uint8_t> saveTzx() const
    {
        return TapeFormats::writeTzx(m_blocks);
    }

    const std::vector<TapeBlock> &blocks() const noexcept
    {
        return m_blocks;
    }
    std::size_t currentBlockIndex() const noexcept
    {
        return m_cursor.blockIndex();
    }
    /// Moves to the start of a block (`blocks().size()` is the end of the tape).
    void seekBlock(std::size_t index) noexcept;
    void rewind() noexcept
    {
        seekBlock(0);
    }

    // ── Playback ──────────────────────────────────────────────────────────
    bool isPlaying() const noexcept
    {
        return m_playing;
    }
    /// Play / stop, like the deck's buttons.
    void setPlaying(bool playing) noexcept;
    /// The signal on the EAR input.
    bool earLevel() const noexcept
    {
        return m_level;
    }

    const TapeOptions &options() const noexcept
    {
        return m_options;
    }
    void setOptions(const TapeOptions &options) noexcept;

    // ── Driven by the emulated machine ────────────────────────────────────
    /// The EAR input as the CPU reads it now (the ULA port); also where loaders are detected.
    bool readEar(MachineContext &ctx);
    /// Brings the signal up to the current T-state: before the CPU writes the ULA port,
    /// and at the end of every scanline.
    void update(MachineContext &ctx)
    {
        if (m_playing || m_micOffPending || m_soundLevel != (m_level && m_options.loadingSound))
            advance(ctx);
    }
    /// Every frame. `is48kMode`: a 48K machine, or a 128K one with its paging locked (the
    /// "stop the tape in 48K mode" blocks apply). `rzxActive`: an input recording plays or
    /// records, the traps and the acceleration (which change the CPU state) are off.
    void onFrameStart(bool is48kMode, bool rzxActive) noexcept;
    /// The ROM's LD-BYTES (`isLoad`) or SA-BYTES routine is about to run: returns true when
    /// the deck did its work (the CPU is then at the routine's return).
    bool handleRomTrap(MachineContext &ctx, bool isLoad);
    /// The emulated machine changed: the playback stops (the position stays).
    void machineChanged() noexcept;

private:
    enum class Acceleration : uint8_t {
        None,
        Increasing,
        Decreasing,
        SoftwareProjects,
        GremlinRising,
        GremlinFalling
    };

    void play(bool autoPlay) noexcept;
    void stop() noexcept;
    void advance(MachineContext &ctx);
    void startTimeline(MachineContext &ctx);
    void nextEdge(MachineContext &ctx, uint64_t at, bool fromAcceleration);
    uint64_t pulseToCpu(uint32_t tapeTstates, MachineContext &ctx) noexcept;
    void setLevel(bool level, uint64_t at, MachineContext &ctx) noexcept;
    bool nextDataBlockIsRom() const noexcept;
    void skipMetadata() noexcept;

    void detectLoader(MachineContext &ctx);
    void detectWhilePlaying(MachineContext &ctx, uint32_t tstatesDiff, uint8_t bDiff);
    void detectWhileStopped(MachineContext &ctx, uint32_t tstatesDiff, uint8_t bDiff);
    void startAfterReads(int reads) noexcept;
    void checkForAcceleration(MachineContext &ctx);
    void accelerate(MachineContext &ctx, bool longPulse);

    std::vector<TapeBlock> m_blocks;
    TapeCursor m_cursor;
    TapeOptions m_options;

    bool m_playing = false;
    bool m_autoPlaying = false; ///< started by the loader detection
    bool m_level = false;
    bool m_soundLevel = false; ///< what the beeper was told
    bool m_stopPending = false;
    bool m_is48kMode = true;
    bool m_rzxActive = false;
    bool m_micOffPending = false;
    bool m_timelineStarted = false;

    // The timeline, in CPU T-states (`Clock::totalTstates()`)
    uint64_t m_nextEdgeAt = 0;
    uint64_t m_lastNow = 0;
    uint64_t m_micOffAt = 0;
    uint64_t m_pausedRemaining = 0; ///< what was left of the pulse when the tape stopped
    uint64_t m_scaleRemainder = 0;
    uint32_t m_edgeSpeed = 1; ///< the CPU speed multiplier `m_nextEdgeAt` was computed with

    // Loader detection (after FUSE's loader.c)
    int m_successiveReads = 0;
    uint32_t m_nonLoaderFrames = 0;
    uint64_t m_lastReadAt = 0;
    bool m_lastReadValid = false;
    uint8_t m_lastB = 0;
    Acceleration m_acceleration = Acceleration::None;
    uint16_t m_accelerationPc = 0;
    bool m_lengthKnown1 = false;
    bool m_lengthLong1 = false;
    bool m_lengthKnown2 = false;
    bool m_lengthLong2 = false;
};

} // namespace zxkit
