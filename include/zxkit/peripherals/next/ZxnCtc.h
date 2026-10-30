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
#include "zxkit/cpu/Im2Controller.h"

#include <array>
#include <cstdint>

namespace zxkit {

/// 4-Channel Z80 CTC Counter/Timer (ports 0x183B .. 0x1B3B on ZX Spectrum Next).
class ZxnCtc {
public:
    struct Channel {
        uint8_t control = 0x03; // Reset state
        uint8_t timeConstant = 0;
        uint8_t downCounter = 0;
        uint16_t prescalerCount = 0;
        bool waitingTimeConstant = false;
    };

    void reset() noexcept;

    uint8_t readChannel(uint8_t idx) const noexcept;

    void writeChannel(uint8_t idx, uint8_t val) noexcept;

    void tickTstates(uint32_t tstates, Im2Controller &im2) noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    std::array<Channel, 4> m_channels{};
    uint8_t m_intVectorBase = 0;
};

} // namespace zxkit
