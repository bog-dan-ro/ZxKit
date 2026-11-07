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

#include "zxkit/memory/Mmu.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

/// Manages prioritized /ROMCS and expansion-bus memory overlays.
/// When multiple peripherals (e.g. Interface 1, DivMMC, Multiface, Beta128)
/// assert /ROMCS simultaneously, the highest-priority active provider wins,
/// and deactivating one cleanly restores the next active overlay or base MMU map.
class RomcsStack
{
public:
    using MapCallback = std::function<void(Mmu &mmu)>;

    struct Provider
    {
        std::string id;
        int priority = 0;
        bool active = false;
        uint16_t startAddr = 0x0000;
        std::size_t count1k = 16;
        MapCallback applyFn;
    };

    explicit RomcsStack(Mmu &mmu) noexcept;

    void clear();

    void registerProvider(std::string id, int priority, MapCallback applyFn,
                          uint16_t startAddr = 0x0000, std::size_t count1k = 16);

    void unregisterProvider(std::string_view id);
    void setActive(std::string_view id, bool active);
    bool isActive(std::string_view id) const noexcept;

    /// Machines like HC-2000 CP/M mode or +2A/+3 All-RAM mode can suppress /ROMCS globally.
    void setSuppressed(bool suppressed);
    bool isSuppressed() const noexcept;

    void refresh();

private:
    Mmu &m_mmu;
    std::vector<Provider> m_providers;
    bool m_suppressed = false;
};

} // namespace zxkit
