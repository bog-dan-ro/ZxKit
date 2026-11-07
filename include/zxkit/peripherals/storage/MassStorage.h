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

#pragma once

#include "zxkit/bus/Peripheral.h"

#include <cstdint>

namespace zxkit {

/// DivMMC / DivIDE Mass Storage Interface
/// Supports 8KB EEPROM + up to 64 x 8KB (512KB) banked RAM, port 0xE3 (CONMEM, MAPRAM, BANK),
/// early M1 automap at 0x3Dxx, late M1 automap at 0x0000/0x0008/0x0038/0x0066/0x04C6/0x0562,
/// late M1 unmap at 0x1FF8..0x1FFF, and SPI/SD Card ports 0xE7 (CS) and 0xEB (SPI data).
class DivMmcPeripheral : public Peripheral
{
public:
    explicit DivMmcPeripheral(std::size_t ramBanks8k = 16) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    BankId romBankId() const noexcept;
    BankId ramBankId() const noexcept;
    uint8_t controlRegister() const noexcept;
    bool isAutomapped() const noexcept;

private:
    void updateOverlayState(MachineContext &ctx);
    void transferSpiByte(uint8_t tx) noexcept;

    std::size_t m_ramBanks8k = 16;
    BankId m_romBank = 0;
    BankId m_ramBank = 0;
    uint8_t m_controlE3 = 0;
    bool m_automap = false;
    uint8_t m_spiCs = 0xFF;
    uint8_t m_spiRxByte = 0xFF;
};

} // namespace zxkit
