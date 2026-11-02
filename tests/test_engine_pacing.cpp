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

#include <zxkit/core/Engine.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace zxkit;
using namespace std::chrono_literals;
namespace {
using Clock = AudioPacingInfo::Clock;

template<class Predicate>
void waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5000ms)
{
    const auto end = Clock::now() + timeout;
    while (!predicate()) {
        assert(Clock::now() < end);
        std::this_thread::sleep_for(1ms);
    }
}

EmulatorConfig machine()
{
    EmulatorConfig config;
    config.romResolver = [](std::string_view, std::size_t expected) {
        std::vector<uint8_t> rom(expected, 0);
        rom[0] = 0xf3; // DI; JP 1: deterministic independent of downloaded ROMs
        rom[1] = 0xc3;
        rom[2] = 1;
        return rom;
    };
    return config;
}

struct Host {
    std::atomic<unsigned> frames{0}, queries{0}, pauses{0}, videos{0};
    std::atomic<uint64_t> produced{0}, consumed{0};
    std::atomic<uint32_t> observedSpeed{0};
    std::atomic_bool started{false};
    Engine *engine = nullptr;
    CallbackHandle<AudioCallback> audio;
    CallbackHandle<VideoFrameCallback> video;
    CallbackHandle<PauseCallback> pause;

    Host()
        : audio([this](std::span<const int16_t> pcm, uint32_t rate, uint8_t channels) {
            assert(rate == 48000 && channels == 2 && pcm.size() % 2 == 0);
            produced += pcm.size() / 2;
            ++frames;
          }),
          video([this](const VideoFramePtr &) { ++videos; }),
          pause([this](const PauseEvent &) { ++pauses; })
    {}

    EngineConfig config(Weak<AudioClockCallback> clock = {})
    {
        EngineConfig result;
        result.startPaused = true;
        result.callbacks.audio = audio.weak();
        result.callbacks.videoFrame = video.weak();
        result.callbacks.paused = pause.weak();
        result.callbacks.audioClock = clock;
        result.threadHooks.started = [this](Emulator &) { started = true; };
        return result;
    }

    void check(const AudioPacingInfo &info)
    {
        // The host has already received PCM and presentation for this frame.
        assert(frames == videos);
        assert(info.sampleRate == 48000 && info.channels == 2);
        assert(info.sampleFrames == 958 || info.sampleFrames == 959);
        assert(info.now <= Clock::now());
        const auto expected = std::chrono::nanoseconds(1'996'800'000ll / info.speedPercent);
        assert(info.frameDuration >= expected - 2ns && info.frameDuration <= expected + 2ns);
        observedSpeed = info.speedPercent;
        // No emulator lock is held during a pacing callback.
        assert(engine->emulator().readLock()->audioSampleRate() == 48000);
        ++queries;
    }

    void resume(Engine &value)
    {
        engine = &value;
        waitFor([&] { return started.load(); });
        value.setPaused(false);
    }
};

void testDeviceClockControlsQueue()
{
    Host host;
    CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) {
        host.check(info);
        // A fake output device permits three frames of priming. It can stop
        // consuming altogether; the Engine must sleep without holding its lock.
        if (host.produced.load() - host.consumed.load() >= 2800)
            return info.now + 5s;
        return info.now;
    });
    Engine engine(machine(), host.config(clock.weak()));
    host.resume(engine);
    waitFor([&] { return host.queries >= 3; });
    std::this_thread::sleep_for(30ms);
    assert(host.frames == 3);
    // A redraw request on a running Engine must not defeat the audio deadline
    // or speed up emulation. Paused refreshes use the separate atomic wakeup.
    engine.refreshFrame();
    std::this_thread::sleep_for(30ms);
    assert(host.frames == 3);
    const auto first = host.produced.load();
    {
        auto computer = engine.emulator().writeLock();
        auto registers = computer->registers();
        registers.setA(42);
        computer->setRegisters(registers);
    }
    assert(engine.emulator().readLock()->registers().A() == 42);
    host.consumed = first;
    // A device/queue change explicitly invalidates the previously returned deadline.
    engine.setAudioClockCallback(clock.weak());
    waitFor([&] { return host.queries >= 6; });
    std::this_thread::sleep_for(30ms);
    assert(host.frames == 6);
    assert(host.produced.load() - first >= 2800);
    const auto stop = Clock::now();
    engine.stop();
    assert(Clock::now() - stop < 500ms);
}

void testControlsInterruptHostDeadline()
{
    Host host;
    CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) {
        host.check(info);
        return info.now + 5s;
    });
    Engine engine(machine(), host.config(clock.weak()));
    host.resume(engine);
    waitFor([&] { return host.queries >= 1; });
    const auto pauseAt = Clock::now();
    engine.setPaused(true);
    waitFor([&] { return host.pauses >= 1; }, 500ms);
    assert(Clock::now() - pauseAt < 500ms);
    const auto audioFrames = host.frames.load();
    const auto queries = host.queries.load();
    const auto videos = host.videos.load();
    engine.refreshFrame();
    waitFor([&] { return host.videos > videos; });
    assert(host.frames == audioFrames && host.queries == queries);
    // Refresh adds a video without audio; use new counters for subsequent frames.
    engine.setVideoFrameCallback({});
    CallbackHandle<AudioClockCallback> resumedClock([&](const AudioPacingInfo &info) {
        host.observedSpeed = info.speedPercent;
        ++host.queries;
        const auto expected = std::chrono::nanoseconds(1'996'800'000ll / info.speedPercent);
        assert(info.frameDuration >= expected - 2ns && info.frameDuration <= expected + 2ns);
        return info.now + 5s;
    });
    engine.setAudioClockCallback(resumedClock.weak());
    engine.setSpeed(200);
    engine.setPaused(false);
    waitFor([&] { return host.observedSpeed == 200; }, 500ms);
    // A fixed-speed change must cancel a clock deadline too.
    engine.setSpeed(50);
    CallbackHandle<AudioClockCallback> slowClock([&](const AudioPacingInfo &info) {
        host.observedSpeed = info.speedPercent;
        const auto expected = std::chrono::nanoseconds(1'996'800'000ll / info.speedPercent);
        assert(info.frameDuration >= expected - 2ns && info.frameDuration <= expected + 2ns);
        return info.now + 5s;
    });
    engine.setAudioClockCallback(slowClock.weak());
    waitFor([&] { return host.observedSpeed == 50; }, 500ms);
    const auto beforeTurbo = host.frames.load();
    engine.setTurbo(true);
    waitFor([&] { return host.frames >= beforeTurbo + 8; }, 1000ms);
    engine.setPaused(true);
    waitFor([&] { return host.pauses >= 2; });
    const auto pausedFrames = host.frames.load();
    std::this_thread::sleep_for(20ms);
    assert(host.frames == pausedFrames);
    engine.stop();
}

void testClockFallbacks()
{
    for (int mode = 0; mode < 3; ++mode) {
        Host host;
        CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) -> std::optional<Clock::time_point> {
            host.check(info);
            if (mode == 1) throw std::runtime_error("output device disappeared");
            return std::nullopt;
        });
        Engine engine(machine(), host.config(clock.weak()));
        if (mode == 2) clock.reset(); // an expired Weak clock is a normal fallback
        const auto start = Clock::now();
        host.resume(engine);
        waitFor([&] { return host.frames >= 4; });
        engine.stop();
        assert(Clock::now() - start >= 30ms); // fallback must not become a busy/turbo loop
        if (mode == 2) assert(host.queries == 0);
        else assert(host.queries >= 3);
    }
}

void testDetachAndDefaultWait()
{
    Host host;
    CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) {
        host.check(info);
        return info.now + 5s;
    });
    Engine engine(machine(), host.config(clock.weak()));
    host.resume(engine);
    waitFor([&] { return host.queries >= 1; });
    clock.reset();
    engine.setAudioClockCallback({});
    waitFor([&] { return host.frames >= 3; }, 500ms);
    // setCallbacks must cancel an outstanding deadline just like the specific setter.
    std::atomic<unsigned> replacementCalls{0};
    CallbackHandle<AudioClockCallback> replacement([&](const AudioPacingInfo &info) {
        ++replacementCalls;
        return info.now + 5s;
    });
    engine.setAudioClockCallback(replacement.weak());
    waitFor([&] { return replacementCalls >= 1; });
    const auto before = host.frames.load();
    auto callbacks = host.config().callbacks;
    engine.setCallbacks(callbacks);
    waitFor([&] { return host.frames > before; }, 500ms);
    engine.stop();

    Host slow;
    auto options = slow.config();
    options.speedPercent = 1; // a normal wall-clock wait lasts nearly two seconds
    Engine lowSpeed(machine(), options);
    slow.resume(lowSpeed);
    waitFor([&] { return slow.frames >= 1; });
    const auto stop = Clock::now();
    lowSpeed.stop();
    assert(Clock::now() - stop < 500ms);
}

void testControlInsideClockCallback()
{
    Host host;
    CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) {
        host.check(info);
        // Notification happens before wait_until starts: its revision predicate
        // must preserve this wakeup rather than sleeping on the stale deadline.
        host.engine->setPaused(true);
        return info.now + 5s;
    });
    Engine engine(machine(), host.config(clock.weak()));
    host.resume(engine);
    waitFor([&] { return host.pauses >= 1; }, 500ms);
    assert(host.frames == 1);
    engine.setPaused(false);
    waitFor([&] { return host.pauses >= 2; }, 500ms);
    assert(host.frames == 2);
    engine.stop();
}

void testTurboBypassesDeviceClock()
{
    for (bool automaticTape : {false, true}) {
        Host host;
        CallbackHandle<AudioClockCallback> clock([&](const AudioPacingInfo &info) {
            ++host.queries;
            return info.now + 5s;
        });
        auto options = host.config(clock.weak());
        options.turbo = !automaticTape;
        Engine engine(machine(), options);
        if (automaticTape) {
            auto computer = engine.emulator().writeLock();
            TapeBlock tone;
            tone.type = TapeBlockType::PureTone;
            tone.pilotPulse = 4000;
            tone.pilotPulses = 65535;
            computer->tapeDeck().load({tone});
            auto tapeOptions = computer->tapeDeck().options();
            tapeOptions.turboWhilePlaying = true;
            computer->tapeDeck().setOptions(tapeOptions);
            computer->tapeDeck().setPlaying(true);
        }
        host.resume(engine);
        waitFor([&] { return host.frames >= 8; });
        engine.stop();
        assert(host.queries == 0);
    }
}
}

int main()
{
    testDeviceClockControlsQueue();
    testControlsInterruptHostDeadline();
    testClockFallbacks();
    testDetachAndDefaultWait();
    testControlInsideClockCallback();
    testTurboBypassesDeviceClock();
    std::puts("Engine device-clock pacing and controls passed");
}
