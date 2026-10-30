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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace zxkit {

std::vector<uint8_t> zlibCompress(std::span<const uint8_t> input, int level = -1);
std::vector<uint8_t> zlibDecompress(std::span<const uint8_t> input, std::size_t expectedSize = 16384);
uint32_t computeCrc32(std::span<const uint8_t> data) noexcept;

struct ZipExtractedEntry {
    std::string filename;
    std::vector<uint8_t> data;
};

/// Extracts the first file from a standard ZIP archive using raw deflate / store.
ZipExtractedEntry extractFirstFromZip(std::span<const uint8_t> zipBytes);

/// Progress callback: (bytes written, total bytes). Returning false cancels.
using ZipProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

/// Streams the entry whose file name (without directories, case-insensitive)
/// is `entryName` out of the ZIP file `zipPath` into `outPath`, without
/// holding it in memory (e.g. a 1GB SD card image). The CRC is verified.
/// Throws IoException / FormatException; a cancelled extraction removes `outPath`.
void extractZipEntryToFile(const std::filesystem::path &zipPath,
                           std::string_view entryName,
                           const std::filesystem::path &outPath,
                           const ZipProgressFn &progress = {});

} // namespace zxkit
