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

#include "zxkit/peripherals/expansion/ZxPrinter.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/machines/RomanianMachines.h"
#include "zxkit/machines/ZxNextMachine.h"
#include "zxkit/media/ScreenshotFormat.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace zxkit {
namespace {

using namespace std::literals;

constexpr uint16_t kChars = 0x5C36;
constexpr int16_t kStartupGap = 64;
constexpr int16_t kScanGap = 128;
// Nominal encoder periods in Spectrum T-states.
constexpr uint32_t dotPeriod(bool slow)
{
    return slow ? 440 : 220;
}

bool isPlus3Family(std::string_view machine)
{
    return machine == "plus2a"sv || machine == "plus3"sv || machine == "plus3e"sv;
}

bool hasAySerial(std::string_view machine)
{
    return machine == "128k"sv || machine == "plus2"sv || machine.starts_with("pentagon"sv)
            || machine == "scorpion"sv || machine == "next"sv || isPlus3Family(machine);
}

bool streamsBytes(std::string_view machine)
{
    return hasAySerial(machine) || machine == "hc2000"sv;
}

bool usesFullDecode(std::string_view machine)
{
    return machine == "tc2048"sv || machine == "tc2068"sv || machine == "ts2068"sv
            || machine == "se"sv;
}

} // namespace

std::vector<uint8_t> ZxPrinterPaper::toPbm() const
{
    if (m_rows.empty())
        return {};
    const auto header = std::format("P4\n{} {}\n", kWidth, m_rows.size());
    std::vector<uint8_t> bytes(header.begin(), header.end());
    bytes.reserve(header.size() + m_rows.size() * Row{}.size());
    for (const auto &row : m_rows)
        bytes.insert(bytes.end(), row.begin(), row.end());
    return bytes;
}

std::vector<uint8_t> ZxPrinterPaper::toPng() const
{
    if (m_rows.empty())
        return {};
    VideoFrame image(kWidth, uint32_t(m_rows.size()));
    for (uint32_t y = 0; y < image.height(); ++y) {
        auto pixels = image.scanline(y);
        for (uint32_t x = 0; x < image.width(); ++x) {
            const bool black = m_rows[y][x / 8] & (0x80 >> (x % 8));
            pixels[x] = black ? packRgba(0, 0, 0) : packRgba(255, 255, 255);
        }
    }
    return encodePng(image);
}

void ZxPrinterPaper::clear()
{
    m_rows.clear();
    m_text.clear();
    m_textLines.clear();
    m_truncated = false;
}

ZxPrinterPeripheral::ZxPrinterPeripheral(std::shared_ptr<ZxPrinterPaper> paper)
    : m_paper(paper ? std::move(paper) : std::make_shared<ZxPrinterPaper>())
{
}

bool ZxPrinterPeripheral::availableOn(const MachineProfile &machine) noexcept
{
    const auto model = machine.id();
    return model == "16k"sv || model == "48k"sv || model == "48k_ntsc"sv || streamsBytes(model)
            || usesFullDecode(model) || model == "cobra_64k"sv || model == "cobra_80k"sv;
}

std::string_view ZxPrinterPeripheral::id() const noexcept
{
    return kId;
}

std::string_view ZxPrinterPeripheral::name() const noexcept
{
    return "Sinclair ZX Printer"sv;
}

bool ZxPrinterPeripheral::supportsMachine(const MachineProfile &machine) const noexcept
{
    return availableOn(machine);
}

void ZxPrinterPeripheral::onAttach(MachineContext &ctx)
{
    m_context = &ctx;
    const auto model = ctx.machine ? ctx.machine->id() : ""sv;
    // Select only FB on boards with extra partially decoded ports. This keeps
    // ZX printing from driving their disk, paging and expansion registers.
    const auto *cobra = dynamic_cast<const CobraMachine *>(ctx.machine);
    const bool fullDecode = cobra || usesFullDecode(model)
            || (streamsBytes(model) && model != "128k"sv && model != "plus2"sv);
    ctx.ioBus.registerHandler(
            std::string(kId), fullDecode ? 0x00FF : 0x0004, fullDecode ? 0x00FB : 0x0000,
            [this, cobra](uint16_t) -> IoReadResult {
                if (cobra && !cobra->isSpectrumModeLocked())
                    return { 0xFF, 0 };
                return { readPort(), 0xFF, cobra != nullptr };
            },
            [this, cobra, &ctx](uint16_t, uint8_t value) {
                if (cobra && !cobra->isSpectrumModeLocked()) {
                    ctx.ioBus.declineWrite();
                    return;
                }
                writePort(value);
            },
            0, cobra != nullptr);
    if (hasAySerial(model)) {
        // The legacy RS-232 output is connected to AY port A.
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0xC002, 0x8000, nullptr,
                                        [this](uint16_t, uint8_t value) { writeAySerial(value); });
    }

    const auto *next = dynamic_cast<const ZxNextMachine *>(ctx.machine);
    if (isPlus3Family(model) || next) {
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0xF002, 0x0000, nullptr,
                                        [this, next](uint16_t, uint8_t value) {
                                            if (!next || next->isPlus3Mode())
                                                latchParallelByte(value);
                                        });
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0xF002, 0x1000, nullptr,
                                        [this, next](uint16_t, uint8_t value) {
                                            if (!next || next->isPlus3Mode())
                                                writeParallelStrobe((value & 0x10) != 0);
                                        });
    } else if (model == "scorpion"sv) {
        ctx.ioBus.registerSimpleHandler(
                std::string(kId), 0xFFFF, 0xFFDD, nullptr,
                [this](uint16_t, uint8_t value) { latchParallelByte(value); });
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0xFFFF, 0x1FFD, nullptr,
                                        [this](uint16_t, uint8_t value) {
                                            writeParallelStrobe((value & 0x20) != 0);
                                            if (value & 0x02)
                                                writeSerialBit((value & 0x08) == 0);
                                        });
    } else if (model == "hc2000"sv) {
        // The HC-2000 IF-1 firmware selects serial output via EF and drives
        // the inverted TX signal on F7 bit 0.
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0x00FF, 0x00EF, nullptr,
                                        [this](uint16_t, uint8_t value) {
                                            m_hcSerial = (value & 0x01) != 0;
                                            if (!m_hcSerial)
                                                m_stream.bitCount = 0;
                                        });
        ctx.ioBus.registerSimpleHandler(std::string(kId), 0x00FF, 0x00F7, nullptr,
                                        [this](uint16_t, uint8_t value) {
                                            if (m_hcSerial)
                                                writeSerialBit((value & 0x01) == 0);
                                        });
    }
}

void ZxPrinterPeripheral::onDetach(MachineContext &ctx)
{
    writePort(0x04);
    ctx.ioBus.unregisterOwner(kId);
    if (m_stream.columns != 0)
        printTextLine();
    m_context = nullptr;
}

void ZxPrinterPeripheral::reset(MachineContext &, ResetKind)
{
    m_running = false;
    m_slow = false;
    m_nextSlow = false;
    m_stylus = false;
    m_paperLatch = false;
    m_dotLatch = false;
    m_nextDot = 0;
    m_position = 0;
    m_row = {};
    m_textRowCount = 0;
    m_stream = {};
    m_parallelByte = 0;
    m_parallelPending = false;
    m_parallelStrobe = false;
    m_hcSerial = false;
}

void ZxPrinterPeripheral::onFrameEnd()
{
    advance(printerTime());
}

void ZxPrinterPeripheral::clearPaper()
{
    if (m_running && m_context && (!m_context->machine || supportsMachine(*m_context->machine)))
        advance(printerTime());
    m_paper->clear();
    m_row = {};
    m_textRowCount = 0;
    m_stream.columns = 0;
    m_stream.graphics = {};
}

uint8_t ZxPrinterPeripheral::readPort() const
{
    if (!m_running)
        return m_stylus ? 0xBE : 0x3E;

    // Predict encoder and paper edges without changing any state. Debugger peeks
    // and repeated reads leave both latches set until the next OUT.
    const auto now = printerTime();
    const auto paperEdge = m_position < 0
            ? m_nextDot + uint64_t(-m_position - 1) * dotPeriod(m_slow)
            : m_nextDot + uint64_t(ZxPrinterPaper::kWidth - m_position - 1) * dotPeriod(m_slow)
                    + kScanGap * dotPeriod(m_nextSlow);
    const bool paper = m_paperLatch || m_stylus || now >= paperEdge;
    const bool dot = m_dotLatch || now >= m_nextDot;
    return uint8_t(0x3E | (paper ? 0x80 : 0) | (dot ? 1 : 0));
}

void ZxPrinterPeripheral::writePort(uint8_t value)
{
    const auto now = printerTime();
    advance(now);

    m_paperLatch = false;
    m_dotLatch = false;
    if (value & 0x04) {
        if (m_running && m_position >= 0) {
            markDot();
            finishRow();
        }
        m_running = false;
        m_stylus = (value & 0x80) != 0;
        m_textRowCount = 0;
        return;
    }

    m_nextSlow = (value & 0x02) != 0;
    m_stylus = (value & 0x80) != 0;
    if (!m_running) {
        m_running = true;
        m_slow = m_nextSlow;
        m_position = -kStartupGap;
        m_nextDot = now + dotPeriod(m_slow);
    } else if (m_position < 0 && m_slow != m_nextSlow) {
        const auto remaining = m_nextDot - now;
        m_nextDot = now + remaining * dotPeriod(m_nextSlow) / dotPeriod(m_slow);
        m_slow = m_nextSlow;
    }
}

uint64_t ZxPrinterPeripheral::printerTime() const noexcept
{
    const auto &clock = m_context->clock;
    return clock.frameCount() * clock.timings().tstatesPerFrame() + clock.frameBaseTstates();
}

void ZxPrinterPeripheral::advance(uint64_t now)
{
    while (m_running && now >= m_nextDot) {
        m_dotLatch = true;
        if (m_position >= 0)
            markDot();
        if (++m_position == ZxPrinterPaper::kWidth) {
            finishRow();
            m_position = -kScanGap;
            m_slow = m_nextSlow;
        } else if (m_position == 0) {
            m_paperLatch = true;
        }
        m_nextDot += dotPeriod(m_slow);
    }
}

void ZxPrinterPeripheral::markDot()
{
    if (m_stylus)
        m_row[m_position / 8] |= uint8_t(0x80 >> (m_position % 8));
}

void ZxPrinterPeripheral::finishRow()
{
    appendRow(m_row);
    m_row = {};
}

void ZxPrinterPeripheral::appendRow(const ZxPrinterPaper::Row &row)
{
    if (m_paper->m_rows.size() == ZxPrinterPaper::kMaxRows) {
        m_paper->m_truncated = true;
        return;
    }

    m_paper->m_rows.push_back(row);
    m_textRows[m_textRowCount++] = row;
    if (m_textRowCount == m_textRows.size()) {
        recognizeText();
        m_textRowCount = 0;
    }
}

uint8_t ZxPrinterPeripheral::fontByte(uint16_t address) const
{
    const auto *machine = m_context->machine;
    // Byte-stream routines can page out the Spectrum font while they send.
    if (machine && address < 0x4000 && streamsBytes(machine->id())) {
        const auto model = machine->id();
        const auto &rom = m_context->mmu.bank(machine->romBankId());
        if (model == "hc2000"sv)
            return rom.span16k(0)[address];
        if (model == "next"sv || isPlus3Family(model))
            return rom.span16k(3)[address];
        if (!machine->is48kMode())
            return rom.span16k(1)[address];
    }
    return m_context->mmu.peek(address);
}

void ZxPrinterPeripheral::recognizeText()
{
    const auto rows = m_paper->rows();
    // Rewind restores the recognition buffer, while the captured paper stays in place.
    if (rows.size() < m_textRows.size()
        || !std::ranges::equal(rows.last(m_textRows.size()), m_textRows))
        return;

    ZxPrinterPaper::TextLine textLine{ .row = rows.size() - m_textRows.size() };
    const uint16_t chars =
            uint16_t(m_context->mmu.peek(kChars) | (m_context->mmu.peek(kChars + 1) << 8));
    std::array<std::array<uint8_t, 8>, 96> font;
    for (std::size_t glyph = 0; glyph < font.size(); ++glyph) {
        for (std::size_t y = 0; y < font[glyph].size(); ++y)
            font[glyph][y] = fontByte(uint16_t(chars + (glyph + 32) * 8 + y));
    }

    std::string line;
    for (std::size_t x = 0; x < m_row.size(); ++x) {
        std::array<uint8_t, ZxPrinterPaper::kGlyphSize> glyph;
        for (std::size_t y = 0; y < glyph.size(); ++y)
            glyph[y] = m_textRows[y][x];
        if (std::ranges::all_of(glyph, [](uint8_t row) { return row == 0; })) {
            textLine.characters[x] = u' ';
            line += ' ';
            continue;
        }

        const auto match = std::ranges::find(font, glyph);
        if (match == font.end()) {
            line += '?';
            continue;
        }
        const auto code = 32 + (match - font.begin());
        switch (code) {
        case 0x60:
            textLine.characters[x] = u'£';
            line += "\xC2\xA3"sv;
            break;
        case 0x7F:
            textLine.characters[x] = u'©';
            line += "\xC2\xA9"sv;
            break;
        default:
            textLine.characters[x] = char16_t(code);
            line += char(code);
            break;
        }
    }
    m_paper->m_textLines.push_back(textLine);
    const auto last = line.find_last_not_of(' ');
    line.resize(last == std::string::npos ? 0 : last + 1);
    m_paper->m_text += line;
    m_paper->m_text += '\n';
}

void ZxPrinterPeripheral::writeAySerial(uint8_t value)
{
    auto &mixer = m_context->audioMixer;
    const auto &ay = mixer.activeAy();
    if (mixer.activeAyChipIndex() != 0 || ay.selectedRegister() != 14
        || !(ay.registers()[7] & 0x40))
        return;

    writeSerialBit((value & 0x08) != 0);
}

void ZxPrinterPeripheral::writeSerialBit(bool high)
{
    // The Spectrum ROM writes every TX bit, including repeated levels, at any baud rate.
    // A gap longer than a 50-baud bit means an interrupted byte.
    const auto now = printerTime();
    if (now - m_stream.lastBit > m_context->clock.timings().baseClockHz / 25)
        m_stream.bitCount = 0;
    m_stream.lastBit = now;

    if (m_stream.bitCount == 0) {
        if (!high) {
            m_stream.bitCount = 1;
            m_stream.byte = 0;
        }
    } else if (m_stream.bitCount <= 8) {
        m_stream.byte |= uint8_t(high << (m_stream.bitCount - 1));
        ++m_stream.bitCount;
    } else {
        m_stream.bitCount = 0;
        if (high)
            receiveByte(m_stream.byte);
    }
}

void ZxPrinterPeripheral::latchParallelByte(uint8_t value)
{
    m_parallelByte = value;
    m_parallelPending = true;
}

void ZxPrinterPeripheral::writeParallelStrobe(bool high)
{
    if (m_parallelStrobe == high)
        return;
    m_parallelStrobe = high;
    if (!m_parallelPending)
        return;

    // A data write arms the next strobe edge. The +3 ROM can start with
    // either polarity, so the return edge must not print the byte again.
    m_parallelPending = false;
    receiveByte(m_parallelByte);
}

void ZxPrinterPeripheral::receiveByte(uint8_t value)
{
    constexpr int repeat = 3;
    constexpr int copyWidth = ZxPrinterPaper::kWidth * repeat;
    switch (m_stream.mode) {
    case StreamMode::Escape:
        m_stream.mode = value == 'L' ? StreamMode::GraphicsWidthLow : StreamMode::Text;
        return;
    case StreamMode::GraphicsWidthLow:
        m_stream.graphicsWidth = value;
        m_stream.mode = StreamMode::GraphicsWidthHigh;
        return;
    case StreamMode::GraphicsWidthHigh:
        m_stream.graphicsWidth |= uint16_t(value) << 8;
        m_stream.graphicsColumn = 0;
        m_stream.graphics = {};
        m_stream.mode = m_stream.graphicsWidth ? StreamMode::Graphics : StreamMode::Text;
        if (m_stream.columns != 0)
            printTextLine();
        return;
    case StreamMode::Graphics: {
        // The 128K ROM's Epson COPY sends four rows at a time, duplicating
        // each pixel twice vertically and three times horizontally.
        const auto column = m_stream.graphicsColumn++;
        if (m_stream.graphicsWidth == copyWidth && column % repeat == 0) {
            const auto x = column / repeat;
            for (std::size_t y = 0; y < m_stream.graphics.size(); ++y) {
                if (value & (0xC0 >> (2 * y)))
                    m_stream.graphics[y][x / 8] |= uint8_t(0x80 >> (x % 8));
            }
        }
        if (m_stream.graphicsColumn == m_stream.graphicsWidth) {
            if (m_stream.graphicsWidth == copyWidth) {
                for (const auto &row : m_stream.graphics)
                    appendRow(row);
            }
            m_stream.mode = StreamMode::Text;
            m_stream.skipLineFeed = true;
        }
        return;
    }
    case StreamMode::Text:
        break;
    }

    if (value == 0x1B) {
        m_stream.mode = StreamMode::Escape;
    } else if (value == '\r' || value == '\n') {
        if (value == '\r' || !m_stream.skipLineFeed)
            printTextLine();
        m_stream.skipLineFeed = value == '\r';
    } else if (value >= 0x20 && value < 0x80) {
        if (m_stream.columns == m_stream.text.size())
            printTextLine();
        m_stream.text[m_stream.columns++] = value;
        m_stream.skipLineFeed = false;
    }
}

void ZxPrinterPeripheral::printTextLine()
{
    const uint16_t chars =
            uint16_t(m_context->mmu.peek(kChars) | (m_context->mmu.peek(kChars + 1) << 8));
    m_textRowCount = 0;
    for (int y = 0; y < ZxPrinterPaper::kGlyphSize; ++y) {
        ZxPrinterPaper::Row row{};
        for (std::size_t x = 0; x < m_stream.columns; ++x)
            row[x] = fontByte(uint16_t(chars + m_stream.text[x] * ZxPrinterPaper::kGlyphSize + y));
        appendRow(row);
    }
    m_stream.columns = 0;
}

void ZxPrinterPeripheral::saveState(StateWriter &writer) const
{
    if (m_context && m_context->machine && !supportsMachine(*m_context->machine))
        return;
    writer.write(m_nextDot);
    writer.write(m_position);
    writer.writeBool(m_running);
    writer.writeBool(m_slow);
    writer.writeBool(m_nextSlow);
    writer.writeBool(m_stylus);
    writer.writeBool(m_paperLatch);
    writer.writeBool(m_dotLatch);
    writer.writeBytes(m_row);
    writer.write(m_textRowCount);
    for (const auto &row : m_textRows)
        writer.writeBytes(row);

    if (m_context && m_context->machine && streamsBytes(m_context->machine->id())) {
        writer.write(m_stream.lastBit);
        writer.write(m_stream.graphicsWidth);
        writer.write(m_stream.graphicsColumn);
        writer.write(m_stream.bitCount);
        writer.write(m_stream.byte);
        writer.write(uint8_t(m_stream.mode));
        writer.writeBool(m_stream.skipLineFeed);
        writer.write(m_stream.columns);
        writer.writeBytes(m_stream.text);
        for (const auto &row : m_stream.graphics)
            writer.writeBytes(row);
        writer.write(m_parallelByte);
        writer.writeBool(m_parallelPending);
        writer.writeBool(m_parallelStrobe);
        writer.writeBool(m_hcSerial);
    }
}

void ZxPrinterPeripheral::loadState(StateReader &reader)
{
    if (m_context && m_context->machine && !supportsMachine(*m_context->machine))
        return;
    m_nextDot = reader.read<uint64_t>();
    m_position = reader.read<int16_t>();
    m_running = reader.readBool();
    m_slow = reader.readBool();
    m_nextSlow = reader.readBool();
    m_stylus = reader.readBool();
    m_paperLatch = reader.readBool();
    m_dotLatch = reader.readBool();
    reader.readBytes(m_row);
    m_textRowCount = reader.read<uint8_t>();
    if (m_position < -kScanGap || m_position >= ZxPrinterPaper::kWidth
        || m_textRowCount >= m_textRows.size())
        throw FormatException("Invalid ZX Printer state"sv);
    for (auto &row : m_textRows)
        reader.readBytes(row);

    if (m_context && m_context->machine && streamsBytes(m_context->machine->id())) {
        m_stream.lastBit = reader.read<uint64_t>();
        m_stream.graphicsWidth = reader.read<uint16_t>();
        m_stream.graphicsColumn = reader.read<uint16_t>();
        m_stream.bitCount = reader.read<uint8_t>();
        m_stream.byte = reader.read<uint8_t>();
        m_stream.mode = StreamMode(reader.read<uint8_t>());
        m_stream.skipLineFeed = reader.readBool();
        m_stream.columns = reader.read<uint8_t>();
        if (m_stream.bitCount > 9 || m_stream.mode > StreamMode::Graphics
            || m_stream.columns > m_stream.text.size()
            || (m_stream.mode == StreamMode::Graphics
                && m_stream.graphicsColumn >= m_stream.graphicsWidth))
            throw FormatException("Invalid serial printer state"sv);
        reader.readBytes(m_stream.text);
        for (auto &row : m_stream.graphics)
            reader.readBytes(row);
        m_parallelByte = reader.read<uint8_t>();
        m_parallelPending = reader.readBool();
        m_parallelStrobe = reader.readBool();
        m_hcSerial = reader.readBool();
    }
}

} // namespace zxkit
