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

#include "zxkit/cpu/Z80Ctc.h"

namespace zxkit {

void Z80Ctc::reset() noexcept
{
    // A reset stops every channel and disables their interrupts
    for (std::size_t i = 0; i < kChannels; ++i) {
        const bool trigger = m_channels[i].trigger;
        m_channels[i] = Channel{};
        m_channels[i].trigger = trigger;
        if (onInterrupt)
            onInterrupt(i, vector(i), false);
    }
}

uint8_t Z80Ctc::read(std::size_t channel) const noexcept
{
    return m_channels[channel & 3].counter;
}

void Z80Ctc::write(std::size_t channel, uint8_t value) noexcept
{
    channel &= 3;
    Channel &c = m_channels[channel];

    if (c.waitingTimeConstant) {
        c.waitingTimeConstant = false;
        c.timeConstant = value;
        // A running channel takes the new constant at its next zero count
        if (!c.running && !c.waitingTrigger) {
            c.counter = value;
            c.prescale = prescaler(c);
            if (!(c.control & kCounterMode) && (c.control & kTriggerStart))
                c.waitingTrigger = true;
            else
                c.running = true;
        }
        return;
    }

    if (value & 1) {
        c.control = value;
        if (value & kSoftwareReset) {
            c.running = false;
            c.waitingTrigger = false;
        }
        if (value & kTimeConstantFollows)
            c.waitingTimeConstant = true;
        if (((value & kSoftwareReset) || !(value & kInterruptEnable)) && onInterrupt)
            onInterrupt(channel, vector(channel), false);
    } else if (channel == 0) {
        // The interrupt vector: bits 1-2 are the channel's number
        m_vector = value & 0xF8;
    }
}

void Z80Ctc::setTrigger(std::size_t channel, bool level) noexcept
{
    Channel &c = m_channels[channel & 3];
    if (c.trigger == level)
        return;
    c.trigger = level;
    // The programmed edge only
    if (((c.control & kRisingEdge) != 0) != level)
        return;
    if (c.control & kCounterMode) {
        if (c.running)
            decrement(channel & 3);
    } else if (c.waitingTrigger) {
        c.waitingTrigger = false;
        c.running = true;
        c.prescale = prescaler(c);
    }
}

void Z80Ctc::tick(uint32_t tstates) noexcept
{
    for (std::size_t i = 0; i < kChannels; ++i) {
        Channel &c = m_channels[i];
        if (!c.running || (c.control & kCounterMode))
            continue;
        uint32_t left = tstates;
        while (c.running && !(c.control & kCounterMode) && left >= c.prescale) {
            left -= c.prescale;
            c.prescale = prescaler(c);
            decrement(i);
        }
        if (c.running && !(c.control & kCounterMode))
            c.prescale = static_cast<uint16_t>(c.prescale - left);
    }
}

void Z80Ctc::decrement(std::size_t channel) noexcept
{
    Channel &c = m_channels[channel];
    // A time constant of 0 counts 256
    if (--c.counter != 0)
        return;
    c.counter = c.timeConstant;
    if ((c.control & kInterruptEnable) && onInterrupt)
        onInterrupt(channel, vector(channel), true);
    if (onZeroCount)
        onZeroCount(channel);
}

void Z80Ctc::saveState(StateWriter &w) const
{
    w.write(m_vector);
    for (const Channel &c : m_channels) {
        w.write(c.control);
        w.write(c.timeConstant);
        w.write(c.counter);
        w.write(c.prescale);
        w.write(static_cast<uint8_t>((c.waitingTimeConstant ? 1 : 0) | (c.running ? 2 : 0) | (c.waitingTrigger ? 4 : 0)
                                     | (c.trigger ? 8 : 0)));
    }
}

void Z80Ctc::loadState(StateReader &r)
{
    m_vector = r.read<uint8_t>();
    for (Channel &c : m_channels) {
        c.control = r.read<uint8_t>();
        c.timeConstant = r.read<uint8_t>();
        c.counter = r.read<uint8_t>();
        c.prescale = r.read<uint16_t>();
        const uint8_t flags = r.read<uint8_t>();
        c.waitingTimeConstant = flags & 1;
        c.running = flags & 2;
        c.waitingTrigger = flags & 4;
        c.trigger = flags & 8;
    }
}

} // namespace zxkit
