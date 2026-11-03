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

#include "zxkit/bus/IoBus.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zxkit {

/// Selects a supported base machine from an embedded snapshot header.
std::optional<uint8_t> rzxMachineId(std::string_view id) noexcept;
std::string_view rzxSnapshotMachine(std::span<const uint8_t> data, std::string_view extension);
/// SZX AY interface flags: bit 0 Fuller, bit 1 standard 128K ports.
uint8_t rzxSnapshotAyFlags(std::span<const uint8_t> data, std::string_view extension);

enum class RzxState : uint8_t { Idle, Playing, Recording, Completed, Stopped, Error, Reviewing };

struct RzxProgress {
    RzxState state = RzxState::Idle;
    uint64_t completedFrames = 0;
    uint64_t totalFrames = 0;
    std::string error;
    bool operator==(const RzxProgress &) const = default;
};

struct RzxCommand {
    enum class Type { Snapshot, Position };
    Type type = Type::Position;
    std::size_t frame = 0;
    uint32_t tstates = 0;
    std::string extension;
    std::vector<uint8_t> snapshot;
};

/// RZX 0.12/0.13 input stream. Execution/timing belongs to Emulator; direct
/// consumers supply M1 fetch counts (excluding interrupt acknowledge cycles).
class RzxStream {
public:
    RzxStream() = default;
    RzxStream(const RzxStream &) = delete;
    RzxStream &operator=(const RzxStream &) = delete;
    void clear();
    void stopPlayback() noexcept;
    void startRecording(std::vector<uint8_t> initialSnapshot, std::string snapExt, IoBus &ioBus,
                        uint32_t initialTstates = 0);
    void endRecordedFrame(uint32_t fetches);
    void resumeRecording(IoBus &bus);
    void setPlaybackPosition(std::size_t frame, std::size_t command, bool reviewing, IoBus &bus);
    std::size_t commandPosition() const noexcept { return m_nextCommand; }
    uint64_t ticksAt(std::size_t frame) const { return m_ticks.at(frame); }
    std::size_t frameAtTick(uint64_t tick) const;
    std::vector<uint8_t> stopRecording(IoBus &ioBus);
    void loadRzx(std::span<const uint8_t> raw, IoBus &ioBus);
    void connectPlayback(IoBus &ioBus);
    void advancePlaybackFrame(IoBus &ioBus);
    [[noreturn]] void fail(std::string message);

    bool isRecording() const noexcept { return m_state == RzxState::Recording; }
    bool isPlaying() const noexcept { return (m_state == RzxState::Playing || m_state == RzxState::Reviewing) && m_currentFrame < m_frames.size(); }
    bool isReviewing() const noexcept { return m_state == RzxState::Reviewing; }
    RzxState state() const noexcept { return m_state; }
    std::size_t frameCount() const noexcept { return m_frames.size(); }
    std::size_t playbackFrame() const noexcept { return m_currentFrame; }
    uint16_t playbackFetches() const noexcept;
    RzxProgress progress() const;
    /// Ordered snapshot/position commands before the current input frame.
    std::span<const RzxCommand> pendingCommands() const noexcept;
    void consumeCommands(std::size_t count) noexcept { m_nextCommand += count; }
    std::span<const uint8_t> embeddedSnapshot() const noexcept;
    std::string_view embeddedSnapshotExt() const noexcept;

private:
    struct Frame { uint16_t fetches, inputs; uint32_t offset; };
    std::vector<uint8_t> serializeRzx() const;
    void connectRecording(IoBus &bus);
    void truncateAtCursor();

    std::vector<Frame> m_frames;
    std::vector<uint64_t> m_ticks{0};
    std::vector<uint8_t> m_inputs;
    std::vector<RzxCommand> m_commands;
    std::string m_snapshotExt;
    std::vector<uint8_t> m_snapshotBytes;
    uint32_t m_initialTstates = 0;
    std::size_t m_currentFrame = 0, m_currentPortIdx = 0, m_nextCommand = 0;
    uint32_t m_recordOffset = 0;
    RzxState m_state = RzxState::Idle;
    std::string m_error;
    // IoBus may outlive a standalone stream. Its callbacks then fall through.
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};

} // namespace zxkit
