// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "BasicListing.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
std::string keyword(uint8_t token, BasicDialect dialect)
{
    // Reuse the codec's dialect tables without maintaining a second token map.
    const std::array<uint8_t, 6> line{ 0, 0, 2, 0, token, 13 };
    auto source = BasicCodec::decompile(line, dialect);
    auto start = source.find('{');
    auto end = source.find('}');
    return source.substr(start + 1, end - start - 1);
}

void appendKeyword(std::string &text, std::string_view token)
{
    if (!text.empty() && text.back() != ' ' && text.back() != ':' && text.back() != '('
        && text.back() != ',')
        text += ' ';
    text += token;
    text += ' ';
}

void finishLine(std::string &text)
{
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    text += '\n';
}
} // namespace

Object basicListing(std::span<const uint8_t> data, BasicDialect dialect)
{
    Object output{ { "source", BasicCodec::decompile(data, dialect) } };
    std::string listing;
    std::string effective;
    Array numbers;
    std::size_t position = 0;
    int previous = -1;
    int mismatches = 0;
    while (data.size() - position >= 4) {
        unsigned lineNumber = (data[position] << 8) | data[position + 1];
        std::size_t length = data[position + 2] | (data[position + 3] << 8);
        if (lineNumber > 9999 || int(lineNumber) <= previous || !length
            || length > data.size() - position - 4 || data[position + 3 + length] != 13)
            break;
        previous = int(lineNumber);
        listing += std::to_string(lineNumber) + " "s;
        effective += std::to_string(lineNumber) + " "s;
        auto body = data.subspan(position + 4, length - 1);
        bool quoted = false;
        bool rem = false;
        bool binary = false;
        struct Literal
        {
            std::string text;
            double value;
            std::size_t effectiveStart;
            bool binary;
        };
        std::optional<Literal> literal;
        for (std::size_t index = 0; index < body.size();) {
            auto byte = body[index];
            if (byte == 14 && !quoted && !rem) {
                auto stored = BasicCodec::decodeNumber(body.subspan(index + 1).first<5>());
                Object number{ { "line", lineNumber },
                               { "offset", position + 4 + index },
                               { "storedValue", stored },
                               { "storedHex", hex(body.subspan(index + 1, 5)) } };
                if (literal) {
                    bool matches = std::abs(literal->value - stored)
                            <= 1e-9 * std::max(1.0, std::abs(stored));
                    number["visibleText"] = literal->text;
                    number["matchesVisible"] = matches;
                    if (!matches) {
                        ++mismatches;
                        // Preserve BIN syntax when the stored integer is representable.
                        if (!literal->binary)
                            effective.replace(literal->effectiveStart, std::string::npos,
                                              std::format("{:.10g}", stored));
                        else if (stored >= 0 && stored <= 65535 && std::floor(stored) == stored)
                            effective.replace(literal->effectiveStart, std::string::npos,
                                              std::format("{:b}", unsigned(stored)));
                    }
                } else {
                    number["visibleText"] = nullptr;
                    number["matchesVisible"] = nullptr;
                }
                numbers.push_back(number);
                literal.reset();
                index += 6;
                continue;
            }
            literal.reset();
            bool numericStart = !quoted && !rem
                    && ((byte >= '0' && byte <= '9')
                        || (byte == '.' && index + 1 < body.size() && body[index + 1] >= '0'
                            && body[index + 1] <= '9'));
            if (numericStart && index) {
                auto before = body[index - 1];
                if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z')
                    || before == '$' || (before >= '0' && before <= '9'))
                    numericStart = false;
            }
            if (numericStart) {
                std::size_t count = 0;
                double value = 0;
                if (binary) {
                    while (index + count < body.size()
                           && (body[index + count] == '0' || body[index + count] == '1')) {
                        value = value * 2 + body[index + count] - '0';
                        ++count;
                    }
                } else {
                    auto tail =
                            std::string_view(reinterpret_cast<const char *>(body.data() + index),
                                             body.size() - index);
                    auto parsed = std::from_chars(tail.data(), tail.data() + tail.size(), value);
                    if (parsed.ec == std::errc{})
                        count = parsed.ptr - tail.data();
                }
                if (count) {
                    std::string text(body.begin() + index, body.begin() + index + count);
                    literal = Literal{ text, value, effective.size(), binary };
                    listing += text;
                    effective += text;
                    index += count;
                    binary = false;
                    continue;
                }
            }
            if (byte == '"' && !rem)
                quoted = !quoted;
            auto minimumToken = dialect == BasicDialect::Spectrum48 ? 0xA5
                    : dialect == BasicDialect::Spectrum128          ? 0xA3
                                                                    : 0x81;
            if (!quoted && !rem && byte >= minimumToken) {
                auto token = keyword(byte, dialect);
                appendKeyword(listing, token);
                appendKeyword(effective, token);
                rem = byte == 0xEA;
                binary = byte == 0xC4;
            } else {
                auto text = byte >= 32 && byte < 127 ? std::string(1, char(byte))
                                                     : std::format("\\x{:02X}", byte);
                if (!(byte == ' ' && !quoted && !rem && !listing.empty() && listing.back() == ' '))
                    listing += text;
                if (!(byte == ' ' && !quoted && !rem && !effective.empty()
                      && effective.back() == ' '))
                    effective += text;
            }
            ++index;
        }
        finishLine(listing);
        finishLine(effective);
        position += 4 + length;
    }
    output["listing"] = listing;
    output["effectiveListing"] = effective;
    output["numericConstants"] = numbers;
    output["numericMismatchCount"] = mismatches;
    output["trailingHex"] = hex(data.subspan(position));
    return output;
}
} // namespace zxkit::mcp_tool
