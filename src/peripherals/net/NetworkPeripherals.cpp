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

#include "zxkit/peripherals/net/NetworkPeripherals.h"

#include "zxkit/cpu/Z80Cpu.h"

namespace zxkit {

std::string_view SpectranetPeripheral::id() const noexcept
{
    return "spectranet";
}

std::string_view SpectranetPeripheral::name() const noexcept
{
    return "Spectranet Ethernet Interface";
}

void SpectranetPeripheral::onAttach(MachineContext &ctx)
{
    m_flashBank = ctx.mmu.registerBank("spectranet_flash", 128 * 1024, false, false);
    m_sramBank  = ctx.mmu.registerBank("spectranet_sram", 128 * 1024, true, true);

    ctx.romcs.registerProvider(
        std::string(id()),
        35,
        [this](Mmu &mmu) {
            mmu.mapOverlay1k(0x0000, m_flashBank, 0, 4, false);
            map4kArea(mmu, 0x1000, m_pageA);
            map4kArea(mmu, 0x2000, m_pageB);
            mmu.mapOverlay1k(0x3000, m_sramBank, 0, 4, true);
        });

    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x0008 || (pc & 0xFFF8) == 0x3FF8) {
            m_pagedIn = true;
            ctx.romcs.setActive(id(), true);
        }
        if (m_trapEnabled && pc == m_trapAddress) {
            ctx.cpu.requestNmi();
        }
    }, std::string(id()));

    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if (pc == 0x007C && m_pagedIn) {
            m_pagedIn = false;
            ctx.romcs.setActive(id(), false);
        }
    }, std::string(id()));

    // Page A (0x003B), Page B (0x013B), Trap (0x023B), Control (0x033B)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ctrl",
        0x03FF,
        0x003B,
        [this](uint16_t) -> uint8_t { return m_pageA; },
        [this, &ctx](uint16_t, uint8_t val) {
            m_pageA = val;
            if (m_pagedIn)
                ctx.romcs.refresh();
        });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_pageb",
        0x03FF,
        0x013B,
        [this](uint16_t) -> uint8_t { return m_pageB; },
        [this, &ctx](uint16_t, uint8_t val) {
            m_pageB = val;
            if (m_pagedIn)
                ctx.romcs.refresh();
        });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_trap",
        0x03FF,
        0x023B,
        nullptr,
        [this](uint16_t, uint8_t val) {
            if (!m_trapHighNext) {
                m_trapAddress = (m_trapAddress & 0xFF00) | val;
                m_trapHighNext = true;
            } else {
                m_trapAddress = static_cast<uint16_t>((static_cast<uint16_t>(val) << 8) | (m_trapAddress & 0x00FF));
                m_trapHighNext = false;
            }
        });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_status",
        0x03FF,
        0x033B,
        [this](uint16_t) -> uint8_t { return m_control; },
        [this, &ctx](uint16_t, uint8_t val) {
            m_control = val;
            m_pagedIn = (val & 0x01) != 0;
            m_trapEnabled = (val & 0x08) != 0;
            ctx.romcs.setActive(id(), m_pagedIn);
        });
}

void SpectranetPeripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(std::string(id()) + "_ctrl");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_pageb");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_trap");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_status");
}

void SpectranetPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_pageA = 0;
    m_pageB = 0;
    m_control = 0;
    m_pagedIn = false;
    m_trapEnabled = false;
    m_trapHighNext = false;
    m_trapAddress = 0;
}

BankId SpectranetPeripheral::flashBankId() const noexcept
{
    return m_flashBank;
}

BankId SpectranetPeripheral::sramBankId() const noexcept
{
    return m_sramBank;
}

void SpectranetPeripheral::map4kArea(Mmu &mmu, uint16_t addr, uint8_t pageCode) const
{
    if (pageCode >= 0xC0) {
        const std::size_t sram4k = pageCode & 0x1F;
        mmu.mapOverlay1k(addr, m_sramBank, sram4k * 4, 4, true);
    } else {
        const std::size_t flash4k = pageCode & 0x1F;
        mmu.mapOverlay1k(addr, m_flashBank, flash4k * 4, 4, false);
    }
}

} // namespace zxkit
