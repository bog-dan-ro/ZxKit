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

#include "zxkit/bus/Peripheral.h"

#include <format>

namespace zxkit {

std::vector<uint8_t> MachineContext::loadRom(std::string_view romName, std::size_t expectedSize) const
{
    auto data = romOverrideResolver ? romOverrideResolver(romName, expectedSize) : std::vector<uint8_t>{};
    if (data.empty()) {
        if (!romResolver)
            throw RomMissingException(std::format("No ROM resolver configured while requesting '{}'", romName));
        data = romResolver(romName, expectedSize);
    }
    if (expectedSize != 0 && data.size() != expectedSize) {
        throw RomMissingException(std::format("ROM '{}' size mismatch: expected {} bytes, got {}",
                                              romName, expectedSize, data.size()));
    }
    return data;
}

} // namespace zxkit
