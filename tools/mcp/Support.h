// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit::mcp_tool {
using Json = boost::json::value;
using Object = boost::json::object;
using Array = boost::json::array;
using Bytes = std::vector<uint8_t>;

int checked(int value, int minimum, int maximum, std::string_view name);
void memoryRange(int address, int length);
Bytes unhex(std::string_view text);
std::string hex(std::span<const uint8_t> bytes);
Bytes readFile(const std::filesystem::path &path);
void writeFile(const std::filesystem::path &path, std::span<const uint8_t> bytes, bool overwrite);
std::vector<uint8_t> embeddedRom(std::string_view name);
std::string upper(std::string text);
} // namespace zxkit::mcp_tool
