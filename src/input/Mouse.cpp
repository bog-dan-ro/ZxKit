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

#include "zxkit/input/Mouse.h"

#include <string>

namespace zxkit {

std::string_view KempstonMousePeripheral::id() const noexcept
{
    return "kempston_mouse";
}

std::string_view KempstonMousePeripheral::name() const noexcept
{
    return "Kempston Mouse";
}

void KempstonMousePeripheral::onAttach(MachineContext &ctx)
{
    // Buttons: port 0xFADF (mask 0x0520 == 0x0000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x0520,
        0x0000,
        [this](uint16_t) -> uint8_t { return m_buttons; },
        nullptr);
    // X axis: port 0xFBDF (mask 0x0520 == 0x0100)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x0520,
        0x0100,
        [this](uint16_t) -> uint8_t { return m_x; },
        nullptr);
    // Y axis: port 0xFFDF (mask 0x0520 == 0x0500)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()),
        0x0520,
        0x0500,
        [this](uint16_t) -> uint8_t { return m_y; },
        nullptr);
}

void KempstonMousePeripheral::onDetach(MachineContext &ctx)
{
    ctx.ioBus.unregisterOwner(id());
}

void KempstonMousePeripheral::reset(MachineContext & /*ctx*/, ResetKind)
{
    m_x = 0;
    m_y = 0;
    m_buttons = 0xFF;
}

void KempstonMousePeripheral::move(int dx, int dy) noexcept
{
    m_x = static_cast<uint8_t>(m_x + dx);
    m_y = static_cast<uint8_t>(m_y - dy);
}

void KempstonMousePeripheral::setButton(uint8_t buttonBit, bool pressed) noexcept
{
    if (pressed)
        m_buttons &= static_cast<uint8_t>(~buttonBit);
    else
        m_buttons |= buttonBit;
}

} // namespace zxkit
