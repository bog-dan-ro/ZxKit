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

#include "zxkit/core/Emulator.h"
#include "zxkit/core/Events.h"
#include "zxkit/core/Shared.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace zxkit {

/// Where an Engine reports to. Every callback is optional.
struct EngineCallbacks {
    Weak<VideoFrameCallback> videoFrame;
    Weak<AudioCallback> audio;
    Weak<PauseCallback> paused;
    Weak<ResumeCallback> resumed;
    Weak<LogCallback> log;
    Weak<MediaActivityCallback> mediaActivity;
    Weak<AudioClockCallback> audioClock;
};

struct EngineConfig {
    bool startPaused = false;
    /// Emulation speed, in percent of the machine's real speed.
    uint32_t speedPercent = 100;
    /// Runs as fast as possible (fast forward), whatever the speed.
    bool turbo = false;
    EngineCallbacks callbacks;
    /// Optional host services owned by the emulation thread. Called with the
    /// emulator locked; use the supplied reference, never acquire its lock again.
    /// Construct thread-affine objects in started, destroy them in stopped.
    /// poll runs once per frame (also on refresh while paused), never per opcode.
    struct ThreadHooks {
        std::function<void(Emulator &)> started;
        std::function<void(Emulator &)> poll;
        std::function<void(Emulator &)> stopped;
    } threadHooks;
};

/// Runs an `Emulator` in real time on its own thread.
///
/// The emulator is shared: any thread talks to it through its lock, e.g.
/// `engine.emulator().writeLock()->loadMediaFile(path)`. The Engine thread takes
/// that lock for one scanline at a time and the lock is a fair spin lock, so a
/// host waits at most a scanline (microseconds) and the Engine thread is never
/// suspended by the kernel while it runs.
///
/// Frames, audio, pauses and errors are reported through `Weak` callbacks, called
/// from the Engine thread with no emulator lock held. While paused the Engine thread
/// sleeps (it doesn't spin).
class Engine {
public:
    explicit Engine(EmulatorConfig emulatorConfig = {}, EngineConfig config = {});
    explicit Engine(Shared<Emulator> emulator, EngineConfig config = {});
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// The emulated computer. The pointer itself never changes, lock it to use it.
    const Shared<Emulator> &emulator() const noexcept { return m_emulator; }

    /// Pauses at the end of the current frame. Breakpoints pause on their own.
    void setPaused(bool paused) noexcept;
    bool isPaused() const noexcept { return m_state.load(std::memory_order_acquire) & kPaused; }
    /// Emulation speed in percent of the machine's real speed (e.g. 50 = half speed).
    void setSpeed(uint32_t percent) noexcept;
    uint32_t speed() const noexcept { return m_speedPercent.load(std::memory_order_relaxed); }
    /// Fast forward: runs as fast as possible, whatever the speed.
    void setTurbo(bool turbo) noexcept;
    bool isTurbo() const noexcept
    {
        return (m_state.load(std::memory_order_acquire) & kTurbo)
               || m_tapeTurbo.load(std::memory_order_relaxed);
    }
    /// Running in real time (100% speed, no turbo): only then the audio plays at its pitch.
    bool isRealTime() const noexcept { return speed() == 100 && !isTurbo(); }

    /// Makes a paused Engine publish a frame of the current state, e.g. after a
    /// snapshot was loaded or memory was edited. (A running Engine does it anyway.)
    void refreshFrame() noexcept;
    /// Steps over the current instruction (see `Emulator::stepOver()`), resuming the
    /// emulation when the instruction needs to run to completion (calls, block instructions).
    void stepOver();
    void stepOut();
    void runTo(uint16_t address);

    /// Stops the Engine thread (the destructor does it). Call it from the owner's thread.
    void stop() noexcept;

    void setCallbacks(EngineCallbacks callbacks);
    void setVideoFrameCallback(Weak<VideoFrameCallback> callback);
    void setAudioCallback(Weak<AudioCallback> callback);
    void setPauseCallback(Weak<PauseCallback> callback);
    void setResumeCallback(Weak<ResumeCallback> callback);
    void setLogCallback(Weak<LogCallback> callback);
    void setMediaActivityCallback(Weak<MediaActivityCallback> callback);
    /// Optional host device-clock pacing. Changing callbacks wakes a pacing
    /// wait; the replacement is used after the next completed frame.
    void setAudioClockCallback(Weak<AudioClockCallback> callback);

private:
    static constexpr uint32_t kPaused = 1u << 0;
    static constexpr uint32_t kTurbo = 1u << 1;
    static constexpr uint32_t kRefresh = 1u << 2;
    static constexpr uint32_t kStop = 1u << 3;
    static constexpr uint32_t kCancelRunControl = 1u << 4;

    void run();
    void setFlag(uint32_t flag, bool on) noexcept;
    void wakePacing() noexcept;
    std::optional<AudioPacingInfo::Clock::time_point> audioDeadline(const AudioPacingInfo &info) noexcept;

    template <typename Function, typename... Args>
    void notify(Weak<Function> EngineCallbacks::*callback, Args &&...args) noexcept;

    Shared<Emulator> m_emulator;
    Shared<EngineCallbacks> m_callbacks;
    EngineConfig::ThreadHooks m_threadHooks;
    std::atomic<uint32_t> m_state{0};
    std::atomic<uint32_t> m_speedPercent{100};
    std::atomic_bool m_tapeTurbo{false};
    std::mutex m_pacingMutex;
    std::condition_variable m_pacingChanged;
    uint64_t m_pacingRevision = 0; // protected by m_pacingMutex
    std::jthread m_thread; // last: it starts after everything else is ready
};

} // namespace zxkit
