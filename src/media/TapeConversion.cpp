// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/TapeConversion.h"
#include "archive/ArchiveInternal.h"
#include <string>
#include <string_view>

namespace zxkit::TapeConversion {
using namespace std::literals;
namespace {
using namespace archive;
void chunk(Bytes &out, std::string_view tag, View data)
{
    std::size_t p = out.size();
    out.resize(p + 8);
    textAt(out, p, tag);
    setDword(out, p + 4, data.size());
    append(out, data);
}
void pulse(Bytes &out, uint32_t value)
{
    require(value <= 0x7FFFFFFF, "PZX pulse too long"sv);
    std::size_t p = out.size();
    if (value < 0x8000) {
        out.resize(p + 2);
        setWord(out, p, value);
    } else {
        if (value >= 65536) {
            out.resize(p + 2);
            setWord(out, p, 0x8001);
            p += 2;
        }
        out.resize(p + 4);
        setWord(out, p, 0x8000 | (value >> 16));
        setWord(out, p + 2, value & 65535);
    }
}
template <class F> void signal(std::span<const TapeBlock> blocks, F fn)
{
    TapeCursor cursor;
    std::size_t steps = 0;
    while (!cursor.atEnd(blocks)) {
        require(++steps <= 100000000, "Tape signal exceeds conversion limit"sv);
        auto edge = cursor.next(blocks);
        fn(edge);
        if (edge.flags & TapeEdge::Flags::TapeEnd)
            break;
        require(!(edge.flags & (TapeEdge::Flags::Stop | TapeEdge::Flags::Stop48k)),
                "Tape stop requires an interactive player"sv);
    }
}
void signalOnly(std::span<const TapeBlock> blocks)
{
    for (auto &b : blocks)
        require(uint8_t(b.type) < 0x20 || uint8_t(b.type) >= 0xF0
                        || b.type == TapeBlockType::SetSignalLevel,
                "Destination cannot preserve tape control/metadata blocks"sv);
}
} // namespace
Bytes writePzx(std::span<const TapeBlock> blocks)
{
    Bytes out;
    const Bytes version{ 1, 0 };
    chunk(out, "PZXT"sv, version);
    TapeCursor playback;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto &b = blocks[index];
        if (b.type == TapeBlockType::TextDesc) {
            Bytes info(b.description.begin(), b.description.end());
            info.push_back(0);
            chunk(out, "BRWS"sv, info);
            playback.skipTo(index + 1);
            continue;
        }
        if (b.type == TapeBlockType::Pause) {
            (void)playback.next(blocks);
            if (!b.pauseAfterMs) {
                const Bytes stop{ 0, 0 };
                chunk(out, "STOP"sv, stop);
            } else {
                Bytes pause(4);
                setDword(pause, 0, uint32_t(b.pauseAfterMs) * 3500);
                chunk(out, "PAUS"sv, pause);
            }
            continue;
        }
        if (b.type == TapeBlockType::StopIf48k) {
            (void)playback.next(blocks);
            const Bytes stop{ 1, 0 };
            chunk(out, "STOP"sv, stop);
            continue;
        }
        signalOnly(std::span(&b, 1));
        std::vector<TapeEdge> edges;
        while (!playback.atEnd(blocks) && playback.blockIndex() == index) {
            require(edges.size() < 10000000, "PZX signal exceeds conversion limit"sv);
            edges.push_back(playback.next(blocks));
        }
        if (b.type == TapeBlockType::SetSignalLevel)
            continue;
        if (b.type == TapeBlockType::StandardData || b.type == TapeBlockType::TurboData
            || b.type == TapeBlockType::PureData) {
            bool pilot = b.type != TapeBlockType::PureData;
            unsigned count = pilot ? b.pilotPulses : 0;
            Bytes pulses;
            if (pilot) {
                if (!edges.empty() && edges.front().level)
                    pulse(pulses, 0);
                for (unsigned i = 0; i < count; ++i)
                    pulse(pulses, b.pilotPulse);
                pulse(pulses, b.sync1Pulse);
                pulse(pulses, b.sync2Pulse);
                chunk(out, "PULS"sv, pulses);
            }
            require(b.usedBits >= 1 && b.usedBits <= 8, "Invalid tape bit count"sv);
            Bytes data(16);
            uint64_t bits = b.data.empty() ? 0 : (b.data.size() - 1) * 8 + b.usedBits;
            require(bits <= 0x7FFFFFFF, "PZX data too large"sv);
            const std::size_t dataStart = pilot ? count + 2 : 0;
            const bool initial = dataStart < edges.size() ? edges[dataStart].level : false;
            setDword(data, 0, bits | (uint32_t(initial) << 31));
            data[6] = 2;
            data[7] = 2;
            setWord(data, 8, b.zeroPulse);
            setWord(data, 10, b.zeroPulse);
            setWord(data, 12, b.onePulse);
            setWord(data, 14, b.onePulse);
            append(data, b.data);
            chunk(out, "DATA"sv, data);
            if (b.pauseAfterMs) {
                Bytes pause(4);
                setDword(pause, 0,
                         uint32_t(b.pauseAfterMs) * 3500 | (uint32_t(edges.back().level) << 31));
                chunk(out, "PAUS"sv, pause);
            }
            continue;
        }
        Bytes pulses;
        bool expected = false;
        for (const auto &e : edges) {
            if (!e.length)
                continue;
            if (e.level != expected)
                pulse(pulses, 0);
            pulse(pulses, e.length);
            expected = !e.level;
            require(pulses.size() < 64 * 1024 * 1024, "PZX output too large"sv);
        }
        chunk(out, "PULS"sv, pulses);
    }
    return out;
}
Bytes writeCsw(std::span<const TapeBlock> blocks)
{
    signalOnly(blocks);
    Bytes out(52, 0);
    textAt(out, 0, "Compressed Square Wave\x1A"sv);
    out[23] = 2;
    out[24] = 0;
    setDword(out, 25, 3500000);
    out[33] = 1;
    textAt(out, 36, "ZxKit"sv);
    uint64_t pending = 0;
    bool current = false;
    bool first = true;
    uint32_t count = 0;
    auto emit = [&] {
        require(pending && pending <= UINT32_MAX, "Invalid CSW pulse length"sv);
        if (pending <= 255)
            out.push_back(uint8_t(pending));
        else {
            auto p = out.size();
            out.resize(p + 5);
            setDword(out, p + 1, pending);
        }
        ++count;
        pending = 0;
        require(out.size() <= 64 * 1024 * 1024, "CSW output too large"sv);
    };
    signal(blocks, [&](const TapeEdge &e) {
        if (!e.length)
            return;
        if (first) {
            current = e.level;
            out[34] = current ? 1 : 0;
            first = false;
        }
        if (e.level != current) {
            emit();
            current = e.level;
        }
        pending += e.length;
    });
    if (pending)
        emit();
    setDword(out, 29, count);
    return out;
}
Bytes tapToWav(std::span<const TapeBlock> blocks, uint32_t rate)
{
    require(rate >= 8000 && rate <= 192000, "WAV sample rate must be 8000..192000"sv);
    for (auto &b : blocks)
        require(b.type == TapeBlockType::StandardData,
                "WAV export accepts TAP standard blocks only"sv);
    Bytes out(44);
    textAt(out, 0, "RIFF"sv);
    textAt(out, 8, "WAVEfmt "sv);
    setDword(out, 16, 16);
    setWord(out, 20, 1);
    setWord(out, 22, 1);
    setDword(out, 24, rate);
    setDword(out, 28, rate);
    setWord(out, 32, 1);
    setWord(out, 34, 8);
    textAt(out, 36, "data"sv);
    uint64_t remainder = 0;
    signal(blocks, [&](const TapeEdge &e) {
        remainder += uint64_t(e.length) * rate;
        auto count = remainder / 3500000;
        remainder %= 3500000;
        require(out.size() + count <= 256 * 1024 * 1024, "WAV output exceeds 256 MiB limit"sv);
        out.insert(out.end(), std::size_t(count), e.level ? 224 : 32);
    });
    auto dataSize = out.size() - 44;
    if (dataSize & 1)
        out.push_back(128);
    setDword(out, 4, out.size() - 8);
    setDword(out, 40, dataSize);
    return out;
}
} // namespace zxkit::TapeConversion
