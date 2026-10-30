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

#include "zxkit/machines/SinclairMachines.h"

#include "zxkit/audio/AudioMixer.h"

#include <array>
#include <format>

namespace zxkit {

Spectrum48kMachine::Spectrum48kMachine(Variant variant) noexcept
    : m_variant(variant)
{
}

std::string_view Spectrum48kMachine::id() const noexcept
{
    switch (m_variant) {
    case Variant::Spec16k:     return "16k";
    case Variant::Spec48kNtsc: return "48k_ntsc";
    default:                   return "48k";
    }
}

std::string_view Spectrum48kMachine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Spec16k:     return "ZX Spectrum 16K";
    case Variant::Spec48kNtsc: return "ZX Spectrum 48K (NTSC)";
    default:                   return "ZX Spectrum 48K";
    }
}

FrameTimings Spectrum48kMachine::timings() const noexcept
{
    return (m_variant == Variant::Spec48kNtsc) ? kTimings48kNtsc : kTimings48k;
}

void Spectrum48kMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    m_romBank = ctx.mmu.registerBank("rom", 16 * 1024, false, false);
    const std::size_t ramPages16k = (m_variant == Variant::Spec16k) ? 1 : 3;
    m_ramBank = ctx.mmu.registerBank("ram", ramPages16k * 16 * 1024, true, true);
    registerStandardUlaPort(ctx, ula);
    ctx.audioMixer.setAyChipCount(0);
}

void Spectrum48kMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            auto romData = ctx.loadRom("48.rom", 16 * 1024);
            ctx.mmu.bank(m_romBank).loadData(0, romData);
        }
    }
    ctx.mmu.resetMappings();
    ctx.mmu.map16k(0x0000, m_romBank, 0, false);
    ctx.mmu.map16k(0x4000, m_ramBank, 0, true);
    if (m_variant != Variant::Spec16k) {
        ctx.mmu.map16k(0x8000, m_ramBank, 1, false);
        ctx.mmu.map16k(0xC000, m_ramBank, 2, false);
    }
    ula.setVramSource(m_ramBank, 0);
}

Spectrum128kMachine::Spectrum128kMachine(bool isPlus2) noexcept
    : m_isPlus2(isPlus2)
{
}

std::string_view Spectrum128kMachine::id() const noexcept
{
    return m_isPlus2 ? "plus2" : "128k";
}

std::string_view Spectrum128kMachine::name() const noexcept
{
    return m_isPlus2 ? "ZX Spectrum +2" : "ZX Spectrum 128K";
}

FrameTimings Spectrum128kMachine::timings() const noexcept
{
    return kTimings128k;
}

void Spectrum128kMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    m_romBank = ctx.mmu.registerBank("rom", 2 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 8 * 16 * 1024, true, true);

    registerStandardUlaPort(ctx, ula);
    registerStandardAyPorts(ctx);

    // 128K paging port 0x7FFD (mask 0x8002 == 0x0000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_7ffd",
        0x8002,
        0x0000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_port7ffd = val;
            m_locked = (val & 0x20) != 0;
            updateMemoryMap(ctx, ula);
        });
}

void Spectrum128kMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            const std::string_view rom0 = m_isPlus2 ? "plus2-0.rom" : "128-0.rom";
            const std::string_view rom1 = m_isPlus2 ? "plus2-1.rom" : "128-1.rom";
            ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom(rom0, 16 * 1024));
            ctx.mmu.bank(m_romBank).loadData(16 * 1024, ctx.loadRom(rom1, 16 * 1024));
        }
    }
    m_port7ffd = 0;
    m_locked = false;
    updateMemoryMap(ctx, ula);
}

uint8_t Spectrum128kMachine::lastPort7ffd() const noexcept
{
    return m_port7ffd;
}

void Spectrum128kMachine::setPagingPorts(uint8_t port7ffd, uint8_t /*port1ffd*/, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = port7ffd;
    m_locked = (port7ffd & 0x20) != 0;
    updateMemoryMap(ctx, ula);
}

void Spectrum128kMachine::saveState(StateWriter &w) const
{
    w.write(m_port7ffd);
    w.writeBool(m_locked);
}

void Spectrum128kMachine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = r.read<uint8_t>();
    m_locked   = r.readBool();
    updateMemoryMap(ctx, ula);
}

void Spectrum128kMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    const uint8_t romPage = (m_port7ffd & 0x10) ? 1 : 0;
    const uint8_t ramPage = m_port7ffd & 0x07;
    const uint8_t vramPage = (m_port7ffd & 0x08) ? 7 : 5;

    ctx.mmu.map16k(0x0000, m_romBank, romPage, false);
    ctx.mmu.map16k(0x4000, m_ramBank, 5, true);
    ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
    ctx.mmu.map16k(0xC000, m_ramBank, ramPage, (ramPage & 1) != 0);
    ctx.romcs.refresh();
    ula.setVramSource(m_ramBank, vramPage);
}

SpectrumPlus3Machine::SpectrumPlus3Machine(Variant variant) noexcept
    : m_variant(variant)
{
}

std::string_view SpectrumPlus3Machine::id() const noexcept
{
    switch (m_variant) {
    case Variant::Plus2A: return "plus2a";
    case Variant::Plus3e: return "plus3e";
    default:              return "plus3";
    }
}

std::string_view SpectrumPlus3Machine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Plus2A: return "ZX Spectrum +2A";
    case Variant::Plus3e: return "ZX Spectrum +3e";
    default:              return "ZX Spectrum +3";
    }
}

FrameTimings SpectrumPlus3Machine::timings() const noexcept
{
    return kTimings128k;
}

ContentionPattern SpectrumPlus3Machine::contentionPattern() const noexcept
{
    return ContentionPattern::SinclairPlus2A_3;
}

bool SpectrumPlus3Machine::hasFloatingBus() const noexcept
{
    return false;
}

void SpectrumPlus3Machine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    m_romBank = ctx.mmu.registerBank("rom", 4 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 8 * 16 * 1024, true, true);

    registerStandardUlaPort(ctx, ula);
    registerStandardAyPorts(ctx);

    // +2A/+3 port 0x7FFD (mask 0xC002 == 0x4000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_7ffd",
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

    // +2A/+3 extended paging port 0x1FFD (mask 0xF002 == 0x1000); bit 3 is the disk motor
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_1ffd",
        0xF002,
        0x1000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_locked)
                return;
            m_port1ffd = val;
            updateMemoryMap(ctx, ula);
        });

    if (!hasFloppyDrives())
        return;
    // The uPD765: main status on 0x2FFD, data on 0x3FFD. Its US1 line isn't connected:
    // drives 2 and 3 are drives 0 and 1.
    m_fdc.setUnitMask(0x01);
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_fdc_status",
        0xF002,
        0x2000,
        [this, &ctx](uint16_t) -> uint8_t { return m_fdc.readMainStatus(ctx.clock.totalTstates()); },
        nullptr);
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_fdc_data",
        0xF002,
        0x3000,
        [this, &ctx](uint16_t) -> uint8_t { return m_fdc.readData(ctx.clock.totalTstates()); },
        [this, &ctx](uint16_t, uint8_t val) { m_fdc.writeData(val, ctx.clock.totalTstates()); });
}

std::size_t SpectrumPlus3Machine::diskDriveCount() const noexcept
{
    return hasFloppyDrives() ? 2 : 0;
}

OptionalRef<DiskImage> SpectrumPlus3Machine::diskDrive(std::size_t idx) noexcept
{
    if (!hasFloppyDrives() || idx >= 2)
        return std::nullopt;
    return m_fdc.drive(idx);
}

void SpectrumPlus3Machine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (ctx.romResolver) {
            const std::string_view prefix = (m_variant == Variant::Plus3e) ? "plus3e" : "plus3";
            for (int i = 0; i < 4; ++i) {
                const std::string romFile = std::format("{}-{}.rom", prefix, i);
                ctx.mmu.bank(m_romBank).loadData(static_cast<std::size_t>(i) * 16 * 1024,
                                                 ctx.loadRom(romFile, 16 * 1024));
            }
        }
    }
    m_port7ffd = 0;
    m_port1ffd = 0;
    m_locked = false;
    m_fdc.reset();
    updateMemoryMap(ctx, ula);
}

uint8_t SpectrumPlus3Machine::lastPort7ffd() const noexcept
{
    return m_port7ffd;
}

uint8_t SpectrumPlus3Machine::lastPort1ffd() const noexcept
{
    return m_port1ffd;
}

void SpectrumPlus3Machine::setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = port7ffd;
    m_port1ffd = port1ffd;
    m_locked = (port7ffd & 0x20) != 0;
    updateMemoryMap(ctx, ula);
}

void SpectrumPlus3Machine::saveState(StateWriter &w) const
{
    w.write(m_port7ffd);
    w.write(m_port1ffd);
    w.writeBool(m_locked);
}

void SpectrumPlus3Machine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = r.read<uint8_t>();
    m_port1ffd = r.read<uint8_t>();
    m_locked   = r.readBool();
    updateMemoryMap(ctx, ula);
}

void SpectrumPlus3Machine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    if (m_port1ffd & 0x01) {
        // Special all-RAM CP/M configurations (pages >= 4 are contended on +2A/+3)
        static constexpr std::array<std::array<uint8_t, 4>, 4> kSpecialMaps = {{
            {0, 1, 2, 3},
            {4, 5, 6, 7},
            {4, 5, 6, 3},
            {4, 7, 6, 3}
        }};
        const auto &m = kSpecialMaps[(m_port1ffd >> 1) & 0x03];
        for (int slot = 0; slot < 4; ++slot) {
            const uint8_t p = m[slot];
            ctx.mmu.map16k(static_cast<uint16_t>(slot * 0x4000), m_ramBank, p, p >= 4);
        }
        ctx.romcs.setSuppressed(true);
    } else {
        const uint8_t romPage = static_cast<uint8_t>(((m_port1ffd & 0x04) >> 1) | ((m_port7ffd & 0x10) >> 4));
        const uint8_t ramPage = m_port7ffd & 0x07;
        ctx.mmu.map16k(0x0000, m_romBank, romPage, false);
        ctx.mmu.map16k(0x4000, m_ramBank, 5, true);
        ctx.mmu.map16k(0x8000, m_ramBank, 2, false);
        ctx.mmu.map16k(0xC000, m_ramBank, ramPage, ramPage >= 4);
        ctx.romcs.setSuppressed(false);
        ctx.romcs.refresh();
    }
    const uint8_t vramPage = (m_port7ffd & 0x08) ? 7 : 5;
    ula.setVramSource(m_ramBank, vramPage);
}

} // namespace zxkit
