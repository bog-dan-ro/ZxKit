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
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

struct AssemblerOptions {
    /// Accept the ZX Spectrum Next's Z80N instructions.
    bool z80n = true;
    /// The base of the numbers without a prefix or a suffix (10 or 16). In base 16 they
    /// must start with a digit (`0FF`), so they can't be mistaken for a register (`B`).
    int defaultBase = 10;
};

/// Z80 and Z80N assembler, one instruction at a time (the debugger's inline assembler).
///
/// It knows exactly the instructions the `Disassembler` prints, in the same syntax (its
/// table is built from the Disassembler), so whatever the Disassembler shows can be edited
/// and assembled back, the undocumented instructions included. It also takes the usual
/// spellings: any case and spacing, `(IX+5)`, `(IX)`, `ADD B` for `ADD A,B`, `CP A,B` for
/// `CP B`, `EX AF,AF`, `JP HL`, and numbers as `$FF`, `#FF`, `0xFF`, `0FFh`, `%1010`, `0b1010`
/// or plain (see `AssemblerOptions::defaultBase`). `DEFB n,...` (or `DB`) emits raw bytes.
class Assembler {
public:
    /// The bytes of `line` assembled at address `pc`, or why it can't be assembled.
    static std::expected<std::vector<uint8_t>, std::string> assemble(std::string_view line, uint16_t pc = 0,
                                                                     AssemblerOptions options = {});
    /// Like `assemble()`, but throws a `DebuggerException` when `line` can't be assembled.
    static std::vector<uint8_t> assembleOne(std::string_view line, uint16_t pc = 0, AssemblerOptions options = {});
};

} // namespace zxkit
