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

#include "zxkit/debug/RewindBuffer.h"

#include "zxkit/core/Exception.h"

#include <algorithm>

namespace zxkit {

void RewindBuffer::setCapacity(std::size_t maxFrames)
{
    m_maxFrames = maxFrames;
    evict();
}

void RewindBuffer::setMemoryBudget(std::size_t bytes)
{
    m_maxBytes = bytes;
    evict();
}

void RewindBuffer::clear() noexcept
{
    m_frames.clear();
    m_totalBytes = 0;
}

void RewindBuffer::evict()
{
    while (!m_frames.empty() && (m_frames.size() > m_maxFrames || m_totalBytes > m_maxBytes)) {
        m_totalBytes -= m_frames.front().size();
        m_frames.pop_front();
    }
}

void RewindBuffer::pushSnapshot(std::vector<uint8_t> stateBytes)
{
    if (m_maxFrames == 0 || stateBytes.size() > m_maxBytes)
        return;
    m_totalBytes += stateBytes.size();
    m_frames.push_back(std::move(stateBytes));
    evict();
}

std::vector<uint8_t> RewindBuffer::popStepsBack(std::size_t steps)
{
    if (m_frames.empty())
        throw DebuggerException("Rewind buffer is empty");

    std::vector<uint8_t> chosen;
    const std::size_t count = std::min(steps, m_frames.size());
    for (std::size_t i = 0; i < count; ++i) {
        chosen = std::move(m_frames.back());
        m_frames.pop_back();
        m_totalBytes -= chosen.size();
    }
    return chosen;
}

} // namespace zxkit
