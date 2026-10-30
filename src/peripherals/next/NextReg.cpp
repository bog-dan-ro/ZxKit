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

#include "zxkit/peripherals/next/NextReg.h"

#include <utility>

namespace zxkit {

NextReg::NextReg()
{
    reset(true);
}

void NextReg::reset(bool hard) noexcept
{
    m_selected = 0x00;
    if (hard) {
        m_regs.fill(0x00);
    }
    // Standard TBBlue Issue 2 / Core 3.02.03 defaults
    m_regs[0x00] = 0x0A; // Machine ID: ZX Spectrum Next
    m_regs[0x01] = 0x32; // Core version 3.02
    m_regs[0x03] = 0x03; // Machine type +3 / Next
    m_regs[0x07] = 0x00; // CPU speed 3.5 MHz
    m_regs[0x0E] = 0x03; // Core sub-version .03
    m_regs[0x12] = 0x08; // Layer 2 active RAM 16K bank = 8
    m_regs[0x13] = 0x0B; // Layer 2 shadow RAM 16K bank = 11
    m_regs[0x14] = 0xE3; // Global transparency RGB333 (0xE3)
    m_regs[0x15] = 0x00; // Sprite & Layer system (SLU priority)
    m_regs[0x42] = 0x07; // ULANext ink format
    m_regs[0x4A] = 0xE3; // Fallback colour
    m_regs[0x4B] = 0xE3; // Sprite transparency index
    m_regs[0x4C] = 0x0F; // Tilemap transparency index
    m_regs[0x6E] = 0x2C; // Tilemap base 0x6C00
    m_regs[0x6F] = 0x0C; // Tile definitions base 0x4C00

    // 8x8K MMU slots default to standard 48K layout (0xFF, 0xFF, 10, 11, 4, 5, 0, 1)
    m_regs[0x50] = 0xFF;
    m_regs[0x51] = 0xFF;
    m_regs[0x52] = 10;
    m_regs[0x53] = 11;
    m_regs[0x54] = 4;
    m_regs[0x55] = 5;
    m_regs[0x56] = 0;
    m_regs[0x57] = 1;
}

void NextReg::select(uint8_t reg) noexcept
{
    m_selected = reg;
}

uint8_t NextReg::selected() const noexcept
{
    return m_selected;
}

uint8_t NextReg::readSelected() const
{
    return read(m_selected);
}

void NextReg::writeSelected(uint8_t val)
{
    write(m_selected, val);
}

uint8_t NextReg::read(uint8_t reg) const
{
    const uint8_t val = m_regs[reg];
    if (m_readCallbacks[reg])
        return m_readCallbacks[reg](reg, val);
    return val;
}

uint8_t NextReg::peek(uint8_t reg) const noexcept
{
    return m_regs[reg];
}

void NextReg::write(uint8_t reg, uint8_t val)
{
    const uint8_t oldVal = m_regs[reg];
    m_regs[reg] = val;
    if (m_writeCallbacks[reg])
        m_writeCallbacks[reg](reg, val);
    if (m_watchCallback)
        m_watchCallback(reg, oldVal, m_regs[reg]);
}

void NextReg::setRaw(uint8_t reg, uint8_t val) noexcept
{
    m_regs[reg] = val;
}

void NextReg::setReadHandler(uint8_t reg, ReadCallback cb)
{
    m_readCallbacks[reg] = std::move(cb);
}

void NextReg::setWriteHandler(uint8_t reg, WriteCallback cb)
{
    m_writeCallbacks[reg] = std::move(cb);
}

void NextReg::setWatchCallback(WatchCallback cb)
{
    m_watchCallback = std::move(cb);
}

void NextReg::saveState(StateWriter &w) const
{
    w.write(m_selected);
    w.writeBytes(m_regs);
}

void NextReg::loadState(StateReader &r)
{
    m_selected = r.read<uint8_t>();
    r.readBytes(m_regs);
}

} // namespace zxkit
