// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/MediaView.h"
#include "NumberParsing.h"
#include "zxkit/debug/Disassembler.h"
#include "zxkit/media/MediaPreview.h"
#include "zxkit/media/TapeFormats.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <string>
#include <string_view>

namespace zxkit {
using namespace std::literals;

namespace {
constexpr std::size_t ScreenBytes = 6912;
constexpr std::size_t TextLimit = 1024 * 1024;

std::string lowerSuffix(std::string_view name)
{
    const auto slash = name.rfind('/');
    const auto leaf = slash == name.npos ? name : name.substr(slash + 1);
    const auto dot = leaf.rfind('.');
    std::string suffix(dot == leaf.npos ? std::string_view{} : leaf.substr(dot + 1));
    for (auto &c : suffix)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return suffix;
}

bool validUtf8(std::span<const uint8_t> bytes)
{
    for (std::size_t i = 0; i < bytes.size();) {
        const auto c = bytes[i];
        const std::size_t length = c < 0x80 ? 1
                : (c >> 5) == 6             ? 2
                : (c >> 4) == 14            ? 3
                : (c >> 3) == 30            ? 4
                                            : 0;
        if (!length || bytes.size() - i < length)
            return false;
        for (std::size_t j = 1; j < length; ++j)
            if ((bytes[i + j] & 0xC0) != 0x80)
                return false;
        i += length;
    }
    return true;
}

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        lines.emplace_back(text.substr(start, end == text.npos ? text.npos : end - start));
        if (end == text.npos)
            break;
        start = end + 1;
    }
    return lines;
}

std::vector<TapeBlock> parseBlock(std::span<const uint8_t> bytes)
{
    const bool tzx = bytes.size() >= 7
            && std::ranges::equal(bytes.first(7), "ZXTape!"sv, {}, {},
                                  [](char c) { return uint8_t(c); });
    return tzx ? TapeFormats::parseTzx(bytes) : TapeFormats::parseTap(bytes);
}
} // namespace

bool textFileName(std::string_view name)
{
    static constexpr std::array suffixes = { "txt"sv, "asm"sv, "a80"sv, "z80asm"sv, "doc"sv,
                                             "me"sv,  "prn"sv, "lst"sv, "c"sv,      "h"sv,
                                             "mac"sv, "sub"sv, "bas"sv, "md"sv,     "pok"sv,
                                             "sld"sv, "sym"sv, "nfo"sv, "diz"sv,    "ini"sv,
                                             "cfg"sv };
    return std::ranges::find(suffixes, lowerSuffix(name)) != suffixes.end();
}

MediaContent mediaContent(std::string_view filesystem, std::string_view name,
                          const MediaAttributes &attributes, uint64_t size)
{
    if (filesystem == "tape"sv && name.starts_with("blocks/"sv))
        return MediaContent::TapeBlock;
    // A tape's editable BASIC listings
    if (filesystem == "tape"sv && name.starts_with("basic/"sv))
        return MediaContent::Text;
    if (const auto type = attributes.find("type"sv); type != attributes.end()) {
        if (filesystem.starts_with("trdos"sv)) {
            switch (type->second) {
            case 'B':
                return MediaContent::Basic;
            case 'C':
                return size == ScreenBytes ? MediaContent::Screen : MediaContent::Code;
            case 'D':
                return MediaContent::NumberArray;
            }
        } else {
            switch (type->second) {
            case 0:
                return MediaContent::Basic;
            case 1:
                return MediaContent::NumberArray;
            case 2:
                return MediaContent::CharacterArray;
            case 3:
                return size == ScreenBytes ? MediaContent::Screen : MediaContent::Code;
            }
        }
    }
    return textFileName(name) ? MediaContent::Text : MediaContent::Unknown;
}

uint16_t mediaLoadAddress(std::string_view name, const MediaAttributes &attributes)
{
    if (const auto param1 = attributes.find("param1"sv); param1 != attributes.end())
        return uint16_t(param1->second);
    return lowerSuffix(name) == "com"sv ? 0x100 : 0;
}

MediaContent fileContent(std::string_view name, uint64_t size)
{
    const auto suffix = lowerSuffix(name);
    if (suffix == "scr"sv && size >= ScreenBytes)
        return MediaContent::Screen;
    if (suffix == "sna"sv || suffix == "z80"sv || suffix == "szx"sv)
        return MediaContent::Snapshot;
    return textFileName(name) ? MediaContent::Text : MediaContent::Unknown;
}

std::optional<MediaText> mediaText(std::span<const uint8_t> data)
{
    auto text = data;
    if (const auto end = std::ranges::find(data, uint8_t(0x1A));
        end != data.end() && data.end() - end <= 128)
        text = data.first(std::size_t(end - data.begin()));
    if (text.empty())
        return std::nullopt;
    std::size_t control = 0;
    std::size_t high = 0;
    for (const auto c : text) {
        control += (c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || c == 127;
        high += c >= 128;
    }
    if (control * 50 > text.size() || high * 2 > text.size())
        return std::nullopt;
    MediaText result;
    result.text.assign(text.begin(), text.end());
    if (!validUtf8(text)) {
        for (auto &c : result.text)
            c = char(uint8_t(c) & 0x7F);
        result.utf8 = false;
        result.bit7 = high > 0;
    }
    const std::string_view all(reinterpret_cast<const char *>(data.data()), data.size());
    if (all.find("\r\n"sv) != all.npos)
        result.lineEnding = "\r\n"s;
    else if (all.find('\r') != all.npos && all.find('\n') == all.npos)
        result.lineEnding = "\r"s;
    std::string normalized;
    normalized.reserve(result.text.size());
    for (std::size_t i = 0; i < result.text.size(); ++i) {
        if (result.text[i] != '\r')
            normalized += result.text[i];
        else if (i + 1 >= result.text.size() || result.text[i + 1] != '\n')
            normalized += '\n';
    }
    result.text = std::move(normalized);
    return result;
}

std::vector<std::string> hexLines(std::span<const uint8_t> data, uint32_t address,
                                  std::size_t limit)
{
    std::vector<std::string> lines;
    const auto size = std::min(data.size(), limit);
    const int digits = address + size > 0x10000 ? 6 : 4;
    for (std::size_t offset = 0; offset < size; offset += 16) {
        std::string bytes;
        std::string text;
        for (std::size_t i = offset; i < offset + 16; ++i) {
            if (i < size) {
                bytes += std::format("{:02X} ", data[i]);
                text += char(data[i] >= 32 && data[i] < 127 ? data[i] : '.');
            } else {
                bytes += "   "sv;
            }
        }
        lines.push_back(std::format("{:0{}X}  {} {}", address + offset, digits, bytes, text));
    }
    if (data.size() > size)
        lines.push_back(std::format("\xE2\x80\xA6 {} more bytes", data.size() - size));
    return lines;
}

std::vector<std::string> disassemblyLines(std::span<const uint8_t> data, uint16_t address,
                                          int restart, std::size_t limit)
{
    std::vector<std::string> lines;
    const auto size = std::min(data.size(), limit);
    const auto read = [&](uint16_t at) -> uint8_t {
        const auto offset = uint16_t(at - address);
        return offset < size ? data[offset] : 0;
    };
    const auto restartOffset = std::size_t(restart - address);
    for (std::size_t offset = 0; offset < size;) {
        const auto instruction =
                Disassembler::disassembleOne(read, uint16_t(address + offset), false);
        std::string bytes;
        for (unsigned i = 0; i < instruction.length; ++i)
            bytes += std::format("{:02X} ", instruction.bytes[i]);
        lines.push_back(
                std::format("{:04X}  {:<12}{}", instruction.address, bytes, instruction.mnemonic));
        auto next = offset + std::max<unsigned>(1, instruction.length);
        if (restart >= address && restartOffset > offset && restartOffset < next)
            next = restartOffset;
        offset = next;
    }
    return lines;
}

std::vector<MediaViewPage> viewMedia(std::span<const uint8_t> data, MediaContent content,
                                     uint16_t address, std::string_view name)
{
    using Kind = MediaViewPage::Kind;
    std::vector<MediaViewPage> pages;
    const auto linesPage = [&](Kind kind, std::vector<std::string> lines, uint32_t at = 0) {
        auto &page = pages.emplace_back();
        page.kind = kind;
        page.address = at;
        page.lines = std::move(lines);
    };
    const auto hexPage = [&](std::span<const uint8_t> bytes, uint32_t at, Kind kind = Kind::Hex) {
        linesPage(kind, hexLines(bytes, at), at);
    };
    const auto disassemblyPage = [&](std::span<const uint8_t> bytes, uint16_t at,
                                     Kind kind = Kind::Disassembly) {
        linesPage(kind, disassemblyLines(bytes, at), at);
        const auto kept = bytes.first(std::min<std::size_t>(bytes.size(), 64 * 1024));
        pages.back().bytes.assign(kept.begin(), kept.end());
    };
    const auto screenPage = [&](Kind kind, std::span<const uint8_t> bytes) {
        try {
            MediaViewPage page;
            page.kind = kind;
            page.screen = renderScrToVideoFrame(bytes.first(ScreenBytes));
            pages.push_back(std::move(page));
        } catch (const std::exception &) {
            // Not a screen after all
        }
    };
    // Exactly a screen shows as one first; anything larger can be looked at as one, last
    const auto screenFirst = [&](std::span<const uint8_t> bytes) {
        if (bytes.size() == ScreenBytes)
            screenPage(Kind::Screen, bytes);
    };
    const auto screenLast = [&](std::span<const uint8_t> bytes) {
        if (bytes.size() > ScreenBytes)
            screenPage(Kind::AsScreen, bytes);
    };

    switch (content) {
    case MediaContent::TapeBlock:
        try {
            const auto blocks = parseBlock(data);
            if (!blocks.empty()) {
                const auto &block = blocks.front();
                // A ROM-style block holds its flag and checksum around the data
                std::span<const uint8_t> payload = block.data;
                if (payload.size() >= 2
                    && (payload.size() == ScreenBytes + 2 || block.isRomBlock()))
                    payload = payload.subspan(1, payload.size() - 2);
                screenFirst(payload);
                std::vector<std::string> details{
                    block.description,
                    std::format("TZX block ID: {:02X}", uint8_t(block.type)),
                    std::format("Data: {} bytes", block.data.size()),
                    std::format("Pause after: {} ms", block.pauseAfterMs),
                };
                if (block.type == TapeBlockType::StandardData
                    || block.type == TapeBlockType::TurboData) {
                    details.push_back(std::format("Pilot: {} \xC3\x97 {} T", block.pilotPulses,
                                                  block.pilotPulse));
                    details.push_back(
                            std::format("Sync: {}, {} T", block.sync1Pulse, block.sync2Pulse));
                    details.push_back(std::format("Bits: 0 = {} T, 1 = {} T", block.zeroPulse,
                                                  block.onePulse));
                }
                linesPage(Kind::Block, std::move(details));
                if (!block.data.empty())
                    hexPage(block.data, 0);
                screenLast(payload);
            }
        } catch (const std::exception &e) {
            linesPage(Kind::Block, { e.what() });
        }
        hexPage(data, 0, Kind::Raw);
        break;
    case MediaContent::Basic:
        try {
            auto listing = BasicCodec::listingLines(data, BasicDialect::Spectrum128);
            std::vector<std::string> lines;
            std::vector<uint32_t> calls;
            for (const auto &line : listing) {
                auto &text = lines.emplace_back();
                for (std::size_t k = 0; k < line.size(); ++k) {
                    text += line[k].text;
                    if (line[k].kind != BasicCodec::ListingPart::Kind::Keyword
                        || line[k].text != "USR"sv)
                        continue;
                    // Where USR calls land in the program: code to read, often in a REM
                    for (auto next = k + 1; next < line.size(); ++next) {
                        const auto &part = line[next];
                        if (part.kind == BasicCodec::ListingPart::Kind::Number) {
                            double target = 0;
                            const auto &digits = part.text;
                            parseNumber(digits.data(), digits.data() + digits.size(), target);
                            if (target >= basicProgramStart
                                && target < basicProgramStart + double(data.size())
                                && std::ranges::find(calls, uint32_t(target)) == calls.end())
                                calls.push_back(uint32_t(target));
                        }
                        if (part.kind != BasicCodec::ListingPart::Kind::Text)
                            break;
                    }
                }
            }
            auto &page = pages.emplace_back();
            page.kind = Kind::Basic;
            page.address = basicProgramStart;
            page.lines = std::move(lines);
            page.listing = std::move(listing);
            linesPage(Kind::Unfiltered,
                      splitLines(BasicCodec::decompile(data, BasicDialect::Spectrum128)));
            for (const auto target : calls)
                disassemblyPage(data.subspan(target - basicProgramStart), uint16_t(target),
                                Kind::Usr);
        } catch (const std::exception &e) {
            linesPage(Kind::Basic, { e.what() });
        }
        hexPage(data, basicProgramStart);
        break;
    case MediaContent::NumberArray:
    case MediaContent::CharacterArray:
        try {
            linesPage(Kind::Array,
                      splitLines(BasicCodec::decompileArray(
                              data, content == MediaContent::CharacterArray)));
        } catch (const std::exception &) {
            // Shown in hex only
        }
        hexPage(data, 0);
        break;
    case MediaContent::Screen:
    case MediaContent::Code: {
        const auto origin = uint16_t(address || content == MediaContent::Code ? address : 16384);
        if (content == MediaContent::Screen && data.size() >= ScreenBytes)
            screenPage(Kind::Screen, data);
        else
            screenFirst(data);
        hexPage(data, origin);
        disassemblyPage(data, origin);
        if (content == MediaContent::Code)
            screenLast(data);
        break;
    }
    case MediaContent::Snapshot:
        try {
            MediaViewPage page;
            page.kind = Kind::Screen;
            page.screen = extractMediaPreview(data, name);
            pages.push_back(std::move(page));
        } catch (const std::exception &) {
            // Shown in hex only
        }
        hexPage(data, 0);
        break;
    case MediaContent::Text:
    case MediaContent::Unknown:
        if (const auto text = mediaText(data.first(std::min(data.size(), TextLimit)))) {
            linesPage(Kind::Text, splitLines(text->text));
            hexPage(data, 0);
            break;
        }
        screenFirst(data);
        hexPage(data, address);
        disassemblyPage(data, address);
        screenLast(data);
        break;
    }
    return pages;
}

} // namespace zxkit
