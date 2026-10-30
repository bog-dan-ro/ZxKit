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

#include "zxkit/peripherals/next/Copper.h"

namespace zxkit {

void Copper::reset() noexcept
{
    m_ram.fill(0);
    m_writeAddr = 0;
    m_pc = 0;
    m_mode = 0;
    m_halted = true;
}

void Copper::writeData8(uint8_t byte) noexcept
{
    m_ram[m_writeAddr & (kRamBytes - 1)] = byte;
    m_writeAddr = (m_writeAddr + 1) & (kRamBytes - 1);
}

void Copper::setControlLo(uint8_t val) noexcept
{
    m_writeAddr = (m_writeAddr & 0x0700) | val;
}

void Copper::setControlHi(uint8_t val) noexcept
{
    m_writeAddr = static_cast<uint16_t>(((val & 0x07) << 8) | (m_writeAddr & 0x00FF));
    const uint8_t newMode = (val >> 6) & 0x03;
    m_mode = newMode;
    if (newMode == 0) {
        m_halted = true;
    } else if (newMode == 1 || newMode == 3) {
        m_pc = 0;
        m_halted = false;
    } else if (newMode == 2) {
        m_halted = false;
    }
}

void Copper::onVBlank() noexcept
{
    if (m_mode == 3) {
        m_pc = 0;
        m_halted = false;
    }
}

void Copper::tickScanline(uint16_t vc, NextReg &nextReg)
{
    if (m_mode == 0 || m_halted)
        return;

    // Bound instructions per scanline to avoid infinite loops on self-wait
    for (int steps = 0; steps < 256 && !m_halted; ++steps) {
        const uint16_t addr = (m_pc & 0x03FF) * 2;
        const uint8_t hi = m_ram[addr];
        const uint8_t lo = m_ram[addr + 1];

        if (hi == 0xFF && lo == 0xFF) {
            // HALT (WAIT 511, 63)
            m_halted = true;
            return;
        }

        if (hi & 0x80) {
            // WAIT instruction: bit 15 = 1, bits 14:9 = hpos (0..55), bits 8:0 = vpos (0..311)
            const uint16_t waitVc = static_cast<uint16_t>(((hi & 0x01) << 8) | lo);
            if (vc < waitVc)
                return; // Still waiting for target scanline
            m_pc = (m_pc + 1) & 0x03FF;
        } else {
            // MOVE instruction: bit 15 = 0, bits 14:8 = NextREG (0x00..0x7F), bits 7:0 = value
            const uint8_t reg = hi & 0x7F;
            if (reg != 0x00) // MOVE 0, 0 is NOP
                nextReg.write(reg, lo);
            m_pc = (m_pc + 1) & 0x03FF;
        }
    }
}

uint16_t Copper::pc() const noexcept
{
    return m_pc;
}

uint8_t Copper::mode() const noexcept
{
    return m_mode;
}

bool Copper::isHalted() const noexcept
{
    return m_halted;
}

std::span<const uint8_t> Copper::instructionRam() const noexcept
{
    return m_ram;
}

void Copper::saveState(StateWriter &w) const
{
    w.writeBytes(m_ram);
    w.write(m_writeAddr);
    w.write(m_pc);
    w.write(m_mode);
    w.writeBool(m_halted);
}

void Copper::loadState(StateReader &r)
{
    r.readBytes(m_ram);
    m_writeAddr = r.read<uint16_t>();
    m_pc        = r.read<uint16_t>();
    m_mode      = r.read<uint8_t>();
    m_halted    = r.readBool();
}

} // namespace zxkit
