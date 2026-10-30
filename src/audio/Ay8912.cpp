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

namespace zxkit {

Ay8912::Ay8912()
{
    reset();
}

void Ay8912::reset() noexcept
{
    m_selectedReg = 0;
    m_regs.fill(0);
    m_toneCount.fill(0);
    m_toneOut.fill(1);
    m_noiseCount = 0;
    m_noiseRng = 1;
    m_noiseOut = 1;
    m_envCount = 0;
    m_envStep = 0;
    m_envVolume = 0;
    m_envHolding = false;
    m_envAttack = 0;
}

void Ay8912::selectRegister(uint8_t reg) noexcept
{
    m_selectedReg = reg;
}

uint8_t Ay8912::readRegister() const noexcept
{
    if (m_selectedReg < 16)
        return m_regs[m_selectedReg];
    return 0xFF;
}

void Ay8912::writeRegister(uint8_t val) noexcept
{
    if (m_selectedReg >= 16)
        return;
    m_regs[m_selectedReg] = val & kRegMasks[m_selectedReg];
    if (m_selectedReg == 13) {
        m_envCount = 0;
        m_envStep = 15;
        m_envHolding = false;
        m_envAttack = (m_regs[13] & 0x04) ? 0x0F : 0x00;
        m_envVolume = m_envStep ^ m_envAttack;
    }
}

void Ay8912::renderSamples(std::span<int32_t> stereoAccum,
                           uint32_t sampleRate,
                           uint32_t psgClockHz,
                           AyStereoMode stereoMode) noexcept
{
    const std::size_t frames = stereoAccum.size() / 2;
    if (frames == 0 || sampleRate == 0)
        return;

    // AY internal divider is clock / 8
    const uint32_t ayTickHz = psgClockHz / 8;

    for (std::size_t i = 0; i < frames; ++i) {
        m_phaseAccum += ayTickHz;
        while (m_phaseAccum >= sampleRate) {
            m_phaseAccum -= sampleRate;
            stepInternalTick();
        }

        std::array<int32_t, 3> chVol{};
        const uint8_t mixer = m_regs[7];
        for (int c = 0; c < 3; ++c) {
            const uint8_t toneBit = ((mixer >> c) & 1) | m_toneOut[c];
            const uint8_t noiseBit = ((mixer >> (c + 3)) & 1) | m_noiseOut;
            if (toneBit & noiseBit) {
                const uint8_t vReg = m_regs[8 + c];
                const uint8_t volIdx = (vReg & 0x10) ? m_envVolume : (vReg & 0x0F);
                chVol[c] = kVolumeLevels[volIdx & 0x0F];
            }
        }

        int32_t left = 0;
        int32_t right = 0;
        switch (stereoMode) {
        case AyStereoMode::Mono:
            left = right = (chVol[0] + chVol[1] + chVol[2]) * 2 / 3;
            break;
        case AyStereoMode::ACB:
            left  = chVol[0] + (chVol[2] * 7 / 10) + (chVol[1] * 3 / 10);
            right = chVol[1] + (chVol[2] * 7 / 10) + (chVol[0] * 3 / 10);
            break;
        case AyStereoMode::ABC:
            left  = chVol[0] + (chVol[1] * 7 / 10) + (chVol[2] * 3 / 10);
            right = chVol[2] + (chVol[1] * 7 / 10) + (chVol[0] * 3 / 10);
            break;
        }

        stereoAccum[i * 2]     += left;
        stereoAccum[i * 2 + 1] += right;
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
}

void Ay8912::loadState(StateReader &r)
{
    m_selectedReg = r.read<uint8_t>();
    r.readBytes(m_regs);
    for (auto &tc : m_toneCount)
        tc = r.read<uint16_t>();
    r.readBytes(m_toneOut);
    m_noiseCount = r.read<uint8_t>();
    m_noiseRng   = r.read<uint32_t>();
    m_noiseOut   = r.read<uint8_t>();
    m_envCount   = r.read<uint16_t>();
    m_envStep    = r.read<uint8_t>();
    m_envVolume  = r.read<uint8_t>();
    m_envHolding = r.readBool();
    m_envAttack  = r.read<uint8_t>();
}

void Ay8912::stepInternalTick() noexcept
{
    for (int c = 0; c < 3; ++c) {
        uint16_t period = static_cast<uint16_t>(m_regs[c * 2] | (static_cast<uint16_t>(m_regs[c * 2 + 1]) << 8));
        if (period == 0)
            period = 1;
        if (++m_toneCount[c] >= period) {
            m_toneCount[c] = 0;
            m_toneOut[c] ^= 1;
        }
    }

    if (++m_noiseCount >= ((m_regs[6] == 0) ? 1 : m_regs[6]) * 2) {
        m_noiseCount = 0;
        const uint32_t bit = ((m_noiseRng ^ (m_noiseRng >> 3)) & 1);
        m_noiseRng = (m_noiseRng >> 1) | (bit << 16);
        m_noiseOut = static_cast<uint8_t>(m_noiseRng & 1);
    }

    if (!m_envHolding) {
        uint16_t envPeriod = static_cast<uint16_t>(m_regs[11] | (static_cast<uint16_t>(m_regs[12]) << 8));
        if (envPeriod == 0)
            envPeriod = 1;
        if (++m_envCount >= envPeriod * 2) {
            m_envCount = 0;
            if (m_envStep == 0) {
                const uint8_t shape = m_regs[13];
                if ((shape & 0x08) == 0) {
                    m_envAttack = 0;
                    m_envHolding = true;
                } else if (shape & 0x01) {
                    if (shape & 0x02)
                        m_envAttack ^= 0x0F;
                    m_envHolding = true;
                } else {
                    if (shape & 0x02)
                        m_envAttack ^= 0x0F;
                    m_envStep = 15;
                }
            } else {
                --m_envStep;
            }
            m_envVolume = m_envStep ^ m_envAttack;
        }
    }
}

} // namespace zxkit
