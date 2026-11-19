// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "zxkit/core/Exception.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zxkit::mdr {

inline constexpr std::size_t kHeaderSize = 15;
inline constexpr std::size_t kRecordSize = 528;
inline constexpr std::size_t kSectorSize = kHeaderSize + kRecordSize;
inline constexpr std::size_t kMaxSectors = 254;

inline std::span<const uint8_t> sectors(std::span<const uint8_t> image)
{
    using namespace std::literals;
    const auto count = image.size() / kSectorSize;
    const auto trailerSize = image.size() % kSectorSize;
    if (count == 0 || count > kMaxSectors || trailerSize > 1)
        throw FormatException(
                "Invalid MDR size: expected 1–254 sectors and an optional protection byte"sv);
    return image.first(count * kSectorSize);
}

} // namespace zxkit::mdr
