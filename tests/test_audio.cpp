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
#include "zxkit/audio/AudioMixer.h"
#include "zxkit/core/Emulator.h"
#include "zxkit/machines/ZxNextMachine.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace zxkit;
namespace {
void write(Ay8912 &ay, uint8_t reg, uint8_t value)
{
    ay.selectRegister(reg);
    ay.writeRegister(value);
}
void tone(Ay8912 &ay, uint16_t period = 113)
{
    write(ay, 0, period & 255);
    write(ay, 1, period >> 8);
    write(ay, 7, 0x3e); // A tone only, no noise
    write(ay, 8, 15);
}
std::vector<int32_t> render(Ay8912 &ay, std::size_t frames, uint32_t rate = 48000,
                            uint32_t clock = 1773450)
{
    std::vector<int32_t> pcm(frames * 2);
    ay.renderSamples(pcm, rate, clock);
    return pcm;
}
double acRms(const std::vector<int32_t> &pcm, std::size_t skip = 128)
{
    double mean = 0, energy = 0;
    const auto count = pcm.size() / 2 - skip;
    for (std::size_t i = skip; i < pcm.size() / 2; ++i)
        mean += pcm[2 * i];
    mean /= count;
    for (std::size_t i = skip; i < pcm.size() / 2; ++i)
        energy += std::pow(pcm[2 * i] - mean, 2);
    return std::sqrt(energy / count);
}
void testSpectra()
{
    for (uint32_t rate : { 44100u, 48000u, 96000u }) {
        Ay8912 low, ultrasonic;
        tone(low, 48); // 2 kHz at the chosen clock
        tone(ultrasonic, 3); // 32 kHz at 48 kHz output, aliases to 16 kHz unfiltered
        const uint32_t clock = rate * 32;
        const double audible = acRms(render(low, 8192, rate, clock));
        const double alias = acRms(render(ultrasonic, 8192, rate, clock));
        std::printf("%u Hz: passband RMS %.3f, ultrasonic alias RMS %.3f (%.1f dB)\n", rate,
                    audible, alias, 20 * std::log10(std::max(.001, alias) / audible));
        assert(audible > 3400 && audible < 3800);
        assert(alias < audible / 200); // more than 46 dB rejection
        // Non-integral clock/sample ratios exercise the fractional edge phases.
        Ay8912 fractional;
        tone(fractional, 3);
        assert(acRms(render(fractional, 8192, rate, rate * 37 + 50)) < audible / 200);
        // Periods 0/1 are ultrasonic too; the coalesced path must retain DC.
        tone(ultrasonic, 1);
        const auto dc = render(ultrasonic, 512, rate, clock);
        assert(acRms(dc) < 1);
        assert(std::abs(dc.back() - 1113) <= 1); // right: half of A's 30% pan
    }
    // Keep an audible 12.3 kHz fundamental, reject its aliased third harmonic.
    Ay8912 harmonics;
    tone(harmonics, 9);
    const auto pcm = render(harmonics, 48128);
    const double fundamental = 1773450. / (16 * 9);
    const auto amplitude = [&](double frequency) {
        double sine = 0, cosine = 0, weightSum = 0;
        for (std::size_t i = 0; i < 48000; ++i) {
            const double weight = .5 - .5 * std::cos(2 * 3.141592653589793 * i / 47999);
            const double angle = 2 * 3.141592653589793 * frequency * i / 48000;
            sine += weight * pcm[2 * (i + 128)] * std::sin(angle);
            cosine += weight * pcm[2 * (i + 128)] * std::cos(angle);
            weightSum += weight;
        }
        return 2 * std::hypot(sine, cosine) / weightSum;
    };
    const double signal = amplitude(fundamental);
    const double alias = amplitude(48000 - 3 * fundamental);
    std::printf("Audible fundamental %.3f, aliased harmonic %.3f (%.1f dB)\n", signal, alias,
                20 * std::log10(std::max(.001, alias) / signal));
    assert(signal > 4500);
    assert(alias < signal / 200);
}
void testChunkingResetAndState()
{
    for (auto model : { AyModel::AY8912, AyModel::YM2149 }) {
        for (uint32_t rate : { 44100u, 48000u, 96000u }) {
            Ay8912 whole, chunks;
            for (auto *ay : { &whole, &chunks }) {
                ay->setModel(model);
                tone(*ay, 73);
                write(*ay, 7, 0x30); // tone+noise A/B, tone C
                write(*ay, 6, 3);
                write(*ay, 9, 16);
                write(*ay, 11, 7);
                write(*ay, 13, 10);
            }
            const auto expected = render(whole, 10037, rate);
            std::vector<int32_t> actual;
            for (std::size_t count : { 1, 7, 960, 3, 1024, 11, 8031 }) {
                const auto part = render(chunks, count, rate);
                actual.insert(actual.end(), part.begin(), part.end());
            }
            assert(actual == expected);
            StateWriter state;
            chunks.saveState(state);
            const auto continuation = render(chunks, 1079, rate);
            StateReader reader(state.data());
            chunks.loadState(reader);
            assert(reader.remaining() == 0);
            assert(render(chunks, 1079, rate) == continuation);
            chunks.reset();
            Ay8912 fresh;
            fresh.setModel(model);
            assert(chunks.model() == model);
            tone(chunks);
            tone(fresh);
            assert(render(chunks, 1079, rate) == render(fresh, 1079, rate));
        }
    }
}
void testDacAndPanning()
{
    // Independently rounded measurements (Ayumi DAC curves), full-scale 7422.
    for (auto model : { AyModel::AY8912, AyModel::YM2149 }) {
        Ay8912 ay;
        ay.setModel(model);
        write(ay, 7, 0x3f);
        for (const auto [volume, expectedAy, expectedYm] :
             { std::array{ 0, 0, 0 }, std::array{ 1, 74, 57 }, std::array{ 7, 797, 577 },
               std::array{ 15, 7422, 7422 } }) {
            write(ay, 8, volume);
            const auto pcm = render(ay, 128);
            assert(pcm[pcm.size() - 2] == (model == AyModel::AY8912 ? expectedAy : expectedYm));
        }
        for (auto stereo : { AyStereoMode::Mono, AyStereoMode::ACB, AyStereoMode::ABC }) {
            for (int c = 0; c < 3; ++c) {
                for (int other = 0; other < 3; ++other)
                    write(ay, 8 + other, c == other ? 15 : 0);
                std::array<int32_t, 256> pcm{};
                ay.renderSamples(pcm, 48000, 1773450, stereo);
                const auto l = pcm[254], r = pcm[255];
                if (stereo == AyStereoMode::Mono)
                    assert(l == 4948 && r == 4948);
                else if (c == (stereo == AyStereoMode::ACB ? 2 : 1))
                    assert(l == 5195 && r == 5195);
                else if (c == 0)
                    assert(l == 7422 && r == 2226);
                else
                    assert(l == 2226 && r == 7422);
            }
        }
    }
}
int envelopeLevel(int shape, int step)
{
    const bool attack = shape & 4;
    if (step < 32)
        return attack ? step : 31 - step;
    if (!(shape & 8))
        return 0;
    switch (shape) {
    case 8:
        return 31 - step % 32;
    case 9:
        return 0;
    case 10:
        return (step / 32) & 1 ? step % 32 : 31 - step % 32;
    case 11:
        return 31;
    case 12:
        return step % 32;
    case 13:
        return 31;
    case 14:
        return (step / 32) & 1 ? 31 - step % 32 : step % 32;
    case 15:
        return 0;
    }
    assert(false);
    return 0;
}
void testEnvelopes()
{
    for (auto model : { AyModel::AY8912, AyModel::YM2149 }) {
        std::array<int32_t, 32> levels{};
        // A slow ramp supplies stable observed DAC levels, including YM's
        // intermediate levels. Shape trajectories below are independent.
        Ay8912 reference;
        reference.setModel(model);
        write(reference, 7, 0x3f);
        write(reference, 8, 16);
        write(reference, 11, 128);
        write(reference, 13, 12);
        const auto ramp = render(reference, 32 * 128, 48000, 384000);
        for (int step = 0; step < 32; ++step)
            levels[step] = ramp[2 * (step * 128 + 64)];
        assert(levels[0] == 0 && levels[31] == 7422);
        for (int i = 2; i < 32; i += 2) {
            if (model == AyModel::AY8912)
                assert(levels[i] == levels[i + 1]);
            else
                assert(levels[i] < levels[i + 1]);
        }
        for (int shape = 0; shape < 16; ++shape) {
            Ay8912 ay;
            ay.setModel(model);
            write(ay, 7, 0x3f);
            write(ay, 8, 16);
            write(ay, 11, 128);
            write(ay, 13, shape);
            const auto pcm = render(ay, 100 * 128, 48000, 384000);
            for (int step = 0; step < 100; ++step)
                assert(pcm[2 * (step * 128 + 64)] == levels[envelopeLevel(shape, step)]);
            // Writing the same R13 restarts the envelope too.
            write(ay, 13, shape);
            const auto restarted = render(ay, 256, 48000, 384000);
            assert(restarted[128] == levels[envelopeLevel(shape, 0)]);
        }
        // Old uint16_t counter wrapped for AY periods >=32768, freezing sound.
        Ay8912 slow;
        slow.setModel(model);
        write(slow, 7, 0x3f);
        write(slow, 8, 16);
        write(slow, 11, 0xff);
        write(slow, 12, 0xff);
        write(slow, 13, 0);
        const auto pcm = render(slow, 2 * 65535 + 128, 48000, 384000);
        assert(pcm[128] == 7422);
        assert(pcm.back() < 2226); // channel A's right pan must have fallen
    }
}
void testUltrasonicEnvelopeModulation()
{
    // YM envelope steps can mix an ultrasonic tone back into the passband.
    // At one tick per envelope step and a two-tick tone, the high half-cycle
    // selects the odd DAC levels. Simply replacing the tone with 50% DC loses
    // this correlation, and gives the wrong average as well as wrong harmonics.
    Ay8912 fixed;
    fixed.setModel(AyModel::YM2149);
    write(fixed, 7, 0x3f);
    double expected = 0;
    for (int volume = 0; volume < 16; ++volume) {
        write(fixed, 8, volume);
        const auto pcm = render(fixed, 128);
        expected += pcm[pcm.size() - 2] / 32.;
    }
    Ay8912 modulated;
    modulated.setModel(AyModel::YM2149);
    tone(modulated, 1);
    write(modulated, 8, 16);
    write(modulated, 11, 1);
    write(modulated, 13, 8);
    const auto pcm = render(modulated, 8448, 48000, 1536000);
    double mean = 0;
    for (std::size_t i = 256; i < pcm.size() / 2; ++i)
        mean += pcm[2 * i] / 8192.;
    assert(std::abs(mean - expected) < 1);
}
void testSilentAdvancement()
{
    for (int shape = 0; shape < 16; ++shape) {
        Ay8912 bulk, individual;
        for (auto *ay : { &bulk, &individual }) {
            ay->setModel(AyModel::YM2149);
            write(*ay, 0, 3);
            write(*ay, 2, 79);
            write(*ay, 4, 13);
            write(*ay, 6, 7);
            write(*ay, 11, 11);
            write(*ay, 13, shape);
        }
        render(bulk, 5003);
        std::array<int32_t, 2> sample{};
        for (int i = 0; i < 5003; ++i)
            individual.renderSamples(sample, 48000, 1773450);
        StateWriter a, b;
        bulk.saveState(a);
        individual.saveState(b);
        assert(std::ranges::equal(a.data(), b.data()));
        for (auto *ay : { &bulk, &individual }) {
            write(*ay, 8, 16);
            write(*ay, 9, 15);
        }
        assert(render(bulk, 1027) == render(individual, 1027));
        for (auto *ay : { &bulk, &individual }) {
            write(*ay, 8, 0);
            write(*ay, 9, 0);
        }
        const auto tail = render(bulk, 257);
        std::vector<int32_t> pieces;
        for (std::size_t count : { 1, 3, 7, 80, 166 }) {
            const auto part = render(individual, count);
            pieces.insert(pieces.end(), part.begin(), part.end());
        }
        assert(tail == pieces);
        assert(std::all_of(tail.begin() + 64, tail.end(), [](auto value) { return value == 0; }));
    }
}
void testSpeakerResponses()
{
    for (uint32_t rate : { 44100u, 48000u, 96000u }) {
        const auto gain = [rate](SpeakerModel model, double hz, bool ay) {
            SpeakerFilter filter;
            filter.setModel(model);
            filter.configure(rate);
            double energy = 0;
            for (uint32_t i = 0; i < rate; ++i) {
                const double sample = 5000 * std::sin(2 * 3.141592653589793 * hz * i / rate);
                const auto output = filter.process(ay ? 0 : sample, ay ? sample : 0, 0);
                if (ay)
                    assert(output.right == 0);
                if (i >= rate / 2)
                    energy += output.left * output.left;
            }
            return std::sqrt(energy / (rate / 2)) / (5000 / std::sqrt(2.));
        };
        const double tvLow = gain(SpeakerModel::TV, 30, true);
        const double tvMid = gain(SpeakerModel::TV, 1000, true);
        const double tvHigh = gain(SpeakerModel::TV, 18000, true);
        const double beeperLow = gain(SpeakerModel::Beeper, 50, false);
        const double beeperMid = gain(SpeakerModel::Beeper, 1500, false);
        const double beeperHigh = gain(SpeakerModel::Beeper, 18000, false);
        std::printf("Speaker %u Hz: TV %.3f/%.3f/%.3f, beeper %.3f/%.3f/%.3f\n", rate, tvLow, tvMid,
                    tvHigh, beeperLow, beeperMid, beeperHigh);
        assert(tvLow < .35 && tvMid > .9 && tvMid < 1.05 && tvHigh < .8);
        assert(beeperLow < .01 && beeperMid > .65 && beeperHigh < .4);
        assert(std::abs(gain(SpeakerModel::Beeper, 1000, true) - 1) < 1e-5);
        assert(std::abs(gain(SpeakerModel::Unfiltered, 1000, false) - 1) < 1e-5);
    }
}

void testSpeakerTransitionsAndState()
{
    SpeakerFilter filter;
    filter.configure(48000);
    assert(filter.bypassed());
    assert(filter.process(0, 1000, -1000).left == 1000);
    filter.setModel(SpeakerModel::TV);
    assert(!filter.bypassed());
    double previous = 1000;
    for (int i = 0; i < 480; ++i) {
        // Rapid changes are deferred until the current crossfade finishes.
        if (i == 50)
            filter.setModel(SpeakerModel::Unfiltered);
        if (i == 100)
            filter.setModel(SpeakerModel::Beeper);
        const auto output = filter.process(0, 1000, -1000);
        assert(std::abs(output.left - previous) <= 5);
        assert(output.right == -output.left);
        previous = output.left;
    }
    assert(previous == 1000);
    filter.setModel(SpeakerModel::TV);
    for (int i = 0; i < 47; ++i)
        filter.process(i % 2 ? 4096 : -4096, i * 3, -i * 3);
    StateWriter state;
    filter.saveState(state);
    std::vector<SpeakerFilter::Stereo> expected;
    for (int i = 0; i < 1000; ++i)
        expected.push_back(filter.process(i % 2 ? 4096 : -4096, i * 3, -i * 3));
    StateReader reader(state.data());
    filter.loadState(reader);
    assert(reader.remaining() == 0);
    for (int i = 0; i < 1000; ++i) {
        const auto actual = filter.process(i % 2 ? 4096 : -4096, i * 3, -i * 3);
        assert(actual.left == expected[i].left && actual.right == expected[i].right);
    }
    filter.reset();
    assert(filter.model() == SpeakerModel::TV);
    for (int i = 0; i < 20000; ++i) {
        const auto quiet = filter.process(1000, 500, 500);
        assert(quiet.left == 0 && quiet.right == 0); // constant DC never excites a reset filter
    }
    filter.setModel(SpeakerModel::Unfiltered);
    StateReader differentPreference(state.data());
    filter.loadState(differentPreference);
    assert(filter.model() == SpeakerModel::Unfiltered && filter.bypassed());
}

void testSpeakerRoutingAndMixerState()
{
    for (auto model : { SpeakerModel::Unfiltered, SpeakerModel::TV, SpeakerModel::Beeper }) {
        AudioMixer split, joined;
        for (auto *mixer : { &split, &joined }) {
            mixer->setAyChipCount(0);
            mixer->setSpeakerModel(model);
            mixer->reset();
            mixer->writeBeeper(0, true, false);
            mixer->writeBeeper(50000, false, false);
            mixer->writeTapeEar(69950, true);
        }
        std::vector<int16_t> actual;
        for (uint32_t length : { 69888u, 1000u }) {
            const auto pcm = split.endFrame(length);
            actual.insert(actual.end(), pcm.begin(), pcm.end());
        }
        assert(std::ranges::equal(actual, joined.endFrame(70888)));
    }
    // External DACs stay direct for every speaker choice, including stereo.
    std::vector<int16_t> reference;
    for (auto model : { SpeakerModel::Unfiltered, SpeakerModel::TV, SpeakerModel::Beeper }) {
        AudioMixer mixer;
        mixer.setAyChipCount(0);
        mixer.setSpeakerModel(model);
        mixer.reset();
        mixer.dac().writeChannel(0, 0, 200, mixer.leftBlip(), mixer.rightBlip());
        mixer.dac().writeChannel(10000, 2, 40, mixer.leftBlip(), mixer.rightBlip());
        const auto pcm = mixer.endFrame(69888);
        if (reference.empty())
            reference.assign(pcm.begin(), pcm.end());
        else
            assert(std::ranges::equal(pcm, reference));
    }
    for (auto model : { SpeakerModel::Unfiltered, SpeakerModel::TV, SpeakerModel::Beeper }) {
        AudioMixer mixer;
        mixer.setSpeakerModel(model);
        mixer.reset();
        tone(mixer.activeAy(), 113);
        mixer.writeBeeper(0, true, false);
        mixer.writeBeeper(12345, false, true);
        mixer.writeTapeEar(45678, true);
        mixer.dac().writeMono(50000, 180, mixer.leftBlip(), mixer.rightBlip());
        // AY and ULA writes straddling the video boundary leave pending samples.
        mixer.activeAy().selectRegister(8);
        mixer.writeAyRegister(69950, 13);
        mixer.writeBeeper(69950, true, true);
        mixer.endFrame(69888);
        StateWriter state;
        mixer.saveState(state);
        const auto continuation = mixer.endFrame(69888);
        std::vector<int16_t> expected(continuation.begin(), continuation.end());
        StateReader reader(state.data());
        mixer.loadState(reader);
        assert(reader.remaining() == 0);
        assert(std::ranges::equal(mixer.endFrame(69888), expected));
        mixer.reset();
        assert(mixer.speakerModel() == model);
        for (const auto sample : mixer.endFrame(69888))
            assert(sample == 0);
    }
    // An inactive AY cannot leak through a selected speaker route.
    AudioMixer disabled;
    disabled.setAyChipCount(0);
    write(disabled.activeAy(), 7, 0x3f);
    disabled.activeAy().selectRegister(8);
    disabled.writeAyRegister(30000, 15);
    for (auto sample : disabled.endFrame(69888))
        assert(sample == 0);
}

void testWritesAndHeadroom()
{
    AudioMixer mixer;
    mixer.configure(3546900, 48000, 70908);
    mixer.setAyChipCount(3);
    for (int c = 0; c < 3; ++c) {
        mixer.setActiveAyChip(c);
        auto &ay = mixer.activeAy();
        write(ay, 7, 0x3f);
        write(ay, 8, 15);
        write(ay, 9, 15);
        write(ay, 10, 15);
    }
    const auto pcm = mixer.endFrame(70908);
    // Three loud AYs deliberately saturate instead of wrapping.
    assert(pcm.back() == 32767);
    assert(std::all_of(pcm.begin(), pcm.end(), [](auto s) { return s >= -5000; }));
    // A late register write's ringing/filter tail must survive the video boundary.
    AudioMixer partitioned, single;
    for (auto *mix : { &partitioned, &single }) {
        mix->configure(3500000, 48000, 69888);
        write(mix->activeAy(), 7, 0x3f);
        mix->activeAy().selectRegister(8);
        mix->writeAyRegister(69860, 15);
    }
    std::vector<int16_t> split;
    auto first = partitioned.endFrame(69888);
    split.insert(split.end(), first.begin(), first.end());
    auto second = partitioned.endFrame(1000);
    split.insert(split.end(), second.begin(), second.end());
    const auto joined = single.endFrame(70888);
    assert(std::equal(split.begin(), split.end(), joined.begin(), joined.end()));
}

EmulatorConfig quietMachine(std::string_view machineId)
{
    EmulatorConfig config;
    config.machineId = machineId;
    config.romResolver = [](std::string_view, std::size_t size) {
        return std::vector<uint8_t>(size, 0x76); // HALT: leave sound devices alone.
    };
    return config;
}

void testEmulatorResetSilencesAudio()
{
    using namespace std::literals;
    for (const auto machineId : { "48k"sv, "128k"sv, "next"sv }) {
        for (const auto kind : { ResetKind::Soft, ResetKind::Hard }) {
            for (const auto model :
                 { SpeakerModel::Unfiltered, SpeakerModel::TV, SpeakerModel::Beeper }) {
                for (const bool midFrame : { false, true }) {
                    Emulator emulator(quietMachine(machineId));
                    auto &mixer = emulator.audioMixer();
                    mixer.setSpeakerModel(model);
                    mixer.setAyModel(AyModel::YM2149);
                    mixer.setStereoMode(AyStereoMode::ABC);
                    const auto chipCount = mixer.ayChipCount();
                    for (uint8_t chip = 0; chip < chipCount; ++chip)
                        tone(mixer.ayChip(chip));
                    mixer.writeBeeper(0, true, false);
                    mixer.dac().writeMono(0, 200, mixer.leftBlip(), mixer.rightBlip());
                    assert(emulator.runFrame() == RunStatus::FrameDone);
                    assert(std::ranges::any_of(emulator.audioSamples(),
                                               [](auto sample) { return sample != 0; }));

                    if (midFrame) {
                        for (unsigned line = 0; line < 100; ++line)
                            assert(emulator.runScanline() == RunStatus::LineDone);
                        const auto tstate = emulator.clock().frameBaseTstates();
                        mixer.writeBeeper(tstate, false, true);
                        mixer.writeTapeEar(tstate, true);
                        mixer.dac().writeMono(tstate, 50, mixer.leftBlip(), mixer.rightBlip());
                        for (uint8_t chip = 0; chip < chipCount; ++chip) {
                            mixer.setActiveAyChip(chip);
                            mixer.activeAy().selectRegister(8);
                            mixer.writeAyRegister(tstate, 9);
                        }
                    }

                    emulator.reset(kind);
                    assert(emulator.audioSamples().empty());
                    assert(mixer.ayChipCount() == chipCount);
                    assert(mixer.speakerModel() == model);
                    assert(mixer.ayModel() == AyModel::YM2149);
                    assert(mixer.stereoMode() == AyStereoMode::ABC);
                    for (unsigned frame = 0; frame < 3; ++frame) {
                        assert(emulator.runFrame() == RunStatus::FrameDone);
                        assert(!emulator.audioSamples().empty());
                        assert(std::ranges::all_of(emulator.audioSamples(),
                                                   [](auto sample) { return sample == 0; }));
                    }
                }
            }
        }
    }
}

void testEmulatorNmiAudio()
{
    using namespace std::literals;
    enum class NmiSource { Cpu, Multiface, DivMmc };
    for (const auto machineId : { "128k"sv, "next"sv }) {
        for (const auto source : { NmiSource::Cpu, NmiSource::Multiface, NmiSource::DivMmc }) {
            if (machineId != "next"sv && source != NmiSource::Cpu)
                continue;
            for (const bool silenceAy : { false, true }) {
                auto config = quietMachine(machineId);
                config.romResolver = [silenceAy](std::string_view, std::size_t size) {
                    std::vector<uint8_t> rom(size, 0x76);
                    if (silenceAy) {
                        // NMI handler: select AY register 8, set its volume to zero, HALT.
                        constexpr std::array<uint8_t, 13> handler{ 0x01, 0xFD, 0xFF, 0x3E, 0x08,
                                                                   0xED, 0x79, 0x06, 0xBF, 0xAF,
                                                                   0xED, 0x79, 0x76 };
                        std::ranges::copy(handler, rom.begin() + 0x66);
                    }
                    return rom;
                };
                Emulator emulator(config);
                Emulator reference(quietMachine(machineId));
                for (auto *computer : { &emulator, &reference }) {
                    computer->audioMixer().setSpeakerModel(SpeakerModel::Unfiltered);
                    tone(computer->audioMixer().activeAy());
                    assert(computer->runFrame() == RunStatus::FrameDone);
                    for (unsigned line = 0; line < 100; ++line)
                        assert(computer->runScanline() == RunStatus::LineDone);
                }
                const auto cycles = emulator.clock().totalTstates();
                if (source == NmiSource::Cpu) {
                    emulator.triggerNmi();
                } else {
                    auto &next = static_cast<ZxNextMachine &>(emulator.machine());
                    next.nextReg().write(0x06, 0x18);
                    assert(emulator.triggerNextNmi(source == NmiSource::Multiface));
                }
                assert(emulator.clock().totalTstates() == cycles);
                for (unsigned frame = 0; frame < 3; ++frame) {
                    assert(emulator.runFrame() == RunStatus::FrameDone);
                    assert(reference.runFrame() == RunStatus::FrameDone);
                    assert(!emulator.audioSamples().empty());
                    if (!silenceAy) {
                        // An NMI alone keeps the AY running on its continuous timeline.
                        assert(std::ranges::equal(emulator.audioSamples(),
                                                  reference.audioSamples()));
                        assert(std::ranges::any_of(emulator.audioSamples(),
                                                   [](auto sample) { return sample != 0; }));
                    } else if (frame > 0) {
                        // Fresh handler writes replace the tone; an old PCM frame cannot loop.
                        assert(std::ranges::all_of(emulator.audioSamples(),
                                                   [](auto sample) { return sample == 0; }));
                    }
                }
            }
        }
    }
}
} // namespace
int main()
{
    testSpectra();
    testChunkingResetAndState();
    testDacAndPanning();
    testEnvelopes();
    testUltrasonicEnvelopeModulation();
    testSilentAdvancement();
    testWritesAndHeadroom();
    testSpeakerResponses();
    testSpeakerTransitionsAndState();
    testSpeakerRoutingAndMixerState();
    testEmulatorNmiAudio();
    testEmulatorResetSilencesAudio();
    std::puts("AY synthesis regressions passed");
}
