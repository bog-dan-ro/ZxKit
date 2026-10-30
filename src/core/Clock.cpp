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

#include "zxkit/core/Clock.h"

namespace zxkit {

void Clock::reset() noexcept
{
    m_totalTstates = 0;
    m_frameTstates = 0;
    m_frameBaseTstates = 0;
    m_baseRemainder = 0;
    m_frameCount = 0;
    m_speed = CpuSpeed::Mhz3_5;
}

void Clock::beginNewFrame() noexcept
{
    // Carry the overshoot of the last instruction into the new frame
    const uint32_t fts = m_timings.tstatesPerFrame();
    m_frameBaseTstates = (m_frameBaseTstates >= fts) ? m_frameBaseTstates - fts : 0;
    m_frameTstates = m_frameBaseTstates << static_cast<uint8_t>(m_speed);
    ++m_frameCount;
}

Clock::RasterPos Clock::rasterPos() const noexcept
{
    const uint32_t baseTs = m_frameBaseTstates;
    const uint16_t line = static_cast<uint16_t>((baseTs / m_timings.tstatesPerLine) % m_timings.linesPerFrame);
    const uint16_t hts  = static_cast<uint16_t>(baseTs % m_timings.tstatesPerLine);
    return {line, hts};
}

void Clock::saveState(StateWriter &w) const
{
    w.write(m_totalTstates);
    w.write(m_frameTstates);
    w.write(m_frameBaseTstates);
    w.write(m_frameCount);
    w.write(static_cast<uint8_t>(m_speed));
}

void Clock::loadState(StateReader &r)
{
    m_totalTstates = r.read<uint64_t>();
    m_frameTstates = r.read<uint32_t>();
    m_frameBaseTstates = r.read<uint32_t>();
    m_baseRemainder = 0;
    m_frameCount   = r.read<uint64_t>();
    m_speed        = static_cast<CpuSpeed>(r.read<uint8_t>());
}

} // namespace zxkit
