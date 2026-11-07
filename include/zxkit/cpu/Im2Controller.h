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

/// Supports both standard Z80 IM2 (data bus byte) and ZX Spectrum Next's
/// 14-vector hardware IM2 priority daisy-chain controller.
class Im2Controller
{
public:
    // 14 on the Next; a Z80 CTC's vectors take 16 (4 channels in 4 vector groups)
    static constexpr std::size_t kMaxVectors = 16;

    void reset() noexcept;

    void setHardwareIm2Enabled(bool enabled) noexcept
    {
        m_hwIm2Enabled = enabled;
    }
    bool isHardwareIm2Enabled() const noexcept
    {
        return m_hwIm2Enabled;
    }

    void setVectorBase(uint8_t base) noexcept
    {
        m_vectorBase = base & 0xE0;
    }
    uint8_t vectorBase() const noexcept
    {
        return m_vectorBase;
    }

    void setDefaultBusByte(uint8_t val) noexcept
    {
        m_defaultBusByte = val;
    }
    uint8_t defaultBusByte() const noexcept
    {
        return m_defaultBusByte;
    }

    void raiseVector(uint8_t index) noexcept;
    void clearVector(uint8_t index) noexcept;

    /// True if a higher-priority hardware vector is pending than any currently in-service vector.
    bool hasPendingInterrupt() const noexcept;

    /// Acknowledges the highest-priority pending interrupt and returns the 8-bit vector low byte.
    uint8_t acknowledge() noexcept;

    /// Called when CPU executes ED 4D (RETI) to pop the highest-priority in-service bit.
    void onReti() noexcept;

    uint16_t pendingMask() const noexcept
    {
        return m_pendingMask;
    }
    uint16_t inServiceMask() const noexcept
    {
        return m_inServiceMask;
    }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    bool m_hwIm2Enabled = false;
    uint8_t m_vectorBase = 0;
    uint16_t m_pendingMask = 0;
    uint16_t m_inServiceMask = 0;
    uint8_t m_defaultBusByte = 0xFF;
};

} // namespace zxkit
