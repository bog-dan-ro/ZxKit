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

#include "zxkit/peripherals/next/ZxnDma.h"

namespace zxkit {

namespace {
// One prescaler unit is 32 cycles of the 28MHz master clock = 4 base T-states
constexpr uint32_t kPrescalerUnitTs = 4;
} // namespace

void ZxnDma::reset() noexcept
{
    m_wr0 = 0;
    m_wr3 = 0;
    m_wr4 = 0;
    m_portAStart = 0;
    m_blockLen = 0;
    m_portAIsIo = false;
    m_portAMode = 1;
    m_portBStart = 0;
    m_portBIsIo = false;
    m_portBMode = 1;
    m_prescaler = 0;
    m_directionAtoB = true;
    m_transferMode = 1;
    m_autoRestart = false;
    m_enabled = false;
    m_srcAddr = 0;
    m_dstAddr = 0;
    m_counter = 0;
    m_length = 0;
    m_nextByteTs = 0;
    m_endOfBlock = false;
    m_atLeastOne = false;
    m_readMask = 0x7F;
    m_readIndex = 0;
    m_state = ExpectCommand;
}

void ZxnDma::startReadSequence() noexcept
{
    m_readIndex = 0;
    while (m_readIndex < 7 && !(m_readMask & (1u << m_readIndex)))
        ++m_readIndex;
}

uint8_t ZxnDma::readPort(bool zilogMode) noexcept
{
    m_zilogMode = zilogMode;
    if (m_readIndex >= 7)
        startReadSequence();
    if (m_readIndex >= 7)
        return 0xFF;

    const uint16_t portA = m_directionAtoB ? m_srcAddr : m_dstAddr;
    const uint16_t portB = m_directionAtoB ? m_dstAddr : m_srcAddr;
    uint8_t result = 0;
    switch (m_readIndex) {
    case 0: // status: 00 endofblock_n 1101 atleastone
        result = static_cast<uint8_t>((m_endOfBlock ? 0x00 : 0x20) | 0x1A | (m_atLeastOne ? 0x01 : 0x00));
        break;
    case 1: result = static_cast<uint8_t>(m_counter); break;
    case 2: result = static_cast<uint8_t>(m_counter >> 8); break;
    case 3: result = static_cast<uint8_t>(portA); break;
    case 4: result = static_cast<uint8_t>(portA >> 8); break;
    case 5: result = static_cast<uint8_t>(portB); break;
    case 6: result = static_cast<uint8_t>(portB >> 8); break;
    default: break;
    }
    // Next field selected by the read mask, wrapping around
    uint8_t next = static_cast<uint8_t>(m_readIndex + 1);
    while (next < 7 && !(m_readMask & (1u << next)))
        ++next;
    m_readIndex = next;
    if (m_readIndex >= 7)
        startReadSequence();
    return result;
}

void ZxnDma::nextWr0State() noexcept
{
    switch (m_state) {
    case ExpectCommand:
        if (m_wr0 & 0x08) { m_state = ExpectWr0PortALo; return; }
        [[fallthrough]];
    case ExpectWr0PortALo:
        if (m_wr0 & 0x10) { m_state = ExpectWr0PortAHi; return; }
        [[fallthrough]];
    case ExpectWr0PortAHi:
        if (m_wr0 & 0x20) { m_state = ExpectWr0LenLo; return; }
        [[fallthrough]];
    case ExpectWr0LenLo:
        if (m_wr0 & 0x40) { m_state = ExpectWr0LenHi; return; }
        [[fallthrough]];
    default:
        m_state = ExpectCommand;
    }
}

void ZxnDma::nextWr3State() noexcept
{
    if (m_state == ExpectCommand && (m_wr3 & 0x08))
        m_state = ExpectWr3Mask;
    else if (m_state != ExpectWr3Match && (m_wr3 & 0x10))
        m_state = ExpectWr3Match;
    else
        m_state = ExpectCommand;
}

void ZxnDma::nextWr4State() noexcept
{
    switch (m_state) {
    case ExpectCommand:
        if (m_wr4 & 0x04) { m_state = ExpectWr4PortBLo; return; }
        [[fallthrough]];
    case ExpectWr4PortBLo:
        if (m_wr4 & 0x08) { m_state = ExpectWr4PortBHi; return; }
        [[fallthrough]];
    case ExpectWr4PortBHi:
        if (m_wr4 & 0x10) { m_state = ExpectWr4Interrupt; return; }
        [[fallthrough]];
    default:
        m_state = ExpectCommand;
    }
}

void ZxnDma::load() noexcept
{
    m_srcAddr = m_directionAtoB ? m_portAStart : m_portBStart;
    m_dstAddr = m_directionAtoB ? m_portBStart : m_portAStart;
    m_counter = 0;
    // ZXN mode transfers exactly the block length, Zilog mode one more byte
    m_length = m_zilogMode ? static_cast<uint32_t>(m_blockLen) + 1 : m_blockLen;
    m_endOfBlock = false;
    m_atLeastOne = false;
}

void ZxnDma::writePort(uint8_t val, Mmu &mmu, IoBus &ioBus, bool zilogMode)
{
    m_zilogMode = zilogMode;
    switch (m_state) {
    case ExpectWr0PortALo:
        m_portAStart = static_cast<uint16_t>((m_portAStart & 0xFF00) | val);
        nextWr0State();
        return;
    case ExpectWr0PortAHi:
        m_portAStart = static_cast<uint16_t>((val << 8) | (m_portAStart & 0x00FF));
        nextWr0State();
        return;
    case ExpectWr0LenLo:
        m_blockLen = static_cast<uint16_t>((m_blockLen & 0xFF00) | val);
        nextWr0State();
        return;
    case ExpectWr0LenHi:
        m_blockLen = static_cast<uint16_t>((val << 8) | (m_blockLen & 0x00FF));
        nextWr0State();
        return;
    case ExpectWr1Timing:
        m_state = (val & 0x20) ? ExpectWr1Prescaler : ExpectCommand;
        return;
    case ExpectWr1Prescaler:
        m_state = ExpectCommand; // port A prescaler: unused by the hardware
        return;
    case ExpectWr2Timing:
        // The ZXN prescaler byte follows only when bit 5 of the timing byte is set
        m_state = (val & 0x20) ? ExpectWr2Prescaler : ExpectCommand;
        return;
    case ExpectWr2Prescaler:
        m_prescaler = val;
        m_state = ExpectCommand;
        return;
    case ExpectWr3Mask:
    case ExpectWr3Match:
        nextWr3State();
        return;
    case ExpectWr4PortBLo:
        m_portBStart = static_cast<uint16_t>((m_portBStart & 0xFF00) | val);
        nextWr4State();
        return;
    case ExpectWr4PortBHi:
        m_portBStart = static_cast<uint16_t>((val << 8) | (m_portBStart & 0x00FF));
        nextWr4State();
        return;
    case ExpectWr4Interrupt:
        m_state = ExpectCommand; // interrupt control: not supported
        return;
    case ExpectWr6ReadMask:
        m_readMask = val & 0x7F;
        startReadSequence();
        m_state = ExpectCommand;
        return;
    case ExpectCommand:
        break;
    }

    if ((val & 0x80) == 0) {
        if ((val & 0x03) != 0) {
            // WR0: direction, port A start address, block length
            m_wr0 = val;
            m_directionAtoB = (val & 0x04) != 0;
            nextWr0State();
        } else if ((val & 0x07) == 0x04) {
            // WR1: port A
            m_portAIsIo = (val & 0x08) != 0;
            m_portAMode = (val >> 4) & 0x03;
            if (val & 0x40)
                m_state = ExpectWr1Timing;
        } else if ((val & 0x07) == 0x00) {
            // WR2: port B
            m_portBIsIo = (val & 0x08) != 0;
            m_portBMode = (val >> 4) & 0x03;
            if (val & 0x40)
                m_state = ExpectWr2Timing;
        }
        return;
    }

    switch (val & 0x83) {
    case 0x80: // WR3: mask / match bytes, enable
        m_wr3 = val;
        nextWr3State();
        if (val & 0x40) {
            m_enabled = true;
            runUnpaced(mmu, ioBus);
        }
        return;
    case 0x81: // WR4: transfer mode, port B start address
        m_wr4 = val;
        m_transferMode = (val >> 5) & 0x03;
        nextWr4State();
        return;
    case 0x82: // WR5: auto restart
        if ((val & 0xC7) == 0x82)
            m_autoRestart = (val & 0x20) != 0;
        return;
    default: // 0x83: WR6 commands
        break;
    }

    switch (val) {
    case 0xC3: // reset
        reset();
        m_zilogMode = zilogMode;
        break;
    case 0xC7: // reset port A timing
        break;
    case 0xCB: // reset port B timing
        m_prescaler = 0;
        break;
    case 0xCF: // load
        load();
        break;
    case 0xD3: // continue: keep the addresses, restart the counter
        m_counter = 0;
        m_endOfBlock = false;
        break;
    case 0x87: // enable
        m_enabled = true;
        runUnpaced(mmu, ioBus);
        break;
    case 0x83: // disable
        m_enabled = false;
        break;
    case 0xBB: // read mask follows
        m_state = ExpectWr6ReadMask;
        break;
    case 0xA7: // initialize the read sequence
        startReadSequence();
        break;
    case 0xBF: // read the status byte next
        m_readIndex = 0;
        break;
    case 0x8B: // reinitialize the status byte
        m_endOfBlock = false;
        m_atLeastOne = false;
        break;
    default: // force ready, interrupt control: nothing to do
        break;
    }
}

bool ZxnDma::transferByte(Mmu &mmu, IoBus &ioBus)
{
    if (m_counter >= m_length) {
        finishBlock();
        return false;
    }
    const bool srcIsIo = m_directionAtoB ? m_portAIsIo : m_portBIsIo;
    const bool dstIsIo = m_directionAtoB ? m_portBIsIo : m_portAIsIo;
    const uint8_t srcMode = m_directionAtoB ? m_portAMode : m_portBMode;
    const uint8_t dstMode = m_directionAtoB ? m_portBMode : m_portAMode;

    // Byte by byte: overlapping copies (e.g. a fill from addr to addr + 1) must replicate
    const uint8_t byte = srcIsIo ? ioBus.read(m_srcAddr) : mmu.read(m_srcAddr);
    if (dstIsIo)
        ioBus.write(m_dstAddr, byte);
    else
        mmu.write(m_dstAddr, byte);

    if (srcMode == 1)
        ++m_srcAddr;
    else if (srcMode == 0)
        --m_srcAddr;
    if (dstMode == 1)
        ++m_dstAddr;
    else if (dstMode == 0)
        --m_dstAddr;

    ++m_counter;
    m_atLeastOne = true;
    if (m_counter >= m_length) {
        finishBlock();
        return false;
    }
    return true;
}

void ZxnDma::finishBlock() noexcept
{
    m_endOfBlock = true;
    if (m_autoRestart) {
        m_srcAddr = m_directionAtoB ? m_portAStart : m_portBStart;
        m_dstAddr = m_directionAtoB ? m_portBStart : m_portAStart;
        m_counter = 0;
        if (m_prescaler == 0)
            m_enabled = false; // an unpaced restart would never end
    } else {
        m_enabled = false;
    }
}

void ZxnDma::runUnpaced(Mmu &mmu, IoBus &ioBus)
{
    if (m_prescaler != 0) {
        m_nextByteTs = 0; // the next tick() sends the first byte
        return;
    }
    while (m_enabled && transferByte(mmu, ioBus)) {
    }
}

void ZxnDma::tick(uint32_t frameBaseTs, Mmu &mmu, IoBus &ioBus)
{
    if (!m_enabled || m_prescaler == 0)
        return;
    if (m_nextByteTs == 0)
        m_nextByteTs = frameBaseTs;
    const uint32_t interval = static_cast<uint32_t>(m_prescaler) * kPrescalerUnitTs;
    while (m_enabled && m_nextByteTs <= frameBaseTs) {
        transferByte(mmu, ioBus);
        m_nextByteTs += interval;
    }
}

void ZxnDma::onFrameEnd(uint32_t frameLength) noexcept
{
    m_nextByteTs = (m_nextByteTs > frameLength) ? m_nextByteTs - frameLength : 0;
}

void ZxnDma::saveState(StateWriter &w) const
{
    w.write(m_portAStart);
    w.write(m_portBStart);
    w.write(m_blockLen);
    w.writeBool(m_portAIsIo);
    w.write(m_portAMode);
    w.writeBool(m_portBIsIo);
    w.write(m_portBMode);
    w.write(m_prescaler);
    w.writeBool(m_directionAtoB);
    w.write(m_transferMode);
    w.writeBool(m_autoRestart);
    w.writeBool(m_zilogMode);
    w.writeBool(m_enabled);
    w.write(m_srcAddr);
    w.write(m_dstAddr);
    w.write(m_counter);
    w.write(m_length);
    w.write(m_nextByteTs);
    w.writeBool(m_endOfBlock);
    w.writeBool(m_atLeastOne);
    w.write(m_readMask);
    w.write(m_readIndex);
}

void ZxnDma::loadState(StateReader &r)
{
    m_portAStart    = r.read<uint16_t>();
    m_portBStart    = r.read<uint16_t>();
    m_blockLen      = r.read<uint16_t>();
    m_portAIsIo     = r.readBool();
    m_portAMode     = r.read<uint8_t>();
    m_portBIsIo     = r.readBool();
    m_portBMode     = r.read<uint8_t>();
    m_prescaler     = r.read<uint8_t>();
    m_directionAtoB = r.readBool();
    m_transferMode  = r.read<uint8_t>();
    m_autoRestart   = r.readBool();
    m_zilogMode     = r.readBool();
    m_enabled       = r.readBool();
    m_srcAddr       = r.read<uint16_t>();
    m_dstAddr       = r.read<uint16_t>();
    m_counter       = r.read<uint16_t>();
    m_length        = r.read<uint32_t>();
    m_nextByteTs    = r.read<uint32_t>();
    m_endOfBlock    = r.readBool();
    m_atLeastOne    = r.readBool();
    m_readMask      = r.read<uint8_t>();
    m_readIndex     = r.read<uint8_t>();
    m_state         = ExpectCommand;
}

} // namespace zxkit
