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

#include "zxkit/audio/Ay8912.h"
#include "zxkit/audio/Beeper.h"
#include "zxkit/audio/BlipBuffer.h"
#include "zxkit/audio/Dac.h"
#include "zxkit/core/StateStream.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace zxkit {

class AudioMixer {
public:
    static constexpr std::size_t kMaxAyChips = 3;

    AudioMixer();

    void configure(uint32_t cpuClockHz, uint32_t sampleRate, uint32_t tstatesPerFrame);
    void reset() noexcept;

    void setAyChipCount(uint8_t count) noexcept;
    uint8_t ayChipCount() const noexcept { return m_ayChipCount; }

    void setActiveAyChip(uint8_t index) noexcept;
    uint8_t activeAyChipIndex() const noexcept { return m_activeAyIndex; }

    Ay8912 &activeAy() noexcept { return m_ayChips[m_activeAyIndex]; }
    const Ay8912 &activeAy() const noexcept { return m_ayChips[m_activeAyIndex]; }
    Ay8912 &ayChip(std::size_t idx) noexcept { return m_ayChips[idx % kMaxAyChips]; }
    const Ay8912 &ayChip(std::size_t idx) const noexcept { return m_ayChips[idx % kMaxAyChips]; }

    /// Writes the selected register of the active AY at `frameTstate` (base clock T-states
    /// into the frame): the chip plays its old settings up to that moment. Sound changing
    /// many times a frame (sampled speech, digidrums) needs this timing.
    void writeAyRegister(uint32_t frameTstate, uint8_t value) noexcept;

    Beeper &beeper() noexcept { return m_beeper; }
    const Beeper &beeper() const noexcept { return m_beeper; }

    Dac &dac() noexcept { return m_dac; }
    const Dac &dac() const noexcept { return m_dac; }

    BlipBuffer &leftBlip() noexcept { return m_leftBlip; }
    BlipBuffer &rightBlip() noexcept { return m_rightBlip; }

    void setStereoMode(AyStereoMode mode) noexcept { m_stereoMode = mode; }
    AyStereoMode stereoMode() const noexcept { return m_stereoMode; }

    void setAyModel(AyModel model) noexcept
    {
        for (auto &ay : m_ayChips)
            ay.setModel(model);
    }
    AyModel ayModel() const noexcept { return m_ayChips.front().model(); }

    uint32_t sampleRate() const noexcept { return m_sampleRate; }

    /// Finalizes the current video frame's audio and returns interleaved stereo int16 PCM samples.
    std::span<const int16_t> endFrame(uint32_t frameTstates) noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    void renderAyUpTo(std::size_t chip, std::size_t sample) noexcept;

    uint32_t m_cpuClockHz = 3'500'000;
    uint32_t m_sampleRate = 48'000;
    uint32_t m_tstatesPerFrame = 69'888;
    uint8_t m_ayChipCount = 1;
    uint8_t m_activeAyIndex = 0;
    AyStereoMode m_stereoMode = AyStereoMode::ACB;

    Beeper m_beeper;
    Dac m_dac;
    std::array<Ay8912, kMaxAyChips> m_ayChips;
    BlipBuffer m_leftBlip;
    BlipBuffer m_rightBlip;

    std::vector<int32_t> m_leftAccum;
    std::vector<int32_t> m_rightAccum;
    std::vector<int32_t> m_ayInterleaved;
    std::vector<int16_t> m_outputPcm;
    std::array<std::size_t, kMaxAyChips> m_ayRendered{}; // samples of this frame each AY rendered
};

} // namespace zxkit
