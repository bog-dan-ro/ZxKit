// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <charconv>
#include <concepts>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>

namespace zxkit {
// Parses a decimal number as std::from_chars does with the general format: an optional
// '-', digits with an optional fraction, and an optional exponent. Unlike
// std::from_chars, it accepts no "inf" or "nan". Always uses the classic locale.
template <std::floating_point Number>
std::from_chars_result parseClassicNumber(const char *first, const char *last, Number &value)
{
    auto skipDigits = [last](const char *p) {
        while (p != last && *p >= '0' && *p <= '9')
            ++p;
        return p;
    };
    const char *mantissa = first;
    if (mantissa != last && *mantissa == '-')
        ++mantissa;
    const char *end = skipDigits(mantissa);
    bool hasDigits = end != mantissa;
    if (end != last && *end == '.') {
        const char *fraction = end + 1;
        end = skipDigits(fraction);
        hasDigits = hasDigits || end != fraction;
    }
    if (!hasDigits)
        return { first, std::errc::invalid_argument };
    if (end != last && (*end == 'e' || *end == 'E')) {
        const char *exponent = end + 1;
        if (exponent != last && (*exponent == '+' || *exponent == '-'))
            ++exponent;
        const char *exponentEnd = skipDigits(exponent);
        if (exponentEnd != exponent)
            end = exponentEnd;
    }

    std::istringstream stream{ std::string(first, end) };
    stream.imbue(std::locale::classic());
    Number parsed{};
    stream >> parsed;
    // The text is a valid number, so the stream fails only when it is out of range.
    if (stream.fail())
        return { end, std::errc::result_out_of_range };
    value = parsed;
    return { end, std::errc{} };
}

// std::from_chars for floating-point numbers. The libc++ in Android NDK r27c has
// only the integer overloads, so it falls back to parseClassicNumber() there.
template <std::floating_point Number>
std::from_chars_result parseNumber(const char *first, const char *last, Number &value)
{
    if constexpr (requires { std::from_chars(first, last, value); })
        return std::from_chars(first, last, value);
    else
        return parseClassicNumber(first, last, value);
}
} // namespace zxkit
