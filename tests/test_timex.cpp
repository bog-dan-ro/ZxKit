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

// The Timex TC2048 / TC2068 / TS2068 and the ZX Spectrum SE: the SCLD's paging, the AY ports and
// the SE's 128K paging through the ports, then each machine's ROM boot (tests/fixtures/roms).

#include "RomTestSupport.h"

#include <cassert>
#include <print>

using namespace zxkit;
using namespace zxkit::test;

namespace {

MemoryBank &bank(Emulator &emulator, std::string_view name)
{
    return emulator.mmu().bank(emulator.mmu().findBank(name));
}

void testTc2068Scld()
{
    Emulator emulator({.machineId = "tc2068"});
    auto &io = emulator.ioBus();
    auto &mmu = emulator.mmu();
    bank(emulator, "ram").span16k(0)[0x0010] = 0x11; // 0x4010 in the home bank
    // The EXROM: one 8K page, seen in every chunk (no ROM loaded: write a marker into the bank)
    bank(emulator, "exrom").span8k(0)[0x0010] = 0xE1;

    assert(mmu.read(0x4010) == 0x11);
    // A chunk selected with bit 7 of 0xFF clear: the DOCK, empty (0xFF)
    io.write(0x00F4, 0x04);
    assert(mmu.read(0x4010) == 0xFF);
    // ... with bit 7 set: the EXROM, in any chunk
    io.write(0x00FF, 0x80);
    assert(mmu.read(0x4010) == 0xE1);
    io.write(0x00F4, 0x81);
    assert(mmu.read(0x0010) == 0xE1 && mmu.read(0xE010) == 0xE1 && mmu.read(0x4010) == 0x11);
    // Read-only
    mmu.write(0x0010, 0x00);
    assert(mmu.read(0x0010) == 0xE1);
    assert(io.read(0x00F4) == 0x81 && io.read(0x00FF) == 0x80);
    io.write(0x00F4, 0x00);
    assert(mmu.read(0x0010) != 0xE1);

    // The AY on 0xF5/0xF6. Register 14 is the joystick port: an input reads 0xFF (no joystick),
    // an output (register 7 bit 6) its latch
    io.write(0x00F5, 0x01);
    io.write(0x00F6, 0x0A);
    assert(io.read(0x00F6) == 0x0A && io.read(0x00F5) == 0x0A);
    io.write(0x00F5, 0x0E);
    io.write(0x00F6, 0x5A);
    assert(io.read(0x00F5) == 0xFF && io.read(0x00F6) == 0xFF);
    io.write(0x00F5, 0x07);
    io.write(0x00F6, 0x40);
    io.write(0x00F5, 0x0E);
    assert(io.read(0x00F6) == 0x5A);
}

void testScldInterruptDisable()
{
    // EI ; HALT at 0x8000: bit 6 of 0xFF keeps the frame interrupt away
    for (const bool disabled : {true, false}) {
        Emulator emulator({.machineId = "tc2048"});
        emulator.mmu().write(0x8000, 0xFB);
        emulator.mmu().write(0x8001, 0x76);
        emulator.ioBus().write(0x00FF, disabled ? 0x40 : 0x00);
        emulator.cpu().registers().PC = 0x8000;
        emulator.cpu().registers().SP = 0xFF00;
        emulator.runFrames(2);
        const auto &regs = emulator.cpu().registers();
        assert(regs.halted == disabled);
    }
}

void testSePaging()
{
    Emulator emulator({.machineId = "se"});
    auto &io = emulator.ioBus();
    auto &mmu = emulator.mmu();
    auto &ram = bank(emulator, "ram");
    for (std::size_t page = 0; page < 9; ++page)
        ram.span16k(page)[0] = static_cast<uint8_t>(0xA0 + page);

    // The home bank: ROM, RAM 5, RAM 8, RAM 0
    assert(mmu.read(0x4000) == 0xA5 && mmu.read(0x8000) == 0xA8 && mmu.read(0xC000) == 0xA0);
    // 0x7FFD, fully decoded and without the 48K lock
    io.write(0x7FFD, 0x23);
    assert(mmu.read(0xC000) == 0xA3);
    io.write(0x7FFD, 0x06);
    assert(mmu.read(0xC000) == 0xA6);
    io.write(0xFFFD, 0x01); // not the paging port
    assert(mmu.read(0xC000) == 0xA6);

    // The DOCK and the EXROM are RAM
    io.write(0x00F4, 0x80);
    mmu.write(0xE000, 0xD7);
    io.write(0x00FF, 0x80);
    mmu.write(0xE000, 0xE7);
    assert(mmu.read(0xE000) == 0xE7);
    io.write(0x00FF, 0x00);
    assert(mmu.read(0xE000) == 0xD7);
    assert(bank(emulator, "dock").span8k(7)[0] == 0xD7 && bank(emulator, "exrom").span8k(7)[0] == 0xE7);

    // With an odd page at 0xC000, bits 2 and 3 page in the DOCK's chunks 6 and 7 there too
    bank(emulator, "dock").span8k(6)[0] = 0xD6;
    io.write(0x00F4, 0x04);
    assert(mmu.read(0xC000) == 0xA6);
    io.write(0x7FFD, 0x01);
    assert(mmu.read(0xC000) == 0xD6);

    // The AY on 0xFFFD/0xBFFD and on 0xF5/0xF6
    io.write(0xFFFD, 0x02);
    io.write(0xBFFD, 0x33);
    assert(io.read(0xFFFD) == 0x33);
    io.write(0x00F5, 0x03);
    io.write(0x00F6, 0x04);
    io.write(0xFFFD, 0x03);
    assert(io.read(0xFFFD) == 0x04);
}

void testTs2068Frame()
{
    // 60Hz: 262 lines at 3.528 MHz. The picture's bottom border rows the short frame doesn't reach
    // still show the border
    Emulator emulator({.machineId = "ts2068"});
    assert(emulator.machine().timings().linesPerFrame == 262 && emulator.machine().timings().baseClockHz == 3'528'000);
    emulator.ioBus().write(0x00FE, 0x02);
    const auto frame = emulator.runFrames(2);
    const auto lock = frame.readLock();
    const uint32_t red = emulator.ula().palette()[2];
    assert(lock->scanline(UlaRenderer::kTotalHeightLines - 1)[0] == red);
    assert(lock->scanline(UlaRenderer::kTotalHeightLines - 4)[0] == red);
}

void testRomBoots(const std::filesystem::path &romDir)
{
    // Each machine's ROM clears the screen and prints its copyright on the last rows
    for (const char *machine : {"tc2048", "tc2068", "ts2068", "se"}) {
        Emulator emulator(romConfig(romDir, machine));
        emulator.runFrames(250);
        const auto &regs = emulator.cpu().registers();
        assert(regs.IM == 1 && regs.IFF1 == 1);
        assert(screenRowHasPixels(emulator, 0x4000, 23));
        assert(!screenRowHasPixels(emulator, 0x4000, 0));
    }

    // SE BASIC runs a line (keywords spelt letter by letter) and prints the result on row 0
    Emulator se(romConfig(romDir, "se"));
    se.runFrames(250);
    type(se, "print 2+2\n");
    se.runFrames(30);
    assert(screenRowHasPixels(se, 0x4000, 0));
}

} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path romDir = argc > 1 ? argv[1] : ZXKIT_TEST_ROM_DIR;
    testTc2068Scld();
    testScldInterruptDisable();
    testSePaging();
    testTs2068Frame();
    testRomBoots(romDir);
    std::println("test_timex: all tests passed");
    return 0;
}
