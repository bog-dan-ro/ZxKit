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

#include "zxkit/core/Shared.h"
#include "zxkit/video/VideoFrame.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>

namespace zxkit {

enum class PauseReason : uint8_t {
    UserRequest = 0,
    ExecutionBreakpoint,
    MemoryWatchpoint,
    PortWatchpoint,
    NextRegWatchpoint,
    RasterBreakpoint,
    EventTrap,
    StepComplete,
    MagicBreakpoint,
    Error
};

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error
};

/// What the media devices are doing, for a host's status icons. `busyDisks` and `sdCardBusy`
/// stay on for a few frames after the last access, so the icons don't flicker.
struct MediaActivity {
    bool tapePlaying = false;
    std::size_t tapeBlock = 0;  ///< the tape's current block (`blocks().size()` at its end)
    uint8_t busyDisks = 0;      ///< a bit per floppy drive (bit 0: drive A:)
    bool sdCardBusy = false;    ///< the SD card (ZX Spectrum Next, DivMMC)

    bool operator==(const MediaActivity &) const = default;
};

/// Why and where the emulation stopped.
struct PauseEvent {
    PauseReason reason = PauseReason::UserRequest;
    uint16_t pc = 0;
    uint64_t cycle = 0;
};

// Callbacks the Engine thread invokes, without holding any emulator lock. Hosts own them
// (`Shared<VideoFrameCallback>`, or simply a `CallbackHandle`) and give the Engine `Weak`
// references, so the Engine never keeps a host object alive nor calls a destroyed one.
// They must return quickly and must not throw (exceptions are dropped).
using VideoFrameCallback = std::function<void(const VideoFramePtr &frame)>;
using AudioCallback = std::function<void(std::span<const int16_t> samples, uint32_t sampleRate, uint8_t channels)>;
using PauseCallback = std::function<void(const PauseEvent &event)>;
using ResumeCallback = std::function<void()>;
using LogCallback = std::function<void(LogLevel level, std::string_view message)>;
/// Called at the end of a frame when the media activity changed.
using MediaActivityCallback = std::function<void(const MediaActivity &activity)>;

/// Owns a callback given to an `Engine` as a `Weak` reference (see `weak()`).
///
/// `reset()` and the destructor wait for a call in progress on the Engine thread
/// to return, so whatever the callback captured can be destroyed right after.
template <typename Function>
class CallbackHandle {
public:
    CallbackHandle() = default;

    template <typename F>
        requires std::constructible_from<Function, F>
    explicit CallbackHandle(F &&function)
        : m_function(makeShared<Function>(std::forward<F>(function)))
    {}

    ~CallbackHandle() { reset(); }

    CallbackHandle(CallbackHandle &&other) noexcept = default;
    CallbackHandle &operator=(CallbackHandle &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_function = std::move(other.m_function);
        }
        return *this;
    }
    CallbackHandle(const CallbackHandle &) = delete;
    CallbackHandle &operator=(const CallbackHandle &) = delete;

    Weak<Function> weak() const noexcept { return m_function; }
    explicit operator bool() const noexcept { return static_cast<bool>(m_function); }

    void reset() noexcept
    {
        if (m_function) {
            // Taking the write lock waits for a running call (it holds a read lock)
            *m_function.writeLock() = nullptr;
            m_function = {};
        }
    }

private:
    Shared<Function> m_function;
};

} // namespace zxkit
