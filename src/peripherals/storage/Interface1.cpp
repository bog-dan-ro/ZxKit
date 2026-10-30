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

#include "zxkit/peripherals/storage/Interface1.h"

#include "zxkit/core/Exception.h"
#include "zxkit/cpu/Z80Cpu.h"

namespace zxkit {

std::string_view Interface1Peripheral::id() const noexcept
{
    return "interface1";
}

std::string_view Interface1Peripheral::name() const noexcept
{
    return "ZX Interface 1 + Microdrive";
}

void Interface1Peripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("if1_rom", 8 * 1024, false, false);

    ctx.romcs.registerProvider(
        std::string(id()),
        20,
        [this](Mmu &mmu) {
            mmu.mapOverlay1k(0x0000, m_romBank, 0, 8, false);
            mmu.mapOverlay1k(0x2000, m_romBank, 0, 8, false);
        });

    // M1 page-in at 0x0008 or 0x1708; M1 page-out after opcode fetch at 0x0700
    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x0008 || pc == 0x1708)
            ctx.romcs.setActive(id(), true);
    }, std::string(id()));
    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if (pc == 0x0700)
            ctx.romcs.setActive(id(), false);
    }, std::string(id()));

    // Port 0xE7: Microdrive data (mask 0x0018 == 0x0000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_mdr_data",
        0x0018,
        0x0000,
        [this](uint16_t) -> uint8_t { return readMdrByte(); },
        [this](uint16_t, uint8_t val) { writeMdrByte(val); });

    // Port 0xEF: Microdrive control/status (mask 0x0018 == 0x0008)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_mdr_ctrl",
        0x0018,
        0x0008,
        [this](uint16_t) -> uint8_t { return readMdrStatus(); },
        [this](uint16_t, uint8_t val) { writeMdrControl(val); });

    // Port 0xF7: RS-232 / ZX Net (mask 0x0018 == 0x0010)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_rs232",
        0x0018,
        0x0010,
        [](uint16_t) -> uint8_t { return 0xFF; },
        [](uint16_t, uint8_t) {});
}

void Interface1Peripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(std::string(id()) + "_mdr_data");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_mdr_ctrl");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_rs232");
}

void Interface1Peripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_activeDrive = -1;
    m_commsData = 0;
    m_commsClk = false;
}

void Interface1Peripheral::insertCartridge(std::size_t driveIdx, std::span<const uint8_t> mdrBytes)
{
    if (driveIdx >= kMaxCartridges)
        throw InvalidArgumentException("Microdrive index out of range (0..7)");
    auto &c = m_cartridges[driveIdx];
    if (mdrBytes.size() % kMdrBlockSize == 1) {
        c.writeProtected = (mdrBytes.back() != 0);
        c.data.assign(mdrBytes.begin(), mdrBytes.end() - 1);
    } else {
        c.writeProtected = false;
        c.data.assign(mdrBytes.begin(), mdrBytes.end());
    }
    c.headPos = 0;
    c.inserted = !c.data.empty();
}

BankId Interface1Peripheral::romBankId() const noexcept
{
    return m_romBank;
}

uint8_t Interface1Peripheral::readMdrByte() noexcept
{
    if (m_activeDrive < 0 || m_activeDrive >= static_cast<int>(kMaxCartridges))
        return 0xFF;
    auto &c = m_cartridges[m_activeDrive];
    if (!c.inserted || c.data.empty())
        return 0xFF;
    const uint8_t b = c.data[c.headPos];
    c.headPos = (c.headPos + 1) % c.data.size();
    return b;
}

void Interface1Peripheral::writeMdrByte(uint8_t val) noexcept
{
    if (m_activeDrive < 0 || m_activeDrive >= static_cast<int>(kMaxCartridges))
        return;
    auto &c = m_cartridges[m_activeDrive];
    if (!c.inserted || c.writeProtected || c.data.empty())
        return;
    c.data[c.headPos] = val;
    c.headPos = (c.headPos + 1) % c.data.size();
}

uint8_t Interface1Peripheral::readMdrStatus() const noexcept
{
    uint8_t status = 0xFE; // Bit 0 = write protect (0 = protected)
    if (m_activeDrive >= 0 && m_activeDrive < static_cast<int>(kMaxCartridges)) {
        const auto &c = m_cartridges[m_activeDrive];
        if (!c.writeProtected)
            status |= 0x01;
    }
    return status;
}

void Interface1Peripheral::writeMdrControl(uint8_t val) noexcept
{
    const bool clk = (val & 0x02) != 0;
    if (!clk && m_commsClk) {
        m_commsData = static_cast<uint8_t>((m_commsData << 1) | ((val & 0x01) ? 0 : 1));
        m_activeDrive = -1;
        for (int i = 0; i < 8; ++i) {
            if (m_commsData & (1u << i)) {
                m_activeDrive = i;
                break;
            }
        }
    }
    m_commsClk = clk;
}

} // namespace zxkit
