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

#include "zxkit/core/Engine.h"

#include "zxkit/core/Exception.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zxkit {

Engine::Engine(EmulatorConfig emulatorConfig, EngineConfig config)
    : Engine(makeShared<Emulator>(std::move(emulatorConfig)), std::move(config))
{}

Engine::Engine(Shared<Emulator> emulator, EngineConfig config)
    : m_emulator(std::move(emulator)), m_callbacks(makeShared<EngineCallbacks>(std::move(config.callbacks))),
      m_threadHooks(std::move(config.threadHooks)),
      m_state((config.startPaused ? kPaused : 0u) | (config.turbo ? kTurbo : 0u)),
      m_speedPercent(std::max(config.speedPercent, 1u))
{
    if (!m_emulator)
        throw InvalidArgumentException("An Engine needs an Emulator");
    m_thread = std::jthread([this] { run(); });
}

Engine::~Engine()
{
    stop();
}

void Engine::stop() noexcept
{
    if (!m_thread.joinable())
        return;
    setFlag(kStop, true);
    m_thread.join();
}

void Engine::setFlag(uint32_t flag, bool on) noexcept
{
    bool changed;
    {
        const std::lock_guard lock(m_pacingMutex);
        const auto previous = on ? m_state.fetch_or(flag, std::memory_order_acq_rel)
                                 : m_state.fetch_and(~flag, std::memory_order_acq_rel);
        changed = bool(previous & flag) != on;
        if (changed && flag != kRefresh)
            ++m_pacingRevision;
    }
    m_state.notify_all();
    if (changed && flag != kRefresh)
        m_pacingChanged.notify_all();
}

void Engine::wakePacing() noexcept
{
    {
        const std::lock_guard lock(m_pacingMutex);
        ++m_pacingRevision;
    }
    m_pacingChanged.notify_all();
}

void Engine::setPaused(bool paused) noexcept
{
    setFlag(kPaused | kCancelRunControl, paused);
}

void Engine::setSpeed(uint32_t percent) noexcept
{
    const auto speed = std::max(percent, 1u);
    if (m_speedPercent.exchange(speed, std::memory_order_relaxed) != speed)
        wakePacing();
}

void Engine::setTurbo(bool turbo) noexcept
{
    setFlag(kTurbo, turbo);
}

void Engine::refreshFrame() noexcept
{
    setFlag(kRefresh, true);
}

void Engine::stepOver()
{
    auto emulator = m_emulator.writeLock();
    if (emulator->stepOver()) setPaused(false);
    else refreshFrame();
}

void Engine::stepOut()
{
    auto emulator = m_emulator.writeLock();
    if (emulator->stepOut()) setPaused(false);
    else refreshFrame();
}
void Engine::runTo(uint16_t address)
{
    auto emulator = m_emulator.writeLock();
    if (emulator->runTo(address)) setPaused(false);
    else refreshFrame();
}

void Engine::setCallbacks(EngineCallbacks callbacks)
{
    *m_callbacks.writeLock() = std::move(callbacks);
    wakePacing();
}

void Engine::setVideoFrameCallback(Weak<VideoFrameCallback> callback)
{
    m_callbacks.writeLock()->videoFrame = std::move(callback);
}

void Engine::setAudioCallback(Weak<AudioCallback> callback)
{
    m_callbacks.writeLock()->audio = std::move(callback);
}

void Engine::setPauseCallback(Weak<PauseCallback> callback)
{
    m_callbacks.writeLock()->paused = std::move(callback);
}

void Engine::setResumeCallback(Weak<ResumeCallback> callback)
{
    m_callbacks.writeLock()->resumed = std::move(callback);
}

void Engine::setLogCallback(Weak<LogCallback> callback)
{
    m_callbacks.writeLock()->log = std::move(callback);
}

void Engine::setMediaActivityCallback(Weak<MediaActivityCallback> callback)
{
    m_callbacks.writeLock()->mediaActivity = std::move(callback);
}

void Engine::setAudioClockCallback(Weak<AudioClockCallback> callback)
{
    m_callbacks.writeLock()->audioClock = std::move(callback);
    wakePacing();
}

std::optional<AudioPacingInfo::Clock::time_point> Engine::audioDeadline(const AudioPacingInfo &info) noexcept
{
    const auto callback = m_callbacks.readLock()->audioClock.lock();
    if (callback) {
        try {
            const auto guard = callback.readLock();
            if (*guard)
                return (*guard)(info);
        } catch (...) {
            // A device disappearing or a failed host clock must not stop audio.
        }
    }
    return std::nullopt;
}

template <typename Function, typename... Args>
void Engine::notify(Weak<Function> EngineCallbacks::*callback, Args &&...args) noexcept
{
    // One lock at a time: copy the reference out of the callbacks, then call
    // under the callback's own (read) lock, which is what its owner waits for.
    const Shared<Function> function = ((*m_callbacks.readLock()).*callback).lock();
    if (!function)
        return;
    try {
        const auto guard = function.readLock();
        if (*guard)
            (*guard)(std::forward<Args>(args)...);
    } catch (...) {
        // A host callback must not take the emulation down
    }
}

void Engine::run()
{
    using SteadyClock = std::chrono::steady_clock;

    auto deadline = SteadyClock::now();
    // Starting paused is not an event, the host asked for it
    bool pauseReported = isPaused();
    bool audioPaced = false;
    std::vector<int16_t> audio;
    MediaActivity lastActivity;

    const auto service = [this](const std::function<void(Emulator &)> &hook) {
        if (!hook)
            return;
        try {
            hook(*m_emulator.writeLock());
        } catch (const std::exception &e) {
            notify(&EngineCallbacks::log, LogLevel::Error, std::string_view(e.what()));
        } catch (...) {
            notify(&EngineCallbacks::log, LogLevel::Error, std::string_view("Host service failed"));
        }
    };
    service(m_threadHooks.started);

    for (;;) {
        uint32_t state;
        uint64_t pacingRevision;
        {
            const std::lock_guard lock(m_pacingMutex);
            pacingRevision = m_pacingRevision;
            state = m_state.load(std::memory_order_acquire);
        }
        if (state & kStop)
            break;

        service(m_threadHooks.poll);

        if (state & kPaused) {
            if (state & kCancelRunControl) {
                auto emulator = m_emulator.writeLock();
                // A new run command may have superseded this pause while we waited for the lock.
                if ((m_state.load(std::memory_order_acquire) & (kPaused | kCancelRunControl)) == (kPaused | kCancelRunControl)) {
                    emulator->cancelRunControl();
                    m_state.fetch_and(~kCancelRunControl, std::memory_order_acq_rel);
                    pauseReported = false; // acknowledge even a resume immediately followed by pause
                }
            }
            if (!pauseReported) {
                pauseReported = true;
                PauseEvent event;
                {
                    const auto emulator = m_emulator.readLock();
                    event.pc = emulator->registers().PC;
                    event.cycle = emulator->clock().totalTstates();
                }
                notify(&EngineCallbacks::paused, event);
            } else if (state & kRefresh) {
                m_state.fetch_and(~kRefresh, std::memory_order_acq_rel);
                const VideoFramePtr frame = m_emulator.writeLock()->refreshFrame();
                notify(&EngineCallbacks::videoFrame, frame);
            } else {
                m_state.wait(state, std::memory_order_acquire); // sleeps until something changes
            }
            continue;
        }

        if (pauseReported) {
            pauseReported = false;
            deadline = SteadyClock::now();
            audioPaced = false;
            notify(&EngineCallbacks::resumed);
        }
        if (state & kRefresh)
            m_state.fetch_and(~kRefresh, std::memory_order_acq_rel); // the next frame will do

        // Run a frame, one scanline per lock so other threads get in between
        VideoFramePtr frame;
        std::optional<PauseEvent> pause;
        uint32_t sampleRate = 0;
        double frameRate = 50.0;
        bool tapeTurbo = false;
        MediaActivity activity = lastActivity;
        try {
            for (;;) {
                auto emulator = m_emulator.writeLock();
                const RunStatus status = emulator->runScanline();
                pause = emulator->takePauseEvent();
                if (status == RunStatus::FrameDone) {
                    frame = emulator->lastFrame();
                    const auto samples = emulator->audioSamples();
                    audio.assign(samples.begin(), samples.end());
                    sampleRate = emulator->audioSampleRate();
                    frameRate = emulator->frameRateHz();
                    const auto &tape = emulator->tapeDeck();
                    tapeTurbo = tape.isPlaying() && tape.options().turboWhilePlaying;
                    activity = emulator->mediaActivity();
                    break;
                }
                if (pause || (m_state.load(std::memory_order_relaxed) & kStop))
                    break;
            }
        } catch (const std::exception &e) {
            const std::string message = e.what();
            pause = PauseEvent{PauseReason::Error, m_emulator.readLock()->registers().PC, 0};
            notify(&EngineCallbacks::log, LogLevel::Error, std::string_view(message));
        } catch (...) {
            pause = PauseEvent{PauseReason::Error, m_emulator.readLock()->registers().PC, 0};
            notify(&EngineCallbacks::log, LogLevel::Error, std::string_view("Unknown emulation error"));
        }

        if (activity != lastActivity) {
            lastActivity = activity;
            notify(&EngineCallbacks::mediaActivity, activity);
        }
        if (frame) {
            m_tapeTurbo.store(tapeTurbo, std::memory_order_relaxed);
            if (!audio.empty())
                notify(&EngineCallbacks::audio, std::span<const int16_t>(audio), sampleRate, uint8_t{2});
            notify(&EngineCallbacks::videoFrame, frame);
        }

        if (pause) {
            m_state.fetch_or(kPaused, std::memory_order_acq_rel);
            pauseReported = true;
            notify(&EngineCallbacks::paused, *pause);
            continue;
        }

        if (!frame)
            continue;
        if ((m_state.load(std::memory_order_acquire) & kTurbo) || tapeTurbo) {
            deadline = SteadyClock::now();
            audioPaced = false;
            continue;
        }
        // Paced to the machine's frame rate times the speed. The lock is free while we sleep.
        const uint32_t speedPercent = m_speedPercent.load(std::memory_order_relaxed);
        const double speed = speedPercent / 100.0;
        const auto frameDuration = std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(1.0 / (frameRate * speed)));
        const auto now = SteadyClock::now();
        const AudioPacingInfo pacing{now, std::chrono::duration_cast<std::chrono::nanoseconds>(frameDuration),
                                     audio.size() / 2, sampleRate, speedPercent, 2};
        if (const auto hostDeadline = audioDeadline(pacing)) {
            deadline = *hostDeadline;
            audioPaced = true;
        } else {
            if (audioPaced)
                deadline = now;
            audioPaced = false;
            deadline += frameDuration;
            if (now > deadline && now - deadline > frameDuration * 4)
                deadline = now; // too far behind: don't try to catch up
        }
        if (deadline > SteadyClock::now()) {
            std::unique_lock lock(m_pacingMutex);
            if (m_pacingChanged.wait_until(lock, deadline, [&] { return m_pacingRevision != pacingRevision; }))
                deadline = SteadyClock::now(); // a control change invalidated the old schedule
        }
    }
    service(m_threadHooks.stopped);
}

} // namespace zxkit
