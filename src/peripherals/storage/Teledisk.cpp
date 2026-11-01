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

#include "zxkit/peripherals/storage/FloppyDisk.h"
#include "zxkit/core/Exception.h"

#include <algorithm>
#include <array>
#include <utility>

namespace zxkit {
namespace {

// Format: Dave Dunfield's Teledisk notes (2008), and Will Krantz's wteledsk.
// The adaptive Huffman algorithm is the LZHUF algorithm of Haruhiko Okumura
// (1989), with Teledisk's 4096-byte dictionary and 60-byte lookahead.
constexpr std::size_t kMaxDecodedBytes = 32 * 1024 * 1024;

uint16_t crc16(std::span<const uint8_t> bytes)
{
    uint16_t crc = 0;
    for (uint8_t byte : bytes) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0xA097 : 0));
    }
    return crc;
}

class TelediskReader {
public:
    TelediskReader(std::span<const uint8_t> bytes, bool compressed)
        : m_bytes(bytes), m_compressed(compressed)
    {
        for (unsigned i = 0; i < kSymbols; ++i) {
            m_frequency[i] = 1;
            m_child[i] = kNodes + i;
            m_parent[kNodes + i] = i;
        }
        for (unsigned node = kSymbols, child = 0; node < kNodes; ++node, child += 2) {
            m_frequency[node] = m_frequency[child] + m_frequency[child + 1];
            m_child[node] = child;
            m_parent[child] = m_parent[child + 1] = node;
        }
        m_frequency[kNodes] = 0xFFFF;
        m_dictionary.fill(' ');
    }

    uint8_t byte()
    {
        if (++m_decoded > kMaxDecodedBytes)
            throw FormatException("Teledisk image exceeds decoded size limit");
        if (!m_compressed)
            return input();
        uint8_t value;
        if (m_matchLeft) {
            value = m_dictionary[m_matchPosition];
            m_matchPosition = (m_matchPosition + 1) & 4095;
            --m_matchLeft;
        } else {
            unsigned node = m_child[kRoot];
            while (node < kNodes)
                node = m_child[node + bit()];
            const unsigned symbol = node - kNodes;
            update(symbol);
            if (symbol < 256) {
                value = static_cast<uint8_t>(symbol);
            } else {
                m_matchPosition = (m_position - position() - 1) & 4095;
                m_matchLeft = symbol - 256 + 3;
                value = m_dictionary[m_matchPosition];
                m_matchPosition = (m_matchPosition + 1) & 4095;
                --m_matchLeft;
            }
        }
        m_dictionary[m_position] = value;
        m_position = (m_position + 1) & 4095;
        return value;
    }

    uint16_t word()
    {
        const uint8_t low = byte();
        return static_cast<uint16_t>(low | (byte() << 8));
    }

    std::vector<uint8_t> block(std::size_t size)
    {
        std::vector<uint8_t> result(size);
        for (auto &value : result)
            value = byte();
        return result;
    }

private:
    uint8_t input()
    {
        if (m_offset == m_bytes.size())
            throw FormatException("Truncated Teledisk image");
        return m_bytes[m_offset++];
    }
    unsigned bit()
    {
        if (!m_bits) {
            m_bitBuffer = input();
            m_bits = 8;
        }
        return (m_bitBuffer >> --m_bits) & 1;
    }
    unsigned position()
    {
        // Canonical prefix lengths for the upper six bits of the dictionary offset.
        unsigned code = 0, first = 0, symbol = 0;
        for (unsigned length = 1; length <= 8; ++length) {
            code = (code << 1) | bit();
            const unsigned count = length == 3 ? 1 : length == 4 ? 3 : length == 5 ? 8
                                 : length == 6 ? 12 : length == 7 ? 24 : length == 8 ? 16 : 0;
            if (code < first + count) {
                unsigned result = (symbol + code - first) << 6;
                for (unsigned i = 6; i; --i)
                    result |= bit() << (i - 1);
                return result;
            }
            symbol += count;
            first = (first + count) << 1;
        }
        throw FormatException("Invalid Teledisk dictionary offset");
    }
    void rebuild()
    {
        unsigned leaf = 0;
        for (unsigned i = 0; i < kNodes; ++i) {
            if (m_child[i] >= kNodes) {
                m_frequency[leaf] = (m_frequency[i] + 1) / 2;
                m_child[leaf++] = m_child[i];
            }
        }
        for (unsigned node = kSymbols, child = 0; node < kNodes; ++node, child += 2) {
            const unsigned frequency = m_frequency[child] + m_frequency[child + 1];
            unsigned at = node;
            while (at && frequency < m_frequency[at - 1])
                --at;
            std::move_backward(m_frequency.begin() + at, m_frequency.begin() + node, m_frequency.begin() + node + 1);
            std::move_backward(m_child.begin() + at, m_child.begin() + node, m_child.begin() + node + 1);
            m_frequency[at] = frequency;
            m_child[at] = child;
        }
        for (unsigned i = 0; i < kNodes; ++i) {
            const unsigned child = m_child[i];
            m_parent[child] = i;
            if (child < kNodes)
                m_parent[child + 1] = i;
        }
    }
    void update(unsigned symbol)
    {
        if (m_frequency[kRoot] == 0x8000)
            rebuild();
        unsigned node = m_parent[symbol + kNodes];
        do {
            const unsigned frequency = ++m_frequency[node];
            unsigned next = node + 1;
            if (frequency > m_frequency[next]) {
                while (frequency > m_frequency[++next]) {}
                --next;
                m_frequency[node] = std::exchange(m_frequency[next], frequency);
                std::swap(m_child[node], m_child[next]);
                for (unsigned index : {node, next}) {
                    const unsigned child = m_child[index];
                    m_parent[child] = index;
                    if (child < kNodes)
                        m_parent[child + 1] = index;
                }
                node = next;
            }
            node = m_parent[node];
        } while (node);
    }

    static constexpr unsigned kSymbols = 314;
    static constexpr unsigned kNodes = 2 * kSymbols - 1;
    static constexpr unsigned kRoot = kNodes - 1;
    std::span<const uint8_t> m_bytes;
    bool m_compressed;
    std::size_t m_offset = 0, m_decoded = 0;
    unsigned m_bitBuffer = 0, m_bits = 0;
    std::array<unsigned, kNodes + 1> m_frequency{};
    std::array<unsigned, kNodes> m_child{};
    std::array<unsigned, kNodes + kSymbols> m_parent{};
    std::array<uint8_t, 4096> m_dictionary{};
    unsigned m_position = 4096 - 60;
    unsigned m_matchPosition = 0, m_matchLeft = 0;
};

std::vector<uint8_t> decodeSector(std::span<const uint8_t> encoded, std::size_t size)
{
    if (encoded.empty())
        throw FormatException("Missing Teledisk sector encoding");
    const auto method = encoded.front();
    encoded = encoded.subspan(1);
    std::vector<uint8_t> data;
    const auto repeat = [&](std::span<const uint8_t> pattern, std::size_t count) {
        if (pattern.empty() || count > (size - data.size()) / pattern.size())
            throw FormatException("Teledisk sector expands past its size");
        for (std::size_t i = 0; i < count; ++i)
            data.insert(data.end(), pattern.begin(), pattern.end());
    };
    if (method == 0) {
        repeat(encoded, 1);
    } else if (method == 1) {
        if (encoded.size() != 4)
            throw FormatException("Invalid Teledisk repeated-word encoding");
        repeat(encoded.subspan(2), encoded[0] | (encoded[1] << 8));
    } else if (method == 2) {
        while (!encoded.empty()) {
            if (encoded.size() < 2 || encoded[0] > 14)
                throw FormatException("Invalid Teledisk pattern encoding");
            const std::size_t length = encoded[0] ? std::size_t{1} << encoded[0] : encoded[1];
            const std::size_t count = encoded[0] ? encoded[1] : 1;
            encoded = encoded.subspan(2);
            if (length > encoded.size())
                throw FormatException("Truncated Teledisk sector pattern");
            repeat(encoded.first(length), count);
            encoded = encoded.subspan(length);
        }
    } else {
        throw FormatException("Unknown Teledisk sector encoding");
    }
    if (data.size() != size)
        throw FormatException("Incorrect Teledisk decoded sector size");
    return data;
}

} // namespace

void DiskImage::parseTeledisk(std::span<const uint8_t> raw)
{
    if (raw.size() < 12 || !((raw[0] == 'T' && raw[1] == 'D') || (raw[0] == 't' && raw[1] == 'd')))
        throw FormatException("Invalid Teledisk header");
    if (crc16(raw.first(10)) != (raw[10] | (raw[11] << 8)))
        throw FormatException("Incorrect Teledisk header CRC");
    if (raw[2] != 0)
        throw FormatException("Teledisk multi-volume continuation is not a standalone disk");
    if (raw[0] == 't' && raw[4] < 20)
        throw FormatException("Teledisk 1.x advanced compression is unsupported");
    TelediskReader reader(raw.subspan(12), raw[0] == 't');
    if (raw[7] & 0x80) {
        const auto crc = reader.word();
        auto comment = reader.block(8);
        const std::size_t length = comment[0] | (comment[1] << 8);
        const auto text = reader.block(length);
        comment.insert(comment.end(), text.begin(), text.end());
        if (crc16(comment) != crc)
            throw FormatException("Incorrect Teledisk comment CRC");
    }
    std::vector<Sector> sectors;
    std::size_t decodedSize = 0;
    for (;;) {
        const uint8_t count = reader.byte();
        if (count == 0xFF)
            break;
        const uint8_t cylinder = reader.byte(), head = reader.byte();
        const auto crc = reader.byte();
        if (static_cast<uint8_t>(crc16(std::array{count, cylinder, head})) != crc)
            throw FormatException("Incorrect Teledisk track CRC");
        if (cylinder == 255 || (head & 0x7F) > 1)
            throw FormatException("Invalid Teledisk physical track");
        for (unsigned i = 0; i < count; ++i) {
            const auto header = reader.block(6);
            if (header[3] > 7)
                throw FormatException("Invalid Teledisk sector size");
            const std::size_t size = std::size_t{128} << header[3];
            if (size > kMaxDecodedBytes - decodedSize)
                throw FormatException("Teledisk sectors exceed size limit");
            decodedSize += size;
            const uint8_t flags = header[4];
            std::vector<uint8_t> data(size, 0);
            if (!(flags & 0x30)) {
                const auto length = reader.word();
                const auto encoded = reader.block(length);
                data = decodeSector(encoded, size);
                if (static_cast<uint8_t>(crc16(data)) != header[5])
                    throw FormatException("Incorrect Teledisk sector CRC");
            }
            // A bogus record for data without an ID must not become an addressable sector.
            if (flags & 0x40)
                continue;
            sectors.push_back(Sector{
                .physTrack = cylinder, .physSide = static_cast<uint8_t>(head & 1),
                .track = header[0], .side = header[1], .sectorId = header[2], .sizeCode = header[3],
                .st1 = static_cast<uint8_t>(((flags & 2) ? 0x20 : 0) | ((flags & 0x20) ? 1 : 0)),
                .st2 = static_cast<uint8_t>(((flags & 2) ? 0x20 : 0) | ((flags & 4) ? 0x40 : 0) | ((flags & 0x20) ? 1 : 0)),
                .mfm = !(head & 0x80),
                .data = std::move(data)
            });
        }
    }
    m_sectors = std::move(sectors);
}

} // namespace zxkit
