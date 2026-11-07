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

#include "zxkit/bus/IoBus.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/memory/Mmu.h"

#include <cstdint>

namespace zxkit {

/// ZXN-DMA / Z80-DMA Controller (ports 0x6B and 0x0B).
/// Memory / I/O transfers in both directions. Port 0x6B is the ZXN mode
/// (a block of N bytes), port 0x0B the Zilog compatible one (N + 1 bytes).
/// Unpaced transfers complete when the DMA is enabled; with a port B
/// prescaler (e.g. sample playback to a DAC) one byte goes out every
/// prescaler x 4 base T-states, driven by `tick()`.
class ZxnDma
{
public:
    void reset() noexcept;

    uint8_t readPort(bool zilogMode) noexcept;
    void writePort(uint8_t val, Mmu &mmu, IoBus &ioBus, bool zilogMode = false);
    /// Advances a paced (prescaler) transfer to `frameBaseTs` (base T-states in the frame).
    void tick(uint32_t frameBaseTs, Mmu &mmu, IoBus &ioBus);
    /// Call at the frame boundary, `frameLength` is the frame length in base T-states.
    void onFrameEnd(uint32_t frameLength) noexcept;

    bool isActive() const noexcept
    {
        return m_enabled;
    }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    enum ParseState : uint8_t {
        ExpectCommand = 0,
        ExpectWr0PortALo,
        ExpectWr0PortAHi,
        ExpectWr0LenLo,
        ExpectWr0LenHi,
        ExpectWr1Timing,
        ExpectWr1Prescaler,
        ExpectWr2Timing,
        ExpectWr2Prescaler,
        ExpectWr3Mask,
        ExpectWr3Match,
        ExpectWr4PortBLo,
        ExpectWr4PortBHi,
        ExpectWr4Interrupt,
        ExpectWr6ReadMask
    };

    void nextWr0State() noexcept;
    void nextWr3State() noexcept;
    void nextWr4State() noexcept;
    void load() noexcept;
    bool transferByte(Mmu &mmu, IoBus &ioBus);
    void finishBlock() noexcept;
    void runUnpaced(Mmu &mmu, IoBus &ioBus);
    void startReadSequence() noexcept;

    uint8_t m_wr0 = 0;
    uint8_t m_wr3 = 0;
    uint8_t m_wr4 = 0;
    uint16_t m_portAStart = 0;
    uint16_t m_portBStart = 0;
    uint16_t m_blockLen = 0;
    bool m_portAIsIo = false;
    uint8_t m_portAMode = 1; // 0 = decrement, 1 = increment, 2/3 = fixed
    bool m_portBIsIo = false;
    uint8_t m_portBMode = 1;
    uint8_t m_prescaler = 0;
    bool m_directionAtoB = true;
    uint8_t m_transferMode = 1; // 0 = byte, 1 = continuous, 2 = burst
    bool m_autoRestart = false;
    bool m_zilogMode = false;
    bool m_enabled = false;

    uint16_t m_srcAddr = 0;
    uint16_t m_dstAddr = 0;
    uint16_t m_counter = 0; // bytes transferred in this block
    uint32_t m_length = 0; // bytes in this block
    uint32_t m_nextByteTs = 0; // paced transfers: when the next byte is due

    bool m_endOfBlock = false;
    bool m_atLeastOne = false;
    uint8_t m_readMask = 0x7F;
    uint8_t m_readIndex = 0;
    ParseState m_state = ExpectCommand;
};

} // namespace zxkit
