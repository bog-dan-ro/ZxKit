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

#include "zxkit/machines/RomanianMachines.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/machines/SinclairMachines.h"

#include <format>

namespace zxkit {

std::string_view HcMachine::id() const noexcept
{
    return "hc2000";
}

std::string_view HcMachine::name() const noexcept
{
    return "ICE Felix HC-2000";
}

FrameTimings HcMachine::timings() const noexcept
{
    return kTimings48k;
}

ContentionPattern HcMachine::contentionPattern() const noexcept
{
    return ContentionPattern::None;
}

bool HcMachine::hasFloatingBus() const noexcept
{
    // Unattached ports must read 0xFF, otherwise CP/M does not boot
    return false;
}

void HcMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    // 2 x 16KB ROMs (0 = BASIC, 1 = CP/M) and 4 x 16KB RAM pages (64KB total)
    m_romBank = ctx.mmu.registerBank("rom", 2 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 4 * 16 * 1024, true, true);
    if (ctx.romResolver) {
        ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("hc2000-basic.rom", 16 * 1024));
        ctx.mmu.bank(m_romBank).loadData(16 * 1024, ctx.loadRom("hc2000-cpm.rom", 16 * 1024));
    }

    registerStandardUlaPort(ctx, ula);
    ctx.audioMixer.setAyChipCount(0);

    // System configuration port 0x7E
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_cfg_7e",
        0x0081,
        0x0000,
        [this](uint16_t) -> uint8_t { return m_config7e; },
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_config7e = val;
            m_locked = (val & 0x04) != 0;
            updateMemoryMap(ctx, ula);
        });

    // CPM signal: set by writing port 0xC7, cleared by 0xC5
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_cpm_c5c7",
        0x00FD,
        0x00C5,
        nullptr,
        [this, &ctx, &ula](uint16_t port, uint8_t) {
            m_cpmA13PullHigh = ((port & 0xFF) == 0xC7);
            updateMemoryMap(ctx, ula);
        });

    m_fdc.onAttach(ctx);
}

void HcMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
    }
    m_config7e = 0x00;
    m_locked = false;
    m_cpmA13PullHigh = false;
    m_fdc.reset(ctx, kind);
    updateMemoryMap(ctx, ula);
}

std::size_t HcMachine::diskDriveCount() const noexcept
{
    return m_fdc.diskDriveCount();
}

OptionalRef<DiskImage> HcMachine::diskDrive(std::size_t idx) noexcept
{
    return m_fdc.diskDrive(idx);
}

uint8_t HcMachine::configPort7e() const noexcept
{
    return m_config7e;
}

bool HcMachine::isCpmA13Active() const noexcept
{
    return m_cpmA13PullHigh;
}

void HcMachine::saveState(StateWriter &w) const
{
    w.write(m_config7e);
    w.writeBool(m_locked);
    w.writeBool(m_cpmA13PullHigh);
}

void HcMachine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_config7e       = r.read<uint8_t>();
    m_locked         = r.readBool();
    m_cpmA13PullHigh = r.readBool();
    updateMemoryMap(ctx, ula);
}

void HcMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    const uint8_t romIdx = (m_config7e & 0x01) ? 1 : 0;
    const bool cpmMode   = (m_config7e & 0x02) != 0;
    const bool vramAtC0  = (m_config7e & 0x08) != 0;

    if (!cpmMode) {
        ctx.mmu.map16k(0x0000, m_romBank, romIdx, false);
    } else {
        ctx.mmu.map16k(0x0000, m_ramBank, 0, false);
    }

    ctx.mmu.map16k(0x4000, m_ramBank, 1, false);
    ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
    ctx.mmu.map16k(0xC000, m_ramBank, 3, false);

    // When the CPM signal is active (port 0xC7), accesses to 0xC000..0xDFFF
    // address the upper 8KB of RAM page 3 (8K page index = 3*2 + 1 = 7).
    if (m_cpmA13PullHigh) {
        ctx.mmu.map8k(0xC000, m_ramBank, 3 * 2 + 1, false);
    }

    // In CP/M mode, the upper 8KB of the selected ROM is mapped at 0xE000..0xFFFF.
    if (cpmMode) {
        ctx.mmu.map8k(0xE000, m_romBank, romIdx * 2 + 1, false);
    }

    ctx.romcs.setSuppressed(cpmMode);

    ula.setVramSource(m_ramBank, vramAtC0 ? 3 : 1);
}

CobraMachine::CobraMachine(bool mode80k) noexcept
    : m_mode80k(mode80k)
{
}

std::string_view CobraMachine::id() const noexcept
{
    return m_mode80k ? "cobra1_80k" : "cobra1_64k";
}

std::string_view CobraMachine::name() const noexcept
{
    return m_mode80k ? "CoBra 1 (80KB)" : "CoBra 1 (64KB)";
}

FrameTimings CobraMachine::timings() const noexcept
{
    return kTimings48k;
}

ContentionPattern CobraMachine::contentionPattern() const noexcept
{
    return ContentionPattern::None;
}

bool CobraMachine::hasFloatingBus() const noexcept
{
    return false;
}

void CobraMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    // Boot ROM (16KB) + 4 System ROMs (4 x 16KB) = 5 x 16KB ROM pages
    m_romBank = ctx.mmu.registerBank("rom", 5 * 16 * 1024, false, false);
    // 5 x 16KB RAM pages (80KB total: 0=DRAM0, 1=VRAM, 2=BootRAM, 3=SysRAM, 4=DRAM1)
    m_ramBank = ctx.mmu.registerBank("ram", 5 * 16 * 1024, true, true);
    if (ctx.romResolver) {
        // Page 0: boot ROM, pages 1..4: Opus, BASIC, Devil BASIC and CP/M system ROMs
        ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("cobra1_boot.rom", 16 * 1024));
        for (std::size_t i = 0; i < 4; ++i)
            ctx.mmu.bank(m_romBank).loadData((i + 1) * 16 * 1024, ctx.loadRom(std::format("cobra1_sys-{}.rom", i), 16 * 1024));
    }

    registerStandardUlaPort(ctx, ula, 0x00FF, 0x00FE);
    ctx.audioMixer.setAyChipCount(0);

    // 8255 Control port 0xDF
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_8255_ctrl",
        0x00FF,
        0x00DF,
        [this](uint16_t) -> uint8_t { return m_8255Ctrl; },
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (val & 0x80) {
                m_8255Ctrl = val;
                reset8255PortC();
            } else {
                const uint8_t bit = (val >> 1) & 0x07;
                uint8_t newC = m_8255PortC;
                if (val & 0x01)
                    newC |= static_cast<uint8_t>(1u << bit);
                else
                    newC &= static_cast<uint8_t>(~(1u << bit));
                write8255PortC(newC, ctx, ula);
            }
        });

    // 8255 Port C on 0xFE
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_8255_portc",
        0x00FF,
        0x00FE,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            write8255PortC(val, ctx, ula);
        });

    // R7 refresh-bit flip-flop hook
    ctx.cpu.setRfshHook([this, &ctx, &ula](uint8_t r7Bit) {
        m_po = (r7Bit != 0) ? 1 : 0;
        // On the falling edge of R7 (PO -> 0), latch 8255 Port C bit 6 into LO6
        if (m_po == 0) {
            m_lo6 = (m_8255PortC >> 6) & 1;
        }
        updateMemoryMap(ctx, ula);
    });
}

void CobraMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard)
        ctx.mmu.bank(m_ramBank).fill(0x00);

    m_8255Ctrl = 0x9B;
    reset8255PortC();
    m_po = 1;
    m_lo6 = 1;
    m_locked = false;
    updateMemoryMap(ctx, ula);
}

uint8_t CobraMachine::poFlipFlop() const noexcept
{
    return m_po;
}

uint8_t CobraMachine::lo6FlipFlop() const noexcept
{
    return m_lo6;
}

bool CobraMachine::isSpectrumModeLocked() const noexcept
{
    return m_locked;
}

void CobraMachine::reset8255PortC() noexcept
{
    m_8255PortC = ((m_8255Ctrl & 0x01) ? 0x0F : 0x00) | ((m_8255Ctrl & 0x08) ? 0xF0 : 0x00);
}

void CobraMachine::write8255PortC(uint8_t val, MachineContext &ctx, UlaRenderer &ula)
{
    uint8_t nextC = (m_8255Ctrl & 0x01) ? (m_8255PortC & 0x0F) : (val & 0x0F);
    nextC |= (m_8255Ctrl & 0x08) ? (m_8255PortC & 0xF0) : (val & 0xF0);
    m_8255PortC = nextC;
    updateMemoryMap(ctx, ula);
}

void CobraMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    if (m_locked)
        return;

    const uint8_t o6 = (m_8255PortC >> 6) & 1;
    const uint8_t to = (m_8255PortC >> 3) & 1;
    const uint8_t so = (m_8255PortC >> 7) & 1;
    const uint8_t sysRomPage = static_cast<uint8_t>(1 + ((to << 1) | so)); // 1..4 in m_romBank

    if (m_po == 1 || m_lo6 == 1) {
        if (!m_mode80k) {
            // 64KB model
            if (m_po == 1) {
                ctx.mmu.map16k(0x0000, m_romBank, 0, false);
                ctx.mmu.map16k(0x4000, m_romBank, sysRomPage, false);
            } else {
                ctx.mmu.map16k(0x0000, m_ramBank, 2, false);
                ctx.mmu.map16k(0x4000, m_ramBank, 3, false);
            }
            ctx.mmu.map8k(0x8000, m_ramBank, 0, false);
            ctx.mmu.map8k(0xA000, m_ramBank, 3, false);
            ctx.mmu.map8k(0xC000, m_ramBank, 2, false);
            ctx.mmu.map8k(0xE000, m_ramBank, 1, false);
            ula.setVramSource(m_ramBank, 1);
        } else {
            // 80KB model
            if (m_po == 1) {
                ctx.mmu.map16k(0x0000, m_romBank, 0, false);
                ctx.mmu.map16k(0x4000, m_romBank, sysRomPage, false);
                ctx.mmu.map16k(0x8000, m_ramBank, 0, false);
                ctx.mmu.map16k(0xC000, m_ramBank, o6 ? 1 : 4, false);
                ula.setVramSource(m_ramBank, 1);
            } else {
                ctx.mmu.map16k(0x0000, m_ramBank, 0, false);
                ctx.mmu.map16k(0x4000, m_ramBank, o6 ? 1 : 4, false);
                ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
                ctx.mmu.map16k(0xC000, m_ramBank, 3, false);
                ula.setVramSource(m_ramBank, o6 ? 1 : 4);
            }
        }
    } else {
        // PO == 0 && LO6 == 0: Spectrum 48K compatibility mode (16K R/O DRAM#0 at 0x0000, locked)
        m_locked = true;
        ctx.mmu.map16k(0x0000, m_ramBank, 0, false, /*forceReadOnly=*/true);
        ctx.mmu.map16k(0x4000, m_ramBank, 1, false);
        ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
        ctx.mmu.map16k(0xC000, m_ramBank, 3, false);
        ula.setVramSource(m_ramBank, 1);
    }
}

std::string_view Cobra2Machine::id() const noexcept
{
    return "cobra2";
}

std::string_view Cobra2Machine::name() const noexcept
{
    return "CoBra 2 (TS)";
}

FrameTimings Cobra2Machine::timings() const noexcept
{
    return kTimings48k;
}

ContentionPattern Cobra2Machine::contentionPattern() const noexcept
{
    return ContentionPattern::None;
}

bool Cobra2Machine::hasFloatingBus() const noexcept
{
    return false;
}

void Cobra2Machine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    m_romBank = ctx.mmu.registerBank("rom", 4 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 4 * 16 * 1024, true, true);
    if (ctx.romResolver) {
        for (std::size_t i = 0; i < 4; ++i)
            ctx.mmu.bank(m_romBank).loadData(i * 16 * 1024, ctx.loadRom(std::format("cobra2_TS_4_OS-{}.rom", i), 16 * 1024));
    }

    // Port 0xFE is fully decoded (no ULA chip); it also stores the F5 register
    registerStandardUlaPort(ctx, ula, 0x00FF, 0x00FE);
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_f5",
        0x00FF,
        0x00FE,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            // Q3 / Q7 drive A15 / A14 of the EPROM directly, so the ROM bank follows at once
            const bool romChanged = ((m_portFe ^ val) & 0x88) != 0;
            m_portFe = val;
            if (romChanged)
                updateMemoryMap(ctx, ula);
        });

    // Reading port 0x7E clocks the nCE / SM flip-flops
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_cfg_7e",
        0x00FF,
        0x007E,
        [this, &ctx, &ula](uint16_t) -> uint8_t {
            m_latched = m_portFe;
            updateMemoryMap(ctx, ula);
            return 0xFF;
        },
        nullptr);

    // Timex style AY: register select 0xF5, data 0xF6
    ctx.audioMixer.setAyChipCount(1);
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_reg",
        0x00FF,
        0x00F5,
        [&ctx](uint16_t) -> uint8_t { return ctx.audioMixer.activeAy().readRegister(); },
        [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.activeAy().selectRegister(val); });
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_data",
        0x00FF,
        0x00F6,
        nullptr,
        [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.writeAyRegister(ctx.clock.frameBaseTstates(), val); });
}

void Cobra2Machine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard)
        ctx.mmu.bank(m_ramBank).fill(0x00);
    // nCE = 1, SM = 0 after reset would be the 64K mode; the TS boots with all bits clear
    m_portFe = 0;
    m_latched = 0;
    // Start from the boot map (ROM 0 at 0x0000) with every page mapped
    ctx.mmu.map16k(0x4000, m_ramBank, 3, false);
    ctx.mmu.map16k(0x8000, m_ramBank, 0, false);
    ctx.mmu.map16k(0xC000, m_ramBank, 1, false);
    updateMemoryMap(ctx, ula);
}

void Cobra2Machine::saveState(StateWriter &w) const
{
    w.write(m_portFe);
    w.write(m_latched);
}

void Cobra2Machine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_portFe = r.read<uint8_t>();
    m_latched = r.read<uint8_t>();
    updateMemoryMap(ctx, ula);
}

void Cobra2Machine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    const bool nCe = (m_latched & 0x20) != 0;
    const bool sm  = (m_latched & 0x40) != 0;
    // Q3 drives A15 and Q7 A14 of the system EPROM; the ROM select follows F5 directly
    const std::size_t romIdx = static_cast<std::size_t>(((m_portFe & 0x08) ? 2 : 0) | ((m_portFe & 0x80) ? 1 : 0));

    if (!nCe && !sm) {
        // EPROM boot mode
        ctx.mmu.map16k(0x0000, m_romBank, romIdx, false);
        ctx.mmu.map16k(0x4000, m_ramBank, 3, false);
        ctx.mmu.map16k(0x8000, m_ramBank, 0, false);
        ctx.mmu.map16k(0xC000, m_ramBank, 1, false);
    } else if (nCe && sm) {
        // Spectrum mode: RAM page 0 is read-only "ROM"
        ctx.mmu.map16k(0x0000, m_ramBank, 0, false, /*forceReadOnly=*/true);
        ctx.mmu.map16k(0x4000, m_ramBank, 1, false);
        ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
        ctx.mmu.map16k(0xC000, m_ramBank, 3, false);
    } else if (nCe && !sm) {
        // 64KB DRAM mode, transitory between boot and Spectrum mode (and CP/M)
        ctx.mmu.map16k(0x0000, m_ramBank, 2, false);
    }
    ula.setVramSource(m_ramBank, 1);
}

} // namespace zxkit
