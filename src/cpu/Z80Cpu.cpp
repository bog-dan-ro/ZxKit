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

#include "zxkit/cpu/Z80Cpu.h"

#include <array>
#include <bit>

namespace zxkit {

namespace {

using libspectrum_byte         = uint8_t;
using libspectrum_signed_byte  = int8_t;
using libspectrum_word         = uint16_t;
using libspectrum_dword        = uint32_t;
using libspectrum_signed_dword = int32_t;

constexpr uint8_t FLAG_C = Z80Flags::C;
constexpr uint8_t FLAG_N = Z80Flags::N;
constexpr uint8_t FLAG_P = Z80Flags::PV;
constexpr uint8_t FLAG_V = Z80Flags::PV;
constexpr uint8_t FLAG_3 = Z80Flags::F3;
constexpr uint8_t FLAG_H = Z80Flags::H;
constexpr uint8_t FLAG_5 = Z80Flags::F5;
constexpr uint8_t FLAG_Z = Z80Flags::Z;
constexpr uint8_t FLAG_S = Z80Flags::S;

constexpr std::array<uint8_t, 8> halfcarry_add_table = {0, FLAG_H, FLAG_H, FLAG_H, 0, 0, 0, FLAG_H};
constexpr std::array<uint8_t, 8> halfcarry_sub_table = {0, 0, FLAG_H, 0, FLAG_H, 0, FLAG_H, FLAG_H};
constexpr std::array<uint8_t, 8> overflow_add_table  = {0, 0, 0, FLAG_V, FLAG_V, 0, 0, 0};
constexpr std::array<uint8_t, 8> overflow_sub_table  = {0, FLAG_V, 0, 0, 0, 0, FLAG_V, 0};

consteval std::array<uint8_t, 256> makeSz53Table()
{
    std::array<uint8_t, 256> tbl{};
    for (int i = 0; i < 256; ++i)
        tbl[i] = static_cast<uint8_t>(i & (FLAG_3 | FLAG_5 | FLAG_S));
    tbl[0] |= FLAG_Z;
    return tbl;
}

consteval std::array<uint8_t, 256> makeParityTable()
{
    std::array<uint8_t, 256> tbl{};
    for (int i = 0; i < 256; ++i) {
        int p = std::popcount(static_cast<unsigned>(i));
        tbl[i] = (p & 1) ? 0 : FLAG_P;
    }
    return tbl;
}

consteval std::array<uint8_t, 256> makeSz53pTable()
{
    auto sz = makeSz53Table();
    auto par = makeParityTable();
    std::array<uint8_t, 256> tbl{};
    for (int i = 0; i < 256; ++i)
        tbl[i] = sz[i] | par[i];
    return tbl;
}

constexpr auto sz53_table   = makeSz53Table();
constexpr auto parity_table = makeParityTable();
constexpr auto sz53p_table  = makeSz53pTable();

constexpr bool isZ80nOpcode(uint8_t op) noexcept
{
    switch (op) {
    case 0x23: // SWAPNIB
    case 0x24: // MIRROR A
    case 0x27: // TEST nn
    case 0x28: // BSLA DE,B
    case 0x29: // BSRA DE,B
    case 0x2A: // BSRL DE,B
    case 0x2B: // BSRF DE,B
    case 0x2C: // BRLC DE,B
    case 0x30: // MUL D,E
    case 0x31: // ADD HL,A
    case 0x32: // ADD DE,A
    case 0x33: // ADD BC,A
    case 0x34: // ADD HL,nn
    case 0x35: // ADD DE,nn
    case 0x36: // ADD BC,nn
    case 0x8A: // PUSH nn
    case 0x90: // OUTINB
    case 0x91: // NEXTREG reg,val
    case 0x92: // NEXTREG reg,A
    case 0x93: // PIXELDN
    case 0x94: // PIXELAD
    case 0x95: // SETAE
    case 0x98: // JP (C)
    case 0xA4: // LDIX
    case 0xA5: // LDWS
    case 0xAC: // LDDX
    case 0xB4: // LDIRX
    case 0xB6: // LDIRSCALE
    case 0xB7: // LDPIRX
    case 0xBC: // LDDRX
        return true;
    default:
        return false;
    }
}

inline uint8_t &hiByte(uint16_t &word) noexcept
{
    if constexpr (std::endian::native == std::endian::little)
        return reinterpret_cast<uint8_t *>(&word)[1];
    else
        return reinterpret_cast<uint8_t *>(&word)[0];
}

inline uint8_t &loByte(uint16_t &word) noexcept
{
    if constexpr (std::endian::native == std::endian::little)
        return reinterpret_cast<uint8_t *>(&word)[0];
    else
        return reinterpret_cast<uint8_t *>(&word)[1];
}

struct MemptrView {
    uint16_t &w;
    struct ByteView {
        uint8_t &h;
        uint8_t &l;
    } b;

    explicit MemptrView(uint16_t &wz) noexcept
        : w(wz)
        , b{hiByte(wz), loByte(wz)}
    {
    }
};

struct LocalZ80Proxy {
    MemptrView memptr;
    bool &halted;
    bool &iff2_read;
    int64_t &interrupts_enabled_at;
};

inline uint8_t ldiFamilyFlags(uint8_t bytetemp, uint8_t a, uint16_t bcPostDec, uint8_t fIn) noexcept
{
    const uint8_t lookup = static_cast<uint8_t>(bytetemp + a);
    uint8_t f = fIn & (FLAG_C | FLAG_Z | FLAG_S);
    if (bcPostDec != 0)
        f |= FLAG_P;
    if (lookup & 0x08)
        f |= FLAG_3;
    if (lookup & 0x02)
        f |= FLAG_5;
    return f;
}

} // namespace

Z80Cpu::Z80Cpu(Clock &clock, Mmu &mmu, ContentionModel &contention, IoBus &ioBus, Im2Controller &im2)
    : m_clock(clock)
    , m_mmu(mmu)
    , m_contention(contention)
    , m_ioBus(ioBus)
    , m_im2(im2)
{
    reset(true);
}

void Z80Cpu::reset(bool hard) noexcept
{
    m_regs.AF = m_regs.AF_ = 0xFFFF;
    m_regs.I = m_regs.R = m_regs.R7 = 0;
    m_regs.PC = 0;
    m_regs.SP = 0xFFFF;
    m_regs.IFF1 = m_regs.IFF2 = m_regs.IM = 0;
    m_regs.halted = false;
    m_regs.eiDelayed = false;
    m_regs.Q = m_regs.lastQ = 0;
    m_regs.incDecZ = 0;

    if (hard) {
        m_regs.BC = m_regs.DE = m_regs.HL = 0;
        m_regs.BC_ = m_regs.DE_ = m_regs.HL_ = 0;
        m_regs.IX = m_regs.IY = 0;
        m_regs.WZ = 0;
    }

    m_interruptsEnabledAt = -1;
    m_intRequestedAt = 0;
    m_lastR7 = 0;
    m_iff2Read = false;
    m_nmiPending = false;
    m_nmiStackless = false;
    m_intPending = false;
    m_intVector = 0xFF;
    m_stacklessRetnActive = false;
    m_stacklessRetnAddress = 0;
    m_trapped = false;
}

void Z80Cpu::requestNmi(bool stackless) noexcept
{
    m_nmiPending = true;
    m_nmiStackless = stackless;
}

void Z80Cpu::requestInterrupt(uint8_t vector, uint32_t elapsedTstates) noexcept
{
    m_intPending = true;
    m_intVector = vector;
    m_intRequestedAt = m_clock.totalTstates() - elapsedTstates;
}

uint8_t Z80Cpu::readByte(uint16_t addr)
{
    contendRead(addr, 3);
    return m_mmu.read(addr);
}

uint8_t Z80Cpu::readByteInternal(uint16_t addr)
{
    return m_mmu.read(addr);
}

void Z80Cpu::writeByte(uint16_t addr, uint8_t val)
{
    contendRead(addr, 3);
    m_mmu.write(addr, val);
}

void Z80Cpu::contendRead(uint16_t addr, uint32_t time)
{
    if (!m_contention.isDisabled() && m_mmu.isContended(addr)) {
        m_clock.advance(m_contention.memoryDelay(m_clock.frameTstates(), m_clock.speed()));
    }
    m_clock.advance(time);
}

void Z80Cpu::contendReadNoMreq(uint16_t addr, uint32_t time)
{
    if (!m_contention.isDisabled() && m_mmu.isContended(addr)) {
        m_clock.advance(m_contention.noMreqDelay(m_clock.frameTstates(), m_clock.speed()));
    }
    m_clock.advance(time);
}

void Z80Cpu::contendWriteNoMreq(uint16_t addr, uint32_t time)
{
    contendReadNoMreq(addr, time);
}

void Z80Cpu::contendPortPreIo(uint16_t port)
{
    if (!m_contention.isDisabled() && m_mmu.isContended(port)) {
        m_clock.advance(m_contention.noMreqDelay(m_clock.frameTstates(), m_clock.speed()));
    }
    m_clock.advance(1);
}

void Z80Cpu::contendPortPostIo(uint16_t port)
{
    if (!m_contention.isDisabled()) {
        if (m_contention.isPortFromUla(port)) {
            m_clock.advance(m_contention.memoryDelay(m_clock.frameTstates(), m_clock.speed()));
            m_clock.advance(3);
            return;
        }
        if (m_mmu.isContended(port)) {
            m_clock.advance(m_contention.noMreqDelay(m_clock.frameTstates(), m_clock.speed()));
            m_clock.advance(1);
            m_clock.advance(m_contention.noMreqDelay(m_clock.frameTstates(), m_clock.speed()));
            m_clock.advance(1);
            m_clock.advance(m_contention.noMreqDelay(m_clock.frameTstates(), m_clock.speed()));
            m_clock.advance(1);
            return;
        }
    }
    m_clock.advance(3);
}

uint8_t Z80Cpu::readPort(uint16_t port)
{
    contendPortPreIo(port);
    contendPortPostIo(port);
    return m_ioBus.read(port);
}

void Z80Cpu::writePort(uint16_t port, uint8_t val)
{
    contendPortPreIo(port);
    m_ioBus.write(port, val);
    contendPortPostIo(port);
}

bool Z80Cpu::serviceNmi()
{
    if (!m_nmiPending)
        return false;

    m_nmiPending = false;
    if (m_regs.halted) {
        m_regs.PC++;
        m_regs.halted = false;
    }

    m_regs.IFF1 = 0;
    m_regs.incrementR(1);

    if (m_nmiStackless) {
        m_stacklessRetnActive = true;
        m_clock.advance(11);
        m_regs.SP -= 2;
    } else {
        m_stacklessRetnActive = false;
        m_clock.advance(5);
        writeByte(--m_regs.SP, static_cast<uint8_t>(m_regs.PC >> 8));
        writeByte(--m_regs.SP, static_cast<uint8_t>(m_regs.PC & 0xFF));
    }

    m_regs.Q = 0;
    m_regs.PC = 0x0066;
    return true;
}

bool Z80Cpu::serviceInterrupt()
{
    const bool hasHwIm2 = m_im2.isHardwareIm2Enabled() && m_im2.hasPendingInterrupt();
    if (!m_intPending && !hasHwIm2)
        return false;

    if (m_intPending) {
        const uint64_t elapsed = m_clock.totalTstates() - m_intRequestedAt;
        // /INT is held for interruptLength CPU clock cycles (on the Next the pulse
        // counts CPU clocks, so at 28MHz it is 8x shorter in time)
        if (elapsed >= m_clock.timings().interruptLength) {
            m_intPending = false;
            if (!hasHwIm2)
                return false;
        }
    }

    if (!m_regs.IFF1)
        return false;

    if (static_cast<int64_t>(m_clock.totalTstates()) == m_interruptsEnabledAt)
        return false;

    if (m_iff2Read && !m_isCmos) {
        m_regs.setF(m_regs.F() & ~FLAG_P);
    }

    if (m_regs.halted) {
        m_regs.PC++;
        m_regs.halted = false;
    }

    m_regs.IFF1 = m_regs.IFF2 = 0;
    m_regs.incrementR(1);
    // Acknowledging an interrupt does not deassert the external /INT line.
    // EI inside a short handler may accept the same ULA pulse again.
    if (!m_interruptRetrigger)
        m_intPending = false;

    m_clock.advance(7);

    writeByte(--m_regs.SP, static_cast<uint8_t>(m_regs.PC >> 8));
    writeByte(--m_regs.SP, static_cast<uint8_t>(m_regs.PC & 0xFF));

    const uint8_t vec = hasHwIm2 ? m_im2.acknowledge() : m_intVector;
    switch (m_regs.IM) {
    case 0:
    case 1:
    default:
        m_regs.PC = 0x0038;
        break;
    case 2: {
        const uint16_t intAddr = static_cast<uint16_t>((static_cast<uint16_t>(m_regs.I) << 8) | vec);
        const uint8_t lo = readByte(intAddr);
        const uint8_t hi = readByte(static_cast<uint16_t>(intAddr + 1));
        m_regs.PC = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        break;
    }
    }

    m_regs.WZ = m_regs.PC;
    m_regs.Q = 0;
    return true;
}

bool Z80Cpu::handleRetn(bool isReti)
{
    if (isReti)
        m_im2.onReti();

    if (m_activeHooksMask & HookFlags::Retn) {
        for (const auto &h : m_retnHooks)
            h.fn(isReti);
    }

    if (!m_stacklessRetnActive)
        return false;

    m_clock.advance(6);
    m_regs.SP += 2;
    m_regs.PC = m_stacklessRetnAddress;
    m_regs.WZ = m_regs.PC;
    m_stacklessRetnActive = false;
    return true;
}

void Z80Cpu::executeZ80n(uint8_t extOpcode)
{
    switch (extOpcode) {
    case 0x23: { // SWAPNIB
        const uint8_t a = m_regs.A();
        m_regs.setA(static_cast<uint8_t>(((a & 0x0F) << 4) | ((a >> 4) & 0x0F)));
        break;
    }
    case 0x24: { // MIRROR A
        uint8_t a = m_regs.A();
        uint8_t r = 0;
        for (int i = 0; i < 8; ++i) {
            r = static_cast<uint8_t>((r << 1) | (a & 1));
            a >>= 1;
        }
        m_regs.setA(r);
        break;
    }
    case 0x27: { // TEST nn
        const uint8_t nn = readByte(m_regs.PC++);
        const uint8_t temp = m_regs.A() & nn;
        const uint8_t f = FLAG_H | sz53p_table[temp];
        m_regs.setF(f);
        m_regs.Q = f;
        break;
    }
    case 0x28: { // BSLA DE,B
        const uint8_t shift = m_regs.B() & 0x1F;
        const uint32_t v = static_cast<uint32_t>(m_regs.DE) << shift;
        m_regs.DE = static_cast<uint16_t>(v & 0xFFFF);
        break;
    }
    case 0x29: { // BSRA DE,B
        const uint8_t shift = m_regs.B() & 0x1F;
        const bool signBit = (m_regs.DE & 0x8000) != 0;
        if (shift >= 16) {
            m_regs.DE = signBit ? 0xFFFFu : 0x0000u;
        } else if (shift > 0) {
            const uint16_t maskHi = signBit ? static_cast<uint16_t>((0xFFFFu << (16 - shift)) & 0xFFFFu) : 0u;
            m_regs.DE = static_cast<uint16_t>((m_regs.DE >> shift) | maskHi);
        }
        break;
    }
    case 0x2A: { // BSRL DE,B
        const uint8_t shift = m_regs.B() & 0x1F;
        m_regs.DE = (shift >= 16) ? 0 : static_cast<uint16_t>(m_regs.DE >> shift);
        break;
    }
    case 0x2B: { // BSRF DE,B
        const uint8_t shift = m_regs.B() & 0x1F;
        if (shift >= 16) {
            m_regs.DE = 0xFFFFu;
        } else if (shift > 0) {
            const uint16_t maskHi = static_cast<uint16_t>((0xFFFFu << (16 - shift)) & 0xFFFFu);
            m_regs.DE = static_cast<uint16_t>((m_regs.DE >> shift) | maskHi);
        }
        break;
    }
    case 0x2C: { // BRLC DE,B
        uint8_t rot = m_regs.B() & 0x0F;
        if (rot != 0)
            m_regs.DE = static_cast<uint16_t>((m_regs.DE << rot) | (m_regs.DE >> (16 - rot)));
        break;
    }
    case 0x30: { // MUL D,E
        m_regs.DE = static_cast<uint16_t>(m_regs.D()) * static_cast<uint16_t>(m_regs.E());
        break;
    }
    case 0x31: { // ADD HL,A
        m_regs.HL = static_cast<uint16_t>(m_regs.HL + m_regs.A());
        const uint8_t f = m_regs.F() & ~FLAG_C;
        m_regs.setF(f);
        m_regs.Q = f;
        break;
    }
    case 0x32: { // ADD DE,A
        m_regs.DE = static_cast<uint16_t>(m_regs.DE + m_regs.A());
        const uint8_t f = m_regs.F() & ~FLAG_C;
        m_regs.setF(f);
        m_regs.Q = f;
        break;
    }
    case 0x33: { // ADD BC,A
        m_regs.BC = static_cast<uint16_t>(m_regs.BC + m_regs.A());
        const uint8_t f = m_regs.F() & ~FLAG_C;
        m_regs.setF(f);
        m_regs.Q = f;
        break;
    }
    case 0x34: { // ADD HL,nn
        const uint8_t lo = readByte(m_regs.PC++);
        const uint8_t hi = readByte(m_regs.PC++);
        const uint16_t nn = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        m_regs.HL = static_cast<uint16_t>(m_regs.HL + nn);
        m_regs.WZ = nn;
        m_clock.advance(2);
        break;
    }
    case 0x35: { // ADD DE,nn
        const uint8_t lo = readByte(m_regs.PC++);
        const uint8_t hi = readByte(m_regs.PC++);
        const uint16_t nn = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        m_regs.DE = static_cast<uint16_t>(m_regs.DE + nn);
        m_regs.WZ = nn;
        m_clock.advance(2);
        break;
    }
    case 0x36: { // ADD BC,nn
        const uint8_t lo = readByte(m_regs.PC++);
        const uint8_t hi = readByte(m_regs.PC++);
        const uint16_t nn = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        m_regs.BC = static_cast<uint16_t>(m_regs.BC + nn);
        m_regs.WZ = nn;
        m_clock.advance(2);
        break;
    }
    case 0x8A: { // PUSH nn (big-endian immediate)
        const uint8_t hh = readByte(m_regs.PC++);
        const uint8_t ll = readByte(m_regs.PC++);
        m_regs.WZ = static_cast<uint16_t>((m_regs.WZ & 0xFF00) | ll);
        writeByte(--m_regs.SP, hh);
        writeByte(--m_regs.SP, ll);
        m_clock.advance(3);
        break;
    }
    case 0x90: { // OUTINB
        const uint16_t ir = static_cast<uint16_t>((static_cast<uint16_t>(m_regs.I) << 8) | m_regs.fullR());
        contendReadNoMreq(ir, 1);
        const uint8_t temp = readByte(m_regs.HL++);
        writePort(m_regs.BC, temp);
        break;
    }
    case 0x91: { // NEXTREG reg,val
        const uint8_t reg = readByte(m_regs.PC++);
        const uint8_t val = readByte(m_regs.PC++);
        m_ioBus.writeNextRegOpcode(reg, val);
        m_clock.advance(6);
        break;
    }
    case 0x92: { // NEXTREG reg,A
        const uint8_t reg = readByte(m_regs.PC++);
        m_ioBus.writeNextRegOpcode(reg, m_regs.A());
        m_clock.advance(6);
        break;
    }
    case 0x93: { // PIXELDN
        const uint8_t h = m_regs.H();
        const uint8_t l = m_regs.L();
        const uint8_t composite = static_cast<uint8_t>(((h & 0x18) << 3) | ((l & 0xE0) >> 2) | (h & 0x07));
        const uint8_t inc = static_cast<uint8_t>(composite + 1);
        const uint8_t newB = (inc >> 6) & 0x03;
        const uint8_t newR = (inc >> 3) & 0x07;
        const uint8_t newC = inc & 0x07;
        m_regs.setH(static_cast<uint8_t>((h & 0xE0) | (newB << 3) | newC));
        m_regs.setL(static_cast<uint8_t>((l & 0x1F) | (newR << 5)));
        break;
    }
    case 0x94: { // PIXELAD
        const uint8_t d = m_regs.D();
        const uint8_t e = m_regs.E();
        m_regs.setH(static_cast<uint8_t>(0x40 | ((d & 0xC0) >> 3) | (d & 0x07)));
        m_regs.setL(static_cast<uint8_t>(((d & 0x38) << 2) | (e >> 3)));
        break;
    }
    case 0x95: { // SETAE
        m_regs.setA(static_cast<uint8_t>(0x80u >> (m_regs.E() & 0x07)));
        break;
    }
    case 0x98: { // JP (C)
        const uint8_t val = readPort(m_regs.BC);
        m_regs.PC = static_cast<uint16_t>((m_regs.PC & 0xC000) | (static_cast<uint16_t>(val) << 6));
        break;
    }
    case 0xA4:   // LDIX
    case 0xAC:   // LDDX
    case 0xB4:   // LDIRX
    case 0xB6:   // LDIRSCALE
    case 0xBC: { // LDDRX
        const bool isDec = (extOpcode == 0xAC || extOpcode == 0xBC);
        const bool isRepeat = (extOpcode == 0xB4 || extOpcode == 0xB6 || extOpcode == 0xBC);
        const uint8_t a = m_regs.A();
        const uint8_t temp = readByte(m_regs.HL);
        if (temp != a) {
            writeByte(m_regs.DE, temp);
        } else {
            contendWriteNoMreq(m_regs.DE, 1);
            contendWriteNoMreq(m_regs.DE, 1);
            contendWriteNoMreq(m_regs.DE, 1);
        }
        const uint16_t dePreInc = m_regs.DE;
        m_regs.DE++;
        if (isDec)
            m_regs.HL--;
        else
            m_regs.HL++;
        m_regs.BC--;
        const uint8_t f = ldiFamilyFlags(temp, a, m_regs.BC, m_regs.F());
        m_regs.setF(f);
        m_regs.Q = f;
        m_regs.incDecZ = (m_regs.BC != 0) ? 1 : 0;

        int idleCycles = 2;
        if (isRepeat && m_regs.BC != 0) {
            m_regs.PC -= 2;
            idleCycles = 7;
        }
        for (int i = 0; i < idleCycles; ++i)
            contendWriteNoMreq(dePreInc, 1);
        break;
    }
    case 0xA5: { // LDWS
        const uint8_t temp = readByte(m_regs.HL);
        writeByte(m_regs.DE, temp);
        m_regs.setL(static_cast<uint8_t>(m_regs.L() + 1));
        const uint8_t dPost = static_cast<uint8_t>(m_regs.D() + 1);
        m_regs.setD(dPost);
        uint8_t f = m_regs.F() & FLAG_C;
        if (dPost & 0x80)
            f |= FLAG_S;
        if (dPost == 0)
            f |= FLAG_Z;
        if (dPost & 0x08)
            f |= FLAG_3;
        if (dPost & 0x02)
            f |= FLAG_5;
        if (m_regs.incDecZ)
            f |= FLAG_P;
        m_regs.setF(f);
        m_regs.Q = f;
        break;
    }
    case 0xB7: { // LDPIRX
        const uint8_t a = m_regs.A();
        const uint16_t srcAddr = static_cast<uint16_t>((m_regs.HL & 0xFFF8) | (m_regs.DE & 0x0007));
        const uint8_t temp = readByte(srcAddr);
        if (temp != a) {
            writeByte(m_regs.DE, temp);
        } else {
            contendWriteNoMreq(m_regs.DE, 1);
            contendWriteNoMreq(m_regs.DE, 1);
            contendWriteNoMreq(m_regs.DE, 1);
        }
        const uint16_t dePreInc = m_regs.DE;
        m_regs.DE++;
        m_regs.BC--;
        m_regs.incDecZ = (m_regs.BC != 0) ? 1 : 0;
        const uint8_t aluQ = static_cast<uint8_t>(m_regs.B() | temp);
        uint8_t f = m_regs.F() & (FLAG_S | FLAG_Z | FLAG_C);
        if (aluQ & 0x08)
            f |= FLAG_3;
        if (aluQ & 0x02)
            f |= FLAG_5;
        if (m_regs.incDecZ)
            f |= FLAG_P;
        m_regs.setF(f);
        m_regs.Q = f;
        m_regs.WZ = static_cast<uint16_t>((m_regs.WZ & 0xFF00) | 0x00B7u);
        int idleCycles = 2;
        if (m_regs.BC != 0) {
            m_regs.PC -= 2;
            idleCycles = 7;
        }
        for (int i = 0; i < idleCycles; ++i)
            contendWriteNoMreq(dePreInc, 1);
        break;
    }
    default:
        break;
    }
}

template<bool TrackFetches>
uint32_t Z80Cpu::stepInstructionImpl()
{
    const uint64_t startTs = m_clock.totalTstates();

    if (serviceNmi())
        return static_cast<uint32_t>(m_clock.totalTstates() - startTs);

    if constexpr (TrackFetches) {
        if (!m_playbackExecution && serviceInterrupt()) {
            m_trackedInterrupt = true;
            return static_cast<uint32_t>(m_clock.totalTstates() - startTs);
        }
    } else {
        if (serviceInterrupt())
            return static_cast<uint32_t>(m_clock.totalTstates() - startTs);
    }

    if (m_activeHooksMask != 0) [[unlikely]] {
        if ((m_activeHooksMask & HookFlags::Debugger) && m_debuggerHook) {
            if (m_debuggerHook(m_regs.PC)) {
                m_trapped = true;
                return 0;
            }
        }
        if (m_activeHooksMask & HookFlags::MagicBreak) {
            const uint8_t b0 = m_mmu.peek(m_regs.PC);
            const uint8_t b1 = m_mmu.peek(static_cast<uint16_t>(m_regs.PC + 1));
            if ((b0 == 0xED && b1 == 0xFF) || (b0 == 0xDD && b1 == 0x01)) {
                m_regs.PC = static_cast<uint16_t>(m_regs.PC + 2);
                m_regs.incrementR(2);
                if constexpr (TrackFetches) m_trackedFetches += 2;
                m_clock.advance(8);
                m_trapped = true;
                return 8;
            }
        }
        if ((m_activeHooksMask & HookFlags::PreM1) && m_preM1Pages[m_regs.PC >> 8]) {
            const std::size_t page = m_regs.PC >> 8;
            for (const auto &h : m_preM1Hooks) {
                if (h.pages[page])
                    h.fn(m_regs.PC);
            }
        }
    }

    m_iff2Read = false;

    uint16_t &AF  = m_regs.AF;
    uint16_t &BC  = m_regs.BC;
    uint16_t &DE  = m_regs.DE;
    uint16_t &HL  = m_regs.HL;
    uint16_t &AF_ = m_regs.AF_;
    uint16_t &BC_ = m_regs.BC_;
    uint16_t &DE_ = m_regs.DE_;
    uint16_t &HL_ = m_regs.HL_;
    uint16_t &IX  = m_regs.IX;
    uint16_t &IY  = m_regs.IY;
    uint16_t &SP  = m_regs.SP;
    uint16_t &PC  = m_regs.PC;

    uint8_t &A   = hiByte(m_regs.AF);
    uint8_t &F   = loByte(m_regs.AF);
    uint8_t &B   = hiByte(m_regs.BC);
    uint8_t &C   = loByte(m_regs.BC);
    uint8_t &D   = hiByte(m_regs.DE);
    uint8_t &E   = loByte(m_regs.DE);
    uint8_t &H   = hiByte(m_regs.HL);
    uint8_t &L   = loByte(m_regs.HL);
    uint8_t &IXH = hiByte(m_regs.IX);
    uint8_t &IXL = loByte(m_regs.IX);
    uint8_t &IYH = hiByte(m_regs.IY);
    uint8_t &IYL = loByte(m_regs.IY);
    uint8_t &SPH = hiByte(m_regs.SP);
    uint8_t &SPL = loByte(m_regs.SP);
    uint8_t &PCH = hiByte(m_regs.PC);
    uint8_t &PCL = loByte(m_regs.PC);

    uint8_t &I    = m_regs.I;
    uint8_t &R    = m_regs.R;
    uint8_t &R7   = m_regs.R7;
    uint8_t &IFF1 = m_regs.IFF1;
    uint8_t &IFF2 = m_regs.IFF2;
    uint8_t &IM   = m_regs.IM;
    uint8_t &Q    = m_regs.Q;

    LocalZ80Proxy z80{MemptrView(m_regs.WZ), m_regs.halted, m_iff2Read, m_interruptsEnabledAt};

#define slt_trap(hl, a) ((void)0)
#define IS_CMOS (m_isCmos)
#define IR (static_cast<uint16_t>((static_cast<uint16_t>(I) << 8) | (R7 & 0x80) | (R & 0x7F)))
#define tstates (m_clock.totalTstates())
#define readbyte(addr) (this->readByte(addr))
#define readbyte_internal(addr) (this->readByteInternal(addr))
#define writebyte(addr, val) (this->writeByte((addr), (val)))
#define readport(port) (this->readPort(port))
#define writeport(port, val) (this->writePort((port), (val)))
#define contend_read(addr, time) (this->contendRead((addr), (time)))
#define contend_read_no_mreq(addr, time) (this->contendReadNoMreq((addr), (time)))
#define contend_write_no_mreq(addr, time) (this->contendWriteNoMreq((addr), (time)))

#define AND(value) \
{ \
    A &= (value); \
    F = FLAG_H | sz53p_table[A]; \
    Q = F; \
}

#define ADC(value) \
{ \
    uint16_t adctemp = A + (value) + (F & FLAG_C); \
    uint8_t lookup = ((A & 0x88) >> 3) | (((value) & 0x88) >> 2) | ((adctemp & 0x88) >> 1); \
    A = static_cast<uint8_t>(adctemp); \
    F = (adctemp & 0x100 ? FLAG_C : 0) | halfcarry_add_table[lookup & 0x07] | overflow_add_table[lookup >> 4] | sz53_table[A]; \
    Q = F; \
}

#define ADC16(value) \
{ \
    uint32_t add16temp = HL + (value) + (F & FLAG_C); \
    uint8_t lookup = ((HL & 0x8800) >> 11) | (((value) & 0x8800) >> 10) | ((add16temp & 0x8800) >> 9); \
    z80.memptr.w = HL + 1; \
    HL = static_cast<uint16_t>(add16temp); \
    F = (add16temp & 0x10000 ? FLAG_C : 0) | overflow_add_table[lookup >> 4] | (H & (FLAG_3 | FLAG_5 | FLAG_S)) | halfcarry_add_table[lookup & 0x07] | (HL ? 0 : FLAG_Z); \
    Q = F; \
}

#define ADD(value) \
{ \
    uint16_t addtemp = A + (value); \
    uint8_t lookup = ((A & 0x88) >> 3) | (((value) & 0x88) >> 2) | ((addtemp & 0x88) >> 1); \
    A = static_cast<uint8_t>(addtemp); \
    F = (addtemp & 0x100 ? FLAG_C : 0) | halfcarry_add_table[lookup & 0x07] | overflow_add_table[lookup >> 4] | sz53_table[A]; \
    Q = F; \
}

#define ADD16(value1, value2) \
{ \
    uint32_t add16temp = (value1) + (value2); \
    uint8_t lookup = (((value1) & 0x0800) >> 11) | (((value2) & 0x0800) >> 10) | ((add16temp & 0x0800) >> 9); \
    z80.memptr.w = (value1) + 1; \
    (value1) = static_cast<uint16_t>(add16temp); \
    F = (F & (FLAG_V | FLAG_Z | FLAG_S)) | (add16temp & 0x10000 ? FLAG_C : 0) | ((add16temp >> 8) & (FLAG_3 | FLAG_5)) | halfcarry_add_table[lookup]; \
    Q = F; \
}

#define BIT(bit, value) \
{ \
    F = (F & FLAG_C) | FLAG_H | ((value) & (FLAG_3 | FLAG_5)); \
    if (!((value) & (0x01 << (bit)))) F |= FLAG_P | FLAG_Z; \
    if ((bit) == 7 && ((value) & 0x80)) F |= FLAG_S; \
    Q = F; \
}

#define BIT_MEMPTR(bit, value) \
{ \
    F = (F & FLAG_C) | FLAG_H | (z80.memptr.b.h & (FLAG_3 | FLAG_5)); \
    if (!((value) & (0x01 << (bit)))) F |= FLAG_P | FLAG_Z; \
    if ((bit) == 7 && ((value) & 0x80)) F |= FLAG_S; \
    Q = F; \
}

#define CALL() \
{ \
    contend_read_no_mreq(PC, 1); PC++; \
    PUSH16(PCL, PCH); \
    PC = z80.memptr.w; \
}

#define CP(value) \
{ \
    uint16_t cptemp = A - (value); \
    uint8_t lookup = ((A & 0x88) >> 3) | (((value) & 0x88) >> 2) | ((cptemp & 0x88) >> 1); \
    F = (cptemp & 0x100 ? FLAG_C : (cptemp ? 0 : FLAG_Z)) | FLAG_N | halfcarry_sub_table[lookup & 0x07] | overflow_sub_table[lookup >> 4] | ((value) & (FLAG_3 | FLAG_5)) | (cptemp & FLAG_S); \
    Q = F; \
}

#define DEC(value) \
{ \
    F = (F & FLAG_C) | ((value) & 0x0f ? 0 : FLAG_H) | FLAG_N; \
    (value)--; \
    F |= ((value) == 0x7f ? FLAG_V : 0) | sz53_table[value]; \
    Q = F; \
}

#define Z80_IN(reg, port) \
{ \
    z80.memptr.w = (port) + 1; \
    (reg) = readport(port); \
    F = (F & FLAG_C) | sz53p_table[(reg)]; \
    Q = F; \
}

#define INC(value) \
{ \
    (value)++; \
    F = (F & FLAG_C) | ((value) == 0x80 ? FLAG_V : 0) | ((value) & 0x0f ? 0 : FLAG_H) | sz53_table[(value)]; \
    Q = F; \
}

#define LD16_NNRR(regl, regh) \
{ \
    uint16_t ldtemp = readbyte(PC++); \
    ldtemp |= readbyte(PC++) << 8; \
    writebyte(ldtemp++, (regl)); \
    z80.memptr.w = ldtemp; \
    writebyte(ldtemp, (regh)); \
    break; \
}

#define LD16_RRNN(regl, regh) \
{ \
    uint16_t ldtemp = readbyte(PC++); \
    ldtemp |= readbyte(PC++) << 8; \
    (regl) = readbyte(ldtemp++); \
    z80.memptr.w = ldtemp; \
    (regh) = readbyte(ldtemp); \
    break; \
}

#define JP() \
{ \
    PC = z80.memptr.w; \
}

#define JR() \
{ \
    int8_t jrtemp = static_cast<int8_t>(readbyte(PC)); \
    contend_read_no_mreq(PC, 1); contend_read_no_mreq(PC, 1); \
    contend_read_no_mreq(PC, 1); contend_read_no_mreq(PC, 1); \
    contend_read_no_mreq(PC, 1); \
    PC += jrtemp; \
    PC++; \
    z80.memptr.w = PC; \
}

#define OR(value) \
{ \
    A |= (value); \
    F = sz53p_table[A]; \
    Q = F; \
}

#define POP16(regl, regh) \
{ \
    (regl) = readbyte(SP++); \
    (regh) = readbyte(SP++); \
}

#define PUSH16(regl, regh) \
{ \
    writebyte(--SP, (regh)); \
    writebyte(--SP, (regl)); \
}

#define RET() \
{ \
    POP16(PCL, PCH); \
    z80.memptr.w = PC; \
}

#define RL(value) \
{ \
    uint8_t rltemp = (value); \
    (value) = ((value) << 1) | (F & FLAG_C); \
    F = (rltemp >> 7) | sz53p_table[(value)]; \
    Q = F; \
}

#define RLC(value) \
{ \
    (value) = ((value) << 1) | ((value) >> 7); \
    F = ((value) & FLAG_C) | sz53p_table[(value)]; \
    Q = F; \
}

#define RR(value) \
{ \
    uint8_t rrtemp = (value); \
    (value) = ((value) >> 1) | (F << 7); \
    F = (rrtemp & FLAG_C) | sz53p_table[(value)]; \
    Q = F; \
}

#define RRC(value) \
{ \
    F = (value) & FLAG_C; \
    (value) = ((value) >> 1) | ((value) << 7); \
    F |= sz53p_table[(value)]; \
    Q = F; \
}

#define RST(value) \
{ \
    PUSH16(PCL, PCH); \
    PC = (value); \
    z80.memptr.w = PC; \
}

#define SBC(value) \
{ \
    uint16_t sbctemp = A - (value) - (F & FLAG_C); \
    uint8_t lookup = ((A & 0x88) >> 3) | (((value) & 0x88) >> 2) | ((sbctemp & 0x88) >> 1); \
    A = static_cast<uint8_t>(sbctemp); \
    F = (sbctemp & 0x100 ? FLAG_C : 0) | FLAG_N | halfcarry_sub_table[lookup & 0x07] | overflow_sub_table[lookup >> 4] | sz53_table[A]; \
    Q = F; \
}

#define SBC16(value) \
{ \
    uint32_t sub16temp = HL - (value) - (F & FLAG_C); \
    uint8_t lookup = ((HL & 0x8800) >> 11) | (((value) & 0x8800) >> 10) | ((sub16temp & 0x8800) >> 9); \
    z80.memptr.w = HL + 1; \
    HL = static_cast<uint16_t>(sub16temp); \
    F = (sub16temp & 0x10000 ? FLAG_C : 0) | FLAG_N | overflow_sub_table[lookup >> 4] | (H & (FLAG_3 | FLAG_5 | FLAG_S)) | halfcarry_sub_table[lookup & 0x07] | (HL ? 0 : FLAG_Z); \
    Q = F; \
}

#define SLA(value) \
{ \
    F = (value) >> 7; \
    (value) <<= 1; \
    F |= sz53p_table[(value)]; \
    Q = F; \
}

#define SLL(value) \
{ \
    F = (value) >> 7; \
    (value) = ((value) << 1) | 0x01; \
    F |= sz53p_table[(value)]; \
    Q = F; \
}

#define SRA(value) \
{ \
    F = (value) & FLAG_C; \
    (value) = ((value) & 0x80) | ((value) >> 1); \
    F |= sz53p_table[(value)]; \
    Q = F; \
}

#define SRL(value) \
{ \
    F = (value) & FLAG_C; \
    (value) >>= 1; \
    F |= sz53p_table[(value)]; \
    Q = F; \
}

#define SUB(value) \
{ \
    uint16_t subtemp = A - (value); \
    uint8_t lookup = ((A & 0x88) >> 3) | (((value) & 0x88) >> 2) | ((subtemp & 0x88) >> 1); \
    A = static_cast<uint8_t>(subtemp); \
    F = (subtemp & 0x100 ? FLAG_C : 0) | FLAG_N | halfcarry_sub_table[lookup & 0x07] | overflow_sub_table[lookup >> 4] | sz53_table[A]; \
    Q = F; \
}

#define XOR(value) \
{ \
    A ^= (value); \
    F = sz53p_table[A]; \
    Q = F; \
}

#define HAVE_ENOUGH_MEMORY 1
#define RZX_FETCH(delta) do { if constexpr (TrackFetches) m_trackedFetches += (delta); } while (false)

    const uint16_t fetchPc = PC;
    contend_read(PC, 4);
    if ((m_activeHooksMask & HookFlags::EvenM1) && (m_clock.frameTstates() & 1)) [[unlikely]] {
        m_clock.advance(1);
    }
    uint8_t opcode = readbyte_internal(PC);
    if (m_activeHooksMask & HookFlags::PostM1) [[unlikely]] {
        for (const auto &h : m_postM1Hooks)
            h.fn(fetchPc, opcode);
    }
    // R's bit 7 reaches the address bus in this M1's refresh, after the opcode was fetched:
    // a change by LD R,A shows up one instruction later (the CoBra's configurator: the
    // opcode after LD R,A, e.g. JP (HL), still comes from the old memory map)
    if ((m_activeHooksMask & HookFlags::RfshEdge) && m_rfshHook && fetchPc != 0) [[unlikely]] {
        const uint8_t curR7 = m_regs.R7 & 0x80;
        if (curR7 != m_lastR7) {
            m_lastR7 = curR7;
            m_rfshHook(curR7);
        }
    }
    uint8_t last_Q = 0;

end_opcode:
    PC++;
    R++;
    RZX_FETCH(1);
    last_Q = Q;
    Q = 0;

    switch (opcode) {
#include "opcodes/opcodes_base.inc"
    }

#undef RZX_FETCH
#undef HAVE_ENOUGH_MEMORY
#undef XOR
#undef SUB
#undef SRL
#undef SRA
#undef SLL
#undef SLA
#undef SBC16
#undef SBC
#undef RST
#undef RRC
#undef RR
#undef RLC
#undef RL
#undef RET
#undef PUSH16
#undef POP16
#undef OR
#undef JR
#undef JP
#undef LD16_RRNN
#undef LD16_NNRR
#undef INC
#undef Z80_IN
#undef DEC
#undef CP
#undef CALL
#undef BIT_MEMPTR
#undef BIT
#undef ADD16
#undef ADD
#undef ADC16
#undef ADC
#undef AND
#undef contend_write_no_mreq
#undef contend_read_no_mreq
#undef contend_read
#undef writeport
#undef readport
#undef writebyte
#undef readbyte_internal
#undef readbyte
#undef tstates
#undef IR
#undef IS_CMOS
#undef slt_trap

    m_regs.R &= 0x7F;
    m_regs.lastQ = last_Q;

    return static_cast<uint32_t>(m_clock.totalTstates() - startTs);
}

template uint32_t Z80Cpu::stepInstructionImpl<false>();
template uint32_t Z80Cpu::stepInstructionImpl<true>();

void Z80Cpu::acceptPlaybackInterrupt(bool retrigger, bool inhibit)
{
    // Playback boundaries supply INT; automatic pulses/retriggers are suppressed.
    // Long frames follow the RZX forced-INT convention; short ones retain EI delay.
    if (retrigger && inhibit) { m_intPending = false; return; }
    if (!retrigger)
        m_interruptsEnabledAt = -1;
    requestInterrupt();
    serviceInterrupt();
    m_intPending = false;
}

bool Z80Cpu::runTrackedUntil(uint32_t targetFrameTstates, uint32_t fetchLimit, bool playback)
{
    m_trapped = false;
    m_playbackExecution = playback;
    while (m_clock.frameBaseTstates() < targetFrameTstates && m_trackedFetches < fetchLimit && !m_trapped) {
        m_trackedInterrupt = false;
        stepInstructionImpl<true>();
        if (m_trackedInterrupt)
            return true;
    }
    return false;
}

void Z80Cpu::runUntil(uint32_t targetFrameTstates)
{
    m_trapped = false;
    while (m_clock.frameBaseTstates() < targetFrameTstates && !m_trapped) {
        stepInstructionImpl<false>();
    }
}

void Z80Cpu::saveState(StateWriter &w) const
{
    m_regs.saveState(w);
    w.write(m_interruptsEnabledAt);
    w.write(m_intRequestedAt);
    w.write(m_lastR7);
    w.writeBool(m_iff2Read);
    w.writeBool(m_nmiPending);
    w.writeBool(m_nmiStackless);
    w.writeBool(m_intPending);
    w.write(m_intVector);
    w.writeBool(m_stacklessRetnActive);
    w.write(m_stacklessRetnAddress);
    w.writeBool(m_z80nEnabled);
    w.writeBool(m_isCmos);
}

void Z80Cpu::loadState(StateReader &r)
{
    m_regs.loadState(r);
    m_interruptsEnabledAt  = r.read<int64_t>();
    m_intRequestedAt       = r.read<uint64_t>();
    m_lastR7               = r.read<uint8_t>();
    m_iff2Read             = r.readBool();
    m_nmiPending           = r.readBool();
    m_nmiStackless         = r.readBool();
    m_intPending           = r.readBool();
    m_intVector            = r.read<uint8_t>();
    m_stacklessRetnActive  = r.readBool();
    m_stacklessRetnAddress = r.read<uint16_t>();
    m_z80nEnabled          = r.readBool();
    m_isCmos               = r.readBool();
}

} // namespace zxkit
