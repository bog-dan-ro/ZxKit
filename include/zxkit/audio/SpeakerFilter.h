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

namespace zxkit {

enum class SpeakerModel : uint8_t { Unfiltered, TV, Beeper };

/// Representative speaker responses, independent of emulated machine state.
/// TV: ULA + AY through 100 Hz high-pass / 10 kHz low-pass.
/// Beeper: ULA electrical response + 750 Hz acoustic high-pass; AY stays direct.
/// External DACs are mixed by the caller after this stage.
class SpeakerFilter
{
public:
    struct Stereo
    {
        double left, right;
    };

    SpeakerFilter() noexcept
    {
        configure(48000);
    }

    void configure(uint32_t sampleRate) noexcept;
    void reset() noexcept;
    void setModel(SpeakerModel model) noexcept
    {
        m_model = model;
    }
    SpeakerModel model() const noexcept
    {
        return m_model;
    }
    bool bypassed() const noexcept
    {
        return m_model == SpeakerModel::Unfiltered && m_current.model == m_model
                && !m_fadeRemaining;
    }
    Stereo process(double ula, double ayLeft, double ayRight) noexcept;
    void saveState(StateWriter &writer) const;
    void loadState(StateReader &reader);

private:
    struct Path
    {
        SpeakerModel model = SpeakerModel::Unfiltered;
        bool initialized = false;
        std::array<double, 2> previous{}, highPass{}, lowPass{};
        double ula = 0, z1 = 0, z2 = 0;
    };
    Stereo apply(Path &path, double ula, double ayLeft, double ayRight) const noexcept;

    SpeakerModel m_model = SpeakerModel::Unfiltered;
    uint32_t m_sampleRate = 48000;
    uint32_t m_fadeFrames = 240, m_fadeRemaining = 0;
    double m_tvHigh = 0, m_tvLow = 0;
    double m_rise = 0, m_fall = 0;
    double m_b0 = 0, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
    Path m_current, m_previous;
};

} // namespace zxkit
