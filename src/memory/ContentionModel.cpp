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

#include "zxkit/memory/ContentionModel.h"

#include <algorithm>

namespace zxkit {

void ContentionModel::configure(ContentionPattern pattern, const FrameTimings &timings, bool ulaContendsPortLowBit) noexcept
{
    m_pattern = pattern;
    m_timings = timings;
    m_ulaContendsPortLowBit = ulaContendsPortLowBit;
    m_disabled = (pattern == ContentionPattern::None);
    rebuildTable();
}

bool ContentionModel::isPortFromUla(uint16_t port) const noexcept
{
    if (!m_ulaContendsPortLowBit || m_pattern == ContentionPattern::SinclairPlus2A_3 || m_pattern == ContentionPattern::None)
        return false;
    return (port & 0x0001) == 0;
}

void ContentionModel::rebuildTable() noexcept
{
    m_delayTable.fill(0);
    m_tableSize = std::min<std::size_t>(m_timings.tstatesPerFrame(), kMaxFrameTstates);
    if (m_pattern == ContentionPattern::None)
        return;

    static constexpr std::array<uint8_t, 8> kPat48  = {6, 5, 4, 3, 2, 1, 0, 0};
    static constexpr std::array<uint8_t, 8> kPatP2A = {1, 0, 7, 6, 5, 4, 3, 2};
    const auto &pat = (m_pattern == ContentionPattern::SinclairPlus2A_3) ? kPatP2A : kPat48;

    const uint32_t firstLine = m_timings.topBorderLines;
    const uint32_t lastLine  = firstLine + m_timings.screenLines;
    const uint32_t firstTsInLine = m_timings.leftBorderTstates - 1; // Contention starts 1 T-state before first pixel

    for (uint32_t line = firstLine; line < lastLine; ++line) {
        const uint32_t lineBase = line * m_timings.tstatesPerLine + firstTsInLine;
        for (uint32_t x = 0; x < m_timings.screenTstates; ++x) {
            const uint32_t ts = lineBase + x;
            if (ts < m_tableSize)
                m_delayTable[ts] = pat[x & 7];
        }
    }
}

} // namespace zxkit
