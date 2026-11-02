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

#include "zxkit/audio/AudioMixer.h"

#include <algorithm>

namespace zxkit {

AudioMixer::AudioMixer()
{
    configure(3'500'000, 48'000, 69'888);
}

void AudioMixer::configure(uint32_t cpuClockHz, uint32_t sampleRate, uint32_t tstatesPerFrame)
{
    m_cpuClockHz = cpuClockHz;
    m_sampleRate = sampleRate;
    m_tstatesPerFrame = tstatesPerFrame;
    m_leftBlip.configure(cpuClockHz, sampleRate, tstatesPerFrame);
    m_rightBlip.configure(cpuClockHz, sampleRate, tstatesPerFrame);

    const std::size_t maxSamples = (static_cast<std::size_t>(sampleRate) * tstatesPerFrame / cpuClockHz) + 64;
    m_leftAccum.resize(maxSamples, 0);
    m_rightAccum.resize(maxSamples, 0);
    m_ayInterleaved.resize(maxSamples * 2, 0);
    m_outputPcm.resize(maxSamples * 2, 0);
    std::ranges::fill(m_ayInterleaved, 0);
    m_ayRendered.fill(0);
}

void AudioMixer::reset() noexcept
{
    m_beeper.reset();
    m_dac.reset();
    for (auto &ay : m_ayChips)
        ay.reset();
    m_activeAyIndex = 0;
    m_leftBlip.reset();
    m_rightBlip.reset();
    std::ranges::fill(m_ayInterleaved, 0);
    m_ayRendered.fill(0);
}

void AudioMixer::setAyChipCount(uint8_t count) noexcept
{
    m_ayChipCount = std::clamp<uint8_t>(count, 0, kMaxAyChips);
}

void AudioMixer::setActiveAyChip(uint8_t index) noexcept
{
    if (index < kMaxAyChips)
        m_activeAyIndex = index;
}

void AudioMixer::renderAyUpTo(std::size_t chip, std::size_t sample) noexcept
{
    sample = std::min(sample, m_ayInterleaved.size() / 2);
    std::size_t &rendered = m_ayRendered[chip];
    if (sample <= rendered)
        return;
    const auto span = std::span(m_ayInterleaved).subspan(rendered * 2, (sample - rendered) * 2);
    m_ayChips[chip].renderSamples(span, m_sampleRate, m_cpuClockHz / 2, m_stereoMode);
    rendered = sample;
}

void AudioMixer::writeAyRegister(uint32_t frameTstate, uint8_t value) noexcept
{
    renderAyUpTo(m_activeAyIndex, m_leftBlip.samplesUntil(frameTstate));
    m_ayChips[m_activeAyIndex].writeRegister(value);
}

std::span<const int16_t> AudioMixer::endFrame(uint32_t frameTstates) noexcept
{
    std::ranges::fill(m_leftAccum, 0);
    std::ranges::fill(m_rightAccum, 0);

    const std::size_t count = m_leftBlip.endFrame(frameTstates, m_leftAccum);
    m_rightBlip.endFrame(frameTstates, m_rightAccum);

    if (m_ayChipCount > 0 && count > 0) {
        // The AYs rendered up to their last register write already, finish the frame
        for (uint8_t i = 0; i < m_ayChipCount; ++i)
            renderAyUpTo(i, count);
        for (std::size_t s = 0; s < count; ++s) {
            m_leftAccum[s]  += m_ayInterleaved[s * 2];
            m_rightAccum[s] += m_ayInterleaved[s * 2 + 1];
        }
    }
    // AY writes in the last CPU instruction may render beyond the nominal
    // frame boundary. Carry those samples forward just like the delta buffers.
    if (count * 2) {
        std::move(m_ayInterleaved.begin() + count * 2, m_ayInterleaved.end(), m_ayInterleaved.begin());
        std::fill(m_ayInterleaved.end() - count * 2, m_ayInterleaved.end(), 0);
    }
    for (auto &rendered : m_ayRendered)
        rendered = rendered > count ? rendered - count : 0;

    for (std::size_t s = 0; s < count; ++s) {
        m_outputPcm[s * 2]     = static_cast<int16_t>(std::clamp(m_leftAccum[s], -32768, 32767));
        m_outputPcm[s * 2 + 1] = static_cast<int16_t>(std::clamp(m_rightAccum[s], -32768, 32767));
    }

    return std::span<const int16_t>(m_outputPcm.data(), count * 2);
}

void AudioMixer::saveState(StateWriter &w) const
{
    m_beeper.saveState(w);
    m_dac.saveState(w);
    w.write(m_ayChipCount);
    w.write(m_activeAyIndex);
    for (const auto &ay : m_ayChips)
        ay.saveState(w);
}

void AudioMixer::loadState(StateReader &r)
{
    m_beeper.loadState(r);
    m_dac.loadState(r);
    m_ayChipCount   = r.read<uint8_t>();
    m_activeAyIndex = r.read<uint8_t>();
    for (auto &ay : m_ayChips)
        ay.loadState(r);
}

} // namespace zxkit
