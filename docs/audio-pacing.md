# Host audio-clock pacing

`Engine` normally follows the machine frame rate on `std::chrono::steady_clock`.
A host can instead pace frame production from its audio device's playback
position and buffered output, through `EngineCallbacks::audioClock` or
`Engine::setAudioClockCallback()`.

The host owns a `CallbackHandle<AudioClockCallback>`, just as it owns audio and
video callbacks. The Engine calls it once per completed frame, after audio and
video delivery, without an emulator lock. `AudioPacingInfo` contains the frame's
actual PCM sample-frame count, source sample rate/channels, requested fixed speed,
nominal frame duration at that speed, and the current steady-clock time.

Return the earliest steady-clock time at which the next frame should start.
A time at or before `info.now` lets the Engine catch up immediately. Return
`std::nullopt` while the output device is unavailable, suspended, muted or not
ready to provide timing. Missing, empty, expired and throwing callbacks also
fall back to the ordinary wall-clock schedule. Manual and automatic tape turbo
bypass host pacing completely; fixed playback speeds are reported to the host.

The callback must return promptly. It should read a device-clock/queue snapshot
maintained by the output backend, rather than waiting for the device or calling
thread-affine GUI objects. There are no Qt or audio-backend dependencies in ZxKit.
Pacing controls production only: converting source PCM to the device's rate and
handling pause/mute/priming remain the host's responsibility.

For example, after the audio callback has queued/resampled this frame, a host
with a 48 kHz device and a 60 ms queue target could provide:

```cpp
std::atomic<uint32_t> queuedDeviceFrames{0}; // updated by the output backend
std::atomic_bool deviceReady{false};
zxkit::CallbackHandle<zxkit::AudioClockCallback> clock(
    [&](const zxkit::AudioPacingInfo &info)
        -> std::optional<zxkit::AudioPacingInfo::Clock::time_point> {
        if (!deviceReady.load())
            return std::nullopt;
        constexpr uint32_t deviceRate = 48'000;
        constexpr uint32_t target = 2'880;
        const auto queued = queuedDeviceFrames.load();
        const auto excess = queued > target ? queued - target : 0;
        return info.now + std::chrono::nanoseconds(uint64_t(excess) * 1'000'000'000 / deviceRate);
    });
engine.setAudioClockCallback(clock.weak());
```

Queue depth is in **device** sample frames after resampling, not source samples
or bytes. Update/reset the backend's counters with its device and pause/resume
lifecycle. The target is the queue depth before generating the next frame, so buffering
varies by one frame. The example does not implement a backend.

Pausing, stopping, changing fixed speed or turbo, replacing all callbacks, and
setting an audio-clock callback interrupt a pending pacing wait. The next frame
uses the updated settings. Calling `setAudioClockCallback()` with the same Weak
reference can invalidate a deadline after an abrupt device/queue change. Call it
from an owner/control thread; it uses a short control mutex and is not an audio
real-time callback operation. Ordinary per-buffer device progress needs no Engine
notification when the callback returns its predicted drain deadline each frame.

Detach with `engine.setAudioClockCallback({})` when removing the output clock;
this cancels a previously returned future deadline immediately. Destroying the
handle prevents further calls and waits for a call in progress, but does not
by itself cancel an already returned deadline. As with the other callbacks,
keep captured data alive until the handle has been reset/destroyed.

The Engine sleeps without holding the emulator or callback locks. Paused refreshes
publish video without audio or device-clock queries. Running refresh requests
do not bypass pacing or accelerate emulation. Resuming, leaving turbo and
losing a host clock rebase the normal schedule so an old device deadline does not
cause a catch-up burst. The optional callback and interruptible wait do not alter
the machine's instruction, audio-sample or video-frame timing.
