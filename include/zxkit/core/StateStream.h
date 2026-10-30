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

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

class StateWriter {
public:
    StateWriter() = default;
    explicit StateWriter(std::size_t reserveBytes)
    {
        m_buffer.reserve(reserveBytes);
    }

    template <std::integral T>
    void write(T val)
    {
        if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
            val = std::byteswap(val);
        }
        const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(val);
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
    }

    void writeBool(bool val);
    void writeBytes(std::span<const uint8_t> bytes);
    void writeString(std::string_view str);
    /// Writes the characters of a block or file signature ("ZXST", "RZX!", ...), no length.
    void writeTag(std::string_view tag) { m_buffer.insert(m_buffer.end(), tag.begin(), tag.end()); }

    std::span<const uint8_t> data() const noexcept { return m_buffer; }
    std::vector<uint8_t> takeBuffer() noexcept { return std::move(m_buffer); }
    std::size_t size() const noexcept { return m_buffer.size(); }

private:
    std::vector<uint8_t> m_buffer;
};

class StateReader {
public:
    explicit StateReader(std::span<const uint8_t> data) noexcept
        : m_data(data)
    {
    }

    template <std::integral T>
    T read()
    {
        if (m_pos + sizeof(T) > m_data.size()) [[unlikely]]
            throw FormatException("Unexpected end of state stream");
        std::array<uint8_t, sizeof(T)> bytes;
        std::ranges::copy(m_data.subspan(m_pos, sizeof(T)), bytes.begin());
        T val = std::bit_cast<T>(bytes);
        m_pos += sizeof(T);
        if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
            val = std::byteswap(val);
        }
        return val;
    }

    bool readBool();
    void readBytes(std::span<uint8_t> dest);
    std::span<const uint8_t> readSpan(std::size_t length);
    std::string readString();

    std::size_t position() const noexcept { return m_pos; }
    std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    bool eof() const noexcept { return m_pos >= m_data.size(); }

private:
    std::span<const uint8_t> m_data;
    std::size_t m_pos = 0;
};

} // namespace zxkit
