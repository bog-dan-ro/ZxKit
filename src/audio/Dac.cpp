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

#include "zxkit/audio/Dac.h"

namespace zxkit {

void Dac::reset() noexcept
{
    m_channels.fill(0x80);
    m_lastLeft = 0;
    m_lastRight = 0;
}

void Dac::writeChannel(uint32_t frameTstate, uint8_t ch, uint8_t val, BlipBuffer &left, BlipBuffer &right) noexcept
{
    if (ch >= 4)
        return;
    m_channels[ch] = val;
    flush(frameTstate, left, right);
}

void Dac::writeMono(uint32_t frameTstate, uint8_t val, BlipBuffer &left, BlipBuffer &right) noexcept
{
    m_channels[0] = m_channels[1] = m_channels[2] = m_channels[3] = val;
    flush(frameTstate, left, right);
}

uint8_t Dac::channel(uint8_t ch) const noexcept
{
    return (ch < 4) ? m_channels[ch] : 0x80;
}

void Dac::saveState(StateWriter &w) const
{
    w.writeBytes(m_channels);
    w.write(m_lastLeft);
    w.write(m_lastRight);
}

void Dac::loadState(StateReader &r)
{
    r.readBytes(m_channels);
    m_lastLeft  = r.read<int32_t>();
    m_lastRight = r.read<int32_t>();
}

void Dac::flush(uint32_t frameTstate, BlipBuffer &left, BlipBuffer &right) noexcept
{
    const int32_t l = ((static_cast<int32_t>(m_channels[0]) - 128) + (static_cast<int32_t>(m_channels[1]) - 128)) * kDacScale;
    const int32_t r = ((static_cast<int32_t>(m_channels[2]) - 128) + (static_cast<int32_t>(m_channels[3]) - 128)) * kDacScale;
    if (l != m_lastLeft) {
        left.addDelta(frameTstate, l - m_lastLeft);
        m_lastLeft = l;
    }
    if (r != m_lastRight) {
        right.addDelta(frameTstate, r - m_lastRight);
        m_lastRight = r;
    }
}

} // namespace zxkit
