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

#include "zxkit/machines/CloneMachines.h"

#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/machines/SinclairMachines.h"

namespace zxkit {

TimexMachine::TimexMachine(Variant variant) noexcept
    : m_variant(variant)
{
}

std::string_view TimexMachine::id() const noexcept
{
    switch (m_variant) {
    case Variant::Tc2048: return "tc2048";
    case Variant::Tc2068: return "tc2068";
    case Variant::Ts2068: return "ts2068";
    case Variant::SpecSe: return "se";
    }
    return "tc2048";
}

std::string_view TimexMachine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Tc2048: return "Timex TC2048";
    case Variant::Tc2068: return "Timex TC2068";
    case Variant::Ts2068: return "Timex TS2068";
    case Variant::SpecSe: return "ZX Spectrum SE";
    }
    return "Timex TC2048";
}

FrameTimings TimexMachine::timings() const noexcept
{
    return (m_variant == Variant::Ts2068) ? kTimings48kNtsc : kTimings48k;
}

void TimexMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    const std::size_t romPages = (m_variant == Variant::Tc2048) ? 1 : 2;
    const std::size_t ramPages = (m_variant == Variant::SpecSe) ? 9 : 3;
    m_romBank   = ctx.mmu.registerBank("rom", romPages * 16 * 1024, false, false);
    m_ramBank   = ctx.mmu.registerBank("ram", ramPages * 16 * 1024, true, true);
    m_dockBank  = ctx.mmu.registerBank("dock", 64 * 1024, true, false);
    m_exromBank = ctx.mmu.registerBank("exrom", 64 * 1024, false, false);

    registerStandardUlaPort(ctx, ula);
    if (m_variant != Variant::Tc2048)
        registerStandardAyPorts(ctx);

    // SCLD mode port 0xFF
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_scld_ff",
        0x00FF,
        0x00FF,
        [&ula](uint16_t) -> uint8_t { return ula.scldRegister(); },
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            ula.setScldRegister(val);
            updateMemoryMap(ctx);
        });

    // Horizontal MMU port 0xF4 (8K chunk bitmask selecting Home vs Dock/ExRom)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_scld_f4",
        0x00FF,
        0x00F4,
        [this](uint16_t) -> uint8_t { return m_portF4; },
        [this, &ctx](uint16_t, uint8_t val) {
            m_portF4 = val;
            updateMemoryMap(ctx);
        });
}

void TimexMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            if (m_variant == Variant::Tc2048) {
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("tc2048.rom", 16 * 1024));
            } else if (m_variant == Variant::SpecSe) {
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("se-0.rom", 16 * 1024));
                ctx.mmu.bank(m_romBank).loadData(16 * 1024, ctx.loadRom("se-1.rom", 16 * 1024));
            } else {
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("tc2068-0.rom", 8 * 1024));
                ctx.mmu.bank(m_exromBank).loadData(0, ctx.loadRom("tc2068-1.rom", 8 * 1024));
            }
        }
    }
    m_portF4 = 0;
    ula.setScldRegister(0);
    updateMemoryMap(ctx);
    ula.setVramSource(m_ramBank, 0);
}

void TimexMachine::updateMemoryMap(MachineContext &ctx)
{
    ctx.mmu.map16k(0x0000, m_romBank, 0, false);
    ctx.mmu.map16k(0x4000, m_ramBank, 0, true);
    ctx.mmu.map16k(0x8000, m_ramBank, 1, false);
    ctx.mmu.map16k(0xC000, m_ramBank, 2, false);

    if (m_portF4 != 0) {
        for (int slot8k = 0; slot8k < 8; ++slot8k) {
            if (m_portF4 & (1u << slot8k)) {
                ctx.mmu.map8k(static_cast<uint16_t>(slot8k * 0x2000), m_dockBank, slot8k, false);
            }
        }
    }
    ctx.romcs.refresh();
}

PentagonMachine::PentagonMachine(Variant variant) noexcept
    : m_variant(variant)
{
}

std::string_view PentagonMachine::id() const noexcept
{
    switch (m_variant) {
    case Variant::Pentagon512:  return "pentagon512";
    case Variant::Pentagon1024: return "pentagon1024";
    default:                    return "pentagon";
    }
}

std::string_view PentagonMachine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Pentagon512:  return "Pentagon 512K";
    case Variant::Pentagon1024: return "Pentagon 1024SL";
    default:                    return "Pentagon 128K";
    }
}

FrameTimings PentagonMachine::timings() const noexcept
{
    return kTimingsPentagon;
}

ContentionPattern PentagonMachine::contentionPattern() const noexcept
{
    return ContentionPattern::None;
}

bool PentagonMachine::hasFloatingBus() const noexcept
{
    return false;
}

void PentagonMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    const std::size_t ramPages = (m_variant == Variant::Pentagon1024) ? 64 :
                                 (m_variant == Variant::Pentagon512)  ? 32 : 8;
    m_romBank = ctx.mmu.registerBank("rom", 4 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", ramPages * 16 * 1024, true, true);

    registerStandardUlaPort(ctx, ula);
    registerStandardAyPorts(ctx);

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_7ffd",
        0x8002,
        0x0000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_port7ffd = val;
            if (m_variant != Variant::Pentagon1024 || !(m_portEff7 & 0x04))
                m_locked = (val & 0x20) != 0;
            updateMemoryMap(ctx, ula);
        });

    if (m_variant == Variant::Pentagon1024) {
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_eff7",
            0xF008,
            0xE000,
            nullptr,
            [this, &ctx, &ula](uint16_t, uint8_t val) {
                m_portEff7 = val;
                updateMemoryMap(ctx, ula);
            });
    }

    // The Beta 128: TR-DOS is ROM 2. Like in FUSE, it pages in and out whatever ROM is selected
    // (the 128K's interface needs the 48K BASIC one), and the Pentagon starts in the 128 menu
    // (FUSE marks the Beta active at reset but doesn't map its ROM: TR-DOS's own reset code, a
    // 48K style RAM test, would take 3 seconds more)
    m_beta = std::make_unique<Beta128Peripheral>(Beta128Options{
        .machineRomBank = m_romBank,
        .machineRomPage = 2,
    });
    m_beta->onAttach(ctx);
}

void PentagonMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("128p-0.rom", 16 * 1024));
            ctx.mmu.bank(m_romBank).loadData(16 * 1024, ctx.loadRom("128p-1.rom", 16 * 1024));
            ctx.mmu.bank(m_romBank).loadData(2 * 16 * 1024, ctx.loadRom("trdos.rom", 16 * 1024));
            if (m_variant == Variant::Pentagon512 || m_variant == Variant::Pentagon1024)
                ctx.mmu.bank(m_romBank).loadData(3 * 16 * 1024, ctx.loadRom("gluck.rom", 16 * 1024));
        }
    }
    m_port7ffd = 0;
    m_portEff7 = 0;
    m_locked = false;
    m_beta->reset(ctx, kind);
    updateMemoryMap(ctx, ula);
}

uint8_t PentagonMachine::lastPort7ffd() const noexcept
{
    return m_port7ffd;
}

void PentagonMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    const uint8_t romPage = (m_port7ffd & 0x10) ? 1 : 0;
    uint8_t ramPage = m_port7ffd & 0x07;
    if (m_variant == Variant::Pentagon512) {
        ramPage |= static_cast<uint8_t>((m_port7ffd & 0xC0) >> 3);
    } else if (m_variant == Variant::Pentagon1024 && !(m_portEff7 & 0x04)) {
        ramPage |= static_cast<uint8_t>(((m_port7ffd & 0xC0) >> 3) | (m_port7ffd & 0x20));
    }

    if (m_variant == Variant::Pentagon1024 && (m_portEff7 & 0x08))
        ctx.mmu.map16k(0x0000, m_ramBank, 0, false);
    else
        ctx.mmu.map16k(0x0000, m_romBank, romPage, false);

    ctx.mmu.map16k(0x4000, m_ramBank, 5, false);
    ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
    ctx.mmu.map16k(0xC000, m_ramBank, ramPage, false);
    ctx.romcs.refresh();
    ula.setVramSource(m_ramBank, (m_port7ffd & 0x08) ? 7 : 5);
}

std::string_view ScorpionMachine::id() const noexcept
{
    return "scorpion";
}

std::string_view ScorpionMachine::name() const noexcept
{
    return "Scorpion ZS 256";
}

FrameTimings ScorpionMachine::timings() const noexcept
{
    return kTimingsScorpion;
}

ContentionPattern ScorpionMachine::contentionPattern() const noexcept
{
    return ContentionPattern::None;
}

bool ScorpionMachine::hasFloatingBus() const noexcept
{
    return false;
}

void ScorpionMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    m_romBank = ctx.mmu.registerBank("rom", 4 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 16 * 16 * 1024, true, true);

    ctx.cpu.setEvenM1(true);
    registerStandardUlaPort(ctx, ula);
    registerStandardAyPorts(ctx);

    ctx.ioBus.registerSimpleHandler(
        "scorpion_7ffd",
        0xC002,
        0x4000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_port7ffd = val;
            m_locked = (val & 0x20) != 0;
            updateMemoryMap(ctx, ula);
        });

    ctx.ioBus.registerSimpleHandler(
        "scorpion_1ffd",
        0xF002,
        0x1000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_port1ffd = val;
            updateMemoryMap(ctx, ula);
        });

    // The Beta 128: TR-DOS is ROM 3; it pages in and out whatever ROM is selected (FUSE)
    m_beta = std::make_unique<Beta128Peripheral>(Beta128Options{
        .machineRomBank = m_romBank,
        .machineRomPage = 3,
    });
    m_beta->onAttach(ctx);
}

void ScorpionMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    m_beta->reset(ctx, kind);
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            for (int i = 0; i < 4; ++i) {
                ctx.mmu.bank(m_romBank).loadData(static_cast<std::size_t>(i) * 16 * 1024,
                                                 ctx.loadRom(std::format("256s-{}.rom", i), 16 * 1024));
            }
        }
    }
    m_port7ffd = 0;
    m_port1ffd = 0;
    m_locked = false;
    updateMemoryMap(ctx, ula);
}

void ScorpionMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    const uint8_t ramPage = static_cast<uint8_t>((m_port7ffd & 0x07) | ((m_port1ffd & 0x10) >> 1));
    if (m_port1ffd & 0x01) {
        ctx.mmu.map16k(0x0000, m_ramBank, 0, false);
    } else {
        const uint8_t romPage = (m_port1ffd & 0x02) ? 2 : ((m_port7ffd & 0x10) ? 1 : 0);
        ctx.mmu.map16k(0x0000, m_romBank, romPage, false);
    }
    ctx.mmu.map16k(0x4000, m_ramBank, 5, false);
    ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
    ctx.mmu.map16k(0xC000, m_ramBank, ramPage, false);
    ctx.romcs.refresh();
    ula.setVramSource(m_ramBank, (m_port7ffd & 0x08) ? 7 : 5);
}

} // namespace zxkit
