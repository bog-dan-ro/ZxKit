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

#include "zxkit/media/ScreenshotFormat.h"

#include "zxkit/media/Compression.h"

namespace zxkit {

std::vector<uint8_t> encodePng(const VideoFrame &frame)
{
    const uint32_t w = frame.width();
    const uint32_t h = frame.height();

    // Build raw filtered scanlines (filter byte 0 = None per row)
    std::vector<uint8_t> rawScanlines;
    rawScanlines.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 4));
    for (uint32_t y = 0; y < h; ++y) {
        rawScanlines.push_back(0x00); // Filter type 0 (None)
        for (const uint32_t rgba : frame.scanline(y)) { // 0xAABBGGRR
            rawScanlines.push_back(static_cast<uint8_t>(rgba));
            rawScanlines.push_back(static_cast<uint8_t>(rgba >> 8));
            rawScanlines.push_back(static_cast<uint8_t>(rgba >> 16));
            rawScanlines.push_back(static_cast<uint8_t>(rgba >> 24));
        }
    }

    const auto idatPayload = zlibCompress(rawScanlines);

    auto appendBe32 = [](std::vector<uint8_t> &buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>(val >> 24));
        buf.push_back(static_cast<uint8_t>(val >> 16));
        buf.push_back(static_cast<uint8_t>(val >> 8));
        buf.push_back(static_cast<uint8_t>(val));
    };

    auto appendChunk = [&](std::vector<uint8_t> &buf, std::string_view type, std::span<const uint8_t> data) {
        appendBe32(buf, static_cast<uint32_t>(data.size()));
        const std::size_t crcStart = buf.size();
        buf.insert(buf.end(), type.begin(), type.end());
        buf.insert(buf.end(), data.begin(), data.end());
        const uint32_t crc = computeCrc32(std::span<const uint8_t>(buf).subspan(crcStart, 4 + data.size()));
        appendBe32(buf, crc);
    };

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    // IHDR chunk (13 bytes)
    std::vector<uint8_t> ihdr;
    appendBe32(ihdr, w);
    appendBe32(ihdr, h);
    ihdr.push_back(8); // Bit depth 8
    ihdr.push_back(6); // Color type 6 (RGBA)
    ihdr.push_back(0); // Compression
    ihdr.push_back(0); // Filter
    ihdr.push_back(0); // Interlace
    appendChunk(png, "IHDR", ihdr);

    // IDAT chunk
    appendChunk(png, "IDAT", idatPayload);

    // IEND chunk
    appendChunk(png, "IEND", {});

    return png;
}

} // namespace zxkit
