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

#include "zxkit/core/Scheduler.h"

#include <algorithm>
#include <utility>

namespace zxkit {

void Scheduler::clear()
{
    m_heap.clear();
    m_cancelled.clear();
    m_nextEventCycle = std::numeric_limits<uint64_t>::max();
}

EventId Scheduler::scheduleAt(uint64_t targetCycle, Callback cb)
{
    const EventId id = ++m_nextSeq;
    m_heap.push_back(Entry{targetCycle, id, std::move(cb)});
    std::ranges::push_heap(m_heap, std::greater<>{});
    if (targetCycle < m_nextEventCycle)
        m_nextEventCycle = targetCycle;
    return id;
}

EventId Scheduler::scheduleIn(uint64_t currentCycle, uint64_t deltaCycles, Callback cb)
{
    return scheduleAt(currentCycle + deltaCycles, std::move(cb));
}

void Scheduler::cancel(EventId id)
{
    if (id != 0)
        m_cancelled.insert(id);
}

void Scheduler::processDue(uint64_t currentCycle)
{
    while (!m_heap.empty() && m_heap.front().targetCycle <= currentCycle) {
        std::ranges::pop_heap(m_heap, std::greater<>{});
        Entry top = std::move(m_heap.back());
        m_heap.pop_back();

        if (!m_cancelled.empty()) {
            if (auto it = m_cancelled.find(top.id); it != m_cancelled.end()) {
                m_cancelled.erase(it);
                continue;
            }
        }
        if (top.callback)
            top.callback(top.targetCycle);
    }
    refreshNextCycle();
}

void Scheduler::refreshNextCycle()
{
    while (!m_heap.empty()) {
        if (!m_cancelled.empty()) {
            if (auto it = m_cancelled.find(m_heap.front().id); it != m_cancelled.end()) {
                m_cancelled.erase(it);
                std::ranges::pop_heap(m_heap, std::greater<>{});
                m_heap.pop_back();
                continue;
            }
        }
        m_nextEventCycle = m_heap.front().targetCycle;
        return;
    }
    m_nextEventCycle = std::numeric_limits<uint64_t>::max();
}

} // namespace zxkit
