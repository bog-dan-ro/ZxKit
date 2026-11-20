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

#include "zxkit/bus/Peripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

/// Host-side printout. Access through the same lock as the emulator while the printer is attached.
class ZxPrinterPaper
{
public:
    static constexpr int kWidth = 256;
    static constexpr std::size_t kMaxRows = 65'536;
    static constexpr int kGlyphSize = 8;
    using Row = std::array<uint8_t, kWidth / 8>;

    struct TextLine
    {
        std::size_t row = 0;
        /// One character per 8-dot cell. Zero denotes an unrecognized glyph.
        std::array<char16_t, kWidth / kGlyphSize> characters{};
    };

    /// Top-first scanlines, MSB on the left, 1 for a black dot.
    std::span<const Row> rows() const noexcept
    {
        return m_rows;
    }
    /// Best-effort UTF-8 recognition of aligned 8×8 Spectrum glyphs, with ? for unknown glyphs.
    const std::string &text() const noexcept
    {
        return m_text;
    }
    /// Recognized glyphs with their positions on the paper.
    std::span<const TextLine> textLines() const noexcept
    {
        return m_textLines;
    }
    bool truncated() const noexcept
    {
        return m_truncated;
    }

    /// Both encoders return empty data if the paper is empty.
    std::vector<uint8_t> toPbm() const;
    std::vector<uint8_t> toPng() const;
    void clear();

private:
    friend class ZxPrinterPeripheral;
    std::vector<Row> m_rows;
    std::string m_text;
    std::vector<TextLine> m_textLines;
    bool m_truncated = false;
};

/// Sinclair ZX Printer and compatible hardware such as the Alphacom 32.
class ZxPrinterPeripheral : public Peripheral
{
public:
    /// Share the paper with the host to retain it across disconnects, or let the printer own it.
    explicit ZxPrinterPeripheral(std::shared_ptr<ZxPrinterPaper> paper = {});

    static constexpr std::string_view kId = "zxprinter";
    static bool availableOn(const MachineProfile &machine) noexcept;
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    bool supportsMachine(const MachineProfile &machine) const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;
    void onFrameEnd() override;

    const ZxPrinterPaper &paper() const noexcept
    {
        return *m_paper;
    }
    void clearPaper();

    /// Only the transport and unfinished output belong to emulated state.
    void saveState(StateWriter &writer) const override;
    void loadState(StateReader &reader) override;

private:
    uint8_t readPort() const;
    void writePort(uint8_t value);
    uint64_t printerTime() const noexcept;
    void advance(uint64_t now);
    void markDot();
    void finishRow();
    void appendRow(const ZxPrinterPaper::Row &row);
    uint8_t fontByte(uint16_t address) const;
    void recognizeText();

    void writeAySerial(uint8_t value);
    void writeSerialBit(bool high);
    void latchParallelByte(uint8_t value);
    void writeParallelStrobe(bool high);
    void receiveByte(uint8_t value);
    void printTextLine();

    enum class StreamMode : uint8_t { Text, Escape, GraphicsWidthLow, GraphicsWidthHigh, Graphics };

    struct StreamState
    {
        uint64_t lastBit = 0;
        uint16_t graphicsWidth = 0;
        uint16_t graphicsColumn = 0;
        uint8_t bitCount = 0;
        uint8_t byte = 0;
        StreamMode mode = StreamMode::Text;
        bool skipLineFeed = false;
        uint8_t columns = 0;
        ZxPrinterPaper::Row text{};
        std::array<ZxPrinterPaper::Row, 4> graphics{};
    };

    std::shared_ptr<ZxPrinterPaper> m_paper;
    MachineContext *m_context = nullptr;
    uint64_t m_nextDot = 0;
    int16_t m_position = 0;
    bool m_running = false;
    bool m_slow = false;
    bool m_nextSlow = false;
    bool m_stylus = false;
    bool m_paperLatch = false;
    bool m_dotLatch = false;
    ZxPrinterPaper::Row m_row{};
    std::array<ZxPrinterPaper::Row, ZxPrinterPaper::kGlyphSize> m_textRows{};
    uint8_t m_textRowCount = 0;
    StreamState m_stream;

    uint8_t m_parallelByte = 0;
    bool m_parallelPending = false;
    bool m_parallelStrobe = false;
    bool m_hcSerial = false;
};

} // namespace zxkit
