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

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <span>

namespace zxkit {

/// SD / SDHC card in SPI mode, backed by a raw disk image file.
/// The image is accessed block by block, it is never loaded into memory, so
/// multi-GB images are fine. Used by the DivMMC and the ZX Spectrum Next.
class SdCard {
public:
    static constexpr std::size_t kBlockSize = 512;

    /// Opens `image` (read-write unless `readOnly`). Throws IoException.
    void insert(const std::filesystem::path &image, bool readOnly = false);
    void eject();

    bool isInserted() const noexcept { return m_file.is_open(); }
    bool isReadOnly() const noexcept { return m_readOnly; }
    const std::filesystem::path &imagePath() const noexcept { return m_path; }
    uint64_t sizeBytes() const noexcept { return m_size; }

    /// Chip select (true = selected, i.e. /CS low). Deselecting aborts any transfer.
    void setSelected(bool selected) noexcept;
    bool isSelected() const noexcept { return m_selected; }

    /// One full-duplex SPI byte exchange: sends `mosi`, returns MISO.
    uint8_t transfer(uint8_t mosi);

    /// Raw 512-byte block access, also used to read files (ROMs) off the card.
    bool readBlock(uint64_t lba, std::span<uint8_t, kBlockSize> out);
    bool writeBlock(uint64_t lba, std::span<const uint8_t, kBlockSize> in);
    /// Bumped at every block read or written (activity LEDs).
    uint64_t accessCount() const noexcept { return m_accesses; }

private:
    enum class State : uint8_t {
        Idle,
        Command,
        WriteWaitToken,
        WriteData,
    };

    void executeCommand();
    void queueR1(uint8_t r1);
    void queueDataBlock(uint64_t lba);
    void queueRegister(std::span<const uint8_t, 16> reg);
    uint64_t blockFromArg(uint32_t arg) const noexcept;
    uint8_t r1Status() const noexcept { return m_initialized ? 0x00 : 0x01; }
    void buildCsd(std::array<uint8_t, 16> &csd) const noexcept;

    std::fstream m_file;
    std::filesystem::path m_path;
    uint64_t m_size = 0;
    uint64_t m_accesses = 0;
    bool m_readOnly = false;

    bool m_selected = false;
    State m_state = State::Idle;
    std::array<uint8_t, 6> m_cmd{};
    std::size_t m_cmdPos = 0;
    std::deque<uint8_t> m_response;
    uint8_t m_idleByte = 0xFF; // MISO when no response is queued

    bool m_initialized = false;
    bool m_appCmd = false;
    bool m_highCapacity = false;

    bool m_multiRead = false;
    uint64_t m_multiReadLba = 0;
    bool m_multiWrite = false;
    uint64_t m_writeLba = 0;
    std::array<uint8_t, kBlockSize + 2> m_writeBuf{};
    std::size_t m_writePos = 0;
};

} // namespace zxkit
