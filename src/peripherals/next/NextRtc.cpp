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

#include "zxkit/peripherals/next/NextRtc.h"
#include <algorithm>
#include <chrono>

namespace zxkit {
namespace {
uint8_t bcd(unsigned value) { return uint8_t((value / 10) * 16 + value % 10); }
unsigned decimal(unsigned value) { return (value >> 4) * 10 + (value & 15); }
} // namespace
NextRtc::NextRtc()
    : m_now([] {
          return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      })
{
    refreshCalendar();
}

void NextRtc::resetBus() noexcept
{
    m_scl = m_sda = true;
    m_driveLow = false;
    m_phase = Phase::Idle;
    m_address = true;
    m_bits = 0;
    m_ackStep = 0;
}
void NextRtc::setNvram(std::span<const uint8_t, 56> bytes) { std::ranges::copy(bytes, m_regs.begin() + 8); }

void NextRtc::refreshCalendar()
{
    using namespace std::chrono;
    const sys_seconds time{seconds{m_halted ? m_haltedTime : m_now() + m_offset}};
    const auto day = floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss hms{time - day};
    m_regs[0] = bcd(hms.seconds().count()) | (m_halted ? 0x80 : 0);
    m_regs[1] = bcd(hms.minutes().count());
    const auto hour = unsigned(hms.hours().count());
    m_regs[2] =
        (m_regs[2] & 0x40) ? uint8_t(0x40 | (hour >= 12 ? 0x20 : 0) | bcd(hour % 12 ? hour % 12 : 12)) : bcd(hour);
    m_regs[3] = uint8_t((int(weekday{day}.c_encoding()) + m_weekdayOffset + 14) % 7 + 1);
    m_regs[4] = bcd(unsigned(date.day()));
    m_regs[5] = bcd(unsigned(date.month()));
    m_regs[6] = bcd(unsigned(int(date.year()) % 100));
}
void NextRtc::commitCalendar()
{
    if (!m_dirtyDate)
        return;
    m_dirtyDate = false;
    using namespace std::chrono;
    const year_month_day date{year{2000 + int(decimal(m_regs[6]))}, month{decimal(m_regs[5] & 0x1F)},
                              day{decimal(m_regs[4] & 0x3F)}};
    unsigned hour = decimal(m_regs[2] & ((m_regs[2] & 0x40) ? 0x1F : 0x3F));
    if (m_regs[2] & 0x40)
        hour = (hour % 12) + ((m_regs[2] & 0x20) ? 12 : 0);
    const auto minute = decimal(m_regs[1] & 0x7F), second = decimal(m_regs[0] & 0x7F);
    if (!date.ok() || hour > 23 || minute > 59 || second > 59)
        return;
    const auto value =
        duration_cast<seconds>(sys_days{date}.time_since_epoch()).count() + hour * 3600 + minute * 60 + second;
    m_offset = value - m_now();
    m_haltedTime = value;
    m_halted = (m_regs[0] & 0x80) != 0;
    m_weekdayOffset = int(std::clamp<unsigned>(m_regs[3] & 7, 1, 7)) - 1 - int(weekday{sys_days{date}}.c_encoding());
}
void NextRtc::setSda(bool high)
{
    const bool old = m_sda;
    m_sda = high;
    if (!m_scl || old == high)
        return;
    if (old && !high) { // START or repeated START
        commitCalendar();
        refreshCalendar();
        m_phase = Phase::Receive;
        m_address = true;
        m_bits = 0;
        m_byte = 0;
        m_driveLow = false;
    } else { // STOP
        commitCalendar();
        m_phase = Phase::Idle;
        m_driveLow = false;
    }
}
void NextRtc::received()
{
    m_ack = true;
    m_afterAck = Phase::Receive;
    if (m_address) {
        m_address = false;
        m_ack = (m_byte >> 1) == 0x68;
        m_afterAck = !m_ack ? Phase::Idle : (m_byte & 1) ? Phase::Send : Phase::Receive;
        m_pointerNext = !(m_byte & 1);
    } else if (m_pointerNext) {
        m_pointer = m_byte & 63;
        m_pointerNext = false;
    } else {
        m_regs[m_pointer] = m_byte;
        if (m_pointer <= 6)
            m_dirtyDate = true;
        m_pointer = (m_pointer + 1) & 63;
    }
    m_phase = Phase::Ack;
    m_ackStep = 0;
    m_bits = 0;
    m_byte = 0;
}
void NextRtc::prepareSend()
{
    m_phase = Phase::Send;
    m_byte = m_regs[m_pointer];
    m_bits = 0;
    m_driveLow = !(m_byte & 0x80);
}
void NextRtc::setScl(bool high)
{
    if (m_scl == high)
        return;
    m_scl = high;
    if (high) {
        if (m_phase == Phase::Receive) {
            m_byte = uint8_t((m_byte << 1) | (m_sda ? 1 : 0));
            if (++m_bits == 8)
                received();
        } else if (m_phase == Phase::Ack && m_ackStep == 1)
            m_ackStep = 2;
        else if (m_phase == Phase::Send && ++m_bits == 8) {
            m_phase = Phase::MasterAck;
            m_ackStep = 0;
        } else if (m_phase == Phase::MasterAck && m_ackStep == 1) {
            m_ack = !m_sda;
            m_ackStep = 2;
        }
    } else {
        if (m_phase == Phase::Ack) {
            if (m_ackStep == 0) {
                m_driveLow = m_ack;
                m_ackStep = 1;
            } else if (m_ackStep == 2) {
                m_driveLow = false;
                m_phase = m_afterAck;
                if (m_phase == Phase::Send)
                    prepareSend();
            }
        } else if (m_phase == Phase::Send)
            m_driveLow = !(m_byte & (0x80 >> m_bits));
        else if (m_phase == Phase::MasterAck) {
            if (m_ackStep == 0) {
                m_driveLow = false;
                m_ackStep = 1;
            } else if (m_ackStep == 2) {
                if (m_ack) {
                    m_pointer = (m_pointer + 1) & 63;
                    prepareSend();
                } else {
                    m_phase = Phase::Idle;
                    m_driveLow = false;
                }
            }
        }
    }
}
void NextRtc::saveState(StateWriter &w) const
{
    w.writeBytes(m_regs);
    w.write(m_offset);
    w.write(m_haltedTime);
    w.write<int32_t>(m_weekdayOffset);
    w.writeBool(m_halted);
    w.writeBool(m_dirtyDate);
    w.writeBool(m_scl);
    w.writeBool(m_sda);
    w.writeBool(m_driveLow);
    w.writeBool(m_address);
    w.writeBool(m_pointerNext);
    w.writeBool(m_ack);
    w.write(m_pointer);
    w.write(m_byte);
    w.write(m_bits);
    w.write(m_ackStep);
    w.write<uint8_t>(uint8_t(m_phase));
    w.write<uint8_t>(uint8_t(m_afterAck));
}
void NextRtc::loadState(StateReader &r)
{
    r.readBytes(m_regs);
    m_offset = r.read<int64_t>();
    m_haltedTime = r.read<int64_t>();
    m_weekdayOffset = r.read<int32_t>();
    m_halted = r.readBool();
    m_dirtyDate = r.readBool();
    m_scl = r.readBool();
    m_sda = r.readBool();
    m_driveLow = r.readBool();
    m_address = r.readBool();
    m_pointerNext = r.readBool();
    m_ack = r.readBool();
    m_pointer = r.read<uint8_t>() & 63;
    m_byte = r.read<uint8_t>();
    m_bits = r.read<uint8_t>();
    m_ackStep = r.read<uint8_t>();
    m_phase = Phase(r.read<uint8_t>());
    m_afterAck = Phase(r.read<uint8_t>());
    if (m_bits > 8 || m_ackStep > 2 || m_phase > Phase::MasterAck || m_afterAck > Phase::MasterAck)
        throw FormatException("Invalid RTC bus state");
}
} // namespace zxkit
