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

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zxkit {

struct SourceLineLocation {
    std::string file;
    uint32_t line = 0;
    uint16_t page = 0;
    uint16_t address = 0;
};

struct CallStackFrame {
    uint16_t callSitePc = 0;
    uint16_t targetPc = 0;
    uint16_t spAtCall = 0;
    uint64_t cycle = 0;
};

/// Manages symbols (.sym / .map), SLD source-level debugging locations (sjasmplus),
/// and the hardware CallStack tracker.
class SymbolTable {
public:
    void clear();
    void addSymbol(uint16_t address, std::string name);
    std::optional<std::string_view> findSymbol(uint16_t address) const noexcept;
    std::optional<uint16_t> findAddress(std::string_view name) const noexcept;

    /// Parses a sjasmplus SLD (Source Level Debugging) text file (`file|line|def|page|value|type|data`).
    void loadSldText(std::string_view sldText);

    std::optional<SourceLineLocation> findSourceLocation(uint16_t address) const noexcept;
    void pushCallFrame(CallStackFrame frame, std::size_t maxDepth = 128);
    void popCallFrame(uint16_t currentSp) noexcept;

    const std::deque<CallStackFrame> &callStack() const noexcept { return m_callStack; }

private:
    std::unordered_map<uint16_t, std::string> m_addrToSymbol;
    std::unordered_map<std::string, uint16_t> m_symbolToAddr;
    std::unordered_map<uint16_t, SourceLineLocation> m_sldMap;
    std::deque<CallStackFrame> m_callStack;
};

} // namespace zxkit
