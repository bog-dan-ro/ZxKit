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

#include "zxkit/peripherals/expansion/ExpansionPeripherals.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/input/Joystick.h"

namespace zxkit {

MultifacePeripheral::MultifacePeripheral(Model model) noexcept
    : m_model(model)
{
}

std::string_view MultifacePeripheral::id() const noexcept
{
    return "multiface";
}

std::string_view MultifacePeripheral::name() const noexcept
{
    switch (m_model) {
    case Model::Multiface1:   return "Multiface 1";
    case Model::Multiface128: return "Multiface 128";
    case Model::Multiface3:   return "Multiface 3";
    }
    return "Multiface 128";
}

void MultifacePeripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("multiface_rom", 8 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("multiface_ram", 8 * 1024, true, true);

    ctx.romcs.registerProvider(
        std::string(id()),
        90, // Highest expansion priority (NMI debugger/snapshotter)
        [this](Mmu &mmu) {
            mmu.mapOverlay1k(0x0000, m_romBank, 0, 8, false);
            mmu.mapOverlay1k(0x2000, m_ramBank, 0, 8, true);
        });

    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x0066 && m_nmiButtonPressed) {
            m_nmiButtonPressed = false;
            m_pagedIn = true;
            ctx.romcs.setActive(id(), true);
        }
    }, std::string(id()));

    ctx.cpu.addRetnHook([this, &ctx](bool /*isReti*/) {
        if (m_pagedIn) {
            m_pagedIn = false;
            ctx.romcs.setActive(id(), false);
        }
    }, std::string(id()));

    // Multiface 128 page-in port 0xBF, page-out port 0x3F
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_io",
        0x007F,
        0x003F,
        [this, &ctx](uint16_t port) -> uint8_t {
            m_pagedIn = (port & 0x80) != 0;
            ctx.romcs.setActive(id(), m_pagedIn);
            return 0xFF;
        },
        nullptr);
}

void MultifacePeripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(std::string(id()) + "_io");
}

void MultifacePeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_nmiButtonPressed = false;
    m_pagedIn = false;
}

void MultifacePeripheral::pressRedButton(Z80Cpu &cpu) noexcept
{
    m_nmiButtonPressed = true;
    cpu.requestNmi();
}

BankId MultifacePeripheral::romBankId() const noexcept
{
    return m_romBank;
}

BankId MultifacePeripheral::ramBankId() const noexcept
{
    return m_ramBank;
}

bool MultifacePeripheral::isPagedIn() const noexcept
{
    return m_pagedIn;
}

std::string_view UsourcePeripheral::id() const noexcept
{
    return "usource";
}

std::string_view UsourcePeripheral::name() const noexcept
{
    return "Currah uSource";
}

void UsourcePeripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("usource_rom", 2 * 1024, false, false);

    ctx.romcs.registerProvider(
        std::string(id()),
        25,
        [this](Mmu &mmu) {
            for (uint16_t offset = 0; offset < 0x4000; offset += 0x0800)
                mmu.mapOverlay1k(offset, m_romBank, 0, 2, false);
        });

    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x2BAE) {
            m_active = !m_active;
            ctx.romcs.setActive(id(), m_active);
        }
    }, std::string(id()));
}

void UsourcePeripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
}

void UsourcePeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_active = false;
}

BankId UsourcePeripheral::romBankId() const noexcept
{
    return m_romBank;
}

std::string_view ZxPrinterPeripheral::id() const noexcept
{
    return "zxprinter";
}

std::string_view ZxPrinterPeripheral::name() const noexcept
{
    return "Sinclair ZX Printer";
}

void ZxPrinterPeripheral::onAttach(MachineContext &ctx)
{
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x0004,
        0x0000,
        [](uint16_t) -> uint8_t {
            // Bit 0 = stylus ready (1), Bit 7 = new line sync
            return 0x81;
        },
        [this](uint16_t, uint8_t val) {
            m_lastCommand = val;
        });
}

void ZxPrinterPeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(id());
}

void ZxPrinterPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_lastCommand = 0;
}

std::string_view Interface2Peripheral::id() const noexcept
{
    return "interface2";
}

std::string_view Interface2Peripheral::name() const noexcept
{
    return "Sinclair ZX Interface 2";
}

void Interface2Peripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("if2_rom", 16 * 1024, false, false);
    ctx.romcs.registerProvider(
        std::string(id()),
        15,
        [this](Mmu &mmu) {
            mmu.mapOverlay1k(0x0000, m_romBank, 0, 16, false);
        });
}

void Interface2Peripheral::onDetach(MachineContext &ctx)
{
    ctx.romcs.unregisterProvider(id());
}

void Interface2Peripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
}

void Interface2Peripheral::insertCartridge(MachineContext &ctx, std::span<const uint8_t> rom16k)
{
    ctx.mmu.bank(m_romBank).loadData(0, rom16k);
    ctx.romcs.setActive(id(), true);
}

void Interface2Peripheral::ejectCartridge(MachineContext &ctx)
{
    ctx.romcs.setActive(id(), false);
}

BankId Interface2Peripheral::romBankId() const noexcept
{
    return m_romBank;
}

std::string_view SpecDrumPeripheral::id() const noexcept
{
    return "specdrum";
}

std::string_view SpecDrumPeripheral::name() const noexcept
{
    return "Cheetah SpecDrum";
}

void SpecDrumPeripheral::onAttach(MachineContext &ctx)
{
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x00FF,
        0x00DF,
        nullptr,
        [&ctx](uint16_t, uint8_t val) {
            ctx.audioMixer.dac().writeMono(ctx.clock.frameBaseTstates(), val,
                                           ctx.audioMixer.leftBlip(), ctx.audioMixer.rightBlip());
        });
}

void SpecDrumPeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(id());
}

void SpecDrumPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
}

CovoxPeripheral::CovoxPeripheral(uint8_t port) noexcept
    : m_port(port)
{
}

std::string_view CovoxPeripheral::id() const noexcept
{
    return "covox";
}

std::string_view CovoxPeripheral::name() const noexcept
{
    return "Covox DAC";
}

void CovoxPeripheral::onAttach(MachineContext &ctx)
{
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x00FF,
        m_port,
        nullptr,
        [&ctx](uint16_t, uint8_t val) {
            ctx.audioMixer.dac().writeMono(ctx.clock.frameBaseTstates(), val,
                                           ctx.audioMixer.leftBlip(), ctx.audioMixer.rightBlip());
        });
}

void CovoxPeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(id());
}

void CovoxPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
}

std::string_view FullerBoxPeripheral::id() const noexcept
{
    return "fuller";
}

std::string_view FullerBoxPeripheral::name() const noexcept
{
    return "Fuller Box";
}

void FullerBoxPeripheral::setButtons(uint8_t buttons) noexcept
{
    m_buttons = buttons;
    uint8_t state = 0xFF;
    if (buttons & JoystickButtons::Up)
        state &= static_cast<uint8_t>(~0x01);
    if (buttons & JoystickButtons::Down)
        state &= static_cast<uint8_t>(~0x02);
    if (buttons & JoystickButtons::Left)
        state &= static_cast<uint8_t>(~0x04);
    if (buttons & JoystickButtons::Right)
        state &= static_cast<uint8_t>(~0x08);
    if (buttons & JoystickButtons::Fire1)
        state &= static_cast<uint8_t>(~0x80);
    m_joyState = state;
}

void FullerBoxPeripheral::onAttach(MachineContext &ctx)
{
    if (ctx.audioMixer.ayChipCount() == 0)
        ctx.audioMixer.setAyChipCount(1);

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_reg",
        0x00FF,
        0x003F,
        [&ctx](uint16_t) -> uint8_t { return ctx.audioMixer.activeAy().readRegister(); },
        [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.activeAy().selectRegister(val); });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_data",
        0x00FF,
        0x005F,
        nullptr,
        [&ctx](uint16_t, uint8_t val) { ctx.audioMixer.writeAyRegister(ctx.clock.frameBaseTstates(), val); });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_joy",
        0x00FF,
        0x007F,
        [this](uint16_t) -> uint8_t { return m_joyState; },
        nullptr);
}

void FullerBoxPeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(std::string(id()) + "_ay_reg");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_ay_data");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_joy");
}

void FullerBoxPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    // The joystick is the host's: its state survives a reset
    setButtons(m_buttons);
}

} // namespace zxkit
