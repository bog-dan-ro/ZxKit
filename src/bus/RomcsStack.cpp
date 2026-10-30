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

#include "zxkit/bus/RomcsStack.h"

#include <algorithm>
#include <utility>

namespace zxkit {

RomcsStack::RomcsStack(Mmu &mmu) noexcept
    : m_mmu(mmu)
{
}

void RomcsStack::clear()
{
    m_providers.clear();
    m_suppressed = false;
}

void RomcsStack::registerProvider(std::string id,
                                  int priority,
                                  MapCallback applyFn,
                                  uint16_t startAddr,
                                  std::size_t count1k)
{
    unregisterProvider(id);
    m_providers.push_back(Provider{
        .id = std::move(id),
        .priority = priority,
        .active = false,
        .startAddr = startAddr,
        .count1k = count1k,
        .applyFn = std::move(applyFn)
    });
    std::ranges::stable_sort(m_providers, [](const Provider &a, const Provider &b) {
        return a.priority < b.priority; // Applied in ascending priority so highest wins
    });
}

void RomcsStack::unregisterProvider(std::string_view id)
{
    std::erase_if(m_providers, [id](const Provider &p) { return p.id == id; });
    refresh();
}

void RomcsStack::setActive(std::string_view id, bool active)
{
    for (auto &p : m_providers) {
        if (p.id == id) {
            if (p.active == active)
                return;
            p.active = active;
            refresh();
            return;
        }
    }
}

bool RomcsStack::isActive(std::string_view id) const noexcept
{
    for (const auto &p : m_providers) {
        if (p.id == id)
            return p.active;
    }
    return false;
}

void RomcsStack::setSuppressed(bool suppressed)
{
    if (m_suppressed == suppressed)
        return;
    m_suppressed = suppressed;
    refresh();
}

bool RomcsStack::isSuppressed() const noexcept
{
    return m_suppressed;
}

void RomcsStack::refresh()
{
    m_mmu.clearOverlay(0x0000, Mmu::kSlotCount);
    if (m_suppressed)
        return;

    for (const auto &p : m_providers) {
        if (p.active && p.applyFn)
            p.applyFn(m_mmu);
    }
}

} // namespace zxkit
