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

#include <cstdint>
#include <deque>
#include <vector>

namespace zxkit {

/// Ring buffer of serialized machine states for instant frame-level time travel / rewind.
class RewindBuffer
{
public:
    explicit RewindBuffer(std::size_t maxFrames = 300) noexcept : m_maxFrames(maxFrames) { }

    void setCapacity(std::size_t maxFrames);
    /// Upper bound for the memory used by the stored states; the oldest frames
    /// are dropped first (a ZX Next state is ~2MB, a 48K one ~50KB).
    void setMemoryBudget(std::size_t bytes);

    std::size_t capacity() const noexcept
    {
        return m_maxFrames;
    }
    std::size_t memoryBudget() const noexcept
    {
        return m_maxBytes;
    }
    std::size_t memoryUsed() const noexcept
    {
        return m_totalBytes;
    }
    std::size_t size() const noexcept
    {
        return m_frames.size();
    }
    bool empty() const noexcept
    {
        return m_frames.empty();
    }

    void clear() noexcept;
    void pushSnapshot(std::vector<uint8_t> stateBytes);
    std::vector<uint8_t> popStepsBack(std::size_t steps = 1);

private:
    void evict();

    std::size_t m_maxFrames = 300;
    std::size_t m_maxBytes = 128 * 1024 * 1024;
    std::size_t m_totalBytes = 0;
    std::deque<std::vector<uint8_t>> m_frames;
};

} // namespace zxkit
