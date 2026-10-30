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

#include "zxkit/core/StateStream.h"

#include <cstdint>

namespace zxkit {

enum class CpuSpeed : uint8_t {
    Mhz3_5 = 0, // 1x (divisor 8 from 28 MHz)
    Mhz7   = 1, // 2x (divisor 4 from 28 MHz)
    Mhz14  = 2, // 4x (divisor 2 from 28 MHz)
    Mhz28  = 3  // 8x (divisor 1 from 28 MHz)
};

struct FrameTimings {
    uint32_t baseClockHz      = 3'500'000;
    uint16_t leftBorderTstates = 24;
    uint16_t screenTstates     = 128;
    uint16_t rightBorderTstates = 24;
    uint16_t retraceTstates    = 48;
    uint16_t tstatesPerLine    = 224;
    uint16_t topBorderLines    = 64;
    uint16_t screenLines       = 192;
    uint16_t bottomBorderLines = 56;
    uint16_t linesPerFrame     = 312;
    uint16_t interruptLength   = 32;
    uint16_t ulaOriginHc       = 116;
    uint16_t ulaOriginVc       = 64;

    constexpr uint32_t tstatesPerFrame() const noexcept
    {
        return static_cast<uint32_t>(tstatesPerLine) * linesPerFrame;
    }

    constexpr double frameRateHz() const noexcept
    {
        return static_cast<double>(baseClockHz) / static_cast<double>(tstatesPerFrame());
    }
};

class Clock {
public:
    Clock() = default;
    explicit Clock(const FrameTimings &timings) noexcept
        : m_timings(timings)
    {
    }

    void reset() noexcept;

    void setTimings(const FrameTimings &timings) noexcept { m_timings = timings; }
    const FrameTimings &timings() const noexcept { return m_timings; }

    void setSpeed(CpuSpeed speed) noexcept
    {
        m_speed = speed;
        m_baseRemainder = 0;
    }
    CpuSpeed speed() const noexcept { return m_speed; }

    /// Multiplier relative to 3.5 MHz base T-states (1, 2, 4, or 8).
    constexpr uint32_t speedMultiplier() const noexcept
    {
        return 1u << static_cast<uint8_t>(m_speed);
    }

    /// Advance by base T-states (or scaled CPU cycles).
    void advance(uint32_t tstates) noexcept
    {
        m_totalTstates += tstates;
        m_frameTstates += tstates;
        // The frame timeline (video, audio) runs at the base 3.5MHz clock
        m_baseRemainder += tstates;
        const uint8_t shift = static_cast<uint8_t>(m_speed);
        m_frameBaseTstates += m_baseRemainder >> shift;
        m_baseRemainder &= (1u << shift) - 1;
    }

    void beginNewFrame() noexcept;

    void setFrameTstates(uint32_t ts) noexcept
    {
        m_frameTstates = ts;
        m_frameBaseTstates = ts >> static_cast<uint8_t>(m_speed);
        m_baseRemainder = 0;
    }

    uint64_t totalTstates() const noexcept { return m_totalTstates; }
    /// CPU T-states since the frame started (at the current CPU speed).
    uint32_t frameTstates() const noexcept { return m_frameTstates; }
    /// Position in the frame in base (3.5MHz) T-states, whatever the CPU speed:
    /// the timeline of the video and audio.
    uint32_t frameBaseTstates() const noexcept { return m_frameBaseTstates; }
    uint64_t frameCount() const noexcept { return m_frameCount; }

    uint32_t scaledTstatesPerLine() const noexcept
    {
        return m_timings.tstatesPerLine * speedMultiplier();
    }

    uint32_t scaledTstatesPerFrame() const noexcept
    {
        return m_timings.tstatesPerFrame() * speedMultiplier();
    }

    /// Current scanline (0 .. linesPerFrame - 1) and horizontal T-state within line (3.5MHz domain).
    struct RasterPos {
        uint16_t line = 0;
        uint16_t lineTstate = 0;
    };

    RasterPos rasterPos() const noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    FrameTimings m_timings{};
    uint64_t m_totalTstates = 0;
    uint32_t m_frameTstates = 0;
    uint32_t m_frameBaseTstates = 0;
    uint32_t m_baseRemainder = 0;
    uint64_t m_frameCount   = 0;
    CpuSpeed m_speed        = CpuSpeed::Mhz3_5;
};

} // namespace zxkit
