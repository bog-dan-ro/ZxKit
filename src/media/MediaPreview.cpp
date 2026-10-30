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

#include "zxkit/media/MediaPreview.h"

#include "zxkit/core/Exception.h"
#include "zxkit/media/Compression.h"
#include "zxkit/media/SnapshotFormats.h"
#include "zxkit/media/TapeFormats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

namespace zxkit {

namespace {

std::string normalizedExt(std::string_view fileName)
{
    std::string ext = fileName.starts_with('.')
        ? std::string(fileName)
        : std::filesystem::path(fileName).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

MediaClass classifyMediaFile(std::string_view fileName, std::span<const uint8_t> data) noexcept
{
    const std::string ext = normalizedExt(fileName);
    if (ext == ".sna" || ext == ".z80" || ext == ".szx" || ext == ".nex" || ext == ".snx" || ext == ".sp" || ext == ".snp" || ext == ".zxs")
        return MediaClass::Snapshot;
    if (ext == ".tap" || ext == ".tzx" || ext == ".pzx" || ext == ".csw" || ext == ".wav")
        return MediaClass::Tape;
    if (ext == ".dsk" || ext == ".edsk" || ext == ".trd" || ext == ".scl" || ext == ".mgt" || ext == ".img" || ext == ".opd" || ext == ".d40" || ext == ".d80")
        return MediaClass::Disk;
    if (ext == ".mdr")
        return MediaClass::Microdrive;
    if (ext == ".rzx")
        return MediaClass::Recording;
    if (ext == ".scr")
        return MediaClass::Screen;
    if (ext == ".pok")
        return MediaClass::Poke;
    if (ext == ".zip" || ext == ".gz")
        return MediaClass::Archive;

    if (data.size() >= 8) {
        if (std::memcmp(data.data(), "ZXST", 4) == 0 || std::memcmp(data.data(), "Next", 4) == 0)
            return MediaClass::Snapshot;
        if (std::memcmp(data.data(), "ZXTape!", 7) == 0 || std::memcmp(data.data(), "PZXT", 4) == 0)
            return MediaClass::Tape;
        if (std::memcmp(data.data(), "RZX!", 4) == 0)
            return MediaClass::Recording;
        if (std::memcmp(data.data(), "MV - CPC", 8) == 0 || std::memcmp(data.data(), "EXTENDED", 8) == 0 || std::memcmp(data.data(), "SINCLAIR", 8) == 0)
            return MediaClass::Disk;
    }
    return MediaClass::Unknown;
}

VideoFrame renderScrToVideoFrame(std::span<const uint8_t> scr6912)
{
    if (scr6912.size() < 6912)
        throw FormatException("SCR buffer must be at least 6912 bytes");

    VideoFrame frame(256, 192);
    for (uint32_t y = 0; y < 192; ++y) {
        auto line = frame.scanline(y);
        const uint16_t pixelLineOffset = static_cast<uint16_t>(
            ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2));
        const uint16_t attrLineOffset = static_cast<uint16_t>(0x1800 + ((y >> 3) << 5));

        for (uint32_t col = 0; col < 32; ++col) {
            const uint8_t bitmap = scr6912[pixelLineOffset + col];
            const uint8_t attr   = scr6912[attrLineOffset + col];
            const uint8_t bright = (attr & 0x40) ? 8 : 0;
            const uint32_t ink   = kStandardZxPalette[(attr & 0x07) + bright];
            const uint32_t paper = kStandardZxPalette[((attr >> 3) & 0x07) + bright];

            const auto dst = line.subspan(col * 8, 8);
            for (uint32_t x = 0; x < 8; ++x)
                dst[x] = ((bitmap << x) & 0x80) ? ink : paper;
        }
    }
    return frame;
}

VideoFrame extractMediaPreview(std::span<const uint8_t> fileBytes, std::string_view fileName)
{
    const std::string ext = normalizedExt(fileName);

    if (ext == ".zip" || (fileBytes.size() >= 4 && fileBytes[0] == 'P' && fileBytes[1] == 'K' && fileBytes[2] == 0x03 && fileBytes[3] == 0x04)) {
        const auto unzipped = extractFirstFromZip(fileBytes);
        return extractMediaPreview(unzipped.data, unzipped.filename);
    }

    if (ext == ".scr" || fileBytes.size() == 6912) {
        return renderScrToVideoFrame(fileBytes);
    }

    if (ext == ".sna" && fileBytes.size() >= 27 + 6912) {
        return renderScrToVideoFrame(fileBytes.subspan(27, 6912));
    }

    if (ext == ".z80" && fileBytes.size() >= 30) {
        const uint16_t pc = static_cast<uint16_t>(fileBytes[6] | (fileBytes[7] << 8));
        uint8_t flags1 = fileBytes[12];
        if (flags1 == 0xFF)
            flags1 = 1;

        if (pc != 0) {
            // Version 1 Z80
            const auto payload = fileBytes.subspan(30);
            if (flags1 & 0x20) {
                const auto unpacked = SnapshotFormats::decompressZ80Block(payload, 6912, true);
                return renderScrToVideoFrame(unpacked);
            }
            return renderScrToVideoFrame(payload);
        }

        // Version 2/3 Z80: find page 8 (48K 0x4000..0x7FFF or 128K bank 5)
        if (fileBytes.size() >= 34) {
            const uint16_t extLen = static_cast<uint16_t>(fileBytes[30] | (fileBytes[31] << 8));
            std::size_t pos = 32 + extLen;
            while (pos + 3 <= fileBytes.size()) {
                const uint16_t compLen = static_cast<uint16_t>(fileBytes[pos] | (fileBytes[pos + 1] << 8));
                const uint8_t pageCode = fileBytes[pos + 2];
                pos += 3;
                const std::size_t chunkLen = (compLen == 0xFFFF) ? 16384u : compLen;
                if (pos + chunkLen > fileBytes.size())
                    break;
                if (pageCode == 8) {
                    if (compLen == 0xFFFF)
                        return renderScrToVideoFrame(fileBytes.subspan(pos, 6912));
                    const auto unpacked = SnapshotFormats::decompressZ80Block(fileBytes.subspan(pos, compLen), 6912, false);
                    return renderScrToVideoFrame(unpacked);
                }
                pos += chunkLen;
            }
        }
    }

    if (ext == ".szx" && fileBytes.size() >= 8 && std::memcmp(fileBytes.data(), "ZXST", 4) == 0) {
        std::size_t pos = 8;
        while (pos + 8 <= fileBytes.size()) {
            const uint32_t blockSize = static_cast<uint32_t>(fileBytes[pos + 4])
                                     | (static_cast<uint32_t>(fileBytes[pos + 5]) << 8)
                                     | (static_cast<uint32_t>(fileBytes[pos + 6]) << 16)
                                     | (static_cast<uint32_t>(fileBytes[pos + 7]) << 24);
            if (pos + 8 + blockSize > fileBytes.size())
                break;
            if (std::memcmp(fileBytes.data() + pos, "RAMP", 4) == 0 && blockSize >= 3) {
                const uint16_t flags = static_cast<uint16_t>(fileBytes[pos + 8] | (fileBytes[pos + 9] << 8));
                const uint8_t pageNo = fileBytes[pos + 10];
                if (pageNo == 5) {
                    const auto rawPage = fileBytes.subspan(pos + 11, blockSize - 3);
                    if (flags & 0x01) {
                        const auto unpacked = zlibDecompress(rawPage, 16384);
                        return renderScrToVideoFrame(unpacked);
                    }
                    return renderScrToVideoFrame(rawPage);
                }
            }
            pos += 8 + blockSize;
        }
    }

    if (ext == ".tap" || ext == ".tzx") {
        const auto blocks = ext == ".tap" ? TapeFormats::parseTap(fileBytes) : TapeFormats::parseTzx(fileBytes);
        for (const auto &blk : blocks) {
            // Standard 6912-byte SCREEN$ tape block has 1 flag byte + 6912 data bytes + 1 checksum = 6914 bytes
            if (blk.data.size() == 6914) {
                return renderScrToVideoFrame(std::span<const uint8_t>(blk.data.data() + 1, 6912));
            }
        }
    }

    throw FormatException(std::format("Preview extraction not available for '{}'", fileName));
}

} // namespace zxkit
