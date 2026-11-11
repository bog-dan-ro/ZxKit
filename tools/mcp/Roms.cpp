// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
// ROM copyrights and distribution terms: tests/fixtures/roms/README.copyright.
#include "Support.h"
#include <iterator>

namespace zxkit::mcp_tool {
namespace {
constexpr uint8_t spectrum48[] = {
#embed "../../tests/fixtures/roms/48.rom"
};
constexpr uint8_t spectrum128First[] = {
#embed "../../tests/fixtures/roms/128-0.rom"
};
constexpr uint8_t spectrum128Second[] = {
#embed "../../tests/fixtures/roms/128-1.rom"
};
static_assert(sizeof(spectrum48) == 16384);
static_assert(sizeof(spectrum128First) == 16384);
static_assert(sizeof(spectrum128Second) == 16384);
} // namespace

std::vector<uint8_t> embeddedRom(std::string_view name)
{
    using namespace std::literals;
    if (name == "48.rom"sv)
        return { std::begin(spectrum48), std::end(spectrum48) };
    if (name == "128-0.rom"sv)
        return { std::begin(spectrum128First), std::end(spectrum128First) };
    if (name == "128-1.rom"sv)
        return { std::begin(spectrum128Second), std::end(spectrum128Second) };
    return {};
}
} // namespace zxkit::mcp_tool
