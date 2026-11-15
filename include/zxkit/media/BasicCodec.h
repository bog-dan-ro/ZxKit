// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace zxkit {
enum class BasicDialect { Spectrum48, Spectrum128, Next };
namespace BasicCodec {
/// Numbered text lines to Spectrum BASIC bytes, including hidden numeric constants.
/// \\xHH inserts a literal byte; \\ escapes a backslash. !raw HEX preserves variable/array data.
std::vector<uint8_t> compile(std::string_view source,
                             BasicDialect dialect = BasicDialect::Spectrum48);
/// Lossless listing: escapes preserve control codes, hidden constants and stored variables.
std::string decompile(std::span<const uint8_t> data,
                      BasicDialect dialect = BasicDialect::Spectrum48);
/// A listing to read, like the ROM's LIST: spaced keywords, without colour controls or
/// variables, but unhacked. Numbers show the values the program uses: where the stored number
/// differs from the digits LIST shows (a common protection), the stored one. Lines come in any
/// order (repeated numbers, numbers above 9999). Machine code is summed up with its address,
/// at programStart(): in a REM from where a USR lands or its first byte no text has, and in a
/// line BASIC can't hold (code after a credit, as cracked loaders have) from where a USR
/// lands. Block graphics are Unicode, UDGs \A to \U. Not for compile().
std::string listing(std::span<const uint8_t> data, BasicDialect dialect = BasicDialect::Spectrum48);
/// A piece of a listing line, for highlighting. Joined, a line's parts but its controls are
/// its listing() text.
struct ListingPart
{
    enum class Kind : uint8_t {
        LineNumber, ///< right aligned, with the space after it
        Keyword,
        Number,
        String, ///< quotes included
        Comment, ///< what follows REM
        Text, ///< names, operators, punctuation and spaces
        Control, ///< embedded colour, AT and TAB codes: {INK 6}, {AT 0,0}, {TAB 8}
        Code ///< machine code in a REM or a line: {machine code: N bytes at ADDRESS}
    };
    Kind kind = Kind::Text;
    std::string text;
    /// For a number whose stored value differs from its digits: what LIST shows instead, the
    /// digits, or nothing for a stored number with none
    std::optional<std::string> listed;
};
using ListingLine = std::vector<ListingPart>;
std::vector<ListingLine> listingLines(std::span<const uint8_t> data,
                                      BasicDialect dialect = BasicDialect::Spectrum48);
/// Where the program runs: 23755, or 23813 when that start puts more of its USR calls on code,
/// or as many and the program uses Interface 1's syntax (LOAD *, SAVE *, CAT *...). 23813 has
/// Interface 1's 58 bytes of channels before the program, as disk systems such as the
/// HC-2000's do.
uint16_t programStart(std::span<const uint8_t> data);
/// Where a USR call lands inside the program, and the bytes from there to the end of its line
struct UsrCall
{
    uint16_t address = 0;
    std::size_t size = 0;
};
/// The program's USR calls that land inside it, at programStart(), in order: a number (the
/// stored value), VAL"digits", CODE"text", and PEEK 23635+256*PEEK 23636+N (the program's own
/// address plus N)
std::vector<UsrCall> usrCalls(std::span<const uint8_t> data);
std::array<uint8_t, 5> encodeNumber(double value);
double decodeNumber(std::span<const uint8_t, 5> bytes);
/// Array text: comma-separated dimensions on the first line, followed by numbers
/// or quoted, fixed-width strings. Numeric @HHHHHHHHHH values preserve raw floats.
std::vector<uint8_t> compileArray(std::string_view source, bool characterArray = false);
std::string decompileArray(std::span<const uint8_t> data, bool characterArray = false);
} // namespace BasicCodec
} // namespace zxkit
