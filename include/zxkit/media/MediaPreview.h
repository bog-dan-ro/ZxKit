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

#include "zxkit/video/VideoFrame.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace zxkit {

enum class MediaClass : uint8_t {
    Unknown = 0,
    Snapshot,
    Tape,
    Disk,
    Microdrive,
    Recording,
    Screen,
    Poke,
    Archive
};

/// Classifies a media file by filename extension and/or magic bytes.
MediaClass classifyMediaFile(std::string_view fileName, std::span<const uint8_t> data = {}) noexcept;

/// Renders a standard 6912-byte ZX Spectrum screen (256x192 bitmap + 768 attributes)
/// into a 256x192 RGBA8888 VideoFrame without needing a running Engine instance.
VideoFrame renderScrToVideoFrame(std::span<const uint8_t> scr6912);

/// Standalone fast preview extractor for .scr, .sna, .z80, .szx, .tap, .tzx, .nex, and .zip files.
VideoFrame extractMediaPreview(std::span<const uint8_t> fileBytes, std::string_view fileName);

} // namespace zxkit
