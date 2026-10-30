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

#include "zxkit/input/KeyboardMatrix.h"

namespace zxkit {

KeyboardMatrix::KeyboardMatrix()
{
    reset();
}

void KeyboardMatrix::reset() noexcept
{
    m_rows.fill(0x1F); // Active low: bits 0..4 = 1 when released
}

void KeyboardMatrix::setKeyState(ZxKey key, bool pressed) noexcept
{
    const uint8_t code = static_cast<uint8_t>(key);
    const uint8_t row = (code >> 4) & 0x07;
    const uint8_t bit = code & 0x07;
    if (bit >= 5)
        return;
    if (pressed)
        m_rows[row] &= static_cast<uint8_t>(~(1u << bit));
    else
        m_rows[row] |= static_cast<uint8_t>(1u << bit);
}

uint8_t KeyboardMatrix::readRows(uint8_t highAddrByte) const noexcept
{
    uint8_t result = 0x1F;
    for (int row = 0; row < 8; ++row) {
        if ((highAddrByte & (1u << row)) == 0)
            result &= m_rows[row];
    }
    return result;
}

} // namespace zxkit
