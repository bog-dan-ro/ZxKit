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

#include "zxkit/core/Exception.h"
#include "zxkit/core/StateStream.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

using BankId = uint16_t;

class MemoryBank {
public:
    static constexpr std::size_t kUnitPageSize = 1024; // 1 KB base unit

    MemoryBank();
    MemoryBank(BankId id, std::string name, std::size_t sizeBytes, bool writable, bool saveToSnapshot = true);

    void resize(std::size_t sizeBytes, uint8_t fillVal = 0x00);
    void fill(uint8_t value = 0x00) noexcept;
    void loadData(std::size_t offset, std::span<const uint8_t> src);

    BankId id() const noexcept { return m_id; }
    std::string_view name() const noexcept { return m_name; }
    bool isWritable() const noexcept { return m_writable; }
    void setWritable(bool w) noexcept { m_writable = w; }
    bool saveToSnapshot() const noexcept { return m_saveToSnapshot; }
    void setSaveToSnapshot(bool s) noexcept { m_saveToSnapshot = s; }

    std::size_t size() const noexcept { return m_data.size(); }
    std::size_t count1kPages() const noexcept { return m_data.size() / kUnitPageSize; }
    std::size_t count8kPages() const noexcept { return m_data.size() / (8 * kUnitPageSize); }
    std::size_t count16kPages() const noexcept { return m_data.size() / (16 * kUnitPageSize); }

    std::span<uint8_t, kUnitPageSize> page1k(std::size_t page1kIdx)
    {
        if (page1kIdx >= count1kPages()) [[unlikely]]
            throw InvalidArgumentException("1KB page index out of range");
        return std::span(m_data).subspan(page1kIdx * kUnitPageSize).first<kUnitPageSize>();
    }

    std::span<const uint8_t, kUnitPageSize> page1k(std::size_t page1kIdx) const
    {
        if (page1kIdx >= count1kPages()) [[unlikely]]
            throw InvalidArgumentException("1KB page index out of range");
        return std::span(m_data).subspan(page1kIdx * kUnitPageSize).first<kUnitPageSize>();
    }

    std::span<uint8_t> span16k(std::size_t page16kIdx);
    std::span<const uint8_t> span16k(std::size_t page16kIdx) const;
    std::span<uint8_t> span8k(std::size_t page8kIdx);
    std::span<const uint8_t> span8k(std::size_t page8kIdx) const;

    std::span<uint8_t> bytes() noexcept { return m_data; }
    std::span<const uint8_t> bytes() const noexcept { return m_data; }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    BankId m_id = 0;
    std::string m_name;
    bool m_writable = false;
    bool m_saveToSnapshot = true;
    std::vector<uint8_t> m_data;
};

} // namespace zxkit
