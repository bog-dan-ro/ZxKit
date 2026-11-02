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

#include "zxkit/audio/BlipBuffer.h"

#include <algorithm>

namespace zxkit {

void BlipBuffer::configure(uint32_t clockHz, uint32_t sampleRate, uint32_t tstatesPerFrame)
{
    m_clockHz = (clockHz > 0) ? clockHz : 3'500'000;
    m_sampleRate = (sampleRate > 0) ? sampleRate : 48'000;
    m_tstatesPerFrame = (tstatesPerFrame > 0) ? tstatesPerFrame : 69'888;
    const std::size_t maxSamples = (static_cast<std::size_t>(m_sampleRate) * m_tstatesPerFrame / m_clockHz) + 64;
    m_deltas.assign(maxSamples, 0);
    reset();
}

void BlipBuffer::reset() noexcept
{
    m_integrator = 0;
    m_sampleRemainder = 0;
    clearFrame();
}

std::size_t BlipBuffer::samplesUntil(uint32_t frameTstate) const noexcept
{
    return (m_sampleRemainder + static_cast<uint64_t>(frameTstate) * m_sampleRate) / m_clockHz;
}

void BlipBuffer::clearFrame() noexcept
{
    std::ranges::fill(m_deltas, 0);
}

void BlipBuffer::addDelta(uint32_t frameTstate, int32_t delta) noexcept
{
    if (delta == 0 || m_deltas.empty())
        return;
    const std::size_t idx = samplesUntil(frameTstate);
    if (idx < m_deltas.size())
        m_deltas[idx] += delta;
    else
        m_deltas.back() += delta;
}

std::size_t BlipBuffer::endFrame(uint32_t frameTstates, std::span<int32_t> accumOut) noexcept
{
    const std::size_t sampleCount = std::min<std::size_t>(
        samplesUntil(frameTstates),
        std::min(m_deltas.size(), accumOut.size()));

    int32_t level = m_integrator;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        level += m_deltas[i];
        // High-pass DC blocker (~20 Hz cutoff)
        level -= (level >> 10);
        accumOut[i] += level;
    }
    m_integrator = level;
    m_sampleRemainder = (m_sampleRemainder + static_cast<uint64_t>(frameTstates) * m_sampleRate) % m_clockHz;
    // A write in an instruction that straddles the frame boundary belongs to
    // the next frame. Do not discard its delta or the fractional sample clock.
    if (sampleCount) {
        std::move(m_deltas.begin() + sampleCount, m_deltas.end(), m_deltas.begin());
        std::fill(m_deltas.end() - sampleCount, m_deltas.end(), 0);
    }
    return sampleCount;
}

} // namespace zxkit
