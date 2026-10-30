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
    : m_emulator(std::move(emulator))
    , m_callbacks(makeShared<EngineCallbacks>(std::move(config.callbacks)))
    , m_state((config.startPaused ? kPaused : 0u) | (config.turbo ? kTurbo : 0u))
    , m_speedPercent(std::max(config.speedPercent, 1u))
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
    if (on)
        m_state.fetch_or(flag, std::memory_order_acq_rel);
    else
        m_state.fetch_and(~flag, std::memory_order_acq_rel);
    m_state.notify_all();
}

void Engine::setPaused(bool paused) noexcept
{
    setFlag(kPaused, paused);
}

void Engine::setSpeed(uint32_t percent) noexcept
{
    m_speedPercent.store(std::max(percent, 1u), std::memory_order_relaxed);
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
    if (m_emulator.writeLock()->stepOver())
        setPaused(false);
    else
        refreshFrame();
}

void Engine::setCallbacks(EngineCallbacks callbacks)
{
    *m_callbacks.writeLock() = std::move(callbacks);
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
    std::vector<int16_t> audio;
    MediaActivity lastActivity;

    for (;;) {
        const uint32_t state = m_state.load(std::memory_order_acquire);
        if (state & kStop)
            break;

        if (state & kPaused) {
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
            notify(&EngineCallbacks::videoFrame, frame);
            if (!audio.empty())
                notify(&EngineCallbacks::audio, std::span<const int16_t>(audio), sampleRate, uint8_t{2});
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
            continue;
        }
        // Paced to the machine's frame rate times the speed. The lock is free while we sleep.
        const double speed = m_speedPercent.load(std::memory_order_relaxed) / 100.0;
        const auto frameDuration = std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(1.0 / (frameRate * speed)));
        deadline += frameDuration;
        const auto now = SteadyClock::now();
        if (deadline > now)
            std::this_thread::sleep_until(deadline);
        else if (now - deadline > frameDuration * 4)
            deadline = now; // too far behind (e.g. the host held the lock): don't try to catch up
    }
}

} // namespace zxkit
