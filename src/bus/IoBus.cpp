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

void IoBus::clear()
{
    m_handlers.clear();
    m_observers.clear();
    m_floatingBusFn = nullptr;
    m_nextRegOpcodeCb = nullptr;
}

uint64_t IoBus::registerHandler(std::string ownerId,
                                uint16_t mask,
                                uint16_t value,
                                ReadHandler rd,
                                WriteHandler wr,
                                int priority,
                                bool exclusiveWrite)
{
    const uint64_t id = ++m_nextId;
    const int effectivePriority = (priority != 0) ? priority : std::popcount(mask);
    m_handlers.push_back(HandlerEntry{
        .id = id,
        .ownerId = std::move(ownerId),
        .mask = mask,
        .value = value,
        .priority = effectivePriority,
        .exclusiveWrite = exclusiveWrite,
        .read = std::move(rd),
        .write = std::move(wr)
    });
    std::ranges::stable_sort(m_handlers, [](const HandlerEntry &a, const HandlerEntry &b) {
        return a.priority > b.priority;
    });
    return id;
}

uint64_t IoBus::registerSimpleHandler(std::string ownerId,
                                      uint16_t mask,
                                      uint16_t value,
                                      SimpleReadFn rd,
                                      WriteHandler wr,
                                      int priority,
                                      bool exclusiveWrite)
{
    ReadHandler fullRd;
    if (rd) {
        fullRd = [fn = std::move(rd)](uint16_t port) -> IoReadResult {
            return IoReadResult{fn(port), 0xFF};
        };
    }
    return registerHandler(std::move(ownerId), mask, value, std::move(fullRd), std::move(wr), priority, exclusiveWrite);
}

void IoBus::unregisterOwner(std::string_view ownerId)
{
    std::erase_if(m_handlers, [ownerId](const HandlerEntry &e) {
        return e.ownerId == ownerId;
    });
}

void IoBus::addObserver(IoObserver observer)
{
    m_observers.push_back(std::move(observer));
}

void IoBus::setFloatingBusCallback(FloatingBusFn fn)
{
    m_floatingBusFn = std::move(fn);
}

void IoBus::setWatchpointCallback(WatchpointCb cb)
{
    m_watchpointCb = std::move(cb);
}

void IoBus::setNextRegOpcodeCallback(NextRegOpcodeCb cb)
{
    m_nextRegOpcodeCb = std::move(cb);
}

void IoBus::setRzxCallbacks(SimpleReadFn playbackOverride, std::function<void(uint8_t)> recordHook)
{
    m_rzxReadOverride = std::move(playbackOverride);
    m_rzxReadRecord = std::move(recordHook);
}

void IoBus::declineWrite() noexcept
{
    m_writeDeclined = true;
}

uint8_t IoBus::read(uint16_t port)
{
    if (m_rzxReadOverride) {
        const uint8_t val = m_rzxReadOverride(port);
        if (m_watchpointCb)
            m_watchpointCb(port, val, false);
        return val;
    }

    const uint8_t val = peekPort(port, true);
    if (m_rzxReadRecord)
        m_rzxReadRecord(val);
    if (m_watchpointCb)
        m_watchpointCb(port, val, false);
    return val;
}

uint8_t IoBus::peekPort(uint16_t port, bool notifyObservers) const
{
    uint8_t combinedValue = 0xFF;
    uint8_t combinedAttached = 0x00;

    for (const auto &h : m_handlers) {
        if (h.read && (port & h.mask) == h.value) {
            // Handlers with a negative priority (sorted last) are default
            // responders, e.g. a machine's idle joystick port: they only
            // answer when nothing else does
            if (h.priority < 0 && combinedAttached != 0)
                break;
            const IoReadResult res = h.read(port);
            if (res.attachedMask != 0) {
                combinedValue &= static_cast<uint8_t>(~res.attachedMask | res.value);
                combinedAttached |= res.attachedMask;
                if (res.exclusive)
                    break;
            }
        }
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

    for (const auto &h : m_handlers) {
        if (h.write && (port & h.mask) == h.value) {
            m_writeDeclined = false;
            h.write(port, value);
            if (!m_writeDeclined && h.exclusiveWrite)
                break;
        }
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
