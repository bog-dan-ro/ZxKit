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

#include "zxkit/peripherals/next/NextUart.h"
#include <algorithm>

namespace zxkit {

uint64_t NextUart::characterTime(const Channel &ch)
{
    const unsigned bits = 1 + 5 + ((ch.frame >> 3) & 3) + ((ch.frame & 4) ? 1 : 0) + 1 + (ch.frame & 1);
    // FPGA UART clock is 28MHz, eight clocks per base T-state.
    return std::max<uint64_t>(1, (uint64_t(ch.prescaler) * bits + 7) / 8);
}

void NextUart::updateActive() noexcept
{
    m_active = false;
    for (const auto &ch : m_channels)
        m_active |= ch.shifting || !ch.tx.empty() || !ch.wire.empty();
}

void NextUart::reset(bool hard)
{
    m_selected = 0;
    for (auto &ch : m_channels) {
        const auto prescaler = ch.prescaler;
        const auto frame = ch.frame;
        ch = Channel{};
        if (!hard) {
            ch.prescaler = prescaler;
            ch.frame = frame;
        }
    }
    ++m_generation;
    updateActive();
}

void NextUart::startTx(Channel &ch, uint64_t now)
{
    if (ch.shifting || ch.tx.empty() || (ch.frame & 0xC0) || ((ch.frame & 0x20) && !ch.cts))
        return;
    ch.shiftByte = ch.tx.front() & ((1u << (5 + ((ch.frame >> 3) & 3))) - 1);
    ch.tx.pop_front();
    ch.txTime = now + characterTime(ch);
    ch.shifting = true;
}

void NextUart::tick(uint64_t now)
{
    if (!m_active)
        return;
    for (unsigned index = 0; index < 2; ++index) {
        auto &ch = m_channels[index];
        startTx(ch, now);
        while (ch.shifting && ch.txTime <= now) {
            const auto value = ch.shiftByte;
            const auto end = ch.txTime;
            ch.shifting = false;
            // Start the next buffered byte before calling the host. A callback
            // can enqueue incoming bytes, but cannot invalidate this iterator.
            startTx(ch, end);
            if (m_transmit)
                m_transmit(index, value);
        }
        while (!ch.wire.empty() && ch.wire.front().time <= now) {
            if (ch.rx.size() < 512)
                ch.rx.push_back(ch.wire.front().value);
            else
                ch.overflow = true;
            ch.wire.pop_front();
        }
    }
    updateActive();
}

uint8_t NextUart::read(uint16_t port, uint64_t now)
{
    tick(now);
    auto &ch = m_channels[m_selected];
    switch (port) {
    case 0x133B: {
        const uint8_t result = uint8_t((!ch.rx.empty() ? 1 : 0) | (ch.tx.size() >= 64 ? 2 : 0) | (ch.overflow ? 4 : 0) |
                                       (ch.rx.size() >= 384 ? 8 : 0) | (ch.tx.empty() ? 0x10 : 0));
        ch.overflow = false;
        return result;
    }
    case 0x143B: {
        if (ch.rx.empty())
            return 0;
        const auto value = ch.rx.front();
        ch.rx.pop_front();
        return value;
    }
    case 0x153B:
        return uint8_t((ch.prescaler >> 14) & 7);
    case 0x163B:
        return ch.frame;
    default:
        return 0xFF;
    }
}

void NextUart::write(uint16_t port, uint8_t value, uint64_t now)
{
    tick(now);
    if (port == 0x153B)
        m_selected = (value >> 6) & 1;
    auto &ch = m_channels[m_selected];
    switch (port) {
    case 0x133B:
        if (!(ch.frame & 0x80) && ch.tx.size() < 64) {
            ch.tx.push_back(value);
            startTx(ch, now);
        }
        break;
    case 0x143B:
        if (value & 0x80)
            ch.prescaler = (ch.prescaler & ~0x3F80u) | (uint32_t(value & 0x7F) << 7);
        else
            ch.prescaler = (ch.prescaler & ~0x7Fu) | value;
        break;
    case 0x153B:
        if (value & 0x10)
            ch.prescaler = (ch.prescaler & 0x3FFF) | (uint32_t(value & 7) << 14);
        break;
    case 0x163B:
        ch.frame = value;
        if (value & 0x80) {
            ch.rx.clear();
            ch.tx.clear();
            ch.wire.clear();
            ch.shifting = false;
            ch.overflow = false;
            ++m_generation;
        }
        startTx(ch, now);
        break;
    }
    updateActive();
}

size_t NextUart::receive(unsigned channel, std::span<const uint8_t> bytes, uint64_t now)
{
    if (channel >= 2)
        return 0;
    auto &ch = m_channels[channel];
    if (ch.frame & 0x80)
        return 0;
    uint64_t time = ch.wire.empty() ? now : std::max(now, ch.wire.back().time);
    const auto accepted = std::min(bytes.size(), size_t(65536 - ch.wire.size()));
    for (auto value : bytes.first(accepted)) {
        time += characterTime(ch);
        ch.wire.push_back({time, uint8_t(value & ((1u << (5 + ((ch.frame >> 3) & 3))) - 1))});
    }
    updateActive();
    return accepted;
}

void NextUart::setClearToSend(unsigned channel, bool ready) noexcept
{
    if (channel < 2)
        m_channels[channel].cts = ready;
}

void NextUart::saveState(StateWriter &w) const
{
    w.write<uint8_t>(m_selected);
    for (const auto &ch : m_channels) {
        w.write(ch.prescaler);
        w.write(ch.frame);
        w.write(ch.txTime);
        w.write(ch.shiftByte);
        w.writeBool(ch.shifting);
        w.writeBool(ch.overflow);
        w.writeBool(ch.cts);
        for (const auto *queue : {&ch.rx, &ch.tx}) {
            w.write<uint32_t>(queue->size());
            for (auto value : *queue)
                w.write(value);
        }
        w.write<uint32_t>(ch.wire.size());
        for (const auto &byte : ch.wire) {
            w.write(byte.time);
            w.write(byte.value);
        }
    }
}

void NextUart::loadState(StateReader &r)
{
    m_selected = r.read<uint8_t>() & 1;
    for (auto &ch : m_channels) {
        ch.prescaler = r.read<uint32_t>() & 0x1FFFF;
        ch.frame = r.read<uint8_t>();
        ch.txTime = r.read<uint64_t>();
        ch.shiftByte = r.read<uint8_t>();
        ch.shifting = r.readBool();
        ch.overflow = r.readBool();
        ch.cts = r.readBool();
        for (auto *queue : {&ch.rx, &ch.tx}) {
            queue->clear();
            const auto count = r.read<uint32_t>();
            if (count > (queue == &ch.rx ? 512u : 64u))
                throw FormatException("Invalid UART FIFO state");
            for (unsigned i = 0; i < count; ++i)
                queue->push_back(r.read<uint8_t>());
        }
        ch.wire.clear();
        const auto count = r.read<uint32_t>();
        if (count > 65536)
            throw FormatException("Invalid UART wire state");
        for (unsigned i = 0; i < count; ++i)
            ch.wire.push_back({r.read<uint64_t>(), r.read<uint8_t>()});
    }
    ++m_generation; // Host connections cannot be rewound.
    updateActive();
}

} // namespace zxkit
