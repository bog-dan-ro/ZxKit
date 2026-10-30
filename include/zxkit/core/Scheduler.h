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
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

namespace zxkit {

using EventId = uint64_t;

class Scheduler {
public:
    using Callback = std::function<void(uint64_t triggerCycle)>;

    Scheduler() = default;

    void clear();
    EventId scheduleAt(uint64_t targetCycle, Callback cb);
    EventId scheduleIn(uint64_t currentCycle, uint64_t deltaCycles, Callback cb);
    void cancel(EventId id);

    uint64_t nextEventCycle() const noexcept
    {
        return m_nextEventCycle;
    }

    /// Fires all events due at or before `currentCycle`.
    void processDue(uint64_t currentCycle);

private:
    struct Entry {
        uint64_t targetCycle = 0;
        EventId id = 0;
        Callback callback;

        constexpr bool operator>(const Entry &other) const noexcept
        {
            if (targetCycle != other.targetCycle)
                return targetCycle > other.targetCycle;
            return id > other.id;
        }
    };

    void refreshNextCycle();

    std::vector<Entry> m_heap;
    std::unordered_set<EventId> m_cancelled;
    EventId m_nextSeq = 0;
    uint64_t m_nextEventCycle = std::numeric_limits<uint64_t>::max();
};

} // namespace zxkit
