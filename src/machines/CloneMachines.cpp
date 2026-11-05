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

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/input/Joystick.h"
#include "zxkit/machines/SinclairMachines.h"

#include <string_view>

namespace zxkit {

using namespace std::literals;

TimexMachine::TimexMachine(Variant variant) noexcept
    : m_variant(variant)
{
}

std::string_view TimexMachine::id() const noexcept
{
    switch (m_variant) {
    case Variant::Tc2048: return "tc2048"sv;
    case Variant::Tc2068: return "tc2068"sv;
    case Variant::Ts2068: return "ts2068"sv;
    case Variant::SpecSe: return "se"sv;
    }
    return "tc2048"sv;
}

std::string_view TimexMachine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Tc2048: return "Timex TC2048"sv;
    case Variant::Tc2068: return "Timex TC2068"sv;
    case Variant::Ts2068: return "Timex TS2068"sv;
    case Variant::SpecSe: return "ZX Spectrum SE"sv;
    }
    return "Timex TC2048"sv;
}

FrameTimings TimexMachine::timings() const noexcept
{
    switch (m_variant) {
    case Variant::Ts2068: return kTimingsTimex60;
    case Variant::SpecSe: return kTimings48k;
    default:              return kTimingsTimex50;
    }
}

void TimexMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{
    const std::size_t romPages = isSe() ? 2 : 1;
    const std::size_t ramPages = isSe() ? 9 : 3;
    m_romBank = ctx.mmu.registerBank("rom", romPages * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", ramPages * 16 * 1024, true, true);
    if (isSe()) {
        // The SE's DOCK and EXROM are RAM
        m_dockBank = ctx.mmu.registerBank("dock", 64 * 1024, true, true);
        m_exromBank = ctx.mmu.registerBank("exrom", 64 * 1024, true, true);
    } else {
        // An empty DOCK (no cartridge) reads 0xFF; so does the TC2048's EXROM
        m_dockBank = ctx.mmu.registerBank("dock", 8 * 1024, false, false);
        m_exromBank = ctx.mmu.registerBank("exrom", 8 * 1024, false, false);
        ctx.mmu.bank(m_dockBank).fill(0xFF);
        ctx.mmu.bank(m_exromBank).fill(0xFF);
    }

    // The ULA and the AY are fully decoded
    registerStandardUlaPort(ctx, ula, 0x00FF, 0x00FE);
    if (isSe()) {
        ctx.audioMixer.setAyChipCount(1);
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_ay_reg",
            0xFFFF,
            0xFFFD,
            [&ctx](uint16_t) -> uint8_t { return ctx.audioMixer.activeAy().readRegister(); },
            [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.activeAy().selectRegister(val); });
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_ay_data",
            0xFFFF,
            0xBFFD,
            nullptr,
            [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.writeAyRegister(ctx.clock.frameBaseTstates(), val); });
    }
    if (m_variant != Variant::Tc2048) {
        // The Timex AY: register 0xF5, data 0xF6. On the 2068 register 14 is the joystick port:
        // Address bits A8/A9 enable the left/right joystick's active-low lines.
        if (!isSe())
            ctx.audioMixer.setAyChipCount(1);
        const bool joystickPort = !isSe();
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_ay_f5",
            0x00FF,
            0x00F5,
            [&ctx, joystickPort](uint16_t) -> uint8_t {
                const auto &ay = ctx.audioMixer.activeAy();
                return (joystickPort && ay.selectedRegister() == 14) ? 0xFF : ay.readRegister();
            },
            [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.activeAy().selectRegister(val); });
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_ay_f6",
            0x00FF,
            0x00F6,
            joystickPort ? IoBus::SimpleReadFn([this, &ctx](uint16_t port) -> uint8_t {
                const auto &ay = ctx.audioMixer.activeAy();
                if (ay.selectedRegister() != 14)
                    return ay.readRegister();
                if (ay.registers()[7] & 0x40)
                    return ay.registers()[14];
                uint8_t input = 0xFF;
                if (port & 0x0100) input &= m_joysticks[0];
                if (port & 0x0200) input &= m_joysticks[1];
                return input;
            }) : IoBus::SimpleReadFn{},
            [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.writeAyRegister(ctx.clock.frameBaseTstates(), val); });
    }

    // The SCLD's mode register 0xFF
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_scld_ff",
        0x00FF,
        0x00FF,
        [this](uint16_t) -> uint8_t { return m_portFf; },
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            const bool bankChanged = ((m_portFf ^ val) & 0x80) != 0;
            m_portFf = val;
            ula.setScldRegister(val);
            if (bankChanged)
                updateMemoryMap(ctx, ula);
        });

    // The horizontal select 0xF4: a bit for each 8K chunk, the DOCK / EXROM over the home bank
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_scld_f4",
        0x00FF,
        0x00F4,
        [this](uint16_t) -> uint8_t { return m_portF4; },
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            m_portF4 = val;
            updateMemoryMap(ctx, ula);
        });

    if (isSe()) {
        // The 128K paging, fully decoded and without the 48K lock
        ctx.ioBus.registerSimpleHandler(
            std::string(id()) + "_7ffd",
            0xFFFF,
            0x7FFD,
            nullptr,
            [this, &ctx, &ula](uint16_t, uint8_t val) {
                m_port7ffd = val;
                updateMemoryMap(ctx, ula);
            });
    }
}

void TimexMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    m_joysticks.fill(0xFF);
    if (kind == ResetKind::Hard) {
        ctx.mmu.bank(m_ramBank).fill(0x00);
        if (isSe()) {
            ctx.mmu.bank(m_dockBank).fill(0x00);
            ctx.mmu.bank(m_exromBank).fill(0x00);
        }
        if (ctx.romResolver || ctx.romOverrideResolver) {
            switch (m_variant) {
            case Variant::Tc2048:
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("tc2048.rom", 16 * 1024));
                break;
            case Variant::SpecSe:
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("se-0.rom", 16 * 1024));
                ctx.mmu.bank(m_romBank).loadData(16 * 1024, ctx.loadRom("se-1.rom", 16 * 1024));
                break;
            default:
                // The TS2068 has the TC2068's ROMs (FUSE's defaults)
                ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("tc2068-0.rom", 16 * 1024));
                ctx.mmu.bank(m_exromBank).loadData(0, ctx.loadRom("tc2068-1.rom", 8 * 1024));
                break;
            }
        }
    }
    m_portF4 = 0;
    m_portFf = 0;
    m_port7ffd = 0;
    ula.setScldRegister(0);
    updateMemoryMap(ctx, ula);
}

void TimexMachine::onFrameStart(MachineContext &ctx)
{
    // The SCLD's bit 6 disables the frame interrupt
    if (!(m_portFf & 0x40))
        MachineProfile::onFrameStart(ctx);
}

uint8_t TimexMachine::lastPort7ffd() const noexcept
{
    return m_port7ffd;
}

void TimexMachine::setPagingPorts(uint8_t port7ffd, uint8_t /*port1ffd*/, MachineContext &ctx, UlaRenderer &ula)
{
    if (!isSe())
        return;
    m_port7ffd = port7ffd;
    updateMemoryMap(ctx, ula);
}

void TimexMachine::saveState(StateWriter &w) const
{
    w.write(m_portF4);
    w.write(m_portFf);
    w.write(m_port7ffd);
}

void TimexMachine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_joysticks.fill(0xFF);
    m_portF4 = r.read<uint8_t>();
    m_portFf = r.read<uint8_t>();
    m_port7ffd = r.read<uint8_t>();
    updateMemoryMap(ctx, ula);
}

void TimexMachine::setJoystickButtons(unsigned player, uint8_t buttons) noexcept
{
    if (!hasTimexJoysticks() || player >= m_joysticks.size())
        return;
    using B = JoystickButtons;
    uint8_t pressed = 0;
    if (buttons & B::Up) pressed |= 0x01;
    if (buttons & B::Down) pressed |= 0x02;
    if (buttons & B::Left) pressed |= 0x04;
    if (buttons & B::Right) pressed |= 0x08;
    if (buttons & B::Fire1) pressed |= 0x80;
    m_joysticks[player] = static_cast<uint8_t>(~pressed);
}

void TimexMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    // The home bank
    if (isSe()) {
        const uint8_t page = m_port7ffd & 0x07;
        ctx.mmu.map16k(0x0000, m_romBank, (m_port7ffd & 0x10) ? 1 : 0, false);
        ctx.mmu.map16k(0x4000, m_ramBank, 5, true);
        ctx.mmu.map16k(0x8000, m_ramBank, 8, false);
        ctx.mmu.map16k(0xC000, m_ramBank, page, (page & 1) != 0);
        ula.setVramSource(m_ramBank, (m_port7ffd & 0x08) ? 7 : 5);
    } else {
        ctx.mmu.map16k(0x0000, m_romBank, 0, false);
        ctx.mmu.map16k(0x4000, m_ramBank, 0, true);
        ctx.mmu.map16k(0x8000, m_ramBank, 1, false);
        ctx.mmu.map16k(0xC000, m_ramBank, 2, false);
        ula.setVramSource(m_ramBank, 0);
    }

    // The horizontal select: the chunks of the EXROM (bit 7 of 0xFF) or of the DOCK, contended at
    // 0x4000-0x7FFF. The 2068's 8K EXROM and an empty DOCK show the same 8K in every chunk.
    const BankId bank = (m_portFf & 0x80) ? m_exromBank : m_dockBank;
    for (uint8_t chunk = 0; chunk < 8; ++chunk) {
        if (m_portF4 & (1u << chunk))
            ctx.mmu.map8k(static_cast<uint16_t>(chunk * 0x2000), bank, isSe() ? chunk : 0, chunk == 2 || chunk == 3);
    }
    // The SE: with an odd page at 0xC000, bits 2 and 3 page in the chunks 6 and 7 too
    if (isSe() && (m_port7ffd & 0x01)) {
        if (m_portF4 & 0x04)
            ctx.mmu.map8k(0xC000, bank, 6, false);
        if (m_portF4 & 0x08)
            ctx.mmu.map8k(0xE000, bank, 7, false);
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
    case Variant::Pentagon512:  return "pentagon512"sv;
    case Variant::Pentagon1024: return "pentagon1024"sv;
    default:                    return "pentagon"sv;
    }
}

std::string_view PentagonMachine::name() const noexcept
{
    switch (m_variant) {
    case Variant::Pentagon512:  return "Pentagon 512K"sv;
    case Variant::Pentagon1024: return "Pentagon 1024SL"sv;
    default:                    return "Pentagon 128K"sv;
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
        if (ctx.romResolver || ctx.romOverrideResolver) {
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

void PentagonMachine::setPagingPorts(uint8_t port7ffd, uint8_t, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = port7ffd;
    m_locked = (port7ffd & 0x20) != 0;
    updateMemoryMap(ctx, ula);
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
    return "scorpion"sv;
}

std::string_view ScorpionMachine::name() const noexcept
{
    return "Scorpion ZS 256"sv;
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
        if (ctx.romResolver || ctx.romOverrideResolver) {
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
