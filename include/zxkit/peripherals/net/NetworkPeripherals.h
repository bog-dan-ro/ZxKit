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

/// Spectranet W5100 Ethernet & Flash/RAM Banking Interface
/// Demonstrates 4KB MMU banking inside 0x0000..0x3FFF:
/// - 0x0000..0x0FFF: Fixed Flash Page 0
/// - 0x1000..0x1FFF: Programmable Page A (Port 0x003B)
/// - 0x2000..0x2FFF: Programmable Page B (Port 0x013B)
/// - 0x3000..0x3FFF: Fixed SRAM Page 0 (System variables & stack)
class SpectranetPeripheral : public Peripheral {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    BankId flashBankId() const noexcept;
    BankId sramBankId() const noexcept;

private:
    void map4kArea(Mmu &mmu, uint16_t addr, uint8_t pageCode) const;

    BankId m_flashBank = 0;
    BankId m_sramBank = 0;
    uint8_t m_pageA = 0;
    uint8_t m_pageB = 0;
    uint8_t m_control = 0;
    bool m_pagedIn = false;
    bool m_trapEnabled = false;
    bool m_trapHighNext = false;
    uint16_t m_trapAddress = 0;
};

} // namespace zxkit
