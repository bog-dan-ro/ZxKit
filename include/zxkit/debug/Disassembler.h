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

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zxkit {

struct DisasmInstruction
{
    uint16_t address = 0;
    uint8_t length = 1;
    std::array<uint8_t, 4> bytes{};
    std::string mnemonic;
    bool isStepOverCandidate = false;
};

/// Z80 & Z80N Disassembler supporting single-instruction and range disassembly.
class Disassembler
{
public:
    using ReadByteFn = std::function<uint8_t(uint16_t addr)>;

    static DisasmInstruction disassembleOne(const ReadByteFn &readByte, uint16_t pc,
                                            bool z80n = true);
    static std::vector<DisasmInstruction> disassembleCount(const ReadByteFn &readByte,
                                                           uint16_t startPc, std::size_t count,
                                                           bool z80n = true);
};

} // namespace zxkit
