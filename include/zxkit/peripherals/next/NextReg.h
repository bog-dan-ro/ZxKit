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

#include "zxkit/core/StateStream.h"

#include <array>
#include <cstdint>
#include <functional>

namespace zxkit {

class NextReg
{
public:
    using ReadCallback = std::function<uint8_t(uint8_t reg, uint8_t storedVal)>;
    using WriteCallback = std::function<void(uint8_t reg, uint8_t val)>;
    using WatchCallback = std::function<void(uint8_t reg, uint8_t oldVal, uint8_t newVal)>;

    NextReg();

    void reset(bool hard = true) noexcept;

    void select(uint8_t reg) noexcept;
    uint8_t selected() const noexcept;

    uint8_t readSelected() const;
    void writeSelected(uint8_t val);

    uint8_t read(uint8_t reg) const;
    uint8_t peek(uint8_t reg) const noexcept;

    /// Direct register write (used both by port 0x253B and Z80N NEXTREG opcodes).
    /// Note: Z80N NEXTREG opcodes call write(reg, val) directly without altering m_selected.
    void write(uint8_t reg, uint8_t val);

    void setRaw(uint8_t reg, uint8_t val) noexcept;

    void setReadHandler(uint8_t reg, ReadCallback cb);
    void setWriteHandler(uint8_t reg, WriteCallback cb);
    void setWatchCallback(WatchCallback cb);

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    uint8_t m_selected = 0;
    std::array<uint8_t, 256> m_regs{};
    std::array<ReadCallback, 256> m_readCallbacks{};
    std::array<WriteCallback, 256> m_writeCallbacks{};
    WatchCallback m_watchCallback;
};

} // namespace zxkit
