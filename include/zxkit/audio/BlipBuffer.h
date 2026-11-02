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

#include <cstdint>
#include <span>
#include <vector>

namespace zxkit {

/// Fast band-integrated delta buffer for converting T-state-accurate audio
/// amplitude transitions into anti-aliased 16-bit PCM samples.
class BlipBuffer {
public:
    void configure(uint32_t clockHz, uint32_t sampleRate, uint32_t tstatesPerFrame);
    void reset() noexcept;
    void clearFrame() noexcept;
    /// Samples before this point on the current frame's continuous audio timeline.
    std::size_t samplesUntil(uint32_t frameTstate) const noexcept;
    void addDelta(uint32_t frameTstate, int32_t delta) noexcept;
    std::size_t endFrame(uint32_t frameTstates, std::span<int32_t> accumOut) noexcept;

private:
    uint32_t m_clockHz = 3'500'000;
    uint32_t m_sampleRate = 48'000;
    uint32_t m_tstatesPerFrame = 69'888;
    int32_t m_integrator = 0;
    uint32_t m_sampleRemainder = 0;
    std::vector<int32_t> m_deltas;
};

} // namespace zxkit
