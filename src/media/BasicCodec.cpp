// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/BasicCodec.h"
#include "archive/ArchiveInternal.h"
#include "NumberParsing.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>

namespace zxkit::BasicCodec {
using namespace std::literals;
namespace {
using namespace archive;
constexpr std::array<std::string_view, 127> keywords = {
    "TIME"sv,    "PRIVATE"sv,   "IF-ELSE"sv, "ENDIF"sv,   "EXIT"sv,   "REF"sv,      "PEEK$"sv,
    "REG"sv,     "DPOKE"sv,     "DPEEK"sv,   "MOD"sv,     "<<"sv,     ">>"sv,       "UNTIL"sv,
    "ERROR"sv,   "ON"sv,        "DEFPROC"sv, "ENDPROC"sv, "PROC"sv,   "LOCAL"sv,    "DRIVER"sv,
    "WHILE"sv,   "REPEAT"sv,    "ELSE"sv,    "REMOUNT"sv, "BANK"sv,   "TILE"sv,     "LAYER"sv,
    "PALETTE"sv, "SPRITE"sv,    "PWD"sv,     "CD"sv,      "MKDIR"sv,  "RMDIR"sv,    "SPECTRUM"sv,
    "PLAY"sv,    "RND"sv,       "INKEY$"sv,  "PI"sv,      "FN"sv,     "POINT"sv,    "SCREEN$"sv,
    "ATTR"sv,    "AT"sv,        "TAB"sv,     "VAL$"sv,    "CODE"sv,   "VAL"sv,      "LEN"sv,
    "SIN"sv,     "COS"sv,       "TAN"sv,     "ASN"sv,     "ACS"sv,    "ATN"sv,      "LN"sv,
    "EXP"sv,     "INT"sv,       "SQR"sv,     "SGN"sv,     "ABS"sv,    "PEEK"sv,     "IN"sv,
    "USR"sv,     "STR$"sv,      "CHR$"sv,    "NOT"sv,     "BIN"sv,    "OR"sv,       "AND"sv,
    "<="sv,      ">="sv,        "<>"sv,      "LINE"sv,    "THEN"sv,   "TO"sv,       "STEP"sv,
    "DEF FN"sv,  "CAT"sv,       "FORMAT"sv,  "MOVE"sv,    "ERASE"sv,  "OPEN #"sv,   "CLOSE #"sv,
    "MERGE"sv,   "VERIFY"sv,    "BEEP"sv,    "CIRCLE"sv,  "INK"sv,    "PAPER"sv,    "FLASH"sv,
    "BRIGHT"sv,  "INVERSE"sv,   "OVER"sv,    "OUT"sv,     "LPRINT"sv, "LLIST"sv,    "STOP"sv,
    "READ"sv,    "DATA"sv,      "RESTORE"sv, "NEW"sv,     "BORDER"sv, "CONTINUE"sv, "DIM"sv,
    "REM"sv,     "FOR"sv,       "GO TO"sv,   "GO SUB"sv,  "INPUT"sv,  "LOAD"sv,     "LIST"sv,
    "LET"sv,     "PAUSE"sv,     "NEXT"sv,    "POKE"sv,    "PRINT"sv,  "PLOT"sv,     "RUN"sv,
    "SAVE"sv,    "RANDOMIZE"sv, "IF"sv,      "CLS"sv,     "DRAW"sv,   "CLEAR"sv,    "RETURN"sv,
    "COPY"sv
};
bool alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool digit(char c)
{
    return c >= '0' && c <= '9';
}
unsigned hex(char c)
{
    if (digit(c))
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    throw FormatException("Invalid hexadecimal escape"sv);
}
std::string escaped(uint8_t c)
{
    return std::format("\\x{:02X}", c);
}
void rawText(Bytes &out, std::string_view text)
{
    require(text.size() % 2 == 0, "Odd raw hex length"sv);
    for (std::size_t i = 0; i < text.size(); i += 2)
        out.push_back(uint8_t(hex(text[i]) * 16 + hex(text[i + 1])));
}
Bytes tokenize(std::string_view line, BasicDialect dialect)
{
    Bytes out;
    bool quoted = false;
    bool rem = false;
    bool bin = false;
    std::size_t start = dialect == BasicDialect::Spectrum48 ? 36
            : dialect == BasicDialect::Spectrum128          ? 34
                                                            : 0;
    for (std::size_t p = 0; p < line.size();) {
        char c = line[p];
        if (c == '\\') {
            require(p + 1 < line.size(), "Unterminated BASIC escape"sv);
            if (line[p + 1] == '\\') {
                out.push_back('\\');
                p += 2;
                continue;
            }
            if (line[p + 1] == 'x') {
                require(p + 4 <= line.size(), "Incomplete BASIC byte escape"sv);
                out.push_back(uint8_t(hex(line[p + 2]) * 16 + hex(line[p + 3])));
                p += 4;
                continue;
            }
            if (line[p + 1] == '{') {
                auto end = line.find('}', p + 2);
                require(end != line.npos, "Unclosed token escape"sv);
                auto key = upper(line.substr(p + 2, end - p - 2));
                auto i = std::ranges::find(keywords, key);
                require(i != keywords.end(), "Unknown BASIC token"sv);
                auto code = uint8_t(0x81 + std::distance(keywords.begin(), i));
                out.push_back(code);
                rem |= code == 0xEA;
                bin = code == 0xC4;
                p = end + 1;
                continue;
            }
            throw FormatException("Unknown BASIC escape"sv);
        }
        if (c == '"' && !rem) {
            quoted = !quoted;
            out.push_back(c);
            ++p;
            continue;
        }
        if (!quoted && !rem) {
            std::size_t best = 0;
            std::size_t index = 0;
            for (std::size_t i = start; i < keywords.size(); ++i) {
                auto k = keywords[i];
                if (k.size() <= best || p + k.size() > line.size())
                    continue;
                if (upper(line.substr(p, k.size())) != k)
                    continue;
                if (alpha(k[0]) && p
                    && (alpha(line[p - 1]) || digit(line[p - 1]) || line[p - 1] == '$'))
                    continue;
                if (alpha(k.back()) && p + k.size() < line.size()
                    && (alpha(line[p + k.size()]) || digit(line[p + k.size()])
                        || line[p + k.size()] == '$'))
                    continue;
                best = k.size();
                index = i;
            }
            if (best) {
                auto code = uint8_t(0x81 + index);
                out.push_back(code);
                rem = code == 0xEA;
                bin = code == 0xC4;
                p += best;
                continue;
            }
            if ((digit(c) || (c == '.' && p + 1 < line.size() && digit(line[p + 1])))
                && (!p || (!alpha(line[p - 1]) && !digit(line[p - 1]) && line[p - 1] != '$'))) {
                auto tail = line.substr(p);
                std::size_t n = 0;
                double value = 0;
                if (bin) {
                    while (n < tail.size() && (tail[n] == '0' || tail[n] == '1')) {
                        value = value * 2 + (tail[n] - '0');
                        ++n;
                    }
                    require(n > 0, "BIN requires binary digits"sv);
                } else {
                    auto result = parseNumber(tail.data(), tail.data() + tail.size(), value);
                    require(result.ec == std::errc{}, "Invalid BASIC number"sv);
                    n = result.ptr - tail.data();
                }
                out.insert(out.end(), line.begin() + p, line.begin() + p + n);
                p += n;
                bin = false;
                if (!(line.substr(p).starts_with("\\x0E"sv)
                      || line.substr(p).starts_with("\\x0e"sv))) {
                    out.push_back(0x0E);
                    auto number = encodeNumber(value);
                    append(out, number);
                }
                continue;
            }
        }
        require(uint8_t(c) >= 32 && uint8_t(c) < 127,
                "Use byte escapes for non-ASCII BASIC characters"sv);
        out.push_back(uint8_t(c));
        ++p;
    }
    return out;
}
} // namespace

std::array<uint8_t, 5> encodeNumber(double v)
{
    require(std::isfinite(v), "BASIC number must be finite"sv);
    if (v == 0)
        return {};
    if (std::floor(v) == v && v >= -65536 && v <= 65535) {
        int64_t i = int64_t(v);
        return { 0, uint8_t(v < 0 ? 255 : 0), uint8_t(i), uint8_t(i >> 8), 0 };
    }
    bool negative = v < 0;
    int exponent = 0;
    double fraction = std::frexp(std::abs(v), &exponent);
    require(exponent >= -128 && exponent <= 127, "BASIC floating point overflow/underflow"sv);
    uint64_t mantissa = uint64_t(std::llround(std::ldexp(fraction, 32)));
    if (mantissa == UINT64_C(0x100000000)) {
        mantissa >>= 1;
        ++exponent;
    }
    require(exponent <= 127, "BASIC floating point overflow"sv);
    uint32_t stored = uint32_t(mantissa) & 0x7FFFFFFF;
    if (negative)
        stored |= 0x80000000;
    return { uint8_t(exponent + 128), uint8_t(stored >> 24), uint8_t(stored >> 16),
             uint8_t(stored >> 8), uint8_t(stored) };
}
double decodeNumber(std::span<const uint8_t, 5> b)
{
    if (!b[0])
        return double(int(b[2] | (b[3] << 8)) - (b[1] ? 65536 : 0));
    uint32_t bits =
            (uint32_t(b[1] & 127) << 24) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 8) | b[4];
    double v = std::ldexp(1.0 + double(bits) / 2147483648.0, int(b[0]) - 129);
    return b[1] & 128 ? -v : v;
}
Bytes compile(std::string_view source, BasicDialect dialect)
{
    Bytes out;
    std::size_t p = 0;
    int previous = -1;
    bool raw = false;
    while (p < source.size()) {
        auto end = source.find('\n', p);
        if (end == source.npos)
            end = source.size();
        auto line = source.substr(p, end - p);
        p = end + 1;
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (line.starts_with("!raw "sv)) {
            rawText(out, line.substr(5));
            raw = true;
            continue;
        }
        require(!raw, "BASIC lines must precede raw variable/array data"sv);
        unsigned number = 0;
        auto r = std::from_chars(line.data(), line.data() + line.size(), number);
        require(r.ec == std::errc{} && number <= 9999 && int(number) > previous,
                "BASIC line numbers must increase (0..9999)"sv);
        previous = int(number);
        std::size_t pos = r.ptr - line.data();
        require(pos < line.size() && line[pos] == ' ',
                "Separate line number and body with one space"sv);
        auto body = tokenize(line.substr(pos + 1), dialect);
        require(body.size() < 65535, "BASIC line too long"sv);
        out.push_back(uint8_t(number >> 8));
        out.push_back(uint8_t(number));
        std::size_t n = out.size();
        out.resize(n + 2);
        setWord(out, n, body.size() + 1);
        append(out, body);
        out.push_back(13);
    }
    return out;
}
std::string decompile(std::span<const uint8_t> data, BasicDialect dialect)
{
    std::string out;
    std::size_t p = 0;
    int previous = -1;
    while (data.size() - p >= 4) {
        unsigned number = (data[p] << 8) | data[p + 1];
        auto size = word(data, p + 2);
        if (number > 9999 || int(number) <= previous || !size || size > data.size() - p - 4
            || data[p + 3 + size] != 13)
            break;
        previous = int(number);
        out += std::format("{} ", number);
        auto body = slice(data, p + 4, size - 1);
        bool quoted = false;
        bool rem = false;
        for (std::size_t j = 0; j < body.size(); ++j) {
            auto c = body[j];
            if (c == 14 && !quoted && !rem) {
                require(body.size() - j >= 6, "Truncated hidden BASIC number"sv);
                for (unsigned k = 0; k < 6; ++k)
                    out += escaped(body[j++]);
                --j;
                continue;
            }
            if (c == '"' && !rem)
                quoted = !quoted;
            if (!quoted && !rem
                && c >= (dialect == BasicDialect::Spectrum48            ? 0xA5
                                 : dialect == BasicDialect::Spectrum128 ? 0xA3
                                                                        : 0x81)) {
                // Explicit tokens avoid ambiguities next to identifiers and preserve spacing.
                out += std::format("\\{{{}}}", keywords[c - 0x81]);
                rem = c == 0xEA;
            } else if (c == '\\')
                out += "\\\\"sv;
            else if (c >= 32 && c < 127)
                out += char(c);
            else
                out += escaped(c);
        }
        out += '\n';
        p += 4 + size;
    }
    // Includes variables, numeric/string arrays, FOR state, and nonstandard program tails.
    if (p < data.size()) {
        out += "!raw "sv;
        for (auto c : data.subspan(p))
            out += std::format("{:02X}", c);
        out += '\n';
    }
    return out;
}

Bytes compileArray(std::string_view source, bool characters)
{
    auto newline = source.find('\n');
    require(newline != source.npos, "Array text starts with dimensions followed by a newline"sv);
    auto dimensions = source.substr(0, newline);
    if (dimensions.ends_with('\r'))
        dimensions.remove_suffix(1);
    Bytes out{ 0 };
    std::size_t count = 1;
    std::size_t width = 0;
    while (!dimensions.empty()) {
        auto comma = dimensions.find(',');
        auto part = dimensions.substr(0, comma);
        unsigned dimension = 0;
        auto r = std::from_chars(part.data(), part.data() + part.size(), dimension);
        require(r.ec == std::errc{} && r.ptr == part.data() + part.size() && dimension > 0
                        && dimension <= 65535 && out[0] < 255,
                "Invalid BASIC array dimension"sv);
        require(count <= 1048576 / dimension, "BASIC array too large"sv);
        count *= dimension;
        width = dimension;
        ++out[0];
        auto p = out.size();
        out.resize(p + 2);
        setWord(out, p, dimension);
        if (comma == dimensions.npos)
            break;
        dimensions.remove_prefix(comma + 1);
    }
    require(out[0] > 0, "Array needs at least one dimension"sv);
    source.remove_prefix(newline + 1);
    if (characters) {
        std::size_t rows = count / width;
        std::istringstream input{ std::string(source) };
        std::string line;
        for (std::size_t i = 0; i < rows; ++i) {
            require(bool(std::getline(input, line)), "Missing string array row"sv);
            if (line.ends_with('\r'))
                line.pop_back();
            require(line.size() >= 2 && line.front() == '"' && line.back() == '"',
                    "String array rows must be quoted"sv);
            auto bytes = tokenize(line, BasicDialect::Spectrum48);
            require(bytes.front() == '"' && bytes.back() == '"' && bytes.size() - 2 <= width,
                    "String row exceeds last dimension"sv);
            append(out, View(bytes).subspan(1, bytes.size() - 2));
            out.insert(out.end(), width - (bytes.size() - 2), ' ');
        }
        while (std::getline(input, line))
            require(line.empty(), "Extra string array rows"sv);
    } else {
        std::istringstream input{ std::string(source) };
        std::string token;
        for (std::size_t i = 0; i < count; ++i) {
            require(bool(input >> token), "Missing numeric array value"sv);
            if (token.starts_with('@')) {
                require(token.size() == 11,
                        "Raw numeric array values use @ followed by five hex bytes"sv);
                rawText(out, std::string_view(token).substr(1));
            } else {
                double value = 0;
                auto r = parseNumber(token.data(), token.data() + token.size(), value);
                require(r.ec == std::errc{} && r.ptr == token.data() + token.size(),
                        "Invalid array number"sv);
                append(out, encodeNumber(value));
            }
        }
        require(!(input >> token), "Extra numeric array values"sv);
    }
    return out;
}
std::string decompileArray(View data, bool characters)
{
    require(!data.empty() && data[0] > 0, "Array dimensions missing"sv);
    std::size_t count = 1;
    std::size_t width = 0;
    std::size_t pos = 1;
    std::string out;
    for (unsigned i = 0; i < data[0]; ++i) {
        unsigned n = word(data, pos);
        pos += 2;
        require(n && count <= 1048576 / n, "Invalid array dimensions"sv);
        count *= n;
        width = n;
        if (i)
            out += ',';
        out += std::to_string(n);
    }
    require(data.size() - pos == count * (characters ? 1 : 5),
            "Array size disagrees with dimensions"sv);
    out += '\n';
    if (characters) {
        for (std::size_t row = 0; row < count / width; ++row) {
            out += '"';
            for (auto c : slice(data, pos + row * width, width)) {
                if (c == '\\')
                    out += "\\\\"sv;
                else if (c >= 32 && c < 127 && c != '"')
                    out += char(c);
                else
                    out += escaped(c);
            }
            out += "\"\n"sv;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            std::array<uint8_t, 5> raw;
            std::ranges::copy(slice(data, pos + i * 5, 5), raw.begin());
            double value = decodeNumber(raw);
            if (encodeNumber(value) == raw)
                out += std::format("{:.17g}", value);
            else {
                out += '@';
                for (auto c : raw)
                    out += std::format("{:02X}", c);
            }
            out += (i + 1) % width ? ' ' : '\n';
        }
    }
    return out;
}
} // namespace zxkit::BasicCodec
