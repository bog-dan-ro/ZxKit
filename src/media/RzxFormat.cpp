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

#include <cstring>

namespace zxkit {

void RzxStream::clear()
{
    m_snapshotBytes.clear();
    m_snapshotExt.clear();
    m_frames.clear();
    m_currentFrame = 0;
    m_currentPortIdx = 0;
    m_recording = false;
    m_playing = false;
}

void RzxStream::startRecording(std::vector<uint8_t> initialSnapshot, std::string snapExt, IoBus &ioBus)
{
    clear();
    m_snapshotBytes = std::move(initialSnapshot);
    m_snapshotExt = std::move(snapExt);
    m_recording = true;
    m_frames.emplace_back();

    ioBus.setRzxCallbacks(nullptr, [this](uint8_t val) {
        if (m_recording && !m_frames.empty())
            m_frames.back().portReadings.push_back(val);
    });
}

void RzxStream::endRecordedFrame(uint16_t icount)
{
    if (!m_recording)
        return;
    if (!m_frames.empty())
        m_frames.back().instructionCount = icount;
    m_frames.emplace_back();
}

std::vector<uint8_t> RzxStream::stopRecording(IoBus &ioBus)
{
    m_recording = false;
    ioBus.setRzxCallbacks(nullptr, nullptr);
    if (!m_frames.empty() && m_frames.back().portReadings.empty() && m_frames.back().instructionCount == 0)
        m_frames.pop_back();
    return serializeRzx();
}

void RzxStream::loadRzx(std::span<const uint8_t> raw, IoBus &ioBus)
{
    if (raw.size() < 10 || std::memcmp(raw.data(), "RZX!", 4) != 0)
        throw FormatException("Invalid RZX signature (expected 'RZX!')");

    clear();
    std::size_t pos = 10;
    while (pos + 5 <= raw.size()) {
        const uint8_t blockId = raw[pos];
        const uint32_t blockLen = static_cast<uint32_t>(raw[pos + 1])
                                | (static_cast<uint32_t>(raw[pos + 2]) << 8)
                                | (static_cast<uint32_t>(raw[pos + 3]) << 16)
                                | (static_cast<uint32_t>(raw[pos + 4]) << 24);
        if (blockLen < 5 || pos + blockLen > raw.size())
            throw FormatException("Truncated RZX block");

        const auto payload = raw.subspan(pos + 5, blockLen - 5);
        pos += blockLen;

        if (blockId == 0x30 && payload.size() >= 12) {
            // Embedded Snapshot Block
            const uint32_t flags = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8);
            char extBuf[5]{};
            std::memcpy(extBuf, payload.data() + 4, 4);
            m_snapshotExt = std::string(".") + extBuf;
            const uint32_t uncompressedLen = static_cast<uint32_t>(payload[8])
                                           | (static_cast<uint32_t>(payload[9]) << 8)
                                           | (static_cast<uint32_t>(payload[10]) << 16)
                                           | (static_cast<uint32_t>(payload[11]) << 24);
            const auto snapStream = payload.subspan(12);
            if (flags & 0x02)
                m_snapshotBytes = zlibDecompress(snapStream, uncompressedLen);
            else
                m_snapshotBytes.assign(snapStream.begin(), snapStream.end());
        } else if (blockId == 0x80 && payload.size() >= 13) {
            // Input Recording Block (IRB)
            const uint32_t frameCount = static_cast<uint32_t>(payload[0])
                                      | (static_cast<uint32_t>(payload[1]) << 8)
                                      | (static_cast<uint32_t>(payload[2]) << 16)
                                      | (static_cast<uint32_t>(payload[3]) << 24);
            const uint32_t flags = static_cast<uint32_t>(payload[9]) | (static_cast<uint32_t>(payload[10]) << 8);
            const auto rawFrames = payload.subspan(13);
            const std::vector<uint8_t> decoded = (flags & 0x02)
                ? zlibDecompress(rawFrames, frameCount * 8)
                : std::vector<uint8_t>(rawFrames.begin(), rawFrames.end());

            std::size_t fpos = 0;
            for (uint32_t f = 0; f < frameCount && fpos + 4 <= decoded.size(); ++f) {
                const uint16_t icount = static_cast<uint16_t>(decoded[fpos] | (decoded[fpos + 1] << 8));
                const uint16_t inCount = static_cast<uint16_t>(decoded[fpos + 2] | (decoded[fpos + 3] << 8));
                fpos += 4;

                RzxFrameInputs frame;
                frame.instructionCount = icount;
                if (inCount == 0xFFFF) {
                    if (!m_frames.empty())
                        frame.portReadings = m_frames.back().portReadings;
                } else {
                    if (fpos + inCount > decoded.size())
                        throw FormatException("Truncated RZX frame input stream");
                    frame.portReadings.assign(decoded.begin() + static_cast<std::ptrdiff_t>(fpos),
                                              decoded.begin() + static_cast<std::ptrdiff_t>(fpos + inCount));
                    fpos += inCount;
                }
                m_frames.push_back(std::move(frame));
            }
        }
    }

    m_currentFrame = 0;
    m_currentPortIdx = 0;
    m_playing = !m_frames.empty();

    if (m_playing) {
        ioBus.setRzxCallbacks(
            [this](uint16_t) -> uint8_t {
                if (!m_playing || m_currentFrame >= m_frames.size())
                    return 0xFF;
                const auto &readings = m_frames[m_currentFrame].portReadings;
                if (m_currentPortIdx < readings.size())
                    return readings[m_currentPortIdx++];
                return 0xFF;
            },
            nullptr);
    }
}

void RzxStream::advancePlaybackFrame(IoBus &ioBus) noexcept
{
    if (!m_playing)
        return;
    m_currentPortIdx = 0;
    if (++m_currentFrame >= m_frames.size()) {
        m_playing = false;
        ioBus.setRzxCallbacks(nullptr, nullptr);
    }
}

std::vector<uint8_t> RzxStream::serializeRzx() const
{
    StateWriter w;
    // RZX Header (10 bytes): "RZX!", major 0, minor 13, flags 0
    w.writeTag("RZX!");
    w.write<uint8_t>(0);
    w.write<uint8_t>(13);
    w.write<uint32_t>(0);

    if (!m_snapshotBytes.empty()) {
        const auto compSnap = zlibCompress(m_snapshotBytes);
        w.write<uint8_t>(0x30);
        w.write<uint32_t>(static_cast<uint32_t>(5 + 12 + compSnap.size()));
        w.write<uint32_t>(0x02); // Compressed
        w.writeTag(std::string_view("szx\0", 4));
        w.write<uint32_t>(static_cast<uint32_t>(m_snapshotBytes.size()));
        w.writeBytes(compSnap);
    }

    StateWriter rawIrb;
    for (const auto &f : m_frames) {
        rawIrb.write<uint16_t>(f.instructionCount);
        rawIrb.write<uint16_t>(static_cast<uint16_t>(f.portReadings.size()));
        rawIrb.writeBytes(f.portReadings);
    }
    const auto compIrb = zlibCompress(rawIrb.data());
    w.write<uint8_t>(0x80);
    w.write<uint32_t>(static_cast<uint32_t>(5 + 13 + compIrb.size()));
    w.write<uint32_t>(static_cast<uint32_t>(m_frames.size()));
    w.write<uint8_t>(0);
    w.write<uint32_t>(0); // T-state counter
    w.write<uint32_t>(0x02); // Compressed
    w.writeBytes(compIrb);

    return w.takeBuffer();
}

} // namespace zxkit
