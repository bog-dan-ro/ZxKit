// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
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
std::array<uint8_t, 5> encodeNumber(double value);
double decodeNumber(std::span<const uint8_t, 5> bytes);
/// Array text: comma-separated dimensions on the first line, followed by numbers
/// or quoted, fixed-width strings. Numeric @HHHHHHHHHH values preserve raw floats.
std::vector<uint8_t> compileArray(std::string_view source, bool characterArray = false);
std::string decompileArray(std::span<const uint8_t> data, bool characterArray = false);
} // namespace BasicCodec
} // namespace zxkit
