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

#include "zxkit/peripherals/storage/MassStorage.h"

#include "zxkit/cpu/Z80Cpu.h"

namespace zxkit {

DivMmcPeripheral::DivMmcPeripheral(std::size_t ramBanks8k) noexcept
    : m_ramBanks8k(ramBanks8k)
{
}

std::string_view DivMmcPeripheral::id() const noexcept
{
    return "divmmc";
}

std::string_view DivMmcPeripheral::name() const noexcept
{
    return "DivMMC SD Interface";
}

void DivMmcPeripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("divmmc_rom", 8 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("divmmc_ram", m_ramBanks8k * 8 * 1024, true, true);

    ctx.romcs.registerProvider(
        std::string(id()),
        80, // High priority over Interface 1 / standard ROM
        [this](Mmu &mmu) {
            const bool conmem = (m_controlE3 & 0x80) != 0;
            const bool mapram = (m_controlE3 & 0x40) != 0;
            const std::size_t selectedBank = (m_controlE3 & 0x3F) % m_ramBanks8k;

            if (conmem) {
                mmu.mapOverlay1k(0x0000, m_romBank, 0, 8, false);
            } else if (mapram) {
                // MAPRAM maps RAM bank 3 read-only at 0x0000..0x1FFF
                mmu.mapOverlay1k(0x0000, m_ramBank, 3 * 8, 8, false);
            } else {
                mmu.mapOverlay1k(0x0000, m_romBank, 0, 8, false);
            }

            const bool ramWritable = conmem || !mapram || (selectedBank != 3);
            mmu.mapOverlay1k(0x2000, m_ramBank, selectedBank * 8, 8, ramWritable);
        });

    // Early automap before opcode fetch at 0x3D00..0x3DFF
    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if ((pc & 0xFF00) == 0x3D00) {
            m_automap = true;
            updateOverlayState(ctx);
        }
    }, std::string(id()));

    // Late automap / unmap after M1 opcode fetch
    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if ((pc & 0xFFF8) == 0x1FF8) {
            m_automap = false;
            updateOverlayState(ctx);
        } else if (pc == 0x0000 || pc == 0x0008 || pc == 0x0038 ||
                   pc == 0x0066 || pc == 0x04C6 || pc == 0x0562) {
            m_automap = true;
            updateOverlayState(ctx);
        }
    }, std::string(id()));

    // Port 0xE3: DivMMC Control Register
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_e3",
        0x00FF,
        0x00E3,
        [this](uint16_t) -> uint8_t { return m_controlE3; },
        [this, &ctx](uint16_t, uint8_t val) {
            // MAPRAM bit (bit 6) is sticky until hard reset
            m_controlE3 = static_cast<uint8_t>(val | (m_controlE3 & 0x40));
            updateOverlayState(ctx);
        });

    // Port 0xE7: SPI Chip Select
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_spi_cs",
        0x00FF,
        0x00E7,
        [this](uint16_t) -> uint8_t { return m_spiCs; },
        [this](uint16_t, uint8_t val) { m_spiCs = val; });

    // Port 0xEB: SPI Data Transfer
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_spi_data",
        0x00FF,
        0x00EB,
        [this](uint16_t) -> uint8_t { return m_spiRxByte; },
        [this](uint16_t, uint8_t val) { transferSpiByte(val); });
}

void DivMmcPeripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(std::string(id()) + "_e3");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_spi_cs");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_spi_data");
}

void DivMmcPeripheral::reset(MachineContext & /*ctx*/, ResetKind kind)
{
    if (kind == ResetKind::Hard)
        m_controlE3 = 0;
    else
        m_controlE3 &= 0x40; // Preserve sticky MAPRAM on soft reset
    m_automap = false;
    m_spiCs = 0xFF;
    m_spiRxByte = 0xFF;
}

BankId DivMmcPeripheral::romBankId() const noexcept
{
    return m_romBank;
}

BankId DivMmcPeripheral::ramBankId() const noexcept
{
    return m_ramBank;
}

uint8_t DivMmcPeripheral::controlRegister() const noexcept
{
    return m_controlE3;
}

bool DivMmcPeripheral::isAutomapped() const noexcept
{
    return m_automap;
}

void DivMmcPeripheral::updateOverlayState(MachineContext &ctx)
{
    const bool active = m_automap || ((m_controlE3 & 0x80) != 0);
    ctx.romcs.setActive(id(), active);
    if (active)
        ctx.romcs.refresh();
}

void DivMmcPeripheral::transferSpiByte(uint8_t tx) noexcept
{
    if (tx == 0xFF) {
        m_spiRxByte = 0xFF;
    } else if ((tx & 0xC0) == 0x40) {
        const uint8_t cmd = tx & 0x3F;
        m_spiRxByte = (cmd == 0) ? 0x01 : 0x00; // R1 idle / ready response
    }
}

} // namespace zxkit
