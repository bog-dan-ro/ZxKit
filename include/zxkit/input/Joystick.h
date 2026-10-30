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

#include "zxkit/bus/Peripheral.h"

#include <cstdint>

namespace zxkit {

enum class JoystickType : uint8_t {
    None = 0,
    Kempston,
    KempstonLoose,
    Sinclair1,
    Sinclair2,
    Cursor,
    Fuller,
    MegaDrive6
};

struct JoystickButtons {
    static constexpr uint8_t Right = 0x01;
    static constexpr uint8_t Left  = 0x02;
    static constexpr uint8_t Down  = 0x04;
    static constexpr uint8_t Up    = 0x08;
    static constexpr uint8_t Fire1 = 0x10;
    static constexpr uint8_t Fire2 = 0x20;
    static constexpr uint8_t Fire3 = 0x40;
    static constexpr uint8_t Start = 0x80;
};

class KempstonJoystickPeripheral : public Peripheral {
public:
    explicit KempstonJoystickPeripheral(bool looseDecode = false) noexcept
        : m_loose(looseDecode)
    {
    }

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    void setButtons(uint8_t mask) noexcept { m_state = mask; }
    uint8_t buttons() const noexcept { return m_state; }

private:
    bool m_loose = false;
    uint8_t m_state = 0;
};

} // namespace zxkit
