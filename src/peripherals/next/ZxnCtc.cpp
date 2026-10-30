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

#include "zxkit/peripherals/next/ZxnCtc.h"

namespace zxkit {

void ZxnCtc::reset() noexcept
{
    for (auto &ch : m_channels)
        ch = Channel{};
    m_intVectorBase = 0;
}

uint8_t ZxnCtc::readChannel(uint8_t idx) const noexcept
{
    if (idx >= 4)
        return 0xFF;
    return m_channels[idx].downCounter;
}

void ZxnCtc::writeChannel(uint8_t idx, uint8_t val) noexcept
{
    if (idx >= 4)
        return;
    auto &ch = m_channels[idx];
    if (ch.waitingTimeConstant) {
        ch.timeConstant = val;
        ch.downCounter = val;
        ch.waitingTimeConstant = false;
        ch.control &= ~0x02;
        return;
    }

    if ((val & 0x01) == 0) {
        if (idx == 0)
            m_intVectorBase = val & 0xF8;
        return;
    }

    ch.control = val;
    if (val & 0x02) {
        ch.downCounter = ch.timeConstant;
        ch.prescalerCount = 0;
    }
    if (val & 0x04) {
        ch.waitingTimeConstant = true;
    }
}

void ZxnCtc::tickTstates(uint32_t tstates, Im2Controller &im2) noexcept
{
    for (uint8_t i = 0; i < 4; ++i) {
        auto &ch = m_channels[i];
        if ((ch.control & 0x02) || ch.waitingTimeConstant)
            continue;
        // Timer mode (bit 6 == 0)
        if ((ch.control & 0x40) == 0) {
            const uint16_t prescaler = (ch.control & 0x20) ? 256 : 16;
            ch.prescalerCount += static_cast<uint16_t>(tstates);
            while (ch.prescalerCount >= prescaler) {
                ch.prescalerCount -= prescaler;
                if (--ch.downCounter == 0) {
                    ch.downCounter = ch.timeConstant;
                    if (ch.control & 0x80) {
                        // Hardware IM2 vector indices 3..6 are CTC 0..3 on Next
                        im2.raiseVector(static_cast<uint8_t>(3 + i));
                    }
                }
            }
        }
    }
}

void ZxnCtc::saveState(StateWriter &w) const
{
    w.write(m_intVectorBase);
    for (const auto &ch : m_channels) {
        w.write(ch.control);
        w.write(ch.timeConstant);
        w.write(ch.downCounter);
        w.write(ch.prescalerCount);
        w.writeBool(ch.waitingTimeConstant);
    }
}

void ZxnCtc::loadState(StateReader &r)
{
    m_intVectorBase = r.read<uint8_t>();
    for (auto &ch : m_channels) {
        ch.control             = r.read<uint8_t>();
        ch.timeConstant        = r.read<uint8_t>();
        ch.downCounter         = r.read<uint8_t>();
        ch.prescalerCount      = r.read<uint16_t>();
        ch.waitingTimeConstant = r.readBool();
    }
}

} // namespace zxkit
