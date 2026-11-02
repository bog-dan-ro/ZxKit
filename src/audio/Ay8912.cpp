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

#include "zxkit/audio/Ay8912.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace zxkit {
namespace {

// Measured DAC curves from Ayumi (Peter Sovietov, MIT); see LICENSES/ayumi.txt.
constexpr std::array<double, 32> ymDac = {
    0, 0, .00465400167849, .00772106507973,
    .0109559777218, .0139620050355, .0169985503929, .0200198367285,
    .024368657969, .029694056611, .0350652323186, .0403906309606,
    .0485389486534, .0583352407111, .0680552376593, .0777752346075,
    .0925154497597, .111085679408, .129747463188, .148485542077,
    .17666895552, .211551079576, .246387426566, .281101701381,
    .333730067903, .400427252613, .467383840696, .53443198291,
    .635172045472, .75800717174, .879926756695, 1
};
constexpr auto volumeTables = [] {
    std::array<std::array<int32_t, 32>, 2> result{};
    for (std::size_t i = 0; i < 32; ++i) {
        result[0][i] = Ay8912::kVolumeLevels[i / 2];
        result[1][i] = static_cast<int32_t>(ymDac[i] * 7422 + .5);
    }
    return result;
}();

// Causal, integrated Blackman-windowed sinc, cutoff 0.4 * output sample rate.
// Each amplitude edge adds only its finite correction to the unfiltered level.
// There is no accumulating floating-point integrator or per-render allocation.
// The 16-sample group delay is independent of the chip clock and frame size.
constexpr std::size_t filterLength = 32;
constexpr uint32_t filterPhases = 256;
using StepFilter = std::array<std::array<float, filterLength>, filterPhases>;
const StepFilter &stepFilter()
{
    static const StepFilter table = [] {
        constexpr std::size_t count = filterLength * filterPhases;
        std::array<double, count + 1> integral{};
        const auto impulse = [](double t) {
            const double x = t - filterLength / 2.;
            const double sinc = std::abs(x) < 1e-12 ? .8 : std::sin(.8 * std::numbers::pi * x) / (std::numbers::pi * x);
            const double angle = 2 * std::numbers::pi * t / filterLength;
            return sinc * (.42 - .5 * std::cos(angle) + .08 * std::cos(2 * angle));
        };
        for (std::size_t i = 1; i <= count; ++i)
            integral[i] = integral[i - 1] + (impulse(double(i - 1) / filterPhases) + impulse(double(i) / filterPhases)) / (2 * filterPhases);
        StepFilter result{};
        for (std::size_t phase = 0; phase < filterPhases; ++phase)
            for (std::size_t i = 0; i < filterLength; ++i)
                result[phase][i] = static_cast<float>(integral[i * filterPhases + phase] / integral.back() - 1.);
        return result;
    }();
    return table;
}

} // namespace

Ay8912::Ay8912()
{
    (void)stepFilter(); // Prepare once outside the audio render path.
    reset();
}

void Ay8912::reset() noexcept
{
    m_selectedReg = 0;
    m_regs.fill(0);
    m_toneCount.fill(0);
    m_toneOut.fill(1);
    m_toneMask = 7;
    m_outputMask = 0;
    m_outputEnvelope = 0;
    m_averagedToneMask = 0;
    m_noiseCount = 0;
    m_noiseRng = 1;
    m_noiseOut = 1;
    m_envCount = 0;
    m_envStep = 0;
    m_envVolume = 0;
    m_envHolding = false;
    m_envAttack = 0;
    m_phaseAccum = 0;
    m_left = m_right = 0;
    m_filterIndex = 0;
    m_leftTail.fill(0);
    m_rightTail.fill(0);
    m_dirty = true;
    updatePeriods();
}

void Ay8912::updatePeriods() noexcept
{
    for (std::size_t c = 0; c < 3; ++c)
        m_tonePeriod[c] = std::max<uint16_t>(1, m_regs[2 * c] | (uint16_t(m_regs[2 * c + 1]) << 8));
    m_noisePeriod = std::max<uint8_t>(1, m_regs[6]) * 2;
    m_envPeriod = std::max<uint16_t>(1, m_regs[11] | (uint16_t(m_regs[12]) << 8));
}

void Ay8912::selectRegister(uint8_t reg) noexcept
{
    m_selectedReg = reg;
}

uint8_t Ay8912::readRegister() const noexcept
{
    return m_selectedReg < 16 ? m_regs[m_selectedReg] : 0xFF;
}

void Ay8912::writeRegister(uint8_t val) noexcept
{
    if (m_selectedReg >= 16)
        return;
    m_regs[m_selectedReg] = val & kRegMasks[m_selectedReg];
    if (m_selectedReg < 7 || m_selectedReg == 11 || m_selectedReg == 12)
        updatePeriods();
    if (m_selectedReg == 13) {
        m_envCount = 0;
        m_envStep = 31;
        m_envHolding = false;
        m_envAttack = (m_regs[13] & 0x04) ? 0x1F : 0x00;
        m_envVolume = m_envStep ^ m_envAttack;
    }
    m_dirty = true;
}

void Ay8912::updateOutput(AyStereoMode mode, uint64_t elapsed, uint64_t phaseScale) noexcept
{
    const uint8_t mixer = m_regs[7];
    const uint8_t mask = (m_toneMask | mixer | m_averagedToneMask) & ((m_noiseOut ? 7 : 0) | (mixer >> 3)) & 7;
    // AY has 16 distinct DAC levels; its paired internal envelope steps do
    // not change the output. Fixed-volume voices do not use the envelope.
    const uint8_t envelope = ((m_regs[8] | m_regs[9] | m_regs[10]) & 0x10)
        ? (m_model == AyModel::AY8912 ? m_envVolume & 0x1e : m_envVolume) : 0;
    if (!m_dirty && mask == m_outputMask && envelope == m_outputEnvelope)
        return;
    if (m_dirty || envelope != m_outputEnvelope) {
        const auto &levels = volumeTables[m_model == AyModel::YM2149 ? 1 : 0];
        for (int c = 0; c < 3; ++c) {
            const uint8_t volume = m_regs[8 + c];
            int32_t level = levels[(volume & 0x10) ? m_envVolume : (volume & 15) * 2 + 1];
            if (m_averagedToneMask & (1 << c))
                level /= 2;
            if (mode == AyStereoMode::Mono) {
                m_channelLeft[c] = m_channelRight[c] = level;
            } else {
                const int centre = mode == AyStereoMode::ACB ? 2 : 1;
                m_channelLeft[c] = c == 0 ? level : level * (c == centre ? 7 : 3) / 10;
                m_channelRight[c] = c == centre ? level * 7 / 10 : c == 0 ? level * 3 / 10 : level;
            }
        }
    }
    m_dirty = false;
    m_outputMask = mask;
    m_outputEnvelope = envelope;
    int32_t left = 0, right = 0;
    for (int c = 0; c < 3; ++c) {
        if (mask & (1 << c)) {
            left += m_channelLeft[c];
            right += m_channelRight[c];
        }
    }
    if (mode == AyStereoMode::Mono)
        left = right = left * 2 / 3;
    const float deltaLeft = left - m_left;
    const float deltaRight = right - m_right;
    m_left = left;
    m_right = right;
    if (deltaLeft == 0 && deltaRight == 0)
        return;
    const auto phase = std::min<uint64_t>((elapsed * phaseScale + (uint64_t{1} << 31)) >> 32, filterPhases - 1);
    const auto &kernel = stepFilter()[phase];
    const std::size_t first = filterLength - m_filterIndex;
    // Two contiguous spans let compilers vectorize the edge accumulation.
    for (std::size_t i = 0; i < first; ++i) {
        m_leftTail[m_filterIndex + i] += deltaLeft * kernel[i];
        m_rightTail[m_filterIndex + i] += deltaRight * kernel[i];
    }
    for (std::size_t i = first; i < filterLength; ++i) {
        m_leftTail[i - first] += deltaLeft * kernel[i];
        m_rightTail[i - first] += deltaRight * kernel[i];
    }
}

void Ay8912::renderSamples(std::span<int32_t> stereoAccum, uint32_t sampleRate,
                          uint32_t psgClockHz, AyStereoMode stereoMode) noexcept
{
    if (sampleRate == 0 || psgClockHz == 0)
        return;
    if (stereoMode != m_lastStereoMode) {
        m_lastStereoMode = stereoMode;
        m_dirty = true;
    }
    const uint64_t divider = uint64_t(sampleRate) * 8;
    const uint64_t phaseScale = (uint64_t(filterPhases) << 32) / psgClockHz;
    // With noise disabled, a square wave above the output sample rate has
    // only its DC component in the filter's passband. Coalesce those edges;
    // retain the real divider phase so later period/mixer writes stay correct.
    // Noise/envelope-modulated tones keep their edges: AM can create audible products.
    uint8_t averaged = 0;
    for (std::size_t c = 0; c < 3; ++c)
        if (!(m_regs[7] & (1 << c)) && (m_regs[7] & (8 << c)) && !(m_regs[8 + c] & 0x10) &&
            uint64_t(m_tonePeriod[c]) * 16 * sampleRate < psgClockHz)
            averaged |= 1 << c;
    if (averaged != m_averagedToneMask) {
        m_averagedToneMask = averaged;
        m_dirty = true;
    }
    const std::size_t frames = stereoAccum.size() / 2;
    if ((m_regs[8] | m_regs[9] | m_regs[10]) == 0) {
        updateOutput(stereoMode, psgClockHz - 1, phaseScale);
        const uint64_t total = m_phaseAccum + uint64_t(frames) * psgClockHz;
        advanceSilentTicks(total / divider);
        m_phaseAccum = total % divider;
        // Muting may leave a finite filter tail. Consume it, then skip the
        // silent PCM without per-sample hardware or mixer work.
        for (std::size_t i = 0; i < std::min(frames, filterLength); ++i) {
            const float left = m_leftTail[m_filterIndex];
            const float right = m_rightTail[m_filterIndex];
            stereoAccum[2 * i] += static_cast<int32_t>(left + (left >= 0 ? .5f : -.5f));
            stereoAccum[2 * i + 1] += static_cast<int32_t>(right + (right >= 0 ? .5f : -.5f));
            m_leftTail[m_filterIndex] = m_rightTail[m_filterIndex] = 0;
            m_filterIndex = (m_filterIndex + 1) % filterLength;
        }
        if (frames > filterLength)
            m_filterIndex = (m_filterIndex + frames - filterLength) % filterLength;
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        // A register write is at the start of this output interval.
        updateOutput(stereoMode, psgClockHz - 1, phaseScale);
        m_phaseAccum += psgClockHz;
        uint32_t ticks = static_cast<uint32_t>(m_phaseAccum / divider);
        m_phaseAccum %= divider;
        while (ticks) {
            // Skip unchanged hardware ticks, preserving all counters, including
            // noise/envelope state while their channels are inaudible.
            uint32_t step = ticks;
            for (std::size_t c = 0; c < 3; ++c)
                if (!(m_averagedToneMask & (1 << c)))
                    step = std::min(step, m_toneCount[c] < m_tonePeriod[c] ? uint32_t(m_tonePeriod[c] - m_toneCount[c]) : 1);
            step = std::min(step, m_noiseCount < m_noisePeriod ? uint32_t(m_noisePeriod - m_noiseCount) : 1);
            if (!m_envHolding)
                step = std::min(step, m_envCount < m_envPeriod ? m_envPeriod - m_envCount : 1);
            advanceTicks(step);
            ticks -= step;
            const auto elapsed = uint64_t(ticks) * divider + m_phaseAccum;
            updateOutput(stereoMode, elapsed, phaseScale);
        }
        const float left = m_left + m_leftTail[m_filterIndex];
        const float right = m_right + m_rightTail[m_filterIndex];
        stereoAccum[2 * i] += static_cast<int32_t>(left + (left >= 0 ? .5f : -.5f));
        stereoAccum[2 * i + 1] += static_cast<int32_t>(right + (right >= 0 ? .5f : -.5f));
        m_leftTail[m_filterIndex] = m_rightTail[m_filterIndex] = 0;
        m_filterIndex = (m_filterIndex + 1) % filterLength;
    }
}

void Ay8912::advanceTicks(uint32_t ticks) noexcept
{
    for (std::size_t c = 0; c < 3; ++c) {
        if (m_averagedToneMask & (1 << c)) {
            // A newly shortened period takes effect at the next chip tick.
            const uint32_t first = m_toneCount[c] < m_tonePeriod[c] ? m_tonePeriod[c] - m_toneCount[c] : 1;
            if (ticks >= first) {
                const uint32_t remaining = ticks - first;
                const uint32_t toggles = 1 + remaining / m_tonePeriod[c];
                m_toneCount[c] = remaining % m_tonePeriod[c];
                if (toggles & 1) {
                    m_toneOut[c] ^= 1;
                    m_toneMask ^= 1 << c;
                }
            } else {
                m_toneCount[c] += ticks;
            }
            continue;
        }
        m_toneCount[c] += ticks;
        if (m_toneCount[c] >= m_tonePeriod[c]) {
            m_toneCount[c] = 0;
            m_toneOut[c] ^= 1;
            m_toneMask ^= 1 << c;
        }
    }
    m_noiseCount += ticks;
    if (m_noiseCount >= m_noisePeriod) {
        m_noiseCount = 0;
        const uint32_t bit = (m_noiseRng ^ (m_noiseRng >> 3)) & 1;
        m_noiseRng = (m_noiseRng >> 1) | (bit << 16);
        m_noiseOut = m_noiseRng & 1;
    }
    if (m_envHolding)
        return;
    m_envCount += ticks;
    if (m_envCount < m_envPeriod)
        return;
    m_envCount = 0;
    stepEnvelope();
}

void Ay8912::stepEnvelope() noexcept
{
    if (m_envStep == 0) {
        const uint8_t shape = m_regs[13];
        if ((shape & 0x08) == 0) {
            m_envAttack = 0;
            m_envHolding = true;
        } else if (shape & 0x01) {
            if (shape & 0x02)
                m_envAttack ^= 0x1F;
            m_envHolding = true;
        } else {
            if (shape & 0x02)
                m_envAttack ^= 0x1F;
            m_envStep = 31;
        }
    } else {
        --m_envStep;
    }
    m_envVolume = m_envStep ^ m_envAttack;
}

void Ay8912::advanceSilentTicks(uint64_t ticks) noexcept
{
    if (!ticks)
        return;
    // A shortened divider starts at its next tick, then repeats normally.
    const auto advance = [ticks](auto &counter, uint32_t period) {
        const uint64_t first = counter < period ? period - counter : 1;
        if (ticks < first) {
            counter += ticks;
            return uint64_t{0};
        }
        const uint64_t remaining = ticks - first;
        counter = remaining % period;
        return 1 + remaining / period;
    };
    for (std::size_t c = 0; c < 3; ++c) {
        if (advance(m_toneCount[c], m_tonePeriod[c]) & 1) {
            m_toneOut[c] ^= 1;
            m_toneMask ^= 1 << c;
        }
    }
    for (uint64_t changes = advance(m_noiseCount, m_noisePeriod); changes; --changes) {
        const uint32_t bit = (m_noiseRng ^ (m_noiseRng >> 3)) & 1;
        m_noiseRng = (m_noiseRng >> 1) | (bit << 16);
    }
    m_noiseOut = m_noiseRng & 1;
    if (!m_envHolding) {
        for (uint64_t changes = advance(m_envCount, m_envPeriod); changes && !m_envHolding; --changes)
            stepEnvelope();
        if (m_envHolding)
            m_envCount = 0;
    }
}

void Ay8912::saveState(StateWriter &w) const
{
    w.write(m_selectedReg);
    w.writeBytes(m_regs);
    for (auto tc : m_toneCount)
        w.write(tc);
    w.writeBytes(m_toneOut);
    w.write(m_noiseCount);
    w.write(m_noiseRng);
    w.write(m_noiseOut);
    w.write(m_envCount);
    w.write(m_envStep);
    w.write(m_envVolume);
    w.writeBool(m_envHolding);
    w.write(m_envAttack);
    w.write(m_phaseAccum);
    w.write(m_left);
    w.write(m_right);
    w.write(static_cast<uint8_t>(m_filterIndex));
    for (const auto &tail : {m_leftTail, m_rightTail})
        for (float value : tail)
            w.write(std::bit_cast<uint32_t>(value));
}

void Ay8912::loadState(StateReader &r)
{
    m_selectedReg = r.read<uint8_t>();
    r.readBytes(m_regs);
    for (auto &tc : m_toneCount)
        tc = r.read<uint16_t>();
    r.readBytes(m_toneOut);
    m_toneMask = m_toneOut[0] | (m_toneOut[1] << 1) | (m_toneOut[2] << 2);
    m_noiseCount = r.read<uint8_t>();
    m_noiseRng = r.read<uint32_t>();
    m_noiseOut = r.read<uint8_t>();
    m_envCount = r.read<uint32_t>();
    m_envStep = r.read<uint8_t>();
    m_envVolume = r.read<uint8_t>();
    m_envHolding = r.readBool();
    m_envAttack = r.read<uint8_t>();
    m_phaseAccum = r.read<uint64_t>();
    m_left = r.read<int32_t>();
    m_right = r.read<int32_t>();
    m_filterIndex = r.read<uint8_t>() % filterLength;
    for (auto *tail : {&m_leftTail, &m_rightTail})
        for (auto &value : *tail)
            value = std::bit_cast<float>(r.read<uint32_t>());
    updatePeriods();
    m_dirty = true;
}

} // namespace zxkit
