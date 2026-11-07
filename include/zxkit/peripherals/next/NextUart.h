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

#include "zxkit/core/StateStream.h"
#include <array>
#include <deque>
#include <functional>

namespace zxkit {

/// The two Next UARTs. All times are in base 3.5 MHz T-states, independent
/// of CPU turbo. Host callbacks run synchronously on the caller's thread.
/// Host sockets/protocols belong in the consumer. No host code is serialized.
class NextUart
{
public:
    using Transmit = std::function<void(unsigned channel, uint8_t byte)>;
    bool hasTransmitHandler() const noexcept
    {
        return bool(m_transmit);
    }
    void setTransmitHandler(Transmit handler)
    {
        m_transmit = std::move(handler);
    }
    void reset(bool hard);
    uint8_t read(uint16_t port, uint64_t now);
    void write(uint16_t port, uint8_t value, uint64_t now);
    /// Queue bytes arriving on a wire; they enter the hardware FIFO at baud rate.
    /// Returns the number accepted into the bounded host wire queue.
    size_t receive(unsigned channel, std::span<const uint8_t> bytes, uint64_t now);
    void setClearToSend(unsigned channel, bool ready) noexcept;
    void tick(uint64_t now);
    bool active() const noexcept
    {
        return m_active;
    }
    uint64_t generation() const noexcept
    {
        return m_generation;
    }
    uint32_t prescaler(unsigned channel) const
    {
        return m_channels.at(channel).prescaler;
    }
    uint8_t framing(unsigned channel) const
    {
        return m_channels.at(channel).frame;
    }
    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    struct Byte
    {
        uint64_t time;
        uint8_t value;
    };
    struct Channel
    {
        uint32_t prescaler = 243; // FPGA power-on value, uart.vhd
        uint8_t frame = 0x18;
        std::deque<uint8_t> rx, tx;
        std::deque<Byte> wire;
        uint64_t txTime = 0;
        uint8_t shiftByte = 0;
        bool shifting = false;
        bool overflow = false;
        bool cts = true;
    };
    static uint64_t characterTime(const Channel &ch);
    void updateActive() noexcept;
    void startTx(Channel &ch, uint64_t now);
    std::array<Channel, 2> m_channels;
    Transmit m_transmit;
    unsigned m_selected = 0;
    bool m_active = false;
    uint64_t m_generation = 0;
};

} // namespace zxkit
