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
#include <cstring>
#include <span>
#include <vector>

namespace zxkit {

struct RzxFrameInputs {
    uint16_t instructionCount = 0;
    std::vector<uint8_t> portReadings;
};

/// RZX Input Recording & Playback Engine (supports zlib-compressed IRB input recording blocks).
class RzxStream {
public:
    void clear();
    void startRecording(std::vector<uint8_t> initialSnapshot, std::string snapExt, IoBus &ioBus);
    void endRecordedFrame(uint16_t icount = 1);
    std::vector<uint8_t> stopRecording(IoBus &ioBus);
    void loadRzx(std::span<const uint8_t> raw, IoBus &ioBus);
    void advancePlaybackFrame(IoBus &ioBus) noexcept;

    bool isRecording() const noexcept { return m_recording; }
    bool isPlaying() const noexcept { return m_playing; }
    std::size_t frameCount() const noexcept { return m_frames.size(); }
    std::span<const uint8_t> embeddedSnapshot() const noexcept { return m_snapshotBytes; }
    std::string_view embeddedSnapshotExt() const noexcept { return m_snapshotExt; }

private:
    std::vector<uint8_t> serializeRzx() const;

    std::vector<uint8_t> m_snapshotBytes;
    std::string m_snapshotExt;
    std::vector<RzxFrameInputs> m_frames;
    std::size_t m_currentFrame = 0;
    std::size_t m_currentPortIdx = 0;
    bool m_recording = false;
    bool m_playing = false;
};

} // namespace zxkit
