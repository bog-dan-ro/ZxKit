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

#include "RomTestSupport.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>

using namespace zxkit;
using namespace std::literals;

namespace {

void testPorts()
{
    Emulator emulator({ .machineId = "48k"s });
    auto paper = std::make_shared<ZxPrinterPaper>();
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));
    auto &bus = emulator.ioBus();
    auto &clock = emulator.clock();

    assert(bus.peekPort(0xABFB) == 0x3E);
    assert(bus.peekPort(0xABF7) == 0xFF);
    bus.write(0x12EB, 0); // A2 low, fast motor start
    assert(bus.peekPort(0xABFB) == 0x3E);
    clock.advance(219);
    assert(bus.peekPort(0xABFB) == 0x3E);
    clock.advance(1);
    assert(bus.peekPort(0xABFB) == 0x3F);
    assert(bus.peekPort(0xABFB) == 0x3F);
    clock.advance(63 * 220);
    assert(bus.peekPort(0xABFB) == 0xBF);
    bus.write(0x80FB, 0x82); // Black on dot zero. New speed applies on the next scan.
    assert(bus.peekPort(0xABFB) == 0xBE);
    clock.advance(220);
    bus.write(0x00FB, 0x02);
    assert(bus.peekPort(0xABFB) == 0x3E);
    clock.advance(220);
    bus.write(0x00FB, 0x82); // Black on dot two.
    clock.advance(254 * 220);
    assert(bus.peekPort(0xABFB) == 0xBF);
    bus.write(0x00FB, 0x02);
    assert(paper->rows().size() == 1);
    assert(paper->rows()[0][0] == 0xBF);
    assert(paper->rows()[0].back() == 0xFF);
    clock.advance(439);
    assert(bus.peekPort(0xABFB) == 0x3E);
    clock.advance(1);
    assert(bus.peekPort(0xABFB) == 0x3F);
    clock.advance(127 * 440);
    assert(bus.peekPort(0xABFB) == 0xBF);
    bus.write(0x00FB, 0x04);
    assert(bus.peekPort(0xABFB) == 0x3E);
    assert(paper->rows().size() == 2);
    assert(std::ranges::all_of(paper->rows()[1], [](uint8_t byte) { return byte == 0; }));

    emulator.reset();
    assert(paper->rows().size() == 2);
    assert(bus.peekPort(0xABFB) == 0x3E);
    emulator.detachPeripheral(ZxPrinterPeripheral::kId);
    assert(bus.peekPort(0xABFB) == 0xFF);
    assert(paper->rows().size() == 2);
    paper->clear();
    assert(paper->toPbm().empty() && paper->toPng().empty() && paper->text().empty());
    assert(paper->textLines().empty());
}

void testTimexPorts()
{
    Emulator emulator({ .machineId = "tc2048"s });
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>());
    assert(emulator.ioBus().peekPort(0x12FB) == 0x3E);
    assert(emulator.ioBus().peekPort(0x12EB) == 0xFF);
}

void testNextSnapshotCompatibility()
{
    Emulator next({ .machineId = "next"s });
    const auto snapshot = next.saveSnapshot(".szx"sv);

    Emulator emulator({ .machineId = "48k"s });
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>());
    emulator.loadSnapshot(snapshot, ".szx"sv);
    assert(emulator.machine().id() == "next"sv);
    assert(emulator.ioBus().peekPort(0x00FB) == 0x3E);

    emulator.ioBus().write(0x00FB, 0);
    emulator.clock().advance(64 * 220);
    const auto printing = emulator.saveSnapshot(".szx"sv);
    emulator.ioBus().write(0x00FB, 4);
    emulator.loadSnapshot(printing, ".szx"sv);
    assert(emulator.ioBus().peekPort(0x00FB) == 0xBF);
    next.loadSnapshot(printing, ".szx"sv);
    assert(!next.findPeripheral(ZxPrinterPeripheral::kId));

    emulator.selectMachine("48k"sv);
    assert(emulator.ioBus().peekPort(0x00FB) == 0x3E);
}

void testHcSerial()
{
    EmulatorConfig config = { .machineId = "hc2000"s };
    config.romResolver = [](std::string_view name, size_t size) {
        if (name == "hc2000-basic.rom"sv)
            return test::readFile(std::filesystem::path(ZXKIT_TEST_ROM_DIR) / "48.rom");
        return std::vector<uint8_t>(size);
    };
    Emulator emulator(config);
    auto paper = std::make_shared<ZxPrinterPaper>();
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));
    emulator.writeMemory(0x5C36, { 0, 0x3C });
    auto &bus = emulator.ioBus();
    const auto send = [&bus](uint8_t byte) {
        bus.write(0x00F7, 0x11);
        for (unsigned bit = 0; bit < 8; ++bit)
            bus.write(0x00F7, uint8_t(0x10 | (~byte >> bit & 1)));
        bus.write(0x00F7, 0x10);
    };
    send('x');
    send('\r');
    assert(paper->rows().empty());

    bus.write(0x00EF, 0xEF);
    send('p');
    send('\r');
    bus.write(0x00EF, 0xEE);
    assert(paper->rows().size() == 8);
    assert(paper->text() == "p\n"sv);
}

void enter128Basic(Emulator &emulator, bool compatibilityMode)
{
    const bool plus3Family = dynamic_cast<const SpectrumPlus3Machine *>(&emulator.machine());
    emulator.runFrames(plus3Family ? 350 : 100);
    const int menuMoves = compatibilityMode ? 3 : 1;
    for (int i = 0; i < menuMoves; ++i)
        test::tap(emulator, { ZxKey::CapsShift, ZxKey::Num6 });
    test::tap(emulator, { ZxKey::Enter });
    emulator.runFrames(110);
    assert(emulator.machine().is48kMode() == compatibilityMode);
}

void testDirectPrint(std::string machine, bool connected)
{
    Emulator emulator(test::romConfig(ZXKIT_TEST_ROM_DIR, std::move(machine)));
    auto paper = std::make_shared<ZxPrinterPaper>();
    if (connected)
        emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));
    enter128Basic(emulator, false);

    test::type(emulator, "lprint "sv);
    test::tap(emulator, { ZxKey::SymbolShift, ZxKey::P });
    test::tap(emulator, { ZxKey::P });
    test::tap(emulator, { ZxKey::SymbolShift, ZxKey::P });
    test::tap(emulator, { ZxKey::Enter });
    emulator.runFrames(20);

    assert(paper->text() == (connected ? "p\n"sv : ""sv));
    test::tap(emulator, { ZxKey::Space }); // Dismiss the command's report.
    emulator.runFrames(24);
    test::type(emulator, "poke 32768"sv);
    test::tap(emulator, { ZxKey::SymbolShift, ZxKey::N }); // comma
    test::type(emulator, "1\n"sv);
    emulator.runFrames(20);
    assert(emulator.mmu().peek(32768) == 1);
}

void testEditorPrint(std::string machine)
{
    Emulator emulator(test::romConfig(ZXKIT_TEST_ROM_DIR, std::move(machine)));
    auto paper = std::make_shared<ZxPrinterPaper>();
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));
    enter128Basic(emulator, false);

    const auto printFromEditMenu = [&emulator] {
        test::tap(emulator, { ZxKey::CapsShift, ZxKey::Num1 }); // EDIT
        for (int i = 0; i < 3; ++i)
            test::tap(emulator, { ZxKey::CapsShift, ZxKey::Num6 }); // Down to Print.
        test::tap(emulator, { ZxKey::Enter });
        emulator.runFrames(20);
    };

    test::type(emulator, "test"sv);
    printFromEditMenu();
    assert(paper->rows().empty());
    test::tap(emulator, { ZxKey::Space }); // Dismiss the report.
    emulator.runFrames(40);

    test::type(emulator, "10 rem test\n"sv);
    emulator.runFrames(40);
    printFromEditMenu();
    assert(paper->text() == "  10 REM test\n"sv);
    assert(paper->rows().size() == 8);
}

void testParallelCopy()
{
    Emulator emulator(test::romConfig(ZXKIT_TEST_ROM_DIR, "plus3"s));
    auto paper = std::make_shared<ZxPrinterPaper>();
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));
    enter128Basic(emulator, false);

    emulator.writeMemory(0x4800, { 0x80 }); // First dot on screen line 64.
    test::type(emulator, "copy\n"sv);
    for (int frame = 0; frame < 1000 && paper->rows().size() < 176; ++frame)
        emulator.runFrame();
    assert(paper->rows().size() == 176);
    assert(paper->rows()[64][0] == 0x80);
    assert(std::ranges::all_of(paper->rows().back(), [](uint8_t byte) { return byte == 0; }));
}

void testBasic(std::string machine, bool native = false)
{
    Emulator emulator(test::romConfig(ZXKIT_TEST_ROM_DIR, std::move(machine)));
    auto paper = std::make_shared<ZxPrinterPaper>();
    emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>(paper));

    const auto program = BasicCodec::compile("10 LPRINT \"ZX PRINTER?\"\n"
                                             "20 LLIST 100\n"
                                             "30 CLS: PLOT 0,175: PLOT 16,167: PLOT 255,0: COPY\n"
                                             "40 POKE 32768,1: STOP\n"
                                             "100 REM PRINTER TEST\n"sv);
    auto tape = MediaArchive::create(".tap"sv);
    tape.put("files/PRINTER"sv, program,
             { { "type"s, 0 }, { "param1"s, 10 }, { "param2"s, int64_t(program.size()) } });
    if (emulator.machine().id() == "48k"sv)
        emulator.runFrames(100);
    else
        enter128Basic(emulator, !native);

    emulator.loadTape(tape.encode(), ".tap"sv);
    if (native)
        test::type(emulator, "load "sv);
    else
        test::tap(emulator, { ZxKey::J }); // LOAD
    test::tap(emulator, { ZxKey::SymbolShift, ZxKey::P });
    test::tap(emulator, { ZxKey::SymbolShift, ZxKey::P });
    test::tap(emulator, { ZxKey::Enter });
    for (int frame = 0; frame < (native ? 3500 : 600) && emulator.mmu().peek(32768) != 1; ++frame)
        emulator.runFrame();

    assert(emulator.mmu().peek(32768) == 1);
    assert(paper->text().starts_with("ZX PRINTER?\n 100>REM  PRINTER TEST\n"sv));
    const auto textLines = paper->textLines();
    assert(textLines.size() == 24);
    assert(textLines[0].row == 0 && textLines[0].characters[10] == u'?');
    assert(textLines[1].row == 8 && textLines[1].characters[1] == u'1');
    assert(textLines[2].row == 16 && textLines[2].characters[0] == 0);
    assert(textLines[2].characters[1] == u' ');
    assert(paper->rows().size() == 192); // Two text rows + 176 display scanlines.
    for (unsigned y = 0; y < 176; ++y) {
        ZxPrinterPaper::Row expected{};
        if (y == 0)
            expected[0] = 0x80;
        if (y == 8)
            expected[2] = 0x80;
        if (y == 175)
            expected[31] = 0x01;
        assert(paper->rows()[16 + y] == expected);
    }

    const auto pbm = paper->toPbm();
    constexpr auto header = "P4\n256 192\n"sv;
    assert(pbm.size() == header.size() + 192 * 32);
    assert(std::ranges::equal(header, std::span(pbm).first(header.size())));
    assert(pbm[header.size() + 16 * 32] == 0x80);
    assert(pbm.back() == 0x01);

    const auto png = paper->toPng();
    assert(png.size() > 32);
    assert(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    assert(png[18] == 1 && png[19] == 0 && png[23] == 192);
}

} // namespace

int main()
{
    testPorts();
    testTimexPorts();
    testNextSnapshotCompatibility();
    testHcSerial();
    testDirectPrint("128k"s, false);
    testDirectPrint("128k"s, true);
    testDirectPrint("plus3"s, false);
    testDirectPrint("plus3"s, true);
    testEditorPrint("128k"s);
    testEditorPrint("plus2"s);
    testEditorPrint("plus2a"s);
    testEditorPrint("plus3"s);
    testEditorPrint("plus3e"s);
    testParallelCopy();
    testBasic("48k"s);
    testBasic("128k"s);
    testBasic("plus2"s);
    testBasic("128k"s, true);
    testBasic("plus2"s, true);
}
