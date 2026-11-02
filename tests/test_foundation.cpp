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

#include <zxkit/zxkit.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <print>
#include <thread>
#include <vector>

using namespace zxkit;

namespace {

void testExceptions()
{
    bool caughtFormat = false;
    try {
        throw FormatException("Corrupt header");
    } catch (const Exception &ex) {
        caughtFormat = true;
        assert(ex.rawMessage() == "Corrupt header");
        assert(ex.where().line() > 0);
    }
    assert(caughtFormat);
}

void testSpinSharedMutex()
{
    SpinSharedMutex mutex;
    mutex.lock();
    assert(!mutex.try_lock());
    assert(!mutex.try_lock_shared());
    mutex.unlock();

    assert(mutex.try_lock_shared());
    assert(mutex.try_lock_shared()); // readers share
    assert(!mutex.try_lock());       // but keep writers out
    mutex.unlock_shared();
    mutex.unlock_shared();
    assert(mutex.try_lock());
    mutex.unlock();

    // Writers exclude each other and the readers: the pair must always be consistent
    struct Pair {
        uint64_t a = 0;
        uint64_t b = 0;
    };
    auto shared = makeShared<Pair>();
    constexpr int kThreads = 4;
    constexpr int kIterations = 50'000;
    std::atomic<bool> torn{false};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&shared] {
                for (int i = 0; i < kIterations; ++i) {
                    auto pair = shared.writeLock();
                    ++pair->a;
                    ++pair->b;
                }
            });
            threads.emplace_back([&shared, &torn] {
                for (int i = 0; i < kIterations; ++i) {
                    const auto pair = shared.readLock();
                    if (pair->a != pair->b)
                        torn = true;
                }
            });
        }
    }
    assert(!torn);
    assert(shared.readLock()->a == uint64_t{kThreads} * kIterations);

    // A thread re-locking in a tight loop (like the Engine) can't starve another writer
    std::atomic<bool> stop{false};
    std::jthread busy([&shared, &stop] {
        while (!stop.load(std::memory_order_relaxed))
            ++shared.writeLock()->a;
    });
    for (int i = 0; i < 1000; ++i)
        ++shared.writeLock()->b;
    stop = true;
}

void testCallbackHandle()
{
    int calls = 0;
    CallbackHandle<std::function<void(int)>> handle([&calls](int n) { calls += n; });
    const Weak<std::function<void(int)>> weak = handle.weak();
    if (auto fn = weak.lock())
        (*fn.readLock())(2);
    assert(calls == 2);

    handle.reset();
    assert(!weak.lock()); // the owner is gone, so is the callback

    // reset() waits for a call in progress: no call can start or still run after it returns
    std::atomic<bool> alive{true};
    std::atomic<bool> calledWhileDead{false};
    std::atomic<int> started{0};
    CallbackHandle<std::function<void()>> slow([&] {
        ++started;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (!alive)
            calledWhileDead = true;
    });
    std::jthread caller([weak = slow.weak()](std::stop_token token) {
        while (!token.stop_requested()) {
            if (auto fn = weak.lock()) {
                const auto guard = fn.readLock();
                if (*guard)
                    (*guard)();
            }
        }
    });
    while (started < 10)
        std::this_thread::yield();
    slow.reset();
    alive = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    caller.request_stop();
    caller.join();
    assert(!calledWhileDead);
}

void testAyWritesKeepTheirTiming()
{
    // Sampled speech rewrites the AY volume many times a frame: each write must
    // take effect at its own time, not at the end of the frame
    AudioMixer mixer;
    mixer.configure(3'546'900, 48'000, 70'908); // 128K
    mixer.setAyChipCount(1);
    mixer.setStereoMode(AyStereoMode::Mono);
    const auto write = [&mixer](uint32_t tstate, uint8_t reg, uint8_t value) {
        mixer.activeAy().selectRegister(reg);
        mixer.writeAyRegister(tstate, value);
    };
    write(0, 7, 0x3F);     // tone and noise off: a channel outputs its volume
    write(0, 8, 15);       // channel A loud...
    write(35'454, 8, 0);   // ...for half the frame
    const auto pcm = mixer.endFrame(70'908);
    const std::size_t samples = pcm.size() / 2;
    assert(samples > 900);
    assert(pcm[2 * (samples / 4)] > 0);
    assert(pcm[2 * (3 * samples / 4)] == 0);

    // The next frame starts where this one ended (silent), then plays the new writes
    write(17'727, 8, 15);
    const auto next = mixer.endFrame(70'908);
    assert(next[2 * (next.size() / 2 / 8)] == 0);
    assert(next[2 * (next.size() / 2 * 3 / 4)] > 0);
}

void testContinuousAudioClock()
{
    // A frame is not an integral number of output samples. The remainder must
    // survive thousands of frames instead of shortening the stream every 20 ms.
    for (const auto [clock, frame] : {std::pair{3'500'000u, 69'888u},
                                     std::pair{3'546'900u, 70'908u}}) {
        AudioMixer mixer;
        mixer.configure(clock, 48'000, frame);
        mixer.setAyChipCount(0);
        uint64_t samples = 0;
        for (unsigned n = 1; n <= 1'000; ++n) {
            samples += mixer.endFrame(frame).size() / 2;
            assert(samples == uint64_t(n) * frame * 48'000 / clock);
        }
        mixer.beeper().setPortFeBits(0, true, false, mixer.leftBlip(), mixer.rightBlip());
        mixer.endFrame(frame);
        mixer.reset();
        for (const auto sample : mixer.endFrame(frame))
            assert(sample == 0);
    }

    // Splitting the same timeline at arbitrary video boundaries must leave the
    // waveform intact, including writes from a CPU instruction across a boundary.
    BlipBuffer whole, split;
    whole.configure(1'001, 97, 339);
    split.configure(1'001, 97, 113);
    whole.addDelta(112, 4'000);
    whole.addDelta(114, -4'000);
    whole.addDelta(230, 2'000);
    std::vector<int32_t> expected(64, 0);
    expected.resize(whole.endFrame(339, expected));
    std::vector<int32_t> actual;
    for (unsigned n = 0; n < 3; ++n) {
        if (n == 0) {
            split.addDelta(112, 4'000);
            split.addDelta(114, -4'000);
        } else if (n == 1) {
            split.addDelta(117, 2'000);
        }
        std::array<int32_t, 64> block{};
        const auto count = split.endFrame(113, block);
        actual.insert(actual.end(), block.begin(), block.begin() + count);
    }
    assert(actual == expected);
}

void testEmulatorAudioFrameBoundaries()
{
    EmulatorConfig config;
    config.romResolver = [](std::string_view, std::size_t expected) {
        std::vector<uint8_t> rom(expected, 0);
        // DI; JP 1. The 10-cycle loop repeatedly overshoots the 69888-cycle
        // frame boundary, and Clock carries that overshoot into the next frame.
        rom[0] = 0xF3;
        rom[1] = 0xC3;
        rom[2] = 1;
        return rom;
    };
    Emulator emulator(config);
    uint64_t count = 0;
    for (unsigned n = 1; n <= 101; ++n) {
        emulator.runFrame();
        count += emulator.audioSamples().size() / 2;
        assert(count == uint64_t(n) * 69'888 * 48'000 / 3'500'000);
    }

    // Audio must be published before presentation, and the host must be able
    // to identify automatic tape turbo rather than flooding a normal-rate sink.
    std::atomic<unsigned> frames{0};
    bool audioReady = false;
    unsigned orderErrors = 0;
    CallbackHandle<AudioCallback> audio([&](std::span<const int16_t>, uint32_t, uint8_t) { audioReady = true; });
    CallbackHandle<VideoFrameCallback> video([&](const VideoFramePtr &) {
        if (!audioReady) ++orderErrors;
        audioReady = false;
        ++frames;
    });
    EngineConfig options;
    options.startPaused = true;
    options.callbacks.audio = audio.weak();
    options.callbacks.videoFrame = video.weak();
    Engine engine(config, options);
    {
        auto machine = engine.emulator().writeLock();
        TapeBlock tone;
        tone.type = TapeBlockType::PureTone;
        tone.pilotPulse = 4'000;
        tone.pilotPulses = 65'535;
        machine->tapeDeck().load({tone});
        auto tapeOptions = machine->tapeDeck().options();
        tapeOptions.turboWhilePlaying = true;
        machine->tapeDeck().setOptions(tapeOptions);
        machine->tapeDeck().setPlaying(true);
    }
    engine.setPaused(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (frames.load() < 10) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(engine.isTurbo());
    assert(!engine.isRealTime());
    engine.stop();
    assert(orderErrors == 0);
}

void testSchedulerAndStateStream()
{
    Scheduler sched;
    std::vector<int> order;
    sched.scheduleAt(200, [&](uint64_t) { order.push_back(2); });
    sched.scheduleAt(100, [&](uint64_t) { order.push_back(1); });
    const EventId idCancel = sched.scheduleAt(150, [&](uint64_t) { order.push_back(99); });
    sched.cancel(idCancel);

    assert(sched.nextEventCycle() == 100);
    sched.processDue(120);
    assert(order == std::vector<int>{1});
    sched.processDue(250);
    assert((order == std::vector<int>{1, 2}));

    StateWriter w;
    w.write<uint16_t>(0xCAFE);
    w.write<uint32_t>(0x12345678);
    w.writeBool(true);
    w.writeString("zxkit-state");

    StateReader r(w.data());
    assert(r.read<uint16_t>() == 0xCAFE);
    assert(r.read<uint32_t>() == 0x12345678);
    assert(r.readBool() == true);
    assert(r.readString() == "zxkit-state");
    assert(r.eof());
}

} // namespace

int main()
{
    testExceptions();
    testSpinSharedMutex();
    testCallbackHandle();
    testAyWritesKeepTheirTiming();
    testContinuousAudioClock();
    testEmulatorAudioFrameBoundaries();
    testSchedulerAndStateStream();
    std::println("test_foundation: ALL PASSED");
    return 0;
}
