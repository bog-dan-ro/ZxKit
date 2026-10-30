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

#include "zxkit/peripherals/storage/SdCard.h"

#include "zxkit/core/Exception.h"

#include <algorithm>
#include <format>
#include <utility>

namespace zxkit {

namespace {

constexpr uint8_t kTokenStartBlock      = 0xFE;
constexpr uint8_t kTokenStartMultiWrite = 0xFC;
constexpr uint8_t kTokenStopMultiWrite  = 0xFD;
constexpr uint8_t kDataAccepted         = 0x05;
constexpr uint8_t kR1IllegalCommand     = 0x04;
constexpr uint8_t kR1AddressError       = 0x20;

constexpr std::array<uint8_t, 16> kCid = {
    0x03, 'Z', 'X', 'Z', 'X', 'K', 'I', 'T', 0x10, 0x00, 0x00, 0x00, 0x01, 0x01, 0x6A, 0x01
};

} // namespace

void SdCard::insert(const std::filesystem::path &image, bool readOnly)
{
    eject();
    const auto mode = std::ios::binary | std::ios::in | (readOnly ? std::ios::openmode{} : std::ios::out);
    m_file.open(image, mode);
    if (!m_file && !readOnly) {
        // Fall back to read-only when the image itself is not writable
        m_file.clear();
        m_file.open(image, std::ios::binary | std::ios::in);
        readOnly = true;
    }
    if (!m_file)
        throw IoException(std::format("Cannot open SD card image '{}'", image.string()));

    std::error_code ec;
    m_size = std::filesystem::file_size(image, ec);
    if (ec || m_size < kBlockSize) {
        m_file.close();
        throw IoException(std::format("'{}' is not a valid SD card image", image.string()));
    }
    m_path = image;
    m_readOnly = readOnly;
    m_initialized = false;
    m_highCapacity = false;
}

void SdCard::eject()
{
    if (m_file.is_open())
        m_file.close();
    m_path.clear();
    m_size = 0;
    m_selected = false;
    m_state = State::Idle;
    m_response.clear();
    m_initialized = false;
}

void SdCard::setSelected(bool selected) noexcept
{
    if (selected == m_selected)
        return;
    m_selected = selected;
    if (!selected) {
        // Deselecting the card aborts whatever transfer was in progress
        m_state = State::Idle;
        m_cmdPos = 0;
        m_response.clear();
        m_multiRead = false;
        m_multiWrite = false;
        m_idleByte = 0xFF;
    }
}

uint8_t SdCard::transfer(uint8_t mosi)
{
    if (!m_selected || !isInserted())
        return 0xFF;

    switch (m_state) {
    case State::WriteWaitToken:
        if (mosi == kTokenStartBlock || (m_multiWrite && mosi == kTokenStartMultiWrite)) {
            m_state = State::WriteData;
            m_writePos = 0;
            return 0xFF;
        }
        if (m_multiWrite && mosi == kTokenStopMultiWrite) {
            m_multiWrite = false;
            m_state = State::Idle;
            m_response = {0xFF, 0x00};
            return 0xFF;
        }
        break; // waiting for the token, the host may still send commands

    case State::WriteData:
        m_writeBuf[m_writePos++] = mosi;
        if (m_writePos == m_writeBuf.size()) {
            const bool ok = writeBlock(m_writeLba++, std::span<const uint8_t, kBlockSize>(m_writeBuf.data(), kBlockSize));
            // Data response token followed by a short busy period
            m_response = {static_cast<uint8_t>(ok ? kDataAccepted : 0x0D), 0x00, 0x00};
            m_state = m_multiWrite ? State::WriteWaitToken : State::Idle;
        }
        return 0xFF;

    case State::Command:
        m_cmd[m_cmdPos++] = mosi;
        if (m_cmdPos == m_cmd.size()) {
            m_state = State::Idle;
            executeCommand();
        }
        return 0xFF;

    case State::Idle:
        break;
    }

    if ((mosi & 0xC0) == 0x40) {
        // Start of a new command frame, aborts any pending output
        m_response.clear();
        m_multiRead = false;
        m_cmd[0] = mosi;
        m_cmdPos = 1;
        m_state = State::Command;
        return 0xFF;
    }

    if (m_response.empty() && m_multiRead)
        queueDataBlock(m_multiReadLba++);

    if (m_response.empty())
        return m_idleByte;

    const uint8_t out = m_response.front();
    m_response.pop_front();
    return out;
}

bool SdCard::readBlock(uint64_t lba, std::span<uint8_t, kBlockSize> out)
{
    ++m_accesses;
    const uint64_t offset = lba * kBlockSize;
    if (!isInserted() || offset + kBlockSize > m_size) {
        std::ranges::fill(out, 0xFF);
        return false;
    }
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(offset));
    m_file.read(reinterpret_cast<char *>(out.data()), kBlockSize);
    return static_cast<bool>(m_file);
}

bool SdCard::writeBlock(uint64_t lba, std::span<const uint8_t, kBlockSize> in)
{
    ++m_accesses;
    const uint64_t offset = lba * kBlockSize;
    if (!isInserted() || m_readOnly || offset + kBlockSize > m_size)
        return false;
    m_file.clear();
    m_file.seekp(static_cast<std::streamoff>(offset));
    m_file.write(reinterpret_cast<const char *>(in.data()), kBlockSize);
    m_file.flush();
    return static_cast<bool>(m_file);
}

void SdCard::queueR1(uint8_t r1)
{
    m_response.push_back(0xFF); // NCR
    m_response.push_back(r1);
}

void SdCard::queueDataBlock(uint64_t lba)
{
    std::array<uint8_t, kBlockSize> block;
    if (!readBlock(lba, block)) {
        m_response.push_back(0x08); // data error token: out of range
        m_multiRead = false;
        return;
    }
    m_response.push_back(0xFF); // NAC
    m_response.push_back(kTokenStartBlock);
    m_response.insert(m_response.end(), block.begin(), block.end());
    m_response.push_back(0xFF); // CRC16, ignored in SPI mode
    m_response.push_back(0xFF);
}

void SdCard::queueRegister(std::span<const uint8_t, 16> reg)
{
    m_response.push_back(0xFF);
    m_response.push_back(kTokenStartBlock);
    m_response.insert(m_response.end(), reg.begin(), reg.end());
    m_response.push_back(0xFF);
    m_response.push_back(0xFF);
}

uint64_t SdCard::blockFromArg(uint32_t arg) const noexcept
{
    // SDHC cards are block addressed, standard capacity cards byte addressed
    return m_highCapacity ? arg : arg / kBlockSize;
}

void SdCard::buildCsd(std::array<uint8_t, 16> &csd) const noexcept
{
    csd.fill(0);
    if (m_highCapacity) {
        // CSD 2.0: capacity = (C_SIZE + 1) * 512KB
        const uint64_t cSize = std::max<uint64_t>(m_size / (512 * 1024), 1) - 1;
        csd = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00,
               static_cast<uint8_t>((cSize >> 16) & 0x3F),
               static_cast<uint8_t>(cSize >> 8),
               static_cast<uint8_t>(cSize),
               0x7F, 0x80, 0x0A, 0x40, 0x00, 0x01};
        return;
    }
    // CSD 1.0: capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN
    uint8_t blLen = 9;
    uint8_t mult = 0;
    uint64_t blocks = 0;
    for (blLen = 9; blLen <= 11; ++blLen) {
        for (mult = 0; mult < 8; ++mult) {
            blocks = m_size >> (blLen + mult + 2);
            if (blocks <= 4096)
                break;
        }
        if (blocks <= 4096)
            break;
    }
    const uint16_t cSize = static_cast<uint16_t>(std::clamp<uint64_t>(blocks, 1, 4096) - 1);
    csd = {0x00, 0x26, 0x00, 0x32, 0x5B,
           static_cast<uint8_t>(0x50 | blLen),
           static_cast<uint8_t>(0x80 | ((cSize >> 10) & 0x03)),
           static_cast<uint8_t>(cSize >> 2),
           static_cast<uint8_t>(((cSize & 0x03) << 6) | 0x3F),
           static_cast<uint8_t>(0xFC | ((mult >> 1) & 0x03)),
           static_cast<uint8_t>(((mult & 0x01) << 7) | 0x40 | 0x3F),
           0x80, 0x0A, 0x40, 0x00, 0x01};
}

void SdCard::executeCommand()
{
    const uint8_t cmd = m_cmd[0] & 0x3F;
    const uint32_t arg = (static_cast<uint32_t>(m_cmd[1]) << 24) | (static_cast<uint32_t>(m_cmd[2]) << 16) |
                         (static_cast<uint32_t>(m_cmd[3]) << 8) | m_cmd[4];
    const bool appCmd = std::exchange(m_appCmd, false);
    m_idleByte = 0xFF;

    if (appCmd) {
        switch (cmd) {
        case 41: // SD_SEND_OP_COND
            m_highCapacity = (arg & 0x40000000) != 0;
            m_initialized = true;
            queueR1(0x00);
            return;
        case 13: // SD_STATUS
            queueR1(r1Status());
            m_response.push_back(0x00);
            return;
        case 22: // SEND_NUM_WR_BLOCKS
        case 23: // SET_WR_BLK_ERASE_COUNT
        case 42: // SET_CLR_CARD_DETECT
        case 51: // SEND_SCR
            queueR1(r1Status());
            return;
        default:
            break; // fall back to the standard command set
        }
    }

    switch (cmd) {
    case 0: // GO_IDLE_STATE
        m_initialized = false;
        m_highCapacity = false;
        queueR1(0x01);
        m_idleByte = 0x01; // some hosts poll for R1 without clocking out the NCR
        break;
    case 1: // SEND_OP_COND (MMC)
        m_initialized = true;
        queueR1(0x00);
        break;
    case 8: // SEND_IF_COND: R7 echoes the voltage and check pattern
        queueR1(r1Status());
        m_response.insert(m_response.end(), {0x00, 0x00, static_cast<uint8_t>((arg >> 8) & 0x0F), static_cast<uint8_t>(arg)});
        break;
    case 9: { // SEND_CSD
        std::array<uint8_t, 16> csd;
        buildCsd(csd);
        queueR1(r1Status());
        queueRegister(csd);
        break;
    }
    case 10: // SEND_CID
        queueR1(r1Status());
        queueRegister(kCid);
        break;
    case 12: // STOP_TRANSMISSION
        m_multiRead = false;
        m_response = {0xFF, 0xFF, r1Status(), 0x00};
        break;
    case 13: // SEND_STATUS
        queueR1(r1Status());
        m_response.push_back(0x00);
        break;
    case 16: // SET_BLOCKLEN, only 512 byte blocks are supported
        queueR1(arg > kBlockSize ? static_cast<uint8_t>(r1Status() | 0x40) : r1Status());
        break;
    case 17: // READ_SINGLE_BLOCK
    case 18: { // READ_MULTIPLE_BLOCK
        const uint64_t lba = blockFromArg(arg);
        if ((lba + 1) * kBlockSize > m_size) {
            queueR1(kR1AddressError);
            break;
        }
        queueR1(0x00);
        queueDataBlock(lba);
        if (cmd == 18) {
            m_multiRead = true;
            m_multiReadLba = lba + 1;
        }
        break;
    }
    case 24: // WRITE_BLOCK
    case 25: { // WRITE_MULTIPLE_BLOCK
        const uint64_t lba = blockFromArg(arg);
        if ((lba + 1) * kBlockSize > m_size) {
            queueR1(kR1AddressError);
            break;
        }
        queueR1(0x00);
        m_writeLba = lba;
        m_multiWrite = (cmd == 25);
        m_state = State::WriteWaitToken;
        break;
    }
    case 55: // APP_CMD
        m_appCmd = true;
        queueR1(r1Status());
        break;
    case 58: { // READ_OCR
        const uint8_t ocr0 = static_cast<uint8_t>((m_initialized ? 0x80 : 0x00) | (m_highCapacity ? 0x40 : 0x00));
        queueR1(r1Status());
        m_response.insert(m_response.end(), {ocr0, 0xFF, 0x80, 0x00});
        break;
    }
    case 59: // CRC_ON_OFF
        queueR1(r1Status());
        break;
    default:
        queueR1(static_cast<uint8_t>(r1Status() | kR1IllegalCommand));
        break;
    }
}

} // namespace zxkit
