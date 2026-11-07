/* Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later */
// Standalone Linux profiling driver. See docs/profiling.md for build commands.
#include <zxkit/core/Engine.h>
#ifndef ZXKIT_PROFILE_INITIAL
#include <zxkit/video/VideoFilters.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>

using namespace zxkit;
using ProfileClock = std::chrono::steady_clock;

int main(int argc, char **argv)
{
    if (argc < 5) {
        std::fprintf(stderr,
                     "Usage: profile_engine MODE SECONDS ROM_DIRECTORY MEDIA [FILTER SCALE]\n"
                     "MODE: engine, engine-rewind, paused, frames, verify, audio, filters. Use '-' "
                     "for no media.\n");
        return 2;
    }
    const std::string mode = argv[1];
    const double seconds = std::stod(argv[2]);
    if (!(seconds > 0))
        return 2;
    EmulatorConfig config;
    const std::filesystem::path roms = argv[3];
    config.romResolver = [roms](std::string_view name, std::size_t expected) {
        std::ifstream file(roms / name, std::ios::binary);
        std::vector<uint8_t> result((std::istreambuf_iterator<char>(file)), {});
        if (result.size() != expected)
            throw std::runtime_error("Missing or invalid ROM: " + std::string(name));
        return result;
    };
    auto emulator = makeShared<Emulator>(config);
    if (mode == "engine-rewind")
        emulator.writeLock()->setRewindEnabled(true, 300);
    if (std::string(argv[4]) != "-")
        emulator.writeLock()->loadMediaFile(argv[4]);
    std::atomic<uint64_t> frames{ 0 }, samples{ 0 };
    uint64_t checksum = 0;
    CallbackHandle<VideoFrameCallback> video([&](const VideoFramePtr &frame) { ++frames; });
    CallbackHandle<AudioCallback> audio(
            [&](std::span<const int16_t> pcm, uint32_t, uint8_t) { samples += pcm.size() / 2; });
    EngineConfig ec;
    ec.startPaused = true;
    ec.callbacks.videoFrame = video.weak();
    ec.callbacks.audio = audio.weak();
    Engine engine(emulator, ec);
#ifndef ZXKIT_PROFILE_INITIAL
    VideoFilterPipeline pipeline;
    VideoFrame source(320, 256);
    for (uint32_t y = 0; y < source.height(); ++y)
        for (uint32_t x = 0; x < source.width(); ++x)
            source.scanline(y)[x] = packRgba(uint8_t(x * 37), uint8_t(y * 19), uint8_t(x ^ y));
    if (mode == "filters") {
        if (argc != 7)
            return 2;
        bool found = false;
        for (auto info : videoFilterCatalog())
            if (info.id == argv[5]) {
                pipeline.setStages({ { info.filter, uint32_t(std::stoul(argv[6])) } });
                found = true;
            }
        if (!found)
            return 2;
        pipeline.apply(source); // allocate scratch outside timing
    }
#endif
    if (mode == "frames")
        emulator.writeLock()->runFrames(10);
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    const auto start = ProfileClock::now();
    const auto end = start + std::chrono::duration<double>(seconds);
    if (mode == "engine" || mode == "engine-rewind" || mode == "paused") {
        if (mode != "paused")
            engine.setPaused(false);
        std::this_thread::sleep_until(end);
        engine.stop();
    } else if (mode == "frames") {
        while (ProfileClock::now() < end) {
            auto e = emulator.writeLock();
            auto frame = e->runFrames(1);
            checksum += frame.readLock()->pixels()[100];
            ++frames;
            samples += e->audioSamples().size() / 2;
        }
    } else if (mode == "verify") {
        checksum = 14695981039346656037ull;
        const auto hash = [&](auto values) {
            for (auto value : values)
                checksum = (checksum ^ uint64_t(value)) * 1099511628211ull;
        };
        for (unsigned i = 0; i < 200; ++i) {
            auto e = emulator.writeLock();
            const auto frame = e->runFrames(1);
            hash(frame.readLock()->pixels());
            hash(e->audioSamples());
            ++frames;
        }
        hash(emulator.writeLock()->saveSnapshot());
    } else if (mode == "audio") {
        auto e = emulator.writeLock();
        auto &mixer = e->audioMixer();
        mixer.setAyChipCount(1);
        for (auto [reg, value] : { std::pair{ 0, 100 }, { 7, 0x3e }, { 8, 15 } }) {
            mixer.activeAy().selectRegister(uint8_t(reg));
            mixer.activeAy().writeRegister(uint8_t(value));
        }
        while (ProfileClock::now() < end) {
            auto pcm = mixer.endFrame(69888);
            checksum += uint16_t(pcm[0]);
            samples += pcm.size() / 2;
            ++frames;
        }
#ifndef ZXKIT_PROFILE_INITIAL
    } else if (mode == "filters") {
        while (ProfileClock::now() < end) {
            const auto &output = pipeline.apply(source);
            checksum += output.pixels()[100];
            ++frames;
        }
#endif
    } else
        return 2;
    engine.stop();
    getrusage(RUSAGE_SELF, &after);
    const auto cpu = [](timeval v) { return v.tv_sec + v.tv_usec / 1e6; };
    const double wall = std::chrono::duration<double>(ProfileClock::now() - start).count();
    std::printf("{\"mode\":\"%s\",\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f,\"frames\":%llu,"
                "\"samples\":%llu,\"checksum\":%llu,\"max_rss_kib\":%ld,\"voluntary_switches\":%ld,"
                "\"involuntary_switches\":%ld}\n",
                mode.c_str(), wall,
                cpu(after.ru_utime) + cpu(after.ru_stime) - cpu(before.ru_utime)
                        - cpu(before.ru_stime),
                (unsigned long long)frames.load(), (unsigned long long)samples.load(),
                (unsigned long long)checksum, after.ru_maxrss, after.ru_nvcsw - before.ru_nvcsw,
                after.ru_nivcsw - before.ru_nivcsw);
}
