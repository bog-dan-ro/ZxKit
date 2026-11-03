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

#include "zxkit/media/RzxFormat.h"

#include "zxkit/core/Exception.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/media/Compression.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <limits>
#include <zlib.h>

namespace zxkit {
namespace {
constexpr std::size_t maxDecodedBytes = 128 * 1024 * 1024;
constexpr std::size_t maxSnapshotBytes = 16 * 1024 * 1024;
constexpr std::size_t maxFrames = 2'000'000;
constexpr std::array<std::string_view, 16> machines = {"16k", "48k", "128k", "plus2", "plus2a", "plus3", "plus3e", "pentagon", "tc2048", "tc2068", "scorpion", "se", "ts2068", "pentagon512", "pentagon1024", "48k_ntsc"};

std::vector<uint8_t> inflateRzx(std::span<const uint8_t> bytes, std::size_t limit)
{
    struct Inflate {
        z_stream stream{};
        Inflate() { if (inflateInit(&stream) != Z_OK) throw FormatException("RZX inflate initialization failed"); }
        ~Inflate() { inflateEnd(&stream); }
    } inflater;
    auto &stream = inflater.stream;
    stream.next_in = const_cast<Bytef *>(bytes.data());
    stream.avail_in = static_cast<uInt>(bytes.size());
    std::vector<uint8_t> result;
    std::array<uint8_t, 16384> chunk;
    for (;;) {
        stream.next_out = chunk.data();
        stream.avail_out = chunk.size();
        const int ret = inflate(&stream, Z_NO_FLUSH);
        const auto count = chunk.size() - stream.avail_out;
        if (count > limit - result.size()) throw FormatException("RZX decoded data exceeds its size limit");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
        if (ret == Z_STREAM_END) {
            if (stream.avail_in) throw FormatException("Trailing bytes after RZX compressed stream");
            return result;
        }
        if (ret != Z_OK || (!count && !stream.avail_in))
            throw FormatException("Corrupt or truncated RZX compressed stream");
    }
}
std::string extension(std::span<const uint8_t> bytes)
{
    std::string result = ".";
    for (auto c : bytes) {
        if (!c) break;
        if (!std::isalnum(c)) throw FormatException("Invalid RZX snapshot extension");
        result += static_cast<char>(std::tolower(c));
    }
    if (result.size() < 2 || result.size() > 4) throw FormatException("Invalid RZX snapshot extension");
    return result;
}
}

std::optional<uint8_t> rzxMachineId(std::string_view id) noexcept
{
    const auto found = std::ranges::find(machines, id);
    // The snapshot adapters preserve these base machines completely. Other
    // models need additional paging/peripheral snapshot work first.
    if (found == machines.end() || (found - machines.begin() >= 8 && id != "48k_ntsc"))
        return std::nullopt;
    return static_cast<uint8_t>(found - machines.begin());
}

std::string_view rzxSnapshotMachine(std::span<const uint8_t> data, std::string_view ext)
{
    if (ext == ".sna") {
        if (data.size() == 49179) return "48k";
        if (data.size() == 131103 || data.size() == 147487) return "128k";
        throw FormatException("Invalid RZX SNA snapshot size");
    }
    if (ext == ".szx") {
        if (data.size() < 8 || std::memcmp(data.data(), "ZXST", 4)) throw FormatException("Invalid RZX SZX snapshot");
        if (data[6] >= machines.size() || !rzxMachineId(machines[data[6]]))
            throw FormatException("Unsupported RZX SZX machine");
        return machines[data[6]];
    }
    if (ext == ".z80") {
        if (data.size() < 30) throw FormatException("Truncated RZX Z80 snapshot");
        if (data[6] || data[7]) return "48k";
        if (data.size() < 35) throw FormatException("Truncated RZX Z80 extended header");
        const auto size = uint16_t(data[30] | (data[31] << 8));
        if ((size != 23 && size != 54 && size != 55) || data.size() < 32u + size)
            throw FormatException("Unsupported or truncated RZX Z80 extended header");
        const auto mode = data[34];
        const bool modified = data[37] & 0x80;
        if (mode <= 1 || (size != 23 && mode == 3)) return modified ? "16k" : "48k";
        if ((size == 23 && (mode == 3 || mode == 4)) || (size != 23 && mode >= 4 && mode <= 6))
            return modified ? "plus2" : "128k";
        if (mode == 7 || mode == 8) return modified ? "plus2a" : "plus3";
        if (mode == 9) return "pentagon";

        if (mode == 12) return "plus2";
        if (mode == 13) return "plus2a";
        throw FormatException("Unsupported RZX Z80 machine/header");
    }
    throw FormatException("Unsupported RZX snapshot extension");
}

uint8_t rzxSnapshotAyFlags(std::span<const uint8_t> data, std::string_view ext)
{
    if (ext == ".z80" && data.size() >= 55 && !data[6] && !data[7] && (data[37] & 4))
        return data[37] & 0x40 ? 1 : 2;
    if (ext == ".szx" && data.size() >= 8) {
        StateReader chunks(data.subspan(8));
        while (!chunks.eof()) {
            const auto tag = chunks.readSpan(4);
            const auto size = chunks.read<uint32_t>();
            const auto chunk = chunks.readSpan(size);
            if (std::memcmp(tag.data(), "AY\0\0", 4) == 0 && size >= 18)
                return chunk[0] & 3;
        }
    }
    return 0;
}

void RzxStream::clear()
{
    m_frames.clear(); m_inputs.clear(); m_commands.clear();
    m_ticks.assign(1, 0);
    m_snapshotBytes.clear(); m_snapshotExt.clear();
    m_currentFrame = m_currentPortIdx = m_nextCommand = 0;
    m_recordOffset = m_initialTstates = 0;
    m_error.clear();
    m_state = RzxState::Idle;
}

void RzxStream::stopPlayback() noexcept
{
    if (isPlaying()) m_state = RzxState::Stopped;
}

RzxProgress RzxStream::progress() const
{
    return {m_state, m_currentFrame, m_frames.size(), m_error};
}

uint16_t RzxStream::playbackFetches() const noexcept
{
    return isPlaying() && m_currentFrame < m_frames.size() ? m_frames[m_currentFrame].fetches : 0;
}

std::span<const RzxCommand> RzxStream::pendingCommands() const noexcept
{
    std::size_t end = m_nextCommand;
    while (end < m_commands.size() && m_commands[end].frame == m_currentFrame) ++end;
    return std::span(m_commands).subspan(m_nextCommand, end - m_nextCommand);
}

std::span<const uint8_t> RzxStream::embeddedSnapshot() const noexcept
{
    return m_snapshotBytes;
}
std::string_view RzxStream::embeddedSnapshotExt() const noexcept { return m_snapshotExt; }

[[noreturn]] void RzxStream::fail(std::string message)
{
    m_error = std::format("RZX frame {} of {}: {}", m_currentFrame + 1, m_frames.size(), message);
    m_state = RzxState::Error;
    throw FormatException(m_error);
}

void RzxStream::startRecording(std::vector<uint8_t> snapshot, std::string ext, IoBus &bus, uint32_t tstates)
{
    clear();
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    std::array<uint8_t, 4> name{};
    if (ext.empty() || ext.size() > 3) throw FormatException("Invalid recording snapshot extension");
    std::copy(ext.begin(), ext.end(), name.begin());
    m_snapshotExt = extension(name);
    m_snapshotBytes = std::move(snapshot);
    m_initialTstates = tstates;
    m_commands.push_back({RzxCommand::Type::Snapshot, 0, 0, m_snapshotExt, m_snapshotBytes});
    m_commands.push_back({RzxCommand::Type::Position, 0, tstates, {}, {}});
    m_nextCommand = m_commands.size();
    m_state = RzxState::Recording;
    connectRecording(bus);
}

void RzxStream::connectRecording(IoBus &bus)
{
    const std::weak_ptr<int> life = m_lifetime;
    bus.setRzxCallbacks(nullptr, [this, life](uint8_t value) {
        if (life.expired() || !isRecording()) return;
        if (m_inputs.size() - m_recordOffset >= 65534) fail("too many input readings in a frame");
        if (m_inputs.size() == maxDecodedBytes) fail("recording input limit exceeded");
        m_inputs.push_back(value);
    });
}

void RzxStream::endRecordedFrame(uint32_t fetches)
{
    if (!isRecording()) return;
    if (fetches > 65535) fail("fetch count exceeds the RZX 16-bit limit");
    if (m_frames.size() == maxFrames) fail("recording frame limit exceeded");
    if (m_inputs.size() + 4 * (m_frames.size() + 1) + m_snapshotBytes.size() > maxDecodedBytes)
        fail("recording decoded size limit exceeded");
    m_frames.push_back({static_cast<uint16_t>(fetches), static_cast<uint16_t>(m_inputs.size() - m_recordOffset), m_recordOffset});
    m_ticks.push_back(m_ticks.back() + (fetches > 4));
    m_recordOffset = static_cast<uint32_t>(m_inputs.size());
    m_currentFrame = m_frames.size();
}

std::vector<uint8_t> RzxStream::stopRecording(IoBus &bus)
{
    if (!isRecording() && !isReviewing()) throw FormatException("No active RZX recording");
    if (isReviewing()) truncateAtCursor();
    if (m_recordOffset != m_inputs.size()) fail("unfinished recording frame");
    m_state = RzxState::Stopped;
    bus.setRzxCallbacks(nullptr, nullptr);
    return serializeRzx();
}

std::size_t RzxStream::frameAtTick(uint64_t tick) const
{
    const auto it = std::upper_bound(m_ticks.begin(), m_ticks.end(), tick);
    return static_cast<std::size_t>(it - m_ticks.begin() - 1);
}

void RzxStream::setPlaybackPosition(std::size_t frame, std::size_t command, bool reviewing, IoBus &bus)
{
    if (frame > m_frames.size() || command > m_commands.size())
        throw FormatException("Invalid RZX seek position");
    m_currentFrame = frame;
    m_nextCommand = command;
    m_currentPortIdx = 0;
    m_error.clear();
    m_state = reviewing ? RzxState::Reviewing : frame == m_frames.size() ? RzxState::Completed : RzxState::Playing;
    connectPlayback(bus);
}

void RzxStream::truncateAtCursor()
{
    m_frames.resize(m_currentFrame);
    m_ticks.resize(m_currentFrame + 1);
    std::size_t end = 0;
    for (const auto &frame : m_frames)
        end = std::max(end, std::size_t(frame.offset) + frame.inputs);
    m_inputs.resize(end);
    m_recordOffset = static_cast<uint32_t>(end);
    m_commands.resize(m_nextCommand);
    m_currentPortIdx = 0;
}

void RzxStream::resumeRecording(IoBus &bus)
{
    if (!isReviewing()) throw FormatException("Seek a recording before resuming it");
    truncateAtCursor();
    m_state = RzxState::Recording;
    connectRecording(bus);
}

void RzxStream::loadRzx(std::span<const uint8_t> raw, IoBus &bus)
{
    // Parse into temporary storage; a malformed replacement cannot leave half a stream installed.
    if (raw.size() < 10 || raw.size() > maxDecodedBytes || std::memcmp(raw.data(), "RZX!", 4))
        throw FormatException("Invalid RZX header or file size");
    if (raw[4] != 0 || raw[5] < 12 || raw[5] > 13) throw FormatException("Unsupported RZX format revision");
    StateReader reader(raw.subspan(6));
    if (reader.read<uint32_t>() & ~1u) throw FormatException("Unsupported RZX header flags");
    std::vector<Frame> frames;
    std::vector<uint8_t> inputs;
    std::vector<RzxCommand> commands;
    bool creator = false;
    std::size_t decodedTotal = 0;
    while (!reader.eof()) {
        const auto id = reader.read<uint8_t>();
        const auto length = reader.read<uint32_t>();
        if (length < 5) throw FormatException("Invalid RZX block length");
        StateReader block(reader.readSpan(length - 5));
        if (id == 0x10) {
            if (length < 29) throw FormatException("Truncated RZX creator block");
            creator = true;
        } else if (id == 0x30) {
            const auto flags = block.read<uint32_t>();
            const auto ext = extension(block.readSpan(4));
            const auto size = block.read<uint32_t>();
            if (flags & 1) throw FormatException("External RZX snapshot descriptors are not supported; embed the snapshot");
            if (flags & ~2u) throw FormatException("Unsupported RZX snapshot flags");
            if (size > maxSnapshotBytes) throw FormatException("RZX snapshot size limit exceeded");
            auto data = block.readSpan(length - 17);
            auto snapshot = flags & 2 ? inflateRzx(data, size) : std::vector<uint8_t>(data.begin(), data.end());
            if (snapshot.size() != size) throw FormatException("RZX snapshot length mismatch");
            decodedTotal += snapshot.size();
            commands.push_back({RzxCommand::Type::Snapshot, frames.size(), 0, ext, std::move(snapshot)});
        } else if (id == 0x80) {
            const auto count = block.read<uint32_t>();
            if (block.read<uint8_t>() != 0) throw FormatException("Nonzero RZX input reserved byte");
            const auto tstates = block.read<uint32_t>();
            const auto flags = block.read<uint32_t>();
            if (flags & 1) throw FormatException("Encrypted RZX input blocks are not supported");
            if (flags & ~2u) throw FormatException("Unsupported RZX input flags");
            if (count > maxFrames - frames.size()) throw FormatException("RZX frame limit exceeded");
            commands.push_back({RzxCommand::Type::Position, frames.size(), tstates, {}, {}});
            const auto data = block.readSpan(length - 18);
            auto decoded = flags & 2 ? inflateRzx(data, maxDecodedBytes - decodedTotal) : std::vector<uint8_t>(data.begin(), data.end());
            decodedTotal += decoded.size();
            StateReader records(decoded);
            for (uint32_t i = 0; i < count; ++i) {
                Frame frame{records.read<uint16_t>(), records.read<uint16_t>(), static_cast<uint32_t>(inputs.size())};
                if (frame.inputs == 65535) {
                    if (i == 0) throw FormatException("RZX repeated input frame has no predecessor in its block");
                    frame.inputs = frames.back().inputs;
                    frame.offset = frames.back().offset;
                } else {
                    const auto values = records.readSpan(frame.inputs);
                    if (values.size() > maxDecodedBytes - inputs.size()) throw FormatException("RZX input size limit exceeded");
                    inputs.insert(inputs.end(), values.begin(), values.end());
                }
                frames.push_back(frame);
            }
            if (!records.eof()) throw FormatException("Trailing data after RZX input frames");
        }
        if (commands.size() > 100'000) throw FormatException("RZX command limit exceeded");
        if (decodedTotal > maxDecodedBytes) throw FormatException("RZX decoded size limit exceeded");
    }
    if (!creator) throw FormatException("RZX creator block is missing");
    clear();
    m_frames = std::move(frames); m_inputs = std::move(inputs); m_commands = std::move(commands);
    m_ticks.reserve(m_frames.size() + 1);
    for (const auto &frame : m_frames) m_ticks.push_back(m_ticks.back() + (frame.fetches > 4));
    for (const auto &command : m_commands) {
        if (command.type == RzxCommand::Type::Snapshot) {
            m_snapshotBytes = command.snapshot;
            m_snapshotExt = command.extension;
            break;
        }
    }
    m_state = m_frames.empty() ? RzxState::Completed : RzxState::Playing;
    connectPlayback(bus);
}

void RzxStream::connectPlayback(IoBus &bus)
{
    if (!isPlaying()) { bus.setRzxCallbacks(nullptr, nullptr); return; }
    const std::weak_ptr<int> life = m_lifetime;
    bus.setRzxCallbacks([this, life](uint16_t port) -> std::optional<uint8_t> {
        if (life.expired() || !isPlaying()) return std::nullopt;
        const auto &frame = m_frames[m_currentFrame];
        if (m_currentPortIdx == frame.inputs)
            fail(std::format("input overrun at port 0x{:04X}; expected {} reads", port, frame.inputs));
        return m_inputs[frame.offset + m_currentPortIdx++];
    }, nullptr);
}

void RzxStream::advancePlaybackFrame(IoBus &bus)
{
    if (!isPlaying()) return;
    const auto expected = m_frames[m_currentFrame].inputs;
    if (m_currentPortIdx != expected)
        fail(std::format("input underrun; consumed {} of {} reads", m_currentPortIdx, expected));
    m_currentPortIdx = 0;
    if (++m_currentFrame == m_frames.size()) {
        if (!isReviewing()) m_state = RzxState::Completed;
        bus.setRzxCallbacks(nullptr, nullptr);
    }
}

std::vector<uint8_t> RzxStream::serializeRzx() const
{
    StateWriter out;
    out.writeTag("RZX!"); out.write<uint8_t>(0); out.write<uint8_t>(13); out.write<uint32_t>(0);
    out.write<uint8_t>(0x10); out.write<uint32_t>(29);
    std::array<uint8_t, 20> creator{};
    constexpr std::string_view name = "ZxKit";
    std::copy(name.begin(), name.end(), creator.begin());
    out.writeBytes(creator); out.write<uint16_t>(1); out.write<uint16_t>(0);
    std::size_t frame = 0, command = 0;
    bool emittedInput = false;
    uint32_t tstates = 0;
    while (frame < m_frames.size() || command < m_commands.size()) {
        while (command < m_commands.size() && m_commands[command].frame == frame) {
            const auto &item = m_commands[command++];
            if (item.type == RzxCommand::Type::Position) { tstates = item.tstates; continue; }
            const auto compressed = zlibCompress(item.snapshot);
            out.write<uint8_t>(0x30); out.write<uint32_t>(17 + compressed.size()); out.write<uint32_t>(2);
            std::array<uint8_t, 4> ext{};
            std::copy(item.extension.begin() + 1, item.extension.end(), ext.begin());
            out.writeBytes(ext); out.write<uint32_t>(item.snapshot.size()); out.writeBytes(compressed);
        }
        const auto next = command < m_commands.size() ? m_commands[command].frame : m_frames.size();
        if (next == frame) continue;
        StateWriter records;
        for (auto i = frame; i < next; ++i) {
            const auto &item = m_frames[i];
            records.write(item.fetches); records.write(item.inputs);
            records.writeBytes(std::span(m_inputs).subspan(item.offset, item.inputs));
        }
        const auto compressed = zlibCompress(records.data());
        out.write<uint8_t>(0x80); out.write<uint32_t>(18 + compressed.size());
        out.write<uint32_t>(next - frame); out.write<uint8_t>(0);
        out.write(tstates); out.write<uint32_t>(2); out.writeBytes(compressed);
        frame = next; tstates = 0; emittedInput = true;
    }
    if (!emittedInput) {
        const auto compressed = zlibCompress({});
        out.write<uint8_t>(0x80); out.write<uint32_t>(18 + compressed.size());
        out.write<uint32_t>(0); out.write<uint8_t>(0); out.write(tstates); out.write<uint32_t>(2);
        out.writeBytes(compressed);
    }
    return out.takeBuffer();
}

} // namespace zxkit
