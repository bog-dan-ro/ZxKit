/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Helpers for the tests that boot real ROMs from tests/fixtures: a ROM resolver, key taps and
// typing ASCII text the way zxrun does.

#pragma once

#include <zxkit/zxkit.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit::test {

inline std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open test fixture: " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

inline EmulatorConfig romConfig(const std::filesystem::path &romDir, std::string machineId)
{
    EmulatorConfig config;
    config.machineId = std::move(machineId);
    config.romResolver = [romDir](std::string_view name, std::size_t) {
        return readFile(romDir / name);
    };
    return config;
}

/// Holds the keys down for 4 frames, then releases them for 4 frames
inline void tap(Emulator &emulator, std::initializer_list<ZxKey> keys)
{
    for (ZxKey key : keys)
        emulator.setKeyState(key, true);
    emulator.runFrames(4);
    for (ZxKey key : keys)
        emulator.setKeyState(key, false);
    emulator.runFrames(4);
}

/// Types lower case letters, digits, spaces, '.' (SYMBOL SHIFT + M) and '\n' (ENTER)
inline void type(Emulator &emulator, std::string_view text)
{
    static constexpr std::array<ZxKey, 26> kLetters{
        ZxKey::A, ZxKey::B, ZxKey::C, ZxKey::D, ZxKey::E, ZxKey::F, ZxKey::G, ZxKey::H, ZxKey::I,
        ZxKey::J, ZxKey::K, ZxKey::L, ZxKey::M, ZxKey::N, ZxKey::O, ZxKey::P, ZxKey::Q, ZxKey::R,
        ZxKey::S, ZxKey::T, ZxKey::U, ZxKey::V, ZxKey::W, ZxKey::X, ZxKey::Y, ZxKey::Z
    };
    static constexpr std::array<ZxKey, 10> kDigits{ ZxKey::Num0, ZxKey::Num1, ZxKey::Num2,
                                                    ZxKey::Num3, ZxKey::Num4, ZxKey::Num5,
                                                    ZxKey::Num6, ZxKey::Num7, ZxKey::Num8,
                                                    ZxKey::Num9 };
    for (char c : text) {
        if (c >= 'a' && c <= 'z')
            tap(emulator, { kLetters[static_cast<std::size_t>(c - 'a')] });
        else if (c >= '0' && c <= '9')
            tap(emulator, { kDigits[static_cast<std::size_t>(c - '0')] });
        else if (c == ' ')
            tap(emulator, { ZxKey::Space });
        else if (c == '.')
            tap(emulator, { ZxKey::SymbolShift, ZxKey::M });
        else if (c == '+')
            tap(emulator, { ZxKey::SymbolShift, ZxKey::K });
        else if (c == '\n')
            tap(emulator, { ZxKey::Enter });
    }
}

/// The screen's text row `row` (0-23) has ink somewhere: a message is printed there
inline bool screenRowHasPixels(Emulator &emulator, uint16_t screenBase, unsigned row)
{
    const auto base = static_cast<uint16_t>(screenBase + ((row & 0x18) << 8) + ((row & 0x07) << 5));
    for (unsigned line = 0; line < 8; ++line) {
        for (unsigned column = 0; column < 32; ++column) {
            if (emulator.mmu().peek(static_cast<uint16_t>(base + line * 0x100 + column)) != 0)
                return true;
        }
    }
    return false;
}

} // namespace zxkit::test
