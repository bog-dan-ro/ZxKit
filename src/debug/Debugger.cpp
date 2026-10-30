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

#include "zxkit/debug/Debugger.h"

namespace zxkit {

void Debugger::setTraceEnabled(bool enabled, std::size_t maxEntries) noexcept
{
    m_traceEnabled = enabled;
    m_maxTraceEntries = maxEntries;
    if (!enabled)
        m_traceLog.clear();
}

void Debugger::recordTrace(uint64_t cycle, const Z80Registers &regs)
{
    if (!m_traceEnabled)
        return;
    if (m_traceLog.size() >= m_maxTraceEntries)
        m_traceLog.pop_front();
    m_traceLog.push_back(TraceEntry{
        .cycle = cycle,
        .pc = regs.PC,
        .af = regs.AF,
        .bc = regs.BC,
        .de = regs.DE,
        .hl = regs.HL,
        .sp = regs.SP
    });
}

void Debugger::setSymbol(uint16_t address, std::string label)
{
    m_symbolTable.addSymbol(address, std::move(label));
}

} // namespace zxkit
