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
#include <cstdint>
#include <span>

namespace zxkit {

enum class AyStereoMode : uint8_t {
    Mono = 0,
    ACB,
    ABC
};

/// Cycle-stepped AY-3-8912 / YM2149 programmable sound generator.
class Ay8912 {
public:
    static constexpr std::array<int16_t, 16> kVolumeLevels = {
        0, 58, 82, 116, 164, 232, 328, 464,
        656, 928, 1312, 1855, 2624, 3711, 5248, 7422
    };

    static constexpr std::array<uint8_t, 16> kRegMasks = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF
    };

    Ay8912();

    void reset() noexcept;

    void selectRegister(uint8_t reg) noexcept;

    uint8_t selectedRegister() const noexcept
    {
        return m_selectedReg;
    }

    uint8_t readRegister() const noexcept;
    void writeRegister(uint8_t val) noexcept;

    /// Renders `sampleCount` stereo sample pairs into `stereoAccum` (interleaved L, R).
    void renderSamples(std::span<int32_t> stereoAccum,
                       uint32_t sampleRate,
                       uint32_t psgClockHz = 1'773'400,
                       AyStereoMode stereoMode = AyStereoMode::ACB) noexcept;

    const std::array<uint8_t, 16> &registers() const noexcept { return m_regs; }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    void stepInternalTick() noexcept;

    uint8_t m_selectedReg = 0;
    std::array<uint8_t, 16> m_regs{};
    std::array<uint16_t, 3> m_toneCount{};
    std::array<uint8_t, 3> m_toneOut{1, 1, 1};
    uint8_t m_noiseCount = 0;
    uint32_t m_noiseRng = 1;
    uint8_t m_noiseOut = 1;
    uint16_t m_envCount = 0;
    uint8_t m_envStep = 0;
    uint8_t m_envVolume = 0;
    bool m_envHolding = false;
    uint8_t m_envAttack = 0;
    uint32_t m_phaseAccum = 0;
};

} // namespace zxkit
