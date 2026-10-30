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

#include "zxkit/cpu/Z80Registers.h"

namespace zxkit {

void Z80Registers::reset() noexcept
{
    AF = BC = DE = HL = 0xFFFF;
    AF_ = BC_ = DE_ = HL_ = 0xFFFF;
    IX = IY = SP = 0xFFFF;
    PC = WZ = 0x0000;
    I = R = R7 = 0x00;
    IFF1 = IFF2 = IM = 0;
    Q = lastQ = incDecZ = 0;
    halted = false;
    eiDelayed = false;
}

void Z80Registers::saveState(StateWriter &w) const
{
    w.write(AF);
    w.write(BC);
    w.write(DE);
    w.write(HL);
    w.write(AF_);
    w.write(BC_);
    w.write(DE_);
    w.write(HL_);
    w.write(IX);
    w.write(IY);
    w.write(SP);
    w.write(PC);
    w.write(WZ);
    w.write(I);
    w.write(R);
    w.write(R7);
    w.write(IFF1);
    w.write(IFF2);
    w.write(IM);
    w.write(Q);
    w.write(lastQ);
    w.write(incDecZ);
    w.writeBool(halted);
    w.writeBool(eiDelayed);
}

void Z80Registers::loadState(StateReader &r)
{
    AF      = r.read<uint16_t>();
    BC      = r.read<uint16_t>();
    DE      = r.read<uint16_t>();
    HL      = r.read<uint16_t>();
    AF_     = r.read<uint16_t>();
    BC_     = r.read<uint16_t>();
    DE_     = r.read<uint16_t>();
    HL_     = r.read<uint16_t>();
    IX      = r.read<uint16_t>();
    IY      = r.read<uint16_t>();
    SP      = r.read<uint16_t>();
    PC      = r.read<uint16_t>();
    WZ      = r.read<uint16_t>();
    I       = r.read<uint8_t>();
    R       = r.read<uint8_t>();
    R7      = r.read<uint8_t>();
    IFF1    = r.read<uint8_t>();
    IFF2    = r.read<uint8_t>();
    IM      = r.read<uint8_t>();
    Q       = r.read<uint8_t>();
    lastQ   = r.read<uint8_t>();
    incDecZ = r.read<uint8_t>();
    halted    = r.readBool();
    eiDelayed = r.readBool();
}

} // namespace zxkit
