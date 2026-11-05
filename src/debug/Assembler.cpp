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

#include "zxkit/debug/Assembler.h"

#include "zxkit/core/Exception.h"
#include "zxkit/debug/Disassembler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>

namespace zxkit {

using namespace std::literals;

namespace {

// What an operand placeholder of a template stands for
enum class Kind : uint8_t {
    Byte,          // $nn
    Word,          // $nnnn, little endian
    WordBigEndian, // $nnnn, big endian (the Z80N's PUSH nn)
    Displacement,  // +$nn / -$nn in (IX+d), possibly empty: (IX)
    Relative,      // the target address of JR / DJNZ
    Literal        // a number that is part of the instruction (RST $38)
};

struct Operand {
    Kind kind = Kind::Byte;
    uint8_t position = 0; // of its byte(s) in the instruction
    uint16_t literal = 0;
};

// An instruction as the Disassembler prints it, its operands replaced by the
// placeholder characters 1, 2, 3 (the index in `operands` plus one)
struct Template {
    std::string text;
    std::array<uint8_t, 4> bytes{};
    uint8_t length = 0;
    std::vector<Operand> operands;
    bool z80nOnly = false;
};

constexpr std::array<uint8_t, 2> kProbe = {0x12, 0x34};
constexpr std::array<uint8_t, 2> kCheckProbe = {0x9A, 0x78}; // a negative displacement too
constexpr uint16_t kProbePc = 0x8000;

bool isPlaceholder(char c) noexcept
{
    return c >= 1 && c <= 3;
}

DisasmInstruction decode(const std::array<uint8_t, 4> &bytes, bool z80n)
{
    return Disassembler::disassembleOne(
        [&bytes](uint16_t address) { return bytes[(address - kProbePc) & 3]; }, kProbePc, z80n);
}

// The instruction bytes `head` followed by operand bytes from `probe` (for DD CB d op the
// displacement comes before the opcode: `ddcbOp`)
std::array<uint8_t, 4> encoding(std::span<const uint8_t> head, std::optional<uint8_t> ddcbOp,
                                const std::array<uint8_t, 2> &probe)
{
    std::array<uint8_t, 4> bytes{};
    std::ranges::copy(head, bytes.begin());
    if (ddcbOp) {
        bytes[2] = probe[0];
        bytes[3] = *ddcbOp;
    } else {
        for (std::size_t i = head.size(), p = 0; i < bytes.size() && p < probe.size(); ++i, ++p)
            bytes[i] = probe[p];
    }
    return bytes;
}

std::string render(const Template &t, const std::array<uint8_t, 4> &bytes)
{
    std::string out;
    for (const char c : t.text) {
        if (!isPlaceholder(c)) {
            out.push_back(c);
            continue;
        }
        const Operand &op = t.operands[std::size_t(c - 1)];
        const uint8_t b0 = bytes[op.position];
        const uint8_t b1 = op.position + 1 < 4 ? bytes[op.position + 1] : 0;
        switch (op.kind) {
        case Kind::Byte: out += std::format("${:02X}", b0); break;
        case Kind::Word: out += std::format("${:04X}", b0 | (b1 << 8)); break;
        case Kind::WordBigEndian: out += std::format("${:04X}", (b0 << 8) | b1); break;
        case Kind::Displacement: {
            const auto d = static_cast<int8_t>(b0);
            out += std::format("{}${:02X}", d < 0 ? '-' : '+', d < 0 ? -d : d);
            break;
        }
        case Kind::Relative:
            out += std::format("${:04X}", static_cast<uint16_t>(kProbePc + t.length + static_cast<int8_t>(b0)));
            break;
        case Kind::Literal: out += std::format("${:02X}", op.literal); break;
        }
    }
    return out;
}

// Turns the instruction starting with `head` into a template: decodes it with probe
// operand bytes, finds them in the text, and checks the result with other operand bytes.
std::optional<Template> makeTemplate(std::span<const uint8_t> head, std::optional<uint8_t> ddcbOp)
{
    const auto bytes = encoding(head, ddcbOp, kProbe);
    const DisasmInstruction inst = decode(bytes, true);
    if (inst.mnemonic.starts_with("DEFB"))
        return std::nullopt;

    Template t;
    t.text = inst.mnemonic;
    t.length = inst.length;
    t.bytes = bytes;
    t.z80nOnly = head[0] == 0xED && decode(bytes, false).mnemonic != inst.mnemonic;

    std::vector<uint8_t> positions;
    if (ddcbOp)
        positions.push_back(2);
    else
        for (std::size_t i = head.size(); i < t.length; ++i)
            positions.push_back(static_cast<uint8_t>(i));

    const auto replace = [&t](const std::string &what, Operand operand) {
        const auto at = t.text.find(what);
        if (at == std::string::npos)
            return false;
        t.operands.push_back(operand);
        t.text.replace(at, what.size(), 1, static_cast<char>(t.operands.size()));
        return true;
    };

    for (std::size_t i = 0; i < positions.size(); ++i) {
        const uint8_t pos = positions[i];
        const uint8_t b0 = bytes[pos];
        if (i + 1 < positions.size()) {
            const uint8_t b1 = bytes[pos + 1];
            if (replace(std::format("${:04X}", b0 | (b1 << 8)), {Kind::Word, pos})
                || replace(std::format("${:04X}", (b0 << 8) | b1), {Kind::WordBigEndian, pos})) {
                ++i;
                continue;
            }
        }
        const auto target = static_cast<uint16_t>(kProbePc + t.length + static_cast<int8_t>(b0));
        if (!replace(std::format("+${:02X}", b0), {Kind::Displacement, pos})
            && !replace(std::format("${:04X}", target), {Kind::Relative, pos})
            && !replace(std::format("${:02X}", b0), {Kind::Byte, pos}))
            return std::nullopt;
    }
    // The numbers left are part of the instruction
    for (auto at = t.text.find('$'); at != std::string::npos; at = t.text.find('$')) {
        std::size_t end = at + 1;
        while (end < t.text.size() && std::isxdigit(static_cast<unsigned char>(t.text[end])))
            ++end;
        const auto value = static_cast<uint16_t>(std::stoul(t.text.substr(at + 1, end - at - 1), nullptr, 16));
        t.operands.push_back({Kind::Literal, 0, value});
        t.text.replace(at, end - at, 1, static_cast<char>(t.operands.size()));
    }
    for (auto &b : t.bytes) // the operand bytes are filled in when assembling
        b = &b - t.bytes.data() < t.length ? b : 0;
    for (const auto &op : t.operands) {
        if (op.kind == Kind::Literal)
            continue;
        t.bytes[op.position] = 0;
        if (op.kind == Kind::Word || op.kind == Kind::WordBigEndian)
            t.bytes[op.position + 1] = 0;
    }

    // The same instruction with other operand bytes must print as the template says
    const auto check = encoding(head, ddcbOp, kCheckProbe);
    if (render(t, check) != decode(check, true).mnemonic)
        return std::nullopt;
    return t;
}

// Every instruction the Disassembler knows. When several encodings print the same, the
// first one wins, so the documented ones come first.
std::vector<Template> buildTemplates()
{
    std::vector<Template> templates;
    std::unordered_set<std::string> seen;
    const auto add = [&](std::initializer_list<uint8_t> head, std::optional<uint8_t> ddcbOp = std::nullopt) {
        auto t = makeTemplate(std::span(head.begin(), head.size()), ddcbOp);
        if (!t)
            return;
        std::string key = t->text; // RST's numbers are not in the text
        for (const auto &op : t->operands)
            if (op.kind == Kind::Literal)
                key += std::format("#{}", op.literal);
        if (seen.insert(std::move(key)).second)
            templates.push_back(std::move(*t));
    };
    const auto isPrefix = [](unsigned op) { return op == 0xCB || op == 0xDD || op == 0xED || op == 0xFD; };

    for (unsigned op = 0; op < 256; ++op)
        if (!isPrefix(op))
            add({uint8_t(op)});
    for (unsigned op = 0; op < 256; ++op)
        add({0xCB, uint8_t(op)});
    for (unsigned op = 0; op < 256; ++op)
        add({0xED, uint8_t(op)});
    for (const uint8_t prefix : {uint8_t(0xDD), uint8_t(0xFD)}) {
        for (unsigned op = 0; op < 256; ++op)
            if (!isPrefix(op))
                add({prefix, uint8_t(op)});
        // (IX+d) itself first: the forms copying the result to a register come after
        for (unsigned op = 6; op < 256; op += 8)
            add({prefix, 0xCB}, uint8_t(op));
        for (unsigned op = 0; op < 256; ++op)
            add({prefix, 0xCB}, uint8_t(op));
    }
    return templates;
}

const std::vector<Template> &templates()
{
    // Constant data, the same for every emulator: built once, then only read
    static const std::vector<Template> table = buildTemplates();
    return table;
}

std::optional<int32_t> parseNumber(std::string_view s, int defaultBase)
{
    bool negative = false;
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    int base = defaultBase;
    if (s.starts_with('$') || s.starts_with('#')) {
        base = 16;
        s.remove_prefix(1);
    } else if (s.starts_with("0X")) {
        base = 16;
        s.remove_prefix(2);
    } else if (s.starts_with('%')) {
        base = 2;
        s.remove_prefix(1);
    } else if (defaultBase != 16 && s.starts_with("0B") && s.size() > 2) {
        base = 2;
        s.remove_prefix(2);
    } else if (s.size() > 1 && s.ends_with('H') && std::isdigit(static_cast<unsigned char>(s.front()))) {
        base = 16;
        s.remove_suffix(1);
    } else if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front()))) {
        return std::nullopt; // a register name, not a number
    }
    if (s.empty() || s.size() > 16)
        return std::nullopt;
    int32_t value = 0;
    for (const char c : s) {
        int digit = 0;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return std::nullopt;
        if (digit >= base)
            return std::nullopt;
        value = value * base + digit;
        if (value > 0xFFFFF)
            return std::nullopt;
    }
    return negative ? -value : value;
}

// Upper case, no comment, one space after the mnemonic, none anywhere else
std::string normalize(std::string_view line)
{
    if (const auto comment = line.find(';'); comment != std::string_view::npos)
        line = line.substr(0, comment);
    std::string out;
    bool mnemonicDone = false;
    bool pendingSpace = false;
    for (const char ch : line) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!out.empty() && !mnemonicDone) {
                mnemonicDone = true;
                pendingSpace = true;
            }
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

// Other common spellings of the same instruction
std::vector<std::string> spellings(const std::string &line)
{
    std::vector<std::string> out{line};
    const auto space = line.find(' ');
    const std::string op = line.substr(0, space);
    const std::string args = space == std::string::npos ? std::string{} : line.substr(space + 1);
    const bool oneOperand = !args.empty() && args.find(',') == std::string::npos;
    if ((op == "SUB" || op == "AND" || op == "XOR" || op == "OR" || op == "CP") && args.starts_with("A,"))
        out.push_back(op + ' ' + args.substr(2));
    if ((op == "ADD" || op == "ADC" || op == "SBC") && oneOperand)
        out.push_back(op + " A," + args);
    if (line == "EX AF,AF")
        out.emplace_back("EX AF,AF'");
    if (op == "JP" && (args == "HL" || args == "IX" || args == "IY"))
        out.push_back("JP (" + args + ')');
    if (line == "IN F,(C)")
        out.emplace_back("IN (C)");
    if (op == "SLI" || op == "SL1")
        out.push_back("SLL " + args);
    if (line == "MUL")
        out.emplace_back("MUL D,E");
    if (line == "MIRROR")
        out.emplace_back("MIRROR A");
    return out;
}

enum class Match { No, Yes, BadValue };

Match match(const Template &t, std::string_view input, uint16_t pc, int defaultBase, std::vector<uint8_t> &out,
            std::string &error)
{
    std::array<uint8_t, 4> bytes = t.bytes;
    std::size_t j = 0;
    for (std::size_t i = 0; i < t.text.size(); ++i) {
        const char c = t.text[i];
        if (!isPlaceholder(c)) {
            if (j >= input.size() || input[j] != c)
                return Match::No;
            ++j;
            continue;
        }
        // The operand runs up to the template's next character
        const char stop = i + 1 < t.text.size() ? t.text[i + 1] : '\0';
        const std::size_t end = stop ? input.find(stop, j) : input.size();
        if (end == std::string_view::npos)
            return Match::No;
        const std::string_view token = input.substr(j, end - j);
        j = end;

        const Operand &op = t.operands[std::size_t(c - 1)];
        if (op.kind == Kind::Displacement && token.empty())
            continue; // (IX)
        if (op.kind == Kind::Displacement && !token.starts_with('+') && !token.starts_with('-'))
            return Match::No;
        const auto value = parseNumber(token, defaultBase);
        if (!value)
            return Match::No;
        switch (op.kind) {
        case Kind::Byte:
            if (*value < -128 || *value > 255) {
                error = std::format("{} doesn't fit in a byte", token);
                return Match::BadValue;
            }
            bytes[op.position] = static_cast<uint8_t>(*value);
            break;
        case Kind::Word:
        case Kind::WordBigEndian: {
            if (*value < -32768 || *value > 65535) {
                error = std::format("{} doesn't fit in 16 bits", token);
                return Match::BadValue;
            }
            const auto word = static_cast<uint16_t>(*value);
            const bool big = op.kind == Kind::WordBigEndian;
            bytes[op.position] = static_cast<uint8_t>(big ? word >> 8 : word);
            bytes[op.position + 1] = static_cast<uint8_t>(big ? word : word >> 8);
            break;
        }
        case Kind::Displacement:
            if (*value < -128 || *value > 127) {
                error = std::format("Displacement {} is out of range (-128..127)", token);
                return Match::BadValue;
            }
            bytes[op.position] = static_cast<uint8_t>(*value);
            break;
        case Kind::Relative: {
            if (*value < 0 || *value > 0xFFFF) {
                error = std::format("Address {} is out of range", token);
                return Match::BadValue;
            }
            const int32_t distance = *value - (pc + t.length);
            if (distance < -128 || distance > 127) {
                error = std::format("Jump target {} is out of range (-128..127 bytes away)", token);
                return Match::BadValue;
            }
            bytes[op.position] = static_cast<uint8_t>(distance);
            break;
        }
        case Kind::Literal:
            if (*value != op.literal)
                return Match::No;
            break;
        }
    }
    if (j != input.size())
        return Match::No;
    out.assign(bytes.begin(), bytes.begin() + t.length);
    return Match::Yes;
}

std::expected<std::vector<uint8_t>, std::string> defineBytes(std::string_view args, int defaultBase)
{
    std::vector<uint8_t> out;
    while (!args.empty()) {
        const auto comma = args.find(',');
        const std::string_view token = args.substr(0, comma);
        const auto value = parseNumber(token, defaultBase);
        if (!value || *value < -128 || *value > 255)
            return std::unexpected(std::format("'{}' is not a byte", token));
        out.push_back(static_cast<uint8_t>(*value));
        args = comma == std::string_view::npos ? std::string_view{} : args.substr(comma + 1);
    }
    if (out.empty())
        return std::unexpected("DEFB needs at least one byte"s);
    return out;
}

} // namespace

std::expected<std::vector<uint8_t>, std::string> Assembler::assemble(std::string_view line, uint16_t pc,
                                                                     AssemblerOptions options)
{
    const std::string input = normalize(line);
    if (input.empty())
        return std::unexpected("Nothing to assemble"s);
    if (input.starts_with("DEFB ") || input.starts_with("DB "))
        return defineBytes(std::string_view(input).substr(input.find(' ') + 1), options.defaultBase);

    std::vector<uint8_t> bytes;
    std::string error;
    for (const std::string &spelling : spellings(input)) {
        for (const Template &t : templates()) {
            if (t.z80nOnly && !options.z80n)
                continue;
            switch (match(t, spelling, pc, options.defaultBase, bytes, error)) {
            case Match::Yes: return bytes;
            case Match::BadValue: return std::unexpected(error);
            case Match::No: break;
            }
        }
    }
    return std::unexpected(std::format("Unknown instruction '{}'", input));
}

std::vector<uint8_t> Assembler::assembleOne(std::string_view line, uint16_t pc, AssemblerOptions options)
{
    auto bytes = assemble(line, pc, options);
    if (!bytes)
        throw DebuggerException(bytes.error());
    return std::move(*bytes);
}

} // namespace zxkit
