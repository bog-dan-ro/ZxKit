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

#include "zxkit/debug/Breakpoints.h"
#include "zxkit/debug/PokeFinder.h"
#include "zxkit/debug/RewindBuffer.h"
#include "zxkit/debug/SymbolTable.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace zxkit {

using ClientId = uint32_t;
inline constexpr ClientId kLocalDebugClient = 1;

struct TraceEntry
{
    uint64_t cycle = 0;
    uint16_t pc = 0;
    uint16_t af = 0;
    uint16_t bc = 0;
    uint16_t de = 0;
    uint16_t hl = 0;
    uint16_t sp = 0;
};

class Debugger
{
public:
    BreakpointManager &breakpoints() noexcept
    {
        return m_breakpoints;
    }
    const BreakpointManager &breakpoints() const noexcept
    {
        return m_breakpoints;
    }

    PokeFinder &pokeFinder() noexcept
    {
        return m_pokeFinder;
    }
    const PokeFinder &pokeFinder() const noexcept
    {
        return m_pokeFinder;
    }

    RewindBuffer &rewindBuffer() noexcept
    {
        return m_rewindBuffer;
    }
    const RewindBuffer &rewindBuffer() const noexcept
    {
        return m_rewindBuffer;
    }

    void setTraceEnabled(bool enabled, std::size_t maxEntries = 4096) noexcept;
    bool isTraceEnabled() const noexcept
    {
        return m_traceEnabled;
    }

    void recordTrace(uint64_t cycle, const Z80Registers &regs);
    const std::deque<TraceEntry> &traceLog() const noexcept
    {
        return m_traceLog;
    }

    SymbolTable &symbolTable() noexcept
    {
        return m_symbolTable;
    }
    const SymbolTable &symbolTable() const noexcept
    {
        return m_symbolTable;
    }

    void setSymbol(uint16_t address, std::string label);

private:
    BreakpointManager m_breakpoints;
    PokeFinder m_pokeFinder;
    RewindBuffer m_rewindBuffer;
    SymbolTable m_symbolTable;
    bool m_traceEnabled = false;
    std::size_t m_maxTraceEntries = 4096;
    std::deque<TraceEntry> m_traceLog;
};

} // namespace zxkit
