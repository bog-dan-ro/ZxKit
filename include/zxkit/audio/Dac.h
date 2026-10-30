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

#include "zxkit/audio/BlipBuffer.h"
#include "zxkit/core/StateStream.h"

#include <array>
#include <cstdint>

namespace zxkit {

/// 4-channel 8-bit DAC supporting SpecDrum, Covox, Soundrive, and ZX Next DAC A/B/C/D.
/// Channels 0 (A) and 1 (B) feed Left; Channels 2 (C) and 3 (D) feed Right.
class Dac {
public:
    static constexpr int32_t kDacScale = 48;

    void reset() noexcept;

    void writeChannel(uint32_t frameTstate, uint8_t ch, uint8_t val, BlipBuffer &left, BlipBuffer &right) noexcept;
    void writeMono(uint32_t frameTstate, uint8_t val, BlipBuffer &left, BlipBuffer &right) noexcept;
    uint8_t channel(uint8_t ch) const noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    void flush(uint32_t frameTstate, BlipBuffer &left, BlipBuffer &right) noexcept;

    std::array<uint8_t, 4> m_channels{0x80, 0x80, 0x80, 0x80};
    int32_t m_lastLeft = 0;
    int32_t m_lastRight = 0;
};

} // namespace zxkit
