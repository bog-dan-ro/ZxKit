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

// Nominal responses follow FUSE's speaker_filter, tv_filter and ula_filter
// (Copyright (c) 2026 Fredrick Meunier, GPL-2.0-or-later). The nominal speaker
// parameters are representative approximations, not measurements of one unit.
#include "zxkit/audio/SpeakerFilter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace zxkit {
namespace {
double flushTiny(double value) noexcept { return std::abs(value) < 1e-20 ? 0 : value; }
}

void SpeakerFilter::configure(uint32_t sampleRate) noexcept
{
    m_sampleRate = sampleRate ? sampleRate : 48000;
    const double rate = m_sampleRate;
    const double tau = 2 * std::numbers::pi;
    m_tvHigh = std::exp(-tau * std::min(100., rate * .2) / rate);
    m_tvLow = -std::expm1(-tau * std::min(10000., rate * .4) / rate);
    m_rise = -std::expm1(-1 / (rate * 36.53558495933805e-6));
    m_fall = -std::expm1(-1 / (rate * 68.86073172982108e-6));
    const double omega = tau * std::min(750., rate * .2) / rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / std::sqrt(2.);
    const double a0 = 1 + alpha;
    m_b0 = m_b2 = (1 + cosine) / (2 * a0);
    m_b1 = -(1 + cosine) / a0;
    m_a1 = -2 * cosine / a0;
    m_a2 = (1 - alpha) / a0;
    m_fadeFrames = std::max(1u, m_sampleRate / 200); // 5 ms
    reset();
}

void SpeakerFilter::reset() noexcept
{
    m_current = {};
    m_current.model = m_model;
    m_previous = m_current;
    m_fadeRemaining = 0;
}

SpeakerFilter::Stereo SpeakerFilter::apply(Path &path, double ula, double ayLeft, double ayRight) const noexcept
{
    if (path.model == SpeakerModel::Unfiltered)
        return {ula + ayLeft, ula + ayRight};
    if (!path.initialized)
        path.ula = ula;
    else
        path.ula += (ula > path.ula ? m_rise : m_fall) * (ula - path.ula);
    path.ula = flushTiny(path.ula);
    if (path.model == SpeakerModel::TV) {
        std::array<double, 2> input{path.ula + ayLeft, path.ula + ayRight};
        if (!path.initialized) {
            path.previous = input;
            path.initialized = true;
            return {0, 0};
        }
        for (std::size_t c = 0; c < 2; ++c) {
            path.highPass[c] = flushTiny(m_tvHigh * (path.highPass[c] + input[c] - path.previous[c]));
            path.previous[c] = input[c];
            path.lowPass[c] = flushTiny(path.lowPass[c] + m_tvLow * (path.highPass[c] - path.lowPass[c]));
        }
        return {path.lowPass[0], path.lowPass[1]};
    }
    // The electrical response precedes the small moving-coil speaker.
    if (!path.initialized) {
        path.ula = ula;
        path.z1 = -m_b0 * ula;
        path.z2 = m_b2 * ula;
        path.initialized = true;
        return {ayLeft, ayRight};
    }
    const double output = m_b0 * path.ula + path.z1;
    path.z1 = flushTiny(m_b1 * path.ula - m_a1 * output + path.z2);
    path.z2 = flushTiny(m_b2 * path.ula - m_a2 * output);
    return {output + ayLeft, output + ayRight};
}

SpeakerFilter::Stereo SpeakerFilter::process(double ula, double ayLeft, double ayRight) noexcept
{
    if (!m_fadeRemaining && m_current.model != m_model) {
        m_previous = m_current;
        m_current = {};
        m_current.model = m_model;
        m_fadeRemaining = m_fadeFrames;
    }
    const Stereo current = apply(m_current, ula, ayLeft, ayRight);
    if (!m_fadeRemaining)
        return current;
    const Stereo previous = apply(m_previous, ula, ayLeft, ayRight);
    const double mix = double(m_fadeFrames - m_fadeRemaining + 1) / m_fadeFrames;
    --m_fadeRemaining;
    return {previous.left + (current.left - previous.left) * mix,
            previous.right + (current.right - previous.right) * mix};
}

void SpeakerFilter::saveState(StateWriter &writer) const
{
    writer.write(static_cast<uint8_t>(m_model));
    writer.write(m_sampleRate);
    writer.write(m_fadeRemaining);
    for (const Path *path : {&m_current, &m_previous}) {
        writer.write(static_cast<uint8_t>(path->model));
        writer.writeBool(path->initialized);
        for (const auto *values : {&path->previous, &path->highPass, &path->lowPass})
            for (double value : *values)
                writer.write(std::bit_cast<uint64_t>(value));
        for (double value : {path->ula, path->z1, path->z2})
            writer.write(std::bit_cast<uint64_t>(value));
    }
}

void SpeakerFilter::loadState(StateReader &reader)
{
    const auto model = reader.read<uint8_t>();
    const auto rate = reader.read<uint32_t>();
    const auto remaining = reader.read<uint32_t>();
    Path current, previous;
    for (Path *path : {&current, &previous}) {
        const auto tag = reader.read<uint8_t>();
        if (tag > static_cast<uint8_t>(SpeakerModel::Beeper))
            throw FormatException("Invalid speaker filter model");
        path->model = static_cast<SpeakerModel>(tag);
        path->initialized = reader.readBool();
        const auto value = [&reader] {
            const double result = std::bit_cast<double>(reader.read<uint64_t>());
            if (!std::isfinite(result))
                throw FormatException("Invalid speaker filter state");
            return result;
        };
        for (auto *values : {&path->previous, &path->highPass, &path->lowPass})
            for (double &v : *values)
                v = value();
        path->ula = value(); path->z1 = value(); path->z2 = value();
    }
    if (model != static_cast<uint8_t>(m_model) || rate != m_sampleRate) {
        reset(); // Rewind retains the host's current sound preference.
        return;
    }
    if (remaining > m_fadeFrames)
        throw FormatException("Invalid speaker transition state");
    m_current = current;
    m_previous = previous;
    m_fadeRemaining = remaining;
}

} // namespace zxkit
