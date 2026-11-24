// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"
#include "../media/MediaFile.h"

#include <array>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
Object rzxJson(Emulator &emulator)
{
    constexpr std::array states{ "idle",    "playing", "recording", "completed",
                                 "stopped", "error",   "reviewing" };
    const auto progress = emulator.rzx().progress();
    return { { "state", states.at(unsigned(progress.state)) },
             { "completedIntervals", progress.completedFrames },
             { "totalIntervals", progress.totalFrames },
             { "positionMilliseconds", emulator.rzxPositionMilliseconds() },
             { "durationMilliseconds", emulator.rzxDurationMilliseconds() },
             { "seeking", emulator.isRzxSeeking() },
             { "errorMessage", progress.error } };
}
} // namespace

std::string Tools::rzxStatus(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        auto output = rzxJson(*current.emulator);
        output["unsavedRecordingBytes"] = current.pendingRzx.size();
        return output;
    });
}

std::string Tools::startRzxRecording(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        if (!current.pendingRzx.empty())
            throw std::invalid_argument("Save the pending recording before starting another"s);
        current.emulator->startRzxRecording();
        return rzxJson(*current.emulator);
    });
}

std::string Tools::stopRzxRecording(std::string sessionId, std::string path, bool overwrite)
{
    return result([&] {
        auto &current = session(sessionId);
        const auto output = outputPath(path);
        if (upper(output.extension().string()) != ".RZX"sv)
            throw std::invalid_argument("RZX output must use the .rzx extension"s);
        if (current.emulator->isRzxSeeking())
            throw std::invalid_argument("Advance or cancel the seek before saving the recording"s);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        const zxkit::tool::ImageLock lock(zxkit::tool::imagePath(output));
        zxkit::tool::atomicOutput(output, overwrite, [&](const auto &temporary) {
            if (current.pendingRzx.empty())
                current.pendingRzx = current.emulator->stopRzxRecording();
            writeFile(temporary, current.pendingRzx, false);
        });
        const auto size = current.pendingRzx.size();
        current.pendingRzx = {};
        return Object{ { "path", output.string() }, { "bytes", size } };
    });
}

std::string Tools::stopRzxPlayback(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        if (emulator.rzx().isRecording() || emulator.rzx().isReviewing())
            throw std::invalid_argument("Use stopRzxRecording for an active recording"s);
        emulator.stopRzxPlayback();
        return rzxJson(emulator);
    });
}

std::string Tools::beginRzxSeek(std::string sessionId, int target, std::string unit)
{
    return result([&] {
        if (target < 0)
            throw std::invalid_argument("RZX seek target must be nonnegative"s);
        auto &current = session(sessionId);
        auto &emulator = *current.emulator;
        if (unit == "frames"sv)
            emulator.beginRzxSeekFrame(std::size_t(target));
        else if (unit == "milliseconds"sv)
            emulator.beginRzxSeek(uint64_t(target));
        else
            throw std::invalid_argument("RZX seek unit must be frames or milliseconds"s);
        current.lastPause.reset();
        return rzxJson(emulator);
    });
}

std::string Tools::advanceRzxSeek(std::string sessionId, int maxFrames, int maxMilliseconds)
{
    return result([&] {
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        auto &current = session(sessionId);
        auto &emulator = *current.emulator;
        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMilliseconds);
        auto reason = "frameBudget"sv;
        for (int slice = 0; emulator.isRzxSeeking() && slice < maxFrames; ++slice) {
            if (std::chrono::steady_clock::now() >= deadline) {
                reason = "timeBudget"sv;
                break;
            }
            try {
                emulator.advanceRzxSeek(1);
            } catch (...) {
                if (auto pause = emulator.takePauseEvent()) {
                    current.lastPause = pause;
                    reason = "breakpoint"sv;
                    break;
                }
                throw;
            }
        }
        auto output = rzxJson(emulator);
        output["stopReason"] = emulator.isRzxSeeking() ? reason : "complete"sv;
        if (reason == "breakpoint"sv)
            output["lastPause"] = status(current).as_object().at("lastPause");
        return output;
    });
}

std::string Tools::cancelRzxSeek(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        emulator.cancelRzxSeek();
        return rzxJson(emulator);
    });
}

std::string Tools::resumeRzxRecording(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        emulator.resumeRzxRecording();
        return rzxJson(emulator);
    });
}
} // namespace zxkit::mcp_tool
