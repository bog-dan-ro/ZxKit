// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Support.h"
#include <zxkit/media/BasicCodec.h>

namespace zxkit::mcp_tool {
Object basicListing(std::span<const uint8_t> data, BasicDialect dialect);
} // namespace zxkit::mcp_tool
