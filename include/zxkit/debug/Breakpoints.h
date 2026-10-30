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

#include "zxkit/cpu/Z80Registers.h"
#include "zxkit/memory/Mmu.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace zxkit {

enum class BreakpointType : uint8_t {
    Execute = 0,
    MemoryRead,
    MemoryWrite,
    PortRead,
    PortWrite,
    NextRegWrite,
    RasterLine
};

struct Breakpoint {
    uint32_t id = 0;
    BreakpointType type = BreakpointType::Execute;
    bool enabled = true;
    bool temporary = false;
    uint16_t address = 0;
    uint16_t mask = 0xFFFF;
    std::optional<uint16_t> page1k;
    uint32_t hitCount = 0;
    std::function<bool(const Z80Registers &)> condition;
};

class BreakpointManager {
public:
    uint32_t add(Breakpoint bp, Mmu &mmu);
    bool remove(uint32_t id, Mmu &mmu);
    void clear(Mmu &mmu);
    bool checkExecute(uint16_t pc, const Mmu &mmu, const Z80Registers &regs);
    bool checkMemory(uint16_t addr, bool isWrite, const Z80Registers &regs);
    bool checkPort(uint16_t port, bool isWrite, const Z80Registers &regs);

    const std::vector<Breakpoint> &breakpoints() const noexcept { return m_breakpoints; }
    bool empty() const noexcept { return m_breakpoints.empty(); }

    void syncMmuWatchFlags(Mmu &mmu) const noexcept;

private:
    std::vector<Breakpoint> m_breakpoints;
    uint32_t m_nextId = 0;
};

} // namespace zxkit
