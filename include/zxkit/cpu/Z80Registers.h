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

#include "zxkit/core/StateStream.h"

#include <cstdint>

namespace zxkit {

struct Z80Flags
{
    static constexpr uint8_t C = 0x01; // Carry
    static constexpr uint8_t N = 0x02; // Subtract
    static constexpr uint8_t PV = 0x04; // Parity / Overflow
    static constexpr uint8_t F3 = 0x08; // Undocumented X (bit 3)
    static constexpr uint8_t H = 0x10; // Half-carry
    static constexpr uint8_t F5 = 0x20; // Undocumented Y (bit 5)
    static constexpr uint8_t Z = 0x40; // Zero
    static constexpr uint8_t S = 0x80; // Sign
};

struct Z80Registers
{
    uint16_t AF = 0xFFFF;
    uint16_t BC = 0xFFFF;
    uint16_t DE = 0xFFFF;
    uint16_t HL = 0xFFFF;

    uint16_t AF_ = 0xFFFF;
    uint16_t BC_ = 0xFFFF;
    uint16_t DE_ = 0xFFFF;
    uint16_t HL_ = 0xFFFF;

    uint16_t IX = 0xFFFF;
    uint16_t IY = 0xFFFF;
    uint16_t SP = 0xFFFF;
    uint16_t PC = 0x0000;
    uint16_t WZ = 0x0000; // Hidden MEMPTR register

    uint8_t I = 0x00;
    uint8_t R = 0x00; // Low 7 bits incremented per M1
    uint8_t R7 = 0x00; // Bit 7 of R (preserved across M1, updated by LD R,A)
    uint8_t IFF1 = 0;
    uint8_t IFF2 = 0;
    uint8_t IM = 0;
    uint8_t Q = 0; // Internal F latch for SCF/CCF undocumented flags
    uint8_t lastQ = 0;
    uint8_t incDecZ = 0; // T80N shadow Z latch for block/LDWS instructions
    bool halted = false;
    bool eiDelayed = false; // Interrupts inhibited immediately after EI

    void reset() noexcept;

    // 8-bit register accessors
    constexpr uint8_t A() const noexcept
    {
        return static_cast<uint8_t>(AF >> 8);
    }
    constexpr uint8_t F() const noexcept
    {
        return static_cast<uint8_t>(AF & 0xFF);
    }
    constexpr void setA(uint8_t v) noexcept
    {
        AF = static_cast<uint16_t>((AF & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setF(uint8_t v) noexcept
    {
        AF = static_cast<uint16_t>((AF & 0xFF00) | v);
    }

    constexpr uint8_t B() const noexcept
    {
        return static_cast<uint8_t>(BC >> 8);
    }
    constexpr uint8_t C() const noexcept
    {
        return static_cast<uint8_t>(BC & 0xFF);
    }
    constexpr void setB(uint8_t v) noexcept
    {
        BC = static_cast<uint16_t>((BC & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setC(uint8_t v) noexcept
    {
        BC = static_cast<uint16_t>((BC & 0xFF00) | v);
    }

    constexpr uint8_t D() const noexcept
    {
        return static_cast<uint8_t>(DE >> 8);
    }
    constexpr uint8_t E() const noexcept
    {
        return static_cast<uint8_t>(DE & 0xFF);
    }
    constexpr void setD(uint8_t v) noexcept
    {
        DE = static_cast<uint16_t>((DE & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setE(uint8_t v) noexcept
    {
        DE = static_cast<uint16_t>((DE & 0xFF00) | v);
    }

    constexpr uint8_t H() const noexcept
    {
        return static_cast<uint8_t>(HL >> 8);
    }
    constexpr uint8_t L() const noexcept
    {
        return static_cast<uint8_t>(HL & 0xFF);
    }
    constexpr void setH(uint8_t v) noexcept
    {
        HL = static_cast<uint16_t>((HL & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setL(uint8_t v) noexcept
    {
        HL = static_cast<uint16_t>((HL & 0xFF00) | v);
    }

    constexpr uint8_t IXH() const noexcept
    {
        return static_cast<uint8_t>(IX >> 8);
    }
    constexpr uint8_t IXL() const noexcept
    {
        return static_cast<uint8_t>(IX & 0xFF);
    }
    constexpr void setIXH(uint8_t v) noexcept
    {
        IX = static_cast<uint16_t>((IX & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setIXL(uint8_t v) noexcept
    {
        IX = static_cast<uint16_t>((IX & 0xFF00) | v);
    }

    constexpr uint8_t IYH() const noexcept
    {
        return static_cast<uint8_t>(IY >> 8);
    }
    constexpr uint8_t IYL() const noexcept
    {
        return static_cast<uint8_t>(IY & 0xFF);
    }
    constexpr void setIYH(uint8_t v) noexcept
    {
        IY = static_cast<uint16_t>((IY & 0x00FF) | (static_cast<uint16_t>(v) << 8));
    }
    constexpr void setIYL(uint8_t v) noexcept
    {
        IY = static_cast<uint16_t>((IY & 0xFF00) | v);
    }

    constexpr uint8_t fullR() const noexcept
    {
        return static_cast<uint8_t>((R & 0x7F) | (R7 & 0x80));
    }

    constexpr void setFullR(uint8_t val) noexcept
    {
        R = val & 0x7F;
        R7 = val & 0x80;
    }

    constexpr void incrementR(uint8_t count = 1) noexcept
    {
        R = (R + count) & 0x7F;
    }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);
};

} // namespace zxkit
