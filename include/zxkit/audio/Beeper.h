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

#include <cstdint>

namespace zxkit {

class Beeper {
public:
    static constexpr int32_t kEarVolume = 8192;
    static constexpr int32_t kMicVolume = 2048;

    void reset() noexcept;

    void setPortFeBits(uint32_t frameTstate, bool ear, bool mic, BlipBuffer &left, BlipBuffer &right) noexcept;
    void setTapeEar(uint32_t frameTstate, bool tapeEar, BlipBuffer &left, BlipBuffer &right) noexcept;
    void setPortFeBits(uint32_t frameTstate, bool ear, bool mic, BlipBuffer &mono) noexcept;
    void setTapeEar(uint32_t frameTstate, bool tapeEar, BlipBuffer &mono) noexcept;

    bool earBit() const noexcept { return m_earBit; }
    bool micBit() const noexcept { return m_micBit; }
    bool tapeEarBit() const noexcept { return m_tapeEarBit; }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    int32_t amplitudeDelta() noexcept;

    bool m_earBit = false;
    bool m_micBit = false;
    bool m_tapeEarBit = false;
    int32_t m_lastAmplitude = 0;
};

} // namespace zxkit
