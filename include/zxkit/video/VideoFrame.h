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

#include "zxkit/core/Shared.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace zxkit {

/// Visible margins around the 256x192 ULA paper, in standard pixels/lines.
/// Timex doubles horizontal pixels in hi-res; the Next doubles both axes.
/// These are host presentation preferences, not emulated/snapshot state.
struct BorderMargins {
    uint16_t left = 32;
    uint16_t top = 32;
    uint16_t right = 32;
    uint16_t bottom = 32;

    bool operator==(const BorderMargins &) const = default;
};

/// Packs R, G, B, A into little-endian RGBA8888 (0xAABBGGRR in uint32_t on LE).
constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) noexcept
{
    return (static_cast<uint32_t>(a) << 24)
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(g) << 8)
         | static_cast<uint32_t>(r);
}

inline constexpr std::array<uint32_t, 16> kStandardZxPalette = {
    packRgba(  0,   0,   0), // 0: Black
    packRgba(  0,   0, 192), // 1: Blue
    packRgba(192,   0,   0), // 2: Red
    packRgba(192,   0, 192), // 3: Magenta
    packRgba(  0, 192,   0), // 4: Green
    packRgba(  0, 192, 192), // 5: Cyan
    packRgba(192, 192,   0), // 6: Yellow
    packRgba(192, 192, 192), // 7: White
    packRgba(  0,   0,   0), // 8: Bright Black
    packRgba(  0,   0, 255), // 9: Bright Blue
    packRgba(255,   0,   0), // 10: Bright Red
    packRgba(255,   0, 255), // 11: Bright Magenta
    packRgba(  0, 255,   0), // 12: Bright Green
    packRgba(  0, 255, 255), // 13: Bright Cyan
    packRgba(255, 255,   0), // 14: Bright Yellow
    packRgba(255, 255, 255)  // 15: Bright White
};

/// Converts a 9-bit RGB333 colour (RRRGGGBBB) to RGBA8888.
constexpr uint32_t rgb333ToRgba(uint16_t rgb333) noexcept
{
    const uint8_t r3 = (rgb333 >> 6) & 0x07;
    const uint8_t g3 = (rgb333 >> 3) & 0x07;
    const uint8_t b3 = rgb333 & 0x07;
    const uint8_t r8 = static_cast<uint8_t>((r3 << 5) | (r3 << 2) | (r3 >> 1));
    const uint8_t g8 = static_cast<uint8_t>((g3 << 5) | (g3 << 2) | (g3 >> 1));
    const uint8_t b8 = static_cast<uint8_t>((b3 << 5) | (b3 << 2) | (b3 >> 1));
    return packRgba(r8, g8, b8);
}

class VideoFrame {
public:
    struct PaperRect {
        uint32_t x = 32;
        uint32_t y = 32;
        uint32_t width = 256;
        uint32_t height = 192;
        bool operator==(const PaperRect &) const = default;
    };
    /// The ULA's paper rectangle, in this frame's pixel coordinates. Next layers
    /// and CoBra's extra columns can also draw outside this reference rectangle.
    PaperRect paperRect() const noexcept { return m_paperRect; }
    void setPaperRect(PaperRect rect) noexcept { m_paperRect = rect; }
    static constexpr uint32_t kDefaultWidth  = 320;
    static constexpr uint32_t kDefaultHeight = 256;

    VideoFrame();
    VideoFrame(uint32_t width, uint32_t height);

    void resize(uint32_t width, uint32_t height);
    void clear(uint32_t rgba = packRgba(0, 0, 0)) noexcept;

    uint32_t width() const noexcept { return m_width; }
    uint32_t height() const noexcept { return m_height; }
    uint64_t frameNumber() const noexcept { return m_frameNumber; }
    void setFrameNumber(uint64_t num) noexcept { m_frameNumber = num; }

    std::span<const uint32_t> pixels() const noexcept { return m_pixels; }
    std::span<uint32_t> pixels() noexcept { return m_pixels; }

    std::span<uint32_t> scanline(uint32_t y) noexcept
    {
        return std::span(m_pixels).subspan(static_cast<std::size_t>(y) * m_width, m_width);
    }

    std::span<const uint32_t> scanline(uint32_t y) const noexcept
    {
        return std::span(m_pixels).subspan(static_cast<std::size_t>(y) * m_width, m_width);
    }

    void putPixel(uint32_t x, uint32_t y, uint32_t rgba) noexcept
    {
        if (x < m_width && y < m_height)
            m_pixels[static_cast<std::size_t>(y) * m_width + x] = rgba;
    }

    uint32_t pixel(uint32_t x, uint32_t y) const noexcept
    {
        if (x < m_width && y < m_height)
            return m_pixels[static_cast<std::size_t>(y) * m_width + x];
        return 0;
    }

private:
    PaperRect m_paperRect;
    uint32_t m_width = kDefaultWidth;
    uint32_t m_height = kDefaultHeight;
    uint64_t m_frameNumber = 0;
    std::vector<uint32_t> m_pixels;
};

/// A published frame. The emulator never writes a frame again once it is published,
/// and recycles it only after every holder released it.
using VideoFramePtr = Shared<VideoFrame>;

} // namespace zxkit
