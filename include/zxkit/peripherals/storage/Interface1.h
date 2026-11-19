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
#include <optional>
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

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    bool supportsMachine(const MachineProfile &machine) const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    /// Inserts into the specified drive, or the first empty drive. Drive indices are zero-based.
    void insertCartridge(std::size_t driveIdx, std::span<const uint8_t> mdrBytes);
    std::size_t insertCartridge(std::span<const uint8_t> mdrBytes);
    void ejectCartridge(std::size_t driveIdx);
    bool hasCartridge(std::size_t driveIdx) const noexcept;
    bool cartridgeModified(std::size_t driveIdx) const noexcept;
    std::vector<uint8_t> saveCartridge(std::size_t driveIdx) const;
    /// Clears the modified flag if the saved bytes still match the cartridge.
    void markCartridgeSaved(std::size_t driveIdx, std::span<const uint8_t> savedBytes) noexcept;

    BankId romBankId() const noexcept;

private:
    struct Cartridge
    {
        std::vector<uint8_t> data;
        std::optional<uint8_t> protection;
        std::size_t position = 0;
        std::size_t transferred = 0;
        std::size_t blockSize = 0;
        unsigned statusPolls = 0;
        uint8_t lastByte = 0xFF;
        bool modified = false;
    };

    uint8_t readMdrByte() noexcept;
    void writeMdrByte(uint8_t value) noexcept;
    uint8_t readMdrStatus() noexcept;
    void writeMdrControl(uint8_t value) noexcept;
    void restartTransfers() noexcept;

    BankId m_romBank = 0;
    std::array<Cartridge, kMaxCartridges> m_cartridges{};
    uint8_t m_motors = 0;
    uint8_t m_control = 0;
};

} // namespace zxkit
