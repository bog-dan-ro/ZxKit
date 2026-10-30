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

class KempstonMousePeripheral : public Peripheral {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    void move(int dx, int dy) noexcept;
    void setButton(uint8_t buttonBit, bool pressed) noexcept;

private:
    uint8_t m_x = 0;
    uint8_t m_y = 0;
    uint8_t m_buttons = 0xFF; // Active low: bit 0 = right, bit 1 = left, bit 2 = middle
};

} // namespace zxkit
