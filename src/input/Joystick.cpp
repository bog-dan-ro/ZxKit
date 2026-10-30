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

#include "zxkit/input/Joystick.h"

#include "zxkit/cpu/Z80Cpu.h"

#include <string>

namespace zxkit {

std::string_view KempstonJoystickPeripheral::id() const noexcept
{
    return m_loose ? "kempston_loose" : "kempston";
}

std::string_view KempstonJoystickPeripheral::name() const noexcept
{
    return m_loose ? "Kempston Joystick (Loose)" : "Kempston Joystick";
}

void KempstonJoystickPeripheral::onAttach(MachineContext &ctx)
{
    // The ZX Next decodes its Kempston port 0x1F fully (0x0B is the ZXN-DMA)
    const bool fullDecode = ctx.cpu.isZ80nEnabled();
    const uint16_t mask = fullDecode ? 0x00FF : (m_loose ? 0x0020 : 0x00E0);
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        mask,
        fullDecode ? 0x001F : 0x0000,
        [this](uint16_t) -> uint8_t { return m_state; },
        nullptr);
}

void KempstonJoystickPeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(id());
}

void KempstonJoystickPeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_state = 0;
}

} // namespace zxkit
