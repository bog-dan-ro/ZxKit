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

#include "zxkit/bus/IoBus.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace zxkit {

IoBus &IoBus::operator=(const IoBus &other)
{
    if (this == &other)
        return *this;
    m_candidatesDirty = true; // Cached pointers belong to this bus's own handlers.
    m_handlers = other.m_handlers;
    m_observers = other.m_observers;
    m_floatingBusFn = other.m_floatingBusFn;
    m_watchpointCb = other.m_watchpointCb;
    m_nextRegOpcodeCb = other.m_nextRegOpcodeCb;
    m_rzxReadOverride = other.m_rzxReadOverride;
    m_rzxReadRecord = other.m_rzxReadRecord;
    m_nextId = other.m_nextId;
    m_writeDeclined = other.m_writeDeclined;
    return *this;
}

void IoBus::clear()
{
    m_handlers.clear();
    m_candidatesDirty = true;
    m_observers.clear();
    m_floatingBusFn = nullptr;
    m_nextRegOpcodeCb = nullptr;
}

uint64_t IoBus::registerHandler(std::string ownerId, uint16_t mask, uint16_t value, ReadHandler rd, WriteHandler wr,
                                int priority, bool exclusiveWrite)
{
    const uint64_t id = ++m_nextId;
    const int effectivePriority = (priority != 0) ? priority : std::popcount(mask);
    m_handlers.push_back(HandlerEntry{.id = id,
                                      .ownerId = std::move(ownerId),
                                      .mask = mask,
                                      .value = value,
                                      .priority = effectivePriority,
                                      .exclusiveWrite = exclusiveWrite,
                                      .read = std::move(rd),
                                      .write = std::move(wr)});
    std::ranges::stable_sort(m_handlers,
                             [](const HandlerEntry &a, const HandlerEntry &b) { return a.priority > b.priority; });
    m_candidatesDirty = true;
    return id;
}

uint64_t IoBus::registerSimpleHandler(std::string ownerId, uint16_t mask, uint16_t value, SimpleReadFn rd,
                                      WriteHandler wr, int priority, bool exclusiveWrite)
{
    ReadHandler fullRd;
    if (rd) {
        fullRd = [fn = std::move(rd)](uint16_t port) -> IoReadResult { return IoReadResult{fn(port), 0xFF}; };
    }
    return registerHandler(std::move(ownerId), mask, value, std::move(fullRd), std::move(wr), priority, exclusiveWrite);
}

void IoBus::unregisterOwner(std::string_view ownerId)
{
    m_candidatesDirty = true;
    std::erase_if(m_handlers, [ownerId](const HandlerEntry &e) { return e.ownerId == ownerId; });
}

void IoBus::addObserver(IoObserver observer) { m_observers.push_back(std::move(observer)); }

void IoBus::setFloatingBusCallback(FloatingBusFn fn) { m_floatingBusFn = std::move(fn); }

void IoBus::setWatchpointCallback(WatchpointCb cb) { m_watchpointCb = std::move(cb); }

void IoBus::setNextRegOpcodeCallback(NextRegOpcodeCb cb) { m_nextRegOpcodeCb = std::move(cb); }

void IoBus::setRzxCallbacks(std::function<std::optional<uint8_t>(uint16_t)> playbackOverride, std::function<void(uint8_t)> recordHook)
{
    m_rzxReadOverride = std::move(playbackOverride);
    m_rzxReadRecord = std::move(recordHook);
}

void IoBus::declineWrite() noexcept { m_writeDeclined = true; }

uint8_t IoBus::read(uint16_t port)
{
    if (m_rzxReadOverride) {
        if (const auto val = m_rzxReadOverride(port)) {
            if (m_watchpointCb)
                m_watchpointCb(port, *val, false);
            return *val;
        }
    }

    const uint8_t val = peekPort(port, true);
    if (m_rzxReadRecord)
        m_rzxReadRecord(val);
    if (m_watchpointCb)
        m_watchpointCb(port, val, false);
    return val;
}

void IoBus::rebuildCandidates() const
{
    if (!m_dispatch)
        m_dispatch = std::make_unique<DispatchCache>();
    for (unsigned low = 0; low < 256; ++low) {
        auto &reads = m_dispatch->reads[low];
        auto &writes = m_dispatch->writes[low];
        reads.clear();
        writes.clear();
        for (const auto &handler : m_handlers) {
            if ((low & (handler.mask & 0xFF)) != (handler.value & 0xFF))
                continue;
            if (handler.read)
                reads.push_back(&handler);
            if (handler.write)
                writes.push_back(&handler);
        }
    }
    m_candidatesDirty = false;
}

uint8_t IoBus::peekPort(uint16_t port, bool notifyObservers) const
{
    uint8_t combinedValue = 0xFF;
    uint8_t combinedAttached = 0x00;

    const auto combine = [&](const HandlerEntry &h) {
        if ((port & h.mask) == h.value) {
            // Handlers with a negative priority (sorted last) are default
            // responders, e.g. a machine's idle joystick port: they only
            // answer when nothing else does
            if (h.priority < 0 && combinedAttached != 0)
                return false;
            const IoReadResult res = h.read(port);
            if (res.attachedMask != 0) {
                combinedValue &= static_cast<uint8_t>(~res.attachedMask | res.value);
                combinedAttached |= res.attachedMask;
                if (res.exclusive)
                    return false;
            }
        }
        return true;
    };
    if (m_handlers.size() <= 4) {
        for (const auto &h : m_handlers)
            if (h.read && !combine(h))
                break;
    } else {
        if (m_candidatesDirty) [[unlikely]]
            rebuildCandidates();
        for (const auto *h : m_dispatch->reads[uint8_t(port)])
            if (!combine(*h))
                break;
    }

    if (combinedAttached != 0xFF) {
        const uint8_t floatVal = m_floatingBusFn ? m_floatingBusFn(port) : 0xFF;
        combinedValue = static_cast<uint8_t>((combinedValue & combinedAttached) | (floatVal & ~combinedAttached));
    }

    if (notifyObservers) {
        for (const auto &obs : m_observers)
            obs(port, combinedValue, false);
    }
    return combinedValue;
}

void IoBus::write(uint16_t port, uint8_t value)
{
    for (const auto &obs : m_observers)
        obs(port, value, true);

    const auto apply = [&](const HandlerEntry &h) {
        if ((port & h.mask) == h.value) {
            m_writeDeclined = false;
            h.write(port, value);
            if (!m_writeDeclined && h.exclusiveWrite)
                return false;
        }
        return true;
    };
    if (m_handlers.size() <= 4) {
        for (const auto &h : m_handlers)
            if (h.write && !apply(h))
                break;
    } else {
        if (m_candidatesDirty) [[unlikely]]
            rebuildCandidates();
        for (const auto *h : m_dispatch->writes[uint8_t(port)])
            if (!apply(*h))
                break;
    }

    if (m_watchpointCb)
        m_watchpointCb(port, value, true);
}

void IoBus::writeNextRegOpcode(uint8_t reg, uint8_t value)
{
    if (m_nextRegOpcodeCb) {
        m_nextRegOpcodeCb(reg, value);
    } else {
        write(0x243B, reg);
        write(0x253B, value);
    }
}

} // namespace zxkit
