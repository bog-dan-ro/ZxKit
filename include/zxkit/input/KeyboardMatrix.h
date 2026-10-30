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

namespace zxkit {

enum class ZxKey : uint8_t {
    // Row 0 (0xFEFE): A8 = 0
    CapsShift = 0x00, Z = 0x01, X = 0x02, C = 0x03, V = 0x04,
    // Row 1 (0xFDFE): A9 = 0
    A = 0x10, S = 0x11, D = 0x12, F = 0x13, G = 0x14,
    // Row 2 (0xFBFE): A10 = 0
    Q = 0x20, W = 0x21, E = 0x22, R = 0x23, T = 0x24,
    // Row 3 (0xF7FE): A11 = 0
    Num1 = 0x30, Num2 = 0x31, Num3 = 0x32, Num4 = 0x33, Num5 = 0x34,
    // Row 4 (0xEFFE): A12 = 0
    Num0 = 0x40, Num9 = 0x41, Num8 = 0x42, Num7 = 0x43, Num6 = 0x44,
    // Row 5 (0xDFFE): A13 = 0
    P = 0x50, O = 0x51, I = 0x52, U = 0x53, Y = 0x54,
    // Row 6 (0xBFFE): A14 = 0
    Enter = 0x60, L = 0x61, K = 0x62, J = 0x63, H = 0x64,
    // Row 7 (0x7FFE): A15 = 0
    Space = 0x70, SymbolShift = 0x71, M = 0x72, N = 0x73, B = 0x74
};

class KeyboardMatrix {
public:
    KeyboardMatrix();

    void reset() noexcept;
    void setKeyState(ZxKey key, bool pressed) noexcept;

    /// Reads the 5-bit active-low keyboard state for a given high address byte (A15..A8).
    uint8_t readRows(uint8_t highAddrByte) const noexcept;

private:
    std::array<uint8_t, 8> m_rows{};
};

} // namespace zxkit
