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

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace zxkit {

/// Sinclair ZX Interface 1 & Microdrive (.mdr) Peripheral
class Interface1Peripheral : public Peripheral
{
public:
    static constexpr std::size_t kMaxCartridges = 8;
    static constexpr std::size_t kMdrBlockSize =
            543; // 15 header + 15 record + 512 data + 1 checksum

    struct Cartridge
    {
        std::vector<uint8_t> data;
        std::size_t headPos = 0;
        bool writeProtected = false;
        bool inserted = false;
    };

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    void insertCartridge(std::size_t driveIdx, std::span<const uint8_t> mdrBytes);

    BankId romBankId() const noexcept;

private:
    uint8_t readMdrByte() noexcept;
    void writeMdrByte(uint8_t val) noexcept;
    uint8_t readMdrStatus() const noexcept;
    void writeMdrControl(uint8_t val) noexcept;

    BankId m_romBank = 0;
    std::array<Cartridge, kMaxCartridges> m_cartridges{};
    int m_activeDrive = -1;
    uint8_t m_commsData = 0;
    bool m_commsClk = false;
};

} // namespace zxkit
