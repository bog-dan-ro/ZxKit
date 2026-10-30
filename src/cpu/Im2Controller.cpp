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

#include "zxkit/cpu/Im2Controller.h"

#include <bit>

namespace zxkit {

void Im2Controller::reset() noexcept
{
    m_hwIm2Enabled = false;
    m_vectorBase = 0;
    m_pendingMask = 0;
    m_inServiceMask = 0;
    m_defaultBusByte = 0xFF;
}

void Im2Controller::raiseVector(uint8_t index) noexcept
{
    if (index < kMaxVectors)
        m_pendingMask |= static_cast<uint16_t>(1u << index);
}

void Im2Controller::clearVector(uint8_t index) noexcept
{
    if (index < kMaxVectors)
        m_pendingMask &= static_cast<uint16_t>(~(1u << index));
}

bool Im2Controller::hasPendingInterrupt() const noexcept
{
    if (m_pendingMask == 0)
        return false;
    if (!m_hwIm2Enabled || m_inServiceMask == 0)
        return true;
    const int highestPending   = std::countr_zero(m_pendingMask);
    const int highestInService = std::countr_zero(m_inServiceMask);
    return highestPending < highestInService;
}

uint8_t Im2Controller::acknowledge() noexcept
{
    if (!m_hwIm2Enabled) {
        m_pendingMask = 0;
        return m_defaultBusByte;
    }
    if (m_pendingMask == 0)
        return m_defaultBusByte;

    const int idx = std::countr_zero(m_pendingMask);
    const uint16_t bit = static_cast<uint16_t>(1u << idx);
    m_pendingMask &= static_cast<uint16_t>(~bit);
    m_inServiceMask |= bit;
    return static_cast<uint8_t>(m_vectorBase | (idx << 1));
}

void Im2Controller::onReti() noexcept
{
    if (m_inServiceMask != 0) {
        const int idx = std::countr_zero(m_inServiceMask);
        m_inServiceMask &= static_cast<uint16_t>(~(1u << idx));
    }
}

void Im2Controller::saveState(StateWriter &w) const
{
    w.writeBool(m_hwIm2Enabled);
    w.write(m_vectorBase);
    w.write(m_pendingMask);
    w.write(m_inServiceMask);
    w.write(m_defaultBusByte);
}

void Im2Controller::loadState(StateReader &r)
{
    m_hwIm2Enabled   = r.readBool();
    m_vectorBase     = r.read<uint8_t>();
    m_pendingMask    = r.read<uint16_t>();
    m_inServiceMask  = r.read<uint16_t>();
    m_defaultBusByte = r.read<uint8_t>();
}

} // namespace zxkit
