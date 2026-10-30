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
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

struct IoReadResult {
    uint8_t value = 0xFF;
    uint8_t attachedMask = 0xFF; // 0xFF = all bits driven, 0x00 = unattached
    /// The device owns the port: the handlers after it (lower priority) aren't asked, e.g. the
    /// Beta 128's ports while TR-DOS is paged in hide the Kempston joystick's 0x1F.
    bool exclusive = false;
};

class IoBus {
public:
    using ReadHandler      = std::function<IoReadResult(uint16_t port)>;
    using SimpleReadFn     = std::function<uint8_t(uint16_t port)>;
    using WriteHandler     = std::function<void(uint16_t port, uint8_t value)>;
    using IoObserver       = std::function<void(uint16_t port, uint8_t value, bool isWrite)>;
    using FloatingBusFn    = std::function<uint8_t(uint16_t port)>;
    using WatchpointCb     = std::function<void(uint16_t port, uint8_t value, bool isWrite)>;
    using NextRegOpcodeCb  = std::function<void(uint8_t reg, uint8_t value)>;

    struct HandlerEntry {
        uint64_t id = 0;
        std::string ownerId;
        uint16_t mask = 0;
        uint16_t value = 0;
        int priority = 0;
        bool exclusiveWrite = false;
        ReadHandler read;
        WriteHandler write;
    };

    void clear();

    /// `priority` orders the handlers (0: by the number of decoded address bits).
    /// A negative priority makes a default read responder, used only when no
    /// other handler answers the port.
    uint64_t registerHandler(std::string ownerId,
                             uint16_t mask,
                             uint16_t value,
                             ReadHandler rd,
                             WriteHandler wr,
                             int priority = 0,
                             bool exclusiveWrite = false);

    uint64_t registerSimpleHandler(std::string ownerId,
                                   uint16_t mask,
                                   uint16_t value,
                                   SimpleReadFn rd,
                                   WriteHandler wr,
                                   int priority = 0,
                                   bool exclusiveWrite = false);

    void unregisterOwner(std::string_view ownerId);
    void addObserver(IoObserver observer);
    void setFloatingBusCallback(FloatingBusFn fn);
    void setWatchpointCallback(WatchpointCb cb);
    void setNextRegOpcodeCallback(NextRegOpcodeCb cb);
    void setRzxCallbacks(SimpleReadFn playbackOverride, std::function<void(uint8_t)> recordHook);

    /// Called by a write handler when its hardware decode gate is currently disabled.
    void declineWrite() noexcept;

    /// Guest CPU / DMA port read (applies RZX override/recording + observers + watchpoints).
    uint8_t read(uint16_t port);

    /// Pure port read without RZX or watchpoint triggers (can optionally skip observers).
    uint8_t peekPort(uint16_t port, bool notifyObservers = false) const;

    void write(uint16_t port, uint8_t value);
    void writeNextRegOpcode(uint8_t reg, uint8_t value);

private:
    std::vector<HandlerEntry> m_handlers;
    std::vector<IoObserver> m_observers;
    FloatingBusFn m_floatingBusFn;
    WatchpointCb m_watchpointCb;
    NextRegOpcodeCb m_nextRegOpcodeCb;
    SimpleReadFn m_rzxReadOverride;
    std::function<void(uint8_t)> m_rzxReadRecord;
    uint64_t m_nextId = 0;
    bool m_writeDeclined = false;
};

} // namespace zxkit
