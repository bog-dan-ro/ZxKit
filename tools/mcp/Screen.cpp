// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"

#include <zxkit/media/ScreenshotFormat.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
constexpr int columns = 32;
constexpr int rows = 24;
constexpr int glyphHeight = 8;
constexpr uint16_t charsVariable = 0x5C36;
constexpr uint16_t defaultChars = 0x3C00;
constexpr int firstCode = 32;
constexpr int endCode = 128;
constexpr int fontSize = (endCode - firstCode) * glyphHeight;
constexpr int romFontStart = defaultChars + firstCode * glyphHeight;

struct ScreenText
{
    std::array<std::string, rows> lines;
    BankId bankId;
    uint16_t page16k;
    uint16_t offset;
    int fontAddress;
    bool customFont;
};

bool useCustomFont(std::string_view font)
{
    if (font == "rom"sv)
        return false;
    if (font == "auto"sv)
        return true;
    throw std::invalid_argument("font must be auto or rom"s);
}

std::span<const uint8_t> romFont(const Emulator &emulator)
{
    if (emulator.machine().usesRasterUla()) {
        // The Sinclair profiles place their BASIC character set in the last ROM page.
        const auto &rom = emulator.mmu().bank(emulator.machine().romBankId());
        return rom.span16k(rom.count16kPages() - 1).subspan(romFontStart, fontSize);
    }
    static const auto rom = embeddedRom("48.rom"sv);
    return std::span(rom).subspan(romFontStart, fontSize);
}

ScreenText inspectScreenText(Emulator &emulator, bool autoFont)
{
    const auto &ula = emulator.ula();
    const auto mode = ula.displayMode();
    if (emulator.machine().supportsZ80n()
        || (mode != VideoDisplayMode::StandardUla && mode != VideoDisplayMode::SecondScreen))
        throw std::invalid_argument("Screen text requires standard ULA video. Use captureScreen "
                                    "for this display mode"s);

    const auto &mmu = std::as_const(emulator.mmu());
    const auto chars = mmu.peek(charsVariable) | (mmu.peek(charsVariable + 1) << 8);
    const bool customFont = autoFont && chars != 0 && chars != defaultChars
            && chars <= 0x10000 - endCode * glyphHeight;
    const int fontAddress = customFont ? chars + firstCode * glyphHeight : romFontStart;
    const auto custom = customFont ? emulator.readMemory(uint16_t(fontAddress), fontSize) : Bytes{};
    const auto font = customFont ? std::span<const uint8_t>(custom) : romFont(emulator);

    std::unordered_map<uint64_t, char> glyphs;
    glyphs.reserve(endCode - firstCode);
    for (int code = firstCode; code < endCode; ++code) {
        uint64_t glyph = 0;
        for (uint8_t bits : font.subspan((code - firstCode) * glyphHeight, glyphHeight))
            glyph = (glyph << 8) | bits;
        auto [entry, inserted] = glyphs.try_emplace(glyph, char(code));
        if (!inserted)
            entry->second = '\0';
    }

    const uint16_t offset = mode == VideoDisplayMode::SecondScreen ? 0x2000 : ula.vramOffsetBytes();
    ScreenText screen{ {}, ula.vramBankId(), ula.vramPage16k(), offset, fontAddress, customFont };
    const auto vram = mmu.bank(screen.bankId).span16k(screen.page16k);
    for (int row = 0; row < rows; ++row) {
        auto &line = screen.lines[row];
        line.reserve(columns);
        const auto scan = offset + (row / 8) * 2048 + (row % 8) * 32;
        const auto attributes = offset + 6144 + row * columns;
        for (int column = 0; column < columns; ++column) {
            const auto attr = vram[(attributes + column) & 0x3FFF];
            uint64_t glyph = 0;
            for (int y = 0; y < glyphHeight; ++y)
                glyph = (glyph << 8) | vram[(scan + y * 256 + column) & 0x3FFF];
            if ((attr & 7) == ((attr >> 3) & 7) || glyph == 0
                || glyph == std::numeric_limits<uint64_t>::max()) {
                line += ' ';
                continue;
            }
            auto entry = glyphs.find(glyph);
            if (entry == glyphs.end())
                entry = glyphs.find(~glyph);
            line += entry == glyphs.end() ? '\0' : entry->second;
        }
    }
    return screen;
}

Object screenJson(const ScreenText &screen)
{
    Array lines;
    std::string text;
    int unknownCells = 0;
    for (int row = 0; row < rows; ++row) {
        std::string line;
        Array unknown;
        for (int column = 0; column < columns; ++column) {
            switch (char code = screen.lines[row][column]) {
            case '\0':
                unknown.emplace_back(column);
                ++unknownCells;
                line += "\uFFFD"sv;
                break;
            case 0x60:
                line += "\u00A3"sv;
                break;
            case 0x7F:
                line += "\u00A9"sv;
                break;
            default:
                line += code;
                break;
            }
        }
        if (row)
            text += '\n';
        text += line;
        lines.emplace_back(Object{
                { "row", row }, { "text", line }, { "unknownColumns", std::move(unknown) } });
    }
    return { { "text", std::move(text) },
             { "rows", std::move(lines) },
             { "columns", columns },
             { "unknownCells", unknownCells },
             { "bankId", screen.bankId },
             { "page16k", screen.page16k },
             { "offset", screen.offset },
             { "fontSource", screen.customFont ? "chars" : "rom" },
             { "fontAddress", screen.fontAddress } };
}

std::string screenQuery(std::string_view text)
{
    std::string query;
    while (!text.empty()) {
        if (text.starts_with("\u00A3"sv)) {
            query += '\x60';
            text.remove_prefix(2);
        } else if (text.starts_with("\u00A9"sv)) {
            query += '\x7F';
            text.remove_prefix(2);
        } else if (text.front() >= ' ' && text.front() <= '~' && text.front() != '\x60') {
            query += text.front();
            text.remove_prefix(1);
        } else
            throw std::invalid_argument(
                    "Search text must use printable Spectrum characters on one row"s);
        if (query.size() > columns)
            throw std::invalid_argument("Search text must fit in 32 columns"s);
    }
    if (query.empty())
        throw std::invalid_argument("Search text must not be empty"s);
    return query;
}
} // namespace

std::string Tools::readScreenText(std::string sessionId, std::string font)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        auto output = screenJson(inspectScreenText(emulator, useCustomFont(font)));
        output["frame"] = emulator.clock().frameCount();
        output["cycles"] = emulator.clock().totalTstates();
        return output;
    });
}

std::string Tools::waitForScreenText(std::string sessionId, std::string text, std::string font,
                                     int maxFrames, int maxMilliseconds)
{
    return result([&] {
        checked(maxFrames, 1, 10000, "maxFrames"sv);
        checked(maxMilliseconds, 1, 30000, "maxMilliseconds"sv);
        const auto query = screenQuery(text);
        const bool autoFont = useCustomFont(font);
        auto &current = session(sessionId);
        if (current.emulator->isRzxSeeking())
            throw std::invalid_argument("Advance or cancel the active RZX seek before waiting"s);
        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMilliseconds);
        Json match;
        const auto findText = [&] {
            const auto screen = inspectScreenText(*current.emulator, autoFont);
            for (int row = 0; row < rows; ++row) {
                if (const auto column = screen.lines[row].find(query);
                    column != std::string::npos) {
                    match = Object{ { "row", row }, { "column", column }, { "text", text } };
                    return true;
                }
            }
            return false;
        };
        Object output;
        if (findText()) {
            output = status(current).as_object();
            output["completedFrames"] = 0;
            output["stopReason"] = "match";
        } else
            output = executeUntil(current, maxFrames, deadline, {}, findText).as_object();
        output["match"] = std::move(match);
        return output;
    });
}

std::string Tools::captureLastFrame(std::string sessionId, std::string path, bool overwrite)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        const auto &frame = emulator.lastCompletedFrame();
        if (!frame)
            throw std::invalid_argument("Run a frame before capturing a completed raster"s);
        auto view = frame.readLock();
        const auto bytes = encodePng(*view);
        const auto output = outputPath(path);
        writeFile(output, bytes, overwrite);
        return Object{ { "path", output.string() },
                       { "bytes", bytes.size() },
                       { "frame", view->frameNumber() },
                       { "width", view->width() },
                       { "height", view->height() } };
    });
}

std::string Tools::readRasterState(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        const auto &clock = emulator.clock();
        const auto &timings = clock.timings();
        const auto position = clock.rasterPos();
        return Object{ { "frame", clock.frameCount() },
                       { "cycles", clock.totalTstates() },
                       { "line", position.line },
                       { "lineTstate", position.lineTstate },
                       { "baseClockHz", timings.baseClockHz },
                       { "cpuSpeedMultiplier", clock.speedMultiplier() },
                       { "tstatesPerLine", timings.tstatesPerLine },
                       { "linesPerFrame", timings.linesPerFrame },
                       { "vramBankId", emulator.ula().vramBankId() },
                       { "vramPage16k", emulator.ula().vramPage16k() },
                       { "vramOffset", emulator.ula().vramOffsetBytes() },
                       { "im2PendingMask", emulator.im2().pendingMask() },
                       { "im2InServiceMask", emulator.im2().inServiceMask() } };
    });
}
} // namespace zxkit::mcp_tool
