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

#include "zxkit/audio/Beeper.h"

namespace zxkit {

void Beeper::reset() noexcept
{
    m_earBit = false;
    m_micBit = false;
    m_tapeEarBit = false;
    m_lastAmplitude = 0;
}

void Beeper::setPortFeBits(uint32_t frameTstate, bool ear, bool mic, BlipBuffer &left, BlipBuffer &right) noexcept
{
    m_earBit = ear;
    m_micBit = mic;
    updateAmplitude(frameTstate, left, right);
}

void Beeper::setTapeEar(uint32_t frameTstate, bool tapeEar, BlipBuffer &left, BlipBuffer &right) noexcept
{
    m_tapeEarBit = tapeEar;
    updateAmplitude(frameTstate, left, right);
}

void Beeper::saveState(StateWriter &w) const
{
    w.writeBool(m_earBit);
    w.writeBool(m_micBit);
    w.writeBool(m_tapeEarBit);
    w.write(m_lastAmplitude);
}

void Beeper::loadState(StateReader &r)
{
    m_earBit        = r.readBool();
    m_micBit        = r.readBool();
    m_tapeEarBit    = r.readBool();
    m_lastAmplitude = r.read<int32_t>();
}

void Beeper::updateAmplitude(uint32_t frameTstate, BlipBuffer &left, BlipBuffer &right) noexcept
{
    int32_t amp = 0;
    if (m_earBit || m_tapeEarBit)
        amp += kEarVolume;
    if (m_micBit)
        amp += kMicVolume;

    const int32_t delta = amp - m_lastAmplitude;
    if (delta != 0) {
        left.addDelta(frameTstate, delta);
        right.addDelta(frameTstate, delta);
        m_lastAmplitude = amp;
    }
}

} // namespace zxkit
