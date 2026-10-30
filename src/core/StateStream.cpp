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

#include "zxkit/core/StateStream.h"

#include <algorithm>

namespace zxkit {

void StateWriter::writeBool(bool val)
{
    write<uint8_t>(val ? 1 : 0);
}

void StateWriter::writeBytes(std::span<const uint8_t> bytes)
{
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

void StateWriter::writeString(std::string_view str)
{
    write<uint32_t>(static_cast<uint32_t>(str.size()));
    m_buffer.insert(m_buffer.end(), str.begin(), str.end());
}

bool StateReader::readBool()
{
    return read<uint8_t>() != 0;
}

void StateReader::readBytes(std::span<uint8_t> dest)
{
    if (m_pos + dest.size() > m_data.size()) [[unlikely]]
        throw FormatException("Unexpected end of state stream while reading byte block");
    std::ranges::copy(m_data.subspan(m_pos, dest.size()), dest.begin());
    m_pos += dest.size();
}

std::span<const uint8_t> StateReader::readSpan(std::size_t length)
{
    if (m_pos + length > m_data.size()) [[unlikely]]
        throw FormatException("Unexpected end of state stream while reading span");
    auto res = m_data.subspan(m_pos, length);
    m_pos += length;
    return res;
}

std::string StateReader::readString()
{
    const auto len = read<uint32_t>();
    if (m_pos + len > m_data.size()) [[unlikely]]
        throw FormatException("Unexpected end of state stream while reading string");
    std::string s(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
    m_pos += len;
    return s;
}

} // namespace zxkit
