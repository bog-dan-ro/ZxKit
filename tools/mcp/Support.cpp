// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Support.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;

int checked(int value, int minimum, int maximum, std::string_view name)
{
    if (value < minimum || value > maximum)
        throw std::invalid_argument(std::string(name) + " must be in ["s + std::to_string(minimum)
                                    + ", "s + std::to_string(maximum) + "]"s);
    return value;
}

void memoryRange(int address, int length)
{
    checked(address, 0, 65535, "address"sv);
    checked(length, 0, 65536 - address, "length"sv);
}

Bytes unhex(std::string_view text)
{
    if (text.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("Hex input exceeds 4 MiB"s);
    Bytes bytes;
    int high = -1;
    for (unsigned char character : text) {
        if (std::isspace(character))
            continue;
        int digit = character >= '0' && character <= '9' ? character - '0'
                : character >= 'a' && character <= 'f'   ? character - 'a' + 10
                : character >= 'A' && character <= 'F'   ? character - 'A' + 10
                                                         : -1;
        if (digit < 0)
            throw std::invalid_argument(
                    "Expected hexadecimal byte pairs, with optional whitespace"s);
        if (high < 0)
            high = digit;
        else {
            bytes.push_back(uint8_t(high * 16 + digit));
            high = -1;
        }
    }
    if (high >= 0)
        throw std::invalid_argument("Hex input has an incomplete byte"s);
    return bytes;
}

std::string hex(std::span<const uint8_t> bytes)
{
    constexpr auto digits = "0123456789abcdef"sv;
    std::string text;
    text.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

Bytes readFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("Cannot open input: "s + path.string());
    auto length = stream.tellg();
    if (length < 0 || length > 64 * 1024 * 1024)
        throw std::runtime_error("Input size must not exceed 64 MiB"s);
    Bytes bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char *>(bytes.data()), length))
        throw std::runtime_error("Cannot read input: "s + path.string());
    return bytes;
}

void writeFile(const std::filesystem::path &path, std::span<const uint8_t> bytes, bool overwrite)
{
    if (!overwrite && std::filesystem::exists(path))
        throw std::runtime_error("Output exists; set overwrite=true: "s + path.string());
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream
        || !stream.write(reinterpret_cast<const char *>(bytes.data()),
                         std::streamsize(bytes.size()))
        || !stream.flush())
        throw std::runtime_error("Cannot write output: "s + path.string());
}

std::string upper(std::string text)
{
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return char(std::toupper(character)); });
    return text;
}
} // namespace zxkit::mcp_tool
