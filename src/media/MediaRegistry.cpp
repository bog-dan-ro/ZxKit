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

#include "zxkit/media/MediaRegistry.h"

#include "zxkit/core/Exception.h"

#include <format>

namespace zxkit {

void MediaRegistry::registerLoader(std::string extension, LoaderFn loader)
{
    m_loaders[std::move(extension)] = std::move(loader);
}

void MediaRegistry::registerSaver(std::string extension, SaverFn saver)
{
    m_savers[std::move(extension)] = std::move(saver);
}

bool MediaRegistry::hasLoader(std::string_view extension) const noexcept
{
    return m_loaders.contains(std::string(extension));
}

bool MediaRegistry::hasSaver(std::string_view extension) const noexcept
{
    return m_savers.contains(std::string(extension));
}

void MediaRegistry::invokeLoader(std::string_view extension, Emulator &emulator, std::span<const uint8_t> data) const
{
    if (auto it = m_loaders.find(std::string(extension)); it != m_loaders.end()) {
        it->second(emulator, data);
        return;
    }
    throw FormatException(std::format("No custom loader registered for '{}'", extension));
}

std::vector<uint8_t> MediaRegistry::invokeSaver(std::string_view extension, Emulator &emulator) const
{
    if (auto it = m_savers.find(std::string(extension)); it != m_savers.end())
        return it->second(emulator);
    throw FormatException(std::format("No custom saver registered for '{}'", extension));
}

} // namespace zxkit
