// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "zxkit/media/TapeFormats.h"
namespace zxkit::TapeConversion {
/// Exact signal export; unrepresentable control/metadata raises FormatException.
std::vector<uint8_t> writePzx(std::span<const TapeBlock> blocks);
std::vector<uint8_t> writeCsw(std::span<const TapeBlock> blocks);
/// Standard TAP blocks to unsigned 8-bit mono PCM WAV, with cumulative timing.
std::vector<uint8_t> tapToWav(std::span<const TapeBlock> blocks, uint32_t sampleRate = 44100);
} // namespace zxkit::TapeConversion
