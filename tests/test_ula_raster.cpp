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


#include <zxkit/zxkit.h>
#include <cassert>
#include <print>
#include <filesystem>
#include <fstream>
using namespace zxkit;

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char **argv)
{
    Mmu mmu;
    const auto ram = mmu.registerBank("ram", 8 * 16384, true);
    UlaRenderer ula;
    ula.setVramSource(ram, 5);
    VideoFrame frame;
    const uint32_t first = 14336 - 32 * 224 - 16;
    ula.setBorderColor(2);
    ula.beginRasterFrame(kTimings48k);
    ula.renderUntil(first + 10, mmu);
    ula.setBorderColor(1);
    ula.renderUntil(first + 160, mmu);
    ula.copyRasterScanline(0, frame);
    for (unsigned x = 0; x < 320; ++x)
        assert(frame.pixel(x, 0) == kStandardZxPalette[x < 20 ? 2 : 1]);

    auto page5 = mmu.bank(ram).span16k(5);
    page5[0] = 0xff; page5[1] = 0xff;
    page5[0x1800] = 2; page5[0x1801] = 2;
    ula.beginRasterFrame(kTimings48k);
    ula.renderUntil(14337, mmu); // first character's bitmap and attribute latched
    page5[0] = 0; page5[0x1800] = 1;
    page5[0x1801] = 4; // next attribute has not yet been fetched
    ula.renderUntil(14560, mmu);
    ula.copyRasterScanline(32, frame);
    for (unsigned x = 32; x < 40; ++x)
        assert(frame.pixel(x, 32) == kStandardZxPalette[2]);
    for (unsigned x = 40; x < 48; ++x)
        assert(frame.pixel(x, 32) == kStandardZxPalette[4]);

    // Paging between bitmap and attribute fetches must mix the two banks.
    auto page7 = mmu.bank(ram).span16k(7);
    page5[0] = 0xff; page7[0] = 0; page7[0x1800] = 6;
    ula.setVramSource(ram, 5);
    ula.beginRasterFrame(kTimings128k);
    ula.renderUntil(14362, mmu);
    ula.setVramSource(ram, 7);
    ula.renderUntil(14592, mmu);
    ula.copyRasterScanline(32, frame);
    assert(frame.pixel(32, 32) == kStandardZxPalette[6]);

    // A 7FFD write observed by the complete emulator must synchronize raster
    // output before switching screens; subsequent frames start a fresh timeline.
    Emulator e;
    e.selectMachine("128k");
    auto &memory = e.mmu().bank(e.machine().ramBankId());
    memory.span16k(5)[0] = 0xff;
    memory.span16k(5)[0x1800] = 2;
    memory.span16k(7)[0] = memory.span16k(7)[1] = 0xff;
    memory.span16k(7)[0x1800] = memory.span16k(7)[0x1801] = 4;
    e.cpu().registers().PC = 0x8000; // NOPs, interrupts disabled
    for (unsigned n = 0; n < 62; ++n)
        e.runScanline();
    e.clock().advance(14363 - e.clock().frameBaseTstates());
    e.ioBus().write(0x7ffd, 8);
    auto mixed = e.runFrames(1);
    assert(mixed.readLock()->pixel(32, 32) == kStandardZxPalette[2]);
    assert(mixed.readLock()->pixel(40, 32) == kStandardZxPalette[4]);
    auto image = e.runFrames(1);
    assert(image.readLock()->pixel(32, 32) == kStandardZxPalette[4]);
    // Screen writes through a C000 alias also preserve pixels already fetched.
    Emulator alias;
    alias.selectMachine("128k");
    auto &bank = alias.mmu().bank(alias.machine().ramBankId());
    bank.span16k(5)[0] = 0xff;
    bank.span16k(5)[0x1800] = 2;
    alias.ioBus().write(0x7ffd, 5);
    alias.cpu().registers().PC = 0x8000;
    for (unsigned n = 0; n < 62; ++n)
        alias.runScanline();
    alias.clock().advance(14363 - alias.clock().frameBaseTstates());
    alias.mmu().write(0xd800, 4);
    auto latched = alias.runFrames(1);
    assert(bank.span16k(5)[0x1800] == 4);
    assert(latched.readLock()->pixel(32, 32) == kStandardZxPalette[2]);

    if (argc > 2) {
        const std::filesystem::path fixtures = argv[1], roms = argv[2];
        EmulatorConfig cfg;
        cfg.romResolver = [roms](std::string_view name, std::size_t) { return readFile(roms / name); };
        Emulator real(std::move(cfg));
        real.loadMediaFile(fixtures / "48k/BASIC Border (2001-09-30)(Wynne, Michael D.).z80");
        const auto actual = real.runFrames(1000).readLock();
        const auto expected = readFile(fixtures / "reference/basic-border-colours.bin");
        assert(expected.size() == 320 * 256);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const uint32_t pixel = actual->pixels()[i];
            const uint8_t colour = ((pixel & 0xff) > 64 ? 2 : 0)
                                   | (((pixel >> 8) & 0xff) > 64 ? 4 : 0)
                                   | (((pixel >> 16) & 0xff) > 64 ? 1 : 0);
            assert(colour == expected[i]);
        }
        std::println("BASIC Border: all 81,920 pixels match the reference");
    }
    std::println("ULA raster tests passed");
}
