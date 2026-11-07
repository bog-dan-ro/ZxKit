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
#include <cstddef>
#include <cstdint>
#include <functional>

namespace zxkit {

/// The Zilog Z80 CTC: four 8-bit down counters, each a timer (the system clock through a
/// 16 or 256 prescaler) or a counter (edges on its CLK/TRG input). At zero a channel pulses
/// its ZC/TO output, can interrupt (IM2, its vector from the vector word) and reloads.
/// The machine wires the CLK/TRG inputs (`setTrigger`), the ZC/TO outputs (`onZeroCount`)
/// and the interrupts (`onInterrupt`, e.g. into an `Im2Controller`).
class Z80Ctc
{
public:
    static constexpr std::size_t kChannels = 4;

    /// A ZC/TO pulse (channels 0 to 2; channel 3 has no output pin, it is reported too)
    std::function<void(std::size_t channel)> onZeroCount;
    /// A channel raises (`pending`) or withdraws (reset, interrupts disabled) its interrupt
    std::function<void(std::size_t channel, uint8_t vector, bool pending)> onInterrupt;

    void reset() noexcept;

    uint8_t read(std::size_t channel) const noexcept;
    void write(std::size_t channel, uint8_t value) noexcept;

    /// The level of a channel's CLK/TRG input: it counts (counter mode) or starts the timer on
    /// the programmed edge
    void setTrigger(std::size_t channel, bool level) noexcept;
    /// Lets `tstates` system clocks pass (the timers)
    void tick(uint32_t tstates) noexcept;

    uint8_t vector(std::size_t channel) const noexcept
    {
        return static_cast<uint8_t>((m_vector & 0xF8) | ((channel & 3) << 1));
    }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    static constexpr uint8_t kInterruptEnable = 0x80;
    static constexpr uint8_t kCounterMode = 0x40;
    static constexpr uint8_t kPrescaler256 = 0x20;
    static constexpr uint8_t kRisingEdge = 0x10;
    static constexpr uint8_t kTriggerStart = 0x08;
    static constexpr uint8_t kTimeConstantFollows = 0x04;
    static constexpr uint8_t kSoftwareReset = 0x02;

    struct Channel
    {
        uint8_t control = kSoftwareReset | 1;
        uint8_t timeConstant = 0; // 0 is 256
        uint8_t counter = 0;
        uint16_t prescale = 0; // system clocks left until the next timer decrement
        bool waitingTimeConstant = false;
        bool running = false;
        bool waitingTrigger = false;
        bool trigger = false; // the CLK/TRG level
    };

    void decrement(std::size_t channel) noexcept;
    uint16_t prescaler(const Channel &c) const noexcept
    {
        return (c.control & kPrescaler256) ? 256 : 16;
    }

    std::array<Channel, kChannels> m_channels{};
    uint8_t m_vector = 0;
};

} // namespace zxkit
