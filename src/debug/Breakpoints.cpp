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

#include "zxkit/debug/Breakpoints.h"

namespace zxkit {

uint32_t BreakpointManager::add(Breakpoint bp, Mmu &mmu)
{
    bp.id = ++m_nextId;
    m_breakpoints.push_back(std::move(bp));
    syncMmuWatchFlags(mmu);
    return m_breakpoints.back().id;
}

bool BreakpointManager::remove(uint32_t id, Mmu &mmu)
{
    const auto erased = std::erase_if(m_breakpoints, [id](const Breakpoint &b) { return b.id == id; });
    if (erased > 0)
        syncMmuWatchFlags(mmu);
    return erased > 0;
}

void BreakpointManager::clear(Mmu &mmu)
{
    m_breakpoints.clear();
    syncMmuWatchFlags(mmu);
}

bool BreakpointManager::checkExecute(uint16_t pc, const Mmu &mmu, const Z80Registers &regs)
{
    bool hit = false;
    for (auto it = m_breakpoints.begin(); it != m_breakpoints.end();) {
        if (it->enabled && it->type == BreakpointType::Execute && it->address == pc) {
            if (it->page1k.has_value()) {
                const auto &map = mmu.effectiveMapping(pc >> Mmu::kPageShift);
                if (map.readPage1k != *it->page1k) {
                    ++it;
                    continue;
                }
            }
            if (!it->condition || it->condition(regs)) {
                it->hitCount++;
                hit = true;
                if (it->temporary) {
                    it = m_breakpoints.erase(it);
                    continue;
                }
            }
        }
        ++it;
    }
    return hit;
}

bool BreakpointManager::checkMemory(uint16_t addr, bool isWrite, const Z80Registers &regs)
{
    const auto targetType = isWrite ? BreakpointType::MemoryWrite : BreakpointType::MemoryRead;
    for (auto &bp : m_breakpoints) {
        if (bp.enabled && bp.type == targetType && bp.address == addr) {
            if (!bp.condition || bp.condition(regs)) {
                bp.hitCount++;
                return true;
            }
        }
    }
    return false;
}

bool BreakpointManager::checkPort(uint16_t port, bool isWrite, const Z80Registers &regs)
{
    const auto targetType = isWrite ? BreakpointType::PortWrite : BreakpointType::PortRead;
    for (auto &bp : m_breakpoints) {
        if (bp.enabled && bp.type == targetType && ((port & bp.mask) == (bp.address & bp.mask))) {
            if (!bp.condition || bp.condition(regs)) {
                bp.hitCount++;
                return true;
            }
        }
    }
    return false;
}

void BreakpointManager::syncMmuWatchFlags(Mmu &mmu) const noexcept
{
    for (std::size_t slot = 0; slot < Mmu::kSlotCount; ++slot) {
        bool wRead = false;
        bool wWrite = false;
        for (const auto &bp : m_breakpoints) {
            if (!bp.enabled)
                continue;
            if ((bp.address >> Mmu::kPageShift) == slot) {
                if (bp.type == BreakpointType::MemoryRead)
                    wRead = true;
                else if (bp.type == BreakpointType::MemoryWrite)
                    wWrite = true;
            }
        }
        mmu.setSlotWatchFlags(slot, wRead, wWrite);
    }
}

} // namespace zxkit
