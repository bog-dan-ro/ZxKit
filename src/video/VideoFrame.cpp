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

#include "zxkit/video/VideoFrame.h"

#include <algorithm>

namespace zxkit {

VideoFrame::VideoFrame()
    : VideoFrame(kDefaultWidth, kDefaultHeight)
{
}

VideoFrame::VideoFrame(uint32_t width, uint32_t height)
    : m_width(width)
    , m_height(height)
    , m_pixels(static_cast<std::size_t>(width) * height, packRgba(0, 0, 0))
{
}

void VideoFrame::resize(uint32_t width, uint32_t height)
{
    if (m_width == width && m_height == height)
        return;
    m_width = width;
    m_height = height;
    m_pixels.assign(static_cast<std::size_t>(width) * height, packRgba(0, 0, 0));
}

void VideoFrame::clear(uint32_t rgba) noexcept
{
    std::ranges::fill(m_pixels, rgba);
}

} // namespace zxkit
