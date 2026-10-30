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

#include "zxkit/core/Clock.h"

#include <array>
#include <cstdint>

namespace zxkit {

enum class ContentionPattern : uint8_t {
    None = 0,
    Sinclair48K,       // 6, 5, 4, 3, 2, 1, 0, 0
    SinclairPlus2A_3,  // 1, 0, 7, 6, 5, 4, 3, 2
    ZxNext             // VHDL ULA counter contention (48K/128K/+3 selectable)
};

class ContentionModel {
public:
    static constexpr std::size_t kMaxFrameTstates = 80'000;

    ContentionModel() = default;

    void configure(ContentionPattern pattern, const FrameTimings &timings, bool ulaContendsPortLowBit = true) noexcept;

    void setDisabled(bool disabled) noexcept { m_disabled = disabled; }
    bool isDisabled() const noexcept { return m_disabled || m_pattern == ContentionPattern::None; }
    ContentionPattern pattern() const noexcept { return m_pattern; }

    /// Returns wait T-states for a memory MREQ or contended I/O cycle at `frameTstate`.
    uint8_t memoryDelay(uint32_t frameTstate, CpuSpeed speed = CpuSpeed::Mhz3_5) const noexcept
    {
        if (isDisabled() || speed != CpuSpeed::Mhz3_5)
            return 0;
        if (frameTstate < m_tableSize)
            return m_delayTable[frameTstate];
        return 0;
    }

    uint8_t noMreqDelay(uint32_t frameTstate, CpuSpeed speed = CpuSpeed::Mhz3_5) const noexcept
    {
        if (isDisabled() || speed != CpuSpeed::Mhz3_5 || m_pattern == ContentionPattern::SinclairPlus2A_3)
            return 0;
        if (frameTstate < m_tableSize)
            return m_delayTable[frameTstate];
        return 0;
    }

    /// True if the port address is owned by the ULA for contention purposes (e.g. bit 0 == 0 on 48K/128K).
    bool isPortFromUla(uint16_t port) const noexcept;

private:
    void rebuildTable() noexcept;

    ContentionPattern m_pattern = ContentionPattern::None;
    FrameTimings m_timings{};
    bool m_ulaContendsPortLowBit = true;
    bool m_disabled = true;
    std::size_t m_tableSize = 0;
    std::array<uint8_t, kMaxFrameTstates> m_delayTable{};
};

} // namespace zxkit
