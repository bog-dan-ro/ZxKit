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

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zxkit {

class Emulator;

/// Custom file formats for `Emulator::loadMedia()` / `Emulator::saveMedia()`, checked
/// before the built-in ones. A plain value: each emulator owns its own registry.
class MediaRegistry {
public:
    using LoaderFn = std::function<void(Emulator &emulator, std::span<const uint8_t> data)>;
    using SaverFn  = std::function<std::vector<uint8_t>(Emulator &emulator)>;

    void registerLoader(std::string extension, LoaderFn loader);
    void registerSaver(std::string extension, SaverFn saver);
    bool hasLoader(std::string_view extension) const noexcept;
    bool hasSaver(std::string_view extension) const noexcept;
    void invokeLoader(std::string_view extension, Emulator &emulator, std::span<const uint8_t> data) const;
    std::vector<uint8_t> invokeSaver(std::string_view extension, Emulator &emulator) const;

private:
    std::unordered_map<std::string, LoaderFn> m_loaders;
    std::unordered_map<std::string, SaverFn> m_savers;
};

} // namespace zxkit
