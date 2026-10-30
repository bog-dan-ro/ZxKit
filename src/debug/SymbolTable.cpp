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

#include "zxkit/debug/SymbolTable.h"

#include <charconv>
#include <sstream>
#include <vector>

namespace zxkit {

void SymbolTable::clear()
{
    m_addrToSymbol.clear();
    m_symbolToAddr.clear();
    m_sldMap.clear();
    m_callStack.clear();
}

void SymbolTable::addSymbol(uint16_t address, std::string name)
{
    m_symbolToAddr[name] = address;
    m_addrToSymbol[address] = std::move(name);
}

std::optional<std::string_view> SymbolTable::findSymbol(uint16_t address) const noexcept
{
    if (auto it = m_addrToSymbol.find(address); it != m_addrToSymbol.end())
        return it->second;
    return std::nullopt;
}

std::optional<uint16_t> SymbolTable::findAddress(std::string_view name) const noexcept
{
    if (auto it = m_symbolToAddr.find(std::string(name)); it != m_symbolToAddr.end())
        return it->second;
    return std::nullopt;
}

void SymbolTable::loadSldText(std::string_view sldText)
{
    std::istringstream iss{std::string(sldText)};
    std::string row;
    while (std::getline(iss, row)) {
        if (row.empty() || row[0] == '|')
            continue;
        std::vector<std::string_view> cols;
        const std::string_view sv(row);
        // Every '|' separates two columns, so "a|b|" has an empty third one
        for (std::size_t start = 0;;) {
            const auto bar = sv.find('|', start);
            cols.push_back(sv.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start));
            if (bar == std::string_view::npos)
                break;
            start = bar + 1;
        }
        if (cols.size() >= 7) {
            uint32_t lineNo = 0;
            uint16_t pageNo = 0;
            uint16_t addr = 0;
            std::from_chars(cols[1].data(), cols[1].data() + cols[1].size(), lineNo);
            std::from_chars(cols[3].data(), cols[3].data() + cols[3].size(), pageNo);
            std::from_chars(cols[4].data(), cols[4].data() + cols[4].size(), addr);

            if (cols[5] == "T") {
                m_sldMap[addr] = SourceLineLocation{
                    .file = std::string(cols[0]),
                    .line = lineNo,
                    .page = pageNo,
                    .address = addr
                };
            } else if (cols[5] == "L" && !cols[6].empty()) {
                // Label data is "module,label,local[,+flags]"; join the non empty
                // name parts the way sjasmplus spells them: module.label.local
                std::string name;
                std::string_view data = cols[6];
                for (int part = 0; part < 3; ++part) {
                    const auto comma = data.find(',');
                    const std::string_view field = data.substr(0, comma);
                    if (!field.empty() && field[0] != '+') {
                        if (!name.empty())
                            name += '.';
                        name += field;
                    }
                    if (comma == std::string_view::npos)
                        break;
                    data.remove_prefix(comma + 1);
                }
                if (!name.empty())
                    addSymbol(addr, std::move(name));
            }
        }
    }
}

std::optional<SourceLineLocation> SymbolTable::findSourceLocation(uint16_t address) const noexcept
{
    if (auto it = m_sldMap.find(address); it != m_sldMap.end())
        return it->second;
    return std::nullopt;
}

void SymbolTable::pushCallFrame(CallStackFrame frame, std::size_t maxDepth)
{
    if (m_callStack.size() >= maxDepth)
        m_callStack.pop_front();
    m_callStack.push_back(frame);
}

void SymbolTable::popCallFrame(uint16_t currentSp) noexcept
{
    while (!m_callStack.empty() && m_callStack.back().spAtCall <= currentSp)
        m_callStack.pop_back();
}

} // namespace zxkit
