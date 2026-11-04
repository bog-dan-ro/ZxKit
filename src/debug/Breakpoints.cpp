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
    if (unsigned(bp.type) > unsigned(BreakpointType::Event))
        throw InvalidArgumentException("Unknown breakpoint type");
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

bool BreakpointManager::setEnabled(uint32_t id, bool enabled, Mmu &mmu)
{
    for (auto &bp : m_breakpoints) {
        if (bp.id == id) {
            bp.enabled = enabled;
            syncMmuWatchFlags(mmu);
            return true;
        }
    }
    return false;
}

namespace {
bool matchesPage(const Breakpoint &bp, const Mmu &mmu, uint16_t address, bool write)
{
    const auto &map = mmu.effectiveMapping(address >> Mmu::kPageShift);
    return (!bp.bankId || *bp.bankId == (write ? map.writeBankId : map.bankId))
        && (!bp.page1k || *bp.page1k == (write ? map.writePage1k : map.readPage1k));
}
}

bool BreakpointManager::checkExecute(uint16_t pc, const Mmu &mmu, const Z80Registers &regs)
{
    if (!has(BreakpointType::Execute)) return false;
    bool hit = false;
    for (auto it = m_breakpoints.begin(); it != m_breakpoints.end();) {
        if (it->enabled && it->type == BreakpointType::Execute && ((pc & it->mask) == (it->address & it->mask))
            && matchesPage(*it, mmu, pc, false) && (!it->condition || it->condition(regs))) {
            ++it->hitCount;
            hit = true;
            if (it->temporary) { it = m_breakpoints.erase(it); continue; }
        }
        ++it;
    }
    if (hit) updateEnabledTypes();
    return hit;
}

bool BreakpointManager::checkMemory(uint16_t addr, bool isWrite, const Z80Registers &regs, Mmu *mmu)
{
    const auto type = isWrite ? BreakpointType::MemoryWrite : BreakpointType::MemoryRead;
    if (!has(type)) return false;
    bool hit = false, removed = false;
    for (auto it = m_breakpoints.begin(); it != m_breakpoints.end();) {
        if (it->enabled && it->type == type && ((addr & it->mask) == (it->address & it->mask))
            && ((!it->page1k && !it->bankId) || (mmu && matchesPage(*it, *mmu, addr, isWrite)))
            && (!it->condition || it->condition(regs))) {
            ++it->hitCount;
            hit = true;
            if (it->temporary) { it = m_breakpoints.erase(it); removed = true; continue; }
        }
        ++it;
    }
    if (removed) {
        updateEnabledTypes();
        if (mmu) syncMmuWatchFlags(*mmu);
    }
    return hit;
}

bool BreakpointManager::checkValue(BreakpointType type, uint16_t value, const Z80Registers &regs)
{
    if (!has(type)) return false;
    bool hit = false;
    for (auto it = m_breakpoints.begin(); it != m_breakpoints.end();) {
        if (it->enabled && it->type == type && ((value & it->mask) == (it->address & it->mask))
            && (!it->condition || it->condition(regs))) {
            ++it->hitCount;
            hit = true;
            if (it->temporary) { it = m_breakpoints.erase(it); continue; }
        }
        ++it;
    }
    if (hit) updateEnabledTypes();
    return hit;
}

bool BreakpointManager::checkPort(uint16_t port, bool isWrite, const Z80Registers &regs)
{
    return checkValue(isWrite ? BreakpointType::PortWrite : BreakpointType::PortRead, port, regs);
}
bool BreakpointManager::checkNextReg(uint8_t reg, const Z80Registers &regs)
{
    return checkValue(BreakpointType::NextRegWrite, reg, regs);
}
bool BreakpointManager::checkRaster(uint16_t line, const Z80Registers &regs)
{
    return checkValue(BreakpointType::RasterLine, line, regs);
}
bool BreakpointManager::checkEvent(DebugEvent event, const Z80Registers &regs)
{
    return checkValue(BreakpointType::Event, uint16_t(event), regs);
}

void BreakpointManager::updateEnabledTypes() const noexcept
{
    m_enabledTypes = 0;
    for (const auto &bp : m_breakpoints)
        if (bp.enabled) m_enabledTypes |= 1u << unsigned(bp.type);
}

void BreakpointManager::syncMmuWatchFlags(Mmu &mmu) const noexcept
{
    updateEnabledTypes();
    for (std::size_t slot = 0; slot < Mmu::kSlotCount; ++slot) {
        bool read = false, write = false;
        for (const auto &bp : m_breakpoints) {
            if (!bp.enabled) continue;
            // Physical watches must survive paging and aliases at every logical slot.
            const uint16_t slotMask = bp.mask & ~Mmu::kPageMask;
            if (bp.page1k || bp.bankId || ((uint16_t(slot << Mmu::kPageShift) & slotMask) == (bp.address & slotMask))) {
                read |= bp.type == BreakpointType::MemoryRead;
                write |= bp.type == BreakpointType::MemoryWrite;
            }
        }
        mmu.setSlotWatchFlags(slot, read, write);
    }
}

} // namespace zxkit
