// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Support.h"

namespace zxkit::mcp_tool {
Object download(std::string url, const std::filesystem::path &output,
                std::string_view expectedSha256, int milliseconds, bool overwrite,
                uint64_t maximumBytes = 512 * 1024 * 1024);
std::string resolveUrl(std::string_view base, std::string_view location);
std::filesystem::path publishFile(const std::filesystem::path &temporary,
                                  const std::filesystem::path &output, bool overwrite);
} // namespace zxkit::mcp_tool
