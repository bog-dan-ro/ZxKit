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
#include <functional>

namespace zxkit {

/// DS1307-compatible RTC at I2C address 0x68. Open-drain SDA, repeated START,
/// ACK/NACK, sequential access, BCD date/time, 12/24h, CH and battery RAM.
/// Uses UTC by default; a host can supply local calendar seconds or a test clock.
class NextRtc {
  public:
    using TimeSource = std::function<int64_t()>;
    NextRtc();
    void setTimeSource(TimeSource source) { m_now = std::move(source); }
    void resetBus() noexcept;
    void setScl(bool high);
    void setSda(bool high);
    bool scl() const noexcept { return m_scl; }
    bool sda() const noexcept { return m_sda && !m_driveLow; }
    std::span<const uint8_t, 56> nvram() const noexcept { return std::span(m_regs).subspan<8>(); }
    void setNvram(std::span<const uint8_t, 56> bytes);
    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

  private:
    enum class Phase : uint8_t { Idle, Receive, Ack, Send, MasterAck };
    void received();
    void prepareSend();
    void refreshCalendar();
    void commitCalendar();
    std::array<uint8_t, 64> m_regs{};
    TimeSource m_now;
    int64_t m_offset = 0, m_haltedTime = 0;
    int m_weekdayOffset = 0;
    bool m_halted = false, m_dirtyDate = false;
    bool m_scl = true, m_sda = true, m_driveLow = false;
    bool m_address = true, m_pointerNext = false, m_ack = false;
    uint8_t m_pointer = 0, m_byte = 0, m_bits = 0, m_ackStep = 0;
    Phase m_phase = Phase::Idle, m_afterAck = Phase::Idle;
};
} // namespace zxkit
