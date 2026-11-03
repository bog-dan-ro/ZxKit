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

#include "zxkit/memory/MemoryBank.h"

#include <algorithm>
#include <utility>

namespace zxkit {

MemoryBank::MemoryBank() = default;

MemoryBank::MemoryBank(BankId id, std::string name, std::size_t sizeBytes, bool writable, bool saveToSnapshot)
    : m_id(id)
    , m_name(std::move(name))
    , m_writable(writable)
    , m_saveToSnapshot(saveToSnapshot)
    , m_data((sizeBytes + kUnitPageSize - 1) / kUnitPageSize * kUnitPageSize, 0x00)
{
}

void MemoryBank::resize(std::size_t sizeBytes, uint8_t fillVal)
{
    const std::size_t aligned = (sizeBytes + kUnitPageSize - 1) / kUnitPageSize * kUnitPageSize;
    m_data.assign(aligned, fillVal);
}

void MemoryBank::fill(uint8_t value) noexcept
{
    std::ranges::fill(m_data, value);
}

void MemoryBank::loadData(std::size_t offset, std::span<const uint8_t> src)
{
    if (offset + src.size() > m_data.size())
        throw InvalidArgumentException("Data load exceeds MemoryBank capacity");
    std::ranges::copy(src, m_data.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::span<uint8_t> MemoryBank::span16k(std::size_t page16kIdx)
{
    const std::size_t offset = page16kIdx * 16 * kUnitPageSize;
    if (offset + 16 * kUnitPageSize > m_data.size())
        throw InvalidArgumentException("16KB page index out of range");
    return {m_data.data() + offset, 16 * kUnitPageSize};
}

std::span<const uint8_t> MemoryBank::span16k(std::size_t page16kIdx) const
{
    const std::size_t offset = page16kIdx * 16 * kUnitPageSize;
    if (offset + 16 * kUnitPageSize > m_data.size())
        throw InvalidArgumentException("16KB page index out of range");
    return {m_data.data() + offset, 16 * kUnitPageSize};
}

std::span<uint8_t> MemoryBank::span8k(std::size_t page8kIdx)
{
    const std::size_t offset = page8kIdx * 8 * kUnitPageSize;
    if (offset + 8 * kUnitPageSize > m_data.size())
        throw InvalidArgumentException("8KB page index out of range");
    return {m_data.data() + offset, 8 * kUnitPageSize};
}

std::span<const uint8_t> MemoryBank::span8k(std::size_t page8kIdx) const
{
    const std::size_t offset = page8kIdx * 8 * kUnitPageSize;
    if (offset + 8 * kUnitPageSize > m_data.size())
        throw InvalidArgumentException("8KB page index out of range");
    return {m_data.data() + offset, 8 * kUnitPageSize};
}

void MemoryBank::saveState(StateWriter &w) const
{
    w.writeBool(m_writable);
    w.write<uint32_t>(static_cast<uint32_t>(m_data.size()));
    w.writeBytes(m_data);
}

void MemoryBank::loadState(StateReader &r)
{
    m_writable = r.readBool();
    const auto len = r.read<uint32_t>();
    const auto bytes = r.readSpan(len);
    m_data.assign(bytes.begin(), bytes.end());
}

} // namespace zxkit
