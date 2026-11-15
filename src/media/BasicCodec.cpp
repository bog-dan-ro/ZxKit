// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/BasicCodec.h"
#include "archive/ArchiveInternal.h"
#include "NumberParsing.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <iomanip>
#include <optional>
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

namespace {
// A stored number as the ROM would print it: integers in full, others to nine digits
std::string numberText(double value)
{
    if (value == std::trunc(value) && std::fabs(value) < 1e15)
        return std::format("{}", static_cast<long long>(value));
    return std::format("{:.9g}", value);
}

// Whether `text` (decimal, or binary after BIN) is what compile() stores as `stored`
bool listsStoredValue(std::string_view text, bool binary, std::span<const uint8_t, 5> stored)
{
    double value = 0;
    if (binary) {
        if (text.empty())
            return false;
        for (const char c : text) {
            if (c != '0' && c != '1')
                return false;
            value = value * 2 + (c - '0');
        }
    } else {
        const auto result = parseNumber(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return false;
    }
    try {
        if (std::ranges::equal(encodeNumber(value), stored))
            return true;
    } catch (const std::exception &) {
        return false;
    }
    // The ROM converts decimals with its calculator, which truncates: .05 is stored a bit below
    // the exact 0.05. Decoys are other numbers altogether.
    return std::fabs(decodeNumber(stored) - value) <= std::fabs(value) * 1e-8;
}
// Where a 48K or 128K program starts without Interface 1's channels: the address of a byte
constexpr std::size_t BasicStart = 23755;

// Whether the bytes at `p` are a program line: a number below 16384 (variables start with a
// letter, 0x40 and up), a length within the data, and its end. Lines may come in any order.
bool programLine(std::span<const uint8_t> data, std::size_t p)
{
    if (data.size() - p < 4 || data[p] >= 0x40)
        return false;
    const std::size_t size = word(data, p + 2);
    return size && size <= data.size() - p - 4 && data[p + 3 + size] == 13;
}

// Where a program runs with Interface 1's (or a disk system's) 58 bytes of channels before it
constexpr std::size_t ChannelsStart = BasicStart + 58;

constexpr uint8_t TokenCode = 0xAF;
constexpr uint8_t TokenVal = 0xB0;
constexpr uint8_t TokenPeek = 0xBE;
constexpr uint8_t TokenUsr = 0xC0;
constexpr uint8_t TokenRem = 0xEA;

// A USR argument: constant + low * PEEK 23635 + high * PEEK 23636. The program's address,
// PROG, is PEEK 23635+256*PEEK 23636.
struct UsrAddress
{
    double constant = 0;
    double low = 0;
    double high = 0;
    bool known = true;
};

// Reads the factor after a USR: numbers (the stored value), VAL"digits", CODE"text",
// PEEK 23635/23636, parentheses, + - *. Anything else is unknown.
class UsrParser
{
public:
    UsrParser(std::span<const uint8_t> body, std::size_t at) : m_body(body), m_at(at) { }

    UsrAddress factor()
    {
        skipSpaces();
        if (m_at >= m_body.size())
            return unknown();
        const auto c = m_body[m_at];
        if (c == '(') {
            ++m_at;
            auto value = expression();
            skipSpaces();
            if (m_at >= m_body.size() || m_body[m_at] != ')')
                return unknown();
            ++m_at;
            return value;
        }
        if (c == '-') {
            ++m_at;
            auto value = factor();
            value.constant = -value.constant;
            value.low = -value.low;
            value.high = -value.high;
            return value;
        }
        if (c == TokenVal || c == TokenCode) {
            ++m_at;
            const auto text = stringLiteral();
            if (!text)
                return unknown();
            if (c == TokenCode)
                return { text->empty() ? 0 : double(uint8_t(text->front())) };
            double value = 0;
            const auto trimmed = trim(*text);
            const auto result = parseNumber(trimmed.data(), trimmed.data() + trimmed.size(), value);
            if (trimmed.empty() || result.ec != std::errc{}
                || result.ptr != trimmed.data() + trimmed.size())
                return unknown();
            return { value };
        }
        if (c == TokenPeek) {
            ++m_at;
            const auto address = factor();
            if (!address.known || address.low || address.high)
                return unknown();
            if (address.constant == 23635)
                return { 0, 1, 0 };
            if (address.constant == 23636)
                return { 0, 0, 1 };
            return unknown();
        }
        if (const auto value = number())
            return { *value };
        return unknown();
    }

private:
    static UsrAddress unknown()
    {
        return { 0, 0, 0, false };
    }
    static std::string_view trim(std::string_view text)
    {
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);
        while (!text.empty() && text.back() == ' ')
            text.remove_suffix(1);
        return text;
    }
    void skipSpaces()
    {
        while (m_at < m_body.size() && m_body[m_at] == ' ')
            ++m_at;
    }
    std::optional<std::string> stringLiteral()
    {
        skipSpaces();
        if (m_at >= m_body.size() || m_body[m_at] != '"')
            return std::nullopt;
        std::string text;
        for (++m_at; m_at < m_body.size(); ++m_at) {
            if (m_body[m_at] == '"') {
                ++m_at;
                return text;
            }
            text += char(m_body[m_at]);
        }
        return std::nullopt;
    }
    // Digits and the number the program stores after them, which is the one it uses
    std::optional<double> number()
    {
        while (m_at < m_body.size()
               && (digit(char(m_body[m_at])) || m_body[m_at] == '.' || m_body[m_at] == 'E'
                   || m_body[m_at] == 'e'))
            ++m_at;
        if (m_at >= m_body.size() || m_body[m_at] != 14 || m_body.size() - m_at < 6)
            return std::nullopt;
        const auto value = decodeNumber(std::span<const uint8_t, 5>(m_body.data() + m_at + 1, 5));
        m_at += 6;
        return value;
    }
    UsrAddress expression()
    {
        auto sum = term();
        for (;;) {
            skipSpaces();
            if (m_at >= m_body.size() || (m_body[m_at] != '+' && m_body[m_at] != '-'))
                return sum;
            const double sign = m_body[m_at++] == '+' ? 1 : -1;
            const auto next = term();
            sum.known = sum.known && next.known;
            sum.constant += sign * next.constant;
            sum.low += sign * next.low;
            sum.high += sign * next.high;
        }
    }
    UsrAddress term()
    {
        auto product = factor();
        for (;;) {
            skipSpaces();
            if (m_at >= m_body.size() || m_body[m_at] != '*')
                return product;
            ++m_at;
            const auto next = factor();
            // Only a constant can multiply: the result stays a sum of PEEKs
            const bool constantLeft = !product.low && !product.high;
            const bool constantRight = !next.low && !next.high;
            if (!product.known || !next.known || (!constantLeft && !constantRight))
                return unknown();
            const auto &scale = constantLeft ? product : next;
            const auto &value = constantLeft ? next : product;
            product = { value.constant * scale.constant, value.low * scale.constant,
                        value.high * scale.constant };
        }
    }

    std::span<const uint8_t> m_body;
    std::size_t m_at;
};

// A program line: where its body is, where a REM's text starts, and the first byte BASIC
// can't hold outside a REM (a control code no colour, AT or TAB is), or npos
struct LineLayout
{
    std::size_t body = 0;
    std::size_t size = 0;
    std::size_t remText = std::string_view::npos;
    std::size_t unlistable = std::string_view::npos;
};

std::vector<LineLayout> lineLayouts(std::span<const uint8_t> data)
{
    std::vector<LineLayout> out;
    for (std::size_t p = 0; programLine(data, p); p += 4 + word(data, p + 2)) {
        auto &line = out.emplace_back(LineLayout{ p + 4, std::size_t(word(data, p + 2)) - 1 });
        const auto body = slice(data, line.body, line.size);
        bool quoted = false;
        for (std::size_t j = 0; j < body.size(); ++j) {
            const auto c = body[j];
            if (c == '"')
                quoted = !quoted;
            if (!quoted && c == TokenRem) {
                line.remText = j + 1;
                break;
            }
            if (!quoted && c == 14 && body.size() - j >= 6) {
                j += 5;
            } else if (c >= 16 && c <= 23) {
                j += c >= 22 ? 2 : 1;
            } else if (c < 32) {
                line.unlistable = j;
                break;
            }
        }
    }
    return out;
}

// Whether code can start at `offset` in the program: in a REM's text, or in a line BASIC can't
// hold, before its first such byte (code runs from where the USR lands)
bool codeAt(const std::vector<LineLayout> &lines, std::size_t offset)
{
    for (const auto &line : lines) {
        if (offset < line.body || offset >= line.body + line.size)
            continue;
        const auto at = offset - line.body;
        return (line.remText != std::string_view::npos && at >= line.remText)
                || (line.unlistable != std::string_view::npos && at <= line.unlistable);
    }
    return false;
}

// Where a program runs, and the offsets its USR calls land on in it
struct CodeCalls
{
    std::size_t start = BasicStart;
    std::vector<std::size_t> offsets;
};

CodeCalls codeCalls(std::span<const uint8_t> data)
{
    const auto lines = lineLayouts(data);
    std::vector<UsrAddress> calls;
    // LOAD *, SAVE *, CAT *...: Interface 1's syntax, which needs its channels
    bool channels = false;
    for (const auto &line : lines) {
        const auto body = slice(data, line.body, line.size);
        bool quoted = false;
        const auto end = std::min(body.size(), line.remText);
        for (std::size_t j = 0; j < end; ++j) {
            const auto c = body[j];
            if (c == '"')
                quoted = !quoted;
            if (quoted)
                continue;
            if (c == 14 && body.size() - j >= 6) {
                j += 5;
            } else if (c == 0xEF || c == 0xF8 || c == 0xD6 || c == 0xD5 || c == 0xCF || c == 0xD0
                       || c == 0xD1 || c == 0xD2) {
                auto next = j + 1;
                while (next < end && body[next] == ' ')
                    ++next;
                channels = channels || (next < end && body[next] == '*');
            } else if (c == TokenUsr) {
                const auto address = UsrParser(body, j + 1).factor();
                if (address.known
                    && ((!address.low && !address.high)
                        || (address.low == 1 && address.high == 256)))
                    calls.push_back(address);
            }
        }
    }
    // The start that puts the most constant addresses on code; with as many, Interface 1's
    // syntax says the channels are there
    CodeCalls out;
    const auto programEnd = lines.empty() ? 0 : lines.back().body + lines.back().size + 1;
    const auto landings = [&](std::size_t start) {
        return std::ranges::count_if(calls, [&](const UsrAddress &call) {
            return !call.low && call.constant >= double(start)
                    && call.constant < double(start + programEnd)
                    && codeAt(lines, std::size_t(call.constant) - start);
        });
    };
    const auto afterChannels = landings(ChannelsStart);
    const auto plain = landings(BasicStart);
    if (afterChannels > plain || (afterChannels == plain && channels))
        out.start = ChannelsStart;
    for (const auto &call : calls) {
        // PROG + n lands at n, wherever the program is
        const double offset = call.low ? call.constant : call.constant - double(out.start);
        if (offset >= 0 && offset < double(programEnd) && offset == std::trunc(offset)
            && std::ranges::find(out.offsets, std::size_t(offset)) == out.offsets.end())
            out.offsets.push_back(std::size_t(offset));
    }
    return out;
}

// INK to OVER with their value, AT with its line and column, TAB with its column
std::string controlText(std::span<const uint8_t> code)
{
    static constexpr std::array names = { "INK"sv,    "PAPER"sv,   "FLASH"sv,
                                          "BRIGHT"sv, "INVERSE"sv, "OVER"sv };
    if (code[0] == 22)
        return std::format("{{AT {},{}}}", code[1], code[2]);
    if (code[0] == 23)
        return std::format("{{TAB {}}}", code[1] | (code[2] << 8));
    return std::format("{{{} {}}}", names[code[0] - 16], code[1]);
}
} // namespace

std::vector<ListingLine> listingLines(std::span<const uint8_t> data, BasicDialect dialect)
{
    // The ROM's block graphics, by quadrant bits: 1 top right, 2 top left, 4 bottom right,
    // 8 bottom left
    static constexpr std::array<std::string_view, 16> blocks = { " "sv,
                                                                 "\xE2\x96\x9D"sv,
                                                                 "\xE2\x96\x98"sv,
                                                                 "\xE2\x96\x80"sv,
                                                                 "\xE2\x96\x97"sv,
                                                                 "\xE2\x96\x90"sv,
                                                                 "\xE2\x96\x9A"sv,
                                                                 "\xE2\x96\x9C"sv,
                                                                 "\xE2\x96\x96"sv,
                                                                 "\xE2\x96\x9E"sv,
                                                                 "\xE2\x96\x8C"sv,
                                                                 "\xE2\x96\x9B"sv,
                                                                 "\xE2\x96\x84"sv,
                                                                 "\xE2\x96\x9F"sv,
                                                                 "\xE2\x96\x99"sv,
                                                                 "\xE2\x96\x88"sv };
    const unsigned firstToken = dialect == BasicDialect::Spectrum48 ? 0xA5
            : dialect == BasicDialect::Spectrum128                  ? 0xA3
                                                                    : 0x81;
    const auto wordChar = [](uint8_t c) { return alpha(char(c)) || digit(char(c)) || c == '$'; };
    const auto calls = codeCalls(data);
    const auto layouts = lineLayouts(data);
    std::vector<ListingLine> out;
    // Unlike LIST, any line order: protections repeat line numbers, or use ones above 9999
    for (std::size_t p = 0; programLine(data, p); p += 4 + word(data, p + 2)) {
        const unsigned number = (data[p] << 8) | data[p + 1];
        const std::size_t size = word(data, p + 2);
        const auto address = calls.start + p + 4;
        const auto &layout = layouts[out.size()];
        const auto called = [&](std::size_t j) {
            return std::ranges::find(calls.offsets, p + 4 + j) != calls.offsets.end();
        };
        auto &line = out.emplace_back();
        // Joins a part to the previous one of the same kind
        const auto add = [&line](ListingPart::Kind kind, std::string_view text) {
            if (!line.empty() && line.back().kind == kind)
                line.back().text += text;
            else
                line.push_back({ kind, std::string(text) });
        };
        const auto last = [&line]() -> char {
            return line.empty() || line.back().text.empty() ? ' ' : line.back().text.back();
        };
        add(ListingPart::Kind::LineNumber, std::format("{:>4} ", number));
        const auto body = slice(data, p + 4, size - 1);
        bool quoted = false;
        bool rem = false;
        bool binary = false;
        for (std::size_t j = 0; j < body.size(); ++j) {
            auto c = body[j];
            // Machine code: in a REM from where a USR lands or from the first byte no text has; in
            // a line BASIC can't hold from where a USR lands
            if ((rem && (called(j) || (c < 32 && (c < 16 || c > 23))))
                || (!rem && called(j) && layout.unlistable != std::string_view::npos
                    && j <= layout.unlistable)) {
                if (last() != ' ')
                    add(ListingPart::Kind::Text, " "sv);
                add(ListingPart::Kind::Code,
                    std::format("{{machine code: {} bytes at {}}}", body.size() - j, address + j));
                break;
            }
            if (!quoted && !rem && c == 14 && body.size() - j >= 6) {
                // The value the program uses: shown when the digits before it say otherwise
                // (a common protection), and when there are no digits at all
                const std::span<const uint8_t, 5> stored(body.data() + j + 1, 5);
                const auto value = numberText(decodeNumber(stored));
                if (!line.empty() && line.back().kind == ListingPart::Kind::Number) {
                    auto &number = line.back();
                    if (!listsStoredValue(number.text, binary, stored)) {
                        number.listed = std::move(number.text);
                        number.text = value;
                    }
                } else {
                    if (wordChar(uint8_t(last())))
                        add(ListingPart::Kind::Text, " "sv);
                    line.push_back({ ListingPart::Kind::Number, value, std::string() });
                }
                binary = false;
                j += 5;
                continue;
            }
            // Colour, AT and TAB controls, with their values
            if (c >= 16 && c <= 23) {
                const std::size_t values = c >= 22 ? 2 : 1;
                if (body.size() - j > values) {
                    add(ListingPart::Kind::Control, controlText(body.subspan(j, values + 1)));
                    j += values;
                    continue;
                }
            }
            const bool quote = c == '"' && !rem;
            if (!quoted && !rem && c >= firstToken) {
                const auto keyword = keywords[c - 0x81];
                // Statements and operators are spaced out; functions only from words
                const bool statement = c >= 0xC5 || c < 0xA5;
                const bool next = j + 1 < body.size();
                const bool nextWord =
                        next && (wordChar(body[j + 1]) || (!quoted && body[j + 1] >= firstToken));
                if (alpha(keyword.front()) && last() != ' '
                    && (statement || wordChar(uint8_t(last()))))
                    add(ListingPart::Kind::Text, " "sv);
                add(ListingPart::Kind::Keyword, keyword);
                rem = c == 0xEA;
                binary = c == 0xC4;
                if ((alpha(keyword.back()) || keyword.back() == '$') && next && body[j + 1] != ' '
                    && (statement || nextWord))
                    add(ListingPart::Kind::Text, " "sv);
                continue;
            }
            auto kind = rem           ? ListingPart::Kind::Comment
                    : quoted || quote ? ListingPart::Kind::String
                                      : ListingPart::Kind::Text;
            // Numbers start outside names and go on with digits, points and exponents
            if (kind == ListingPart::Kind::Text) {
                const bool continues = !line.empty()
                        && line.back().kind == ListingPart::Kind::Number
                        && (digit(char(c)) || c == '.' || c == 'e' || c == 'E');
                if (continues
                    || ((digit(char(c))
                         || (c == '.' && j + 1 < body.size() && digit(char(body[j + 1]))))
                        && !wordChar(uint8_t(last()))))
                    kind = ListingPart::Kind::Number;
            }
            if (quote)
                quoted = !quoted;
            if (c >= 0x80 && c <= 0x8F)
                add(kind, blocks[c - 0x80]);
            else if (c >= 0x90 && c < firstToken)
                add(kind, std::format("\\{:c}", char('A' + (c - 0x90))));
            else if (c == 0x60)
                add(kind, "\xC2\xA3"sv); // £
            else if (c == 0x7F)
                add(kind, "\xC2\xA9"sv); // ©
            else if (c >= 32 && c < 127)
                add(kind, std::string_view(reinterpret_cast<const char *>(&c), 1));
            else
                add(kind, escaped(c));
        }
    }
    return out;
}

uint16_t programStart(std::span<const uint8_t> data)
{
    return uint16_t(codeCalls(data).start);
}

std::vector<UsrCall> usrCalls(std::span<const uint8_t> data)
{
    const auto calls = codeCalls(data);
    const auto lines = lineLayouts(data);
    std::vector<UsrCall> out;
    for (const auto offset : calls.offsets) {
        const auto line = std::ranges::find_if(lines, [offset](const LineLayout &l) {
            return offset >= l.body && offset < l.body + l.size;
        });
        // In a line, or in a line's number or length
        const auto end = line != lines.end() ? line->body + line->size : offset;
        out.push_back({ uint16_t(calls.start + offset), end - offset });
    }
    return out;
}

std::string listing(std::span<const uint8_t> data, BasicDialect dialect)
{
    std::string out;
    for (const auto &line : listingLines(data, dialect)) {
        for (const auto &part : line)
            if (part.kind != ListingPart::Kind::Control)
                out += part.text;
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
