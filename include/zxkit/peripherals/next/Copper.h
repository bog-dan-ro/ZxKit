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
#include "zxkit/peripherals/next/NextReg.h"

#include <array>
#include <cstdint>
#include <span>

namespace zxkit {

/// ZX Spectrum Next Copper Co-Processor (2 KB instruction RAM = 1024 x 16-bit instructions).
/// Executes WAIT(vpos, hpos) and MOVE(nextreg, value) synchronized to the video raster.
class Copper {
public:
    static constexpr std::size_t kRamBytes = 2048;

    void reset() noexcept;

    void writeData8(uint8_t byte) noexcept;

    void setControlLo(uint8_t val) noexcept;
    void setControlHi(uint8_t val) noexcept;

    void onVBlank() noexcept;

    /// Executes any non-blocking MOVE instructions at the current (vc, hc) raster position.
    void tickScanline(uint16_t vc, NextReg &nextReg);

    uint16_t pc() const noexcept;
    uint8_t mode() const noexcept;
    bool isHalted() const noexcept;
    std::span<const uint8_t> instructionRam() const noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    std::array<uint8_t, kRamBytes> m_ram{};
    uint16_t m_writeAddr = 0;
    uint16_t m_pc = 0;
    uint8_t m_mode = 0;
    bool m_halted = true;
};

} // namespace zxkit
