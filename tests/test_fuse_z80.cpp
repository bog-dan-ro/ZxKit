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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace zxkit;

namespace {

struct FuseTestCase {
    std::string name;
    uint16_t af = 0, bc = 0, de = 0, hl = 0;
    uint16_t af2 = 0, bc2 = 0, de2 = 0, hl2 = 0;
    uint16_t ix = 0, iy = 0, sp = 0, pc = 0, memptr = 0;
    uint8_t i = 0, r = 0;
    unsigned iff1 = 0, iff2 = 0, im = 0;
    int halted = 0;
    int tstates = 0;

    struct MemBlock {
        uint16_t addr = 0;
        std::vector<uint8_t> data;
    };
    std::vector<MemBlock> memBlocks;
};

bool parseTestInput(std::ifstream &f, FuseTestCase &tc)
{
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) {
            tc.name = line;
            break;
        }
    }
    if (f.eof())
        return false;

    if (!std::getline(f, line))
        return false;
    {
        std::istringstream iss(line);
        iss >> std::hex >> tc.af >> tc.bc >> tc.de >> tc.hl
            >> tc.af2 >> tc.bc2 >> tc.de2 >> tc.hl2
            >> tc.ix >> tc.iy >> tc.sp >> tc.pc >> tc.memptr;
    }

    if (!std::getline(f, line))
        return false;
    {
        std::istringstream iss(line);
        unsigned iVal = 0, rVal = 0;
        iss >> std::hex >> iVal >> rVal >> tc.iff1 >> tc.iff2 >> tc.im >> tc.halted >> std::dec >> tc.tstates;
        tc.i = static_cast<uint8_t>(iVal);
        tc.r = static_cast<uint8_t>(rVal);
    }

    tc.memBlocks.clear();
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '\r')
            continue;
        std::istringstream iss(line);
        int addr = -1;
        iss >> std::hex >> addr;
        if (addr == -1)
            break;
        FuseTestCase::MemBlock mb;
        mb.addr = static_cast<uint16_t>(addr);
        int byteVal = -1;
        while (iss >> std::hex >> byteVal) {
            if (byteVal == -1)
                break;
            mb.data.push_back(static_cast<uint8_t>(byteVal));
        }
        tc.memBlocks.push_back(std::move(mb));
    }
    return true;
}

bool parseTestExpected(std::ifstream &f, FuseTestCase &tc)
{
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) {
            tc.name = line;
            break;
        }
    }
    if (f.eof())
        return false;

    std::vector<std::string> block;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            break;
        block.push_back(line);
    }

    int regIdx = -1;
    for (int i = 0; i < static_cast<int>(block.size()); ++i) {
        if (!block[i].empty() && block[i][0] != ' ' && block[i][0] != '\t') {
            regIdx = i;
            break;
        }
    }
    if (regIdx < 0 || regIdx + 1 >= static_cast<int>(block.size()))
        return false;

    {
        std::istringstream iss(block[regIdx]);
        iss >> std::hex >> tc.af >> tc.bc >> tc.de >> tc.hl
            >> tc.af2 >> tc.bc2 >> tc.de2 >> tc.hl2
            >> tc.ix >> tc.iy >> tc.sp >> tc.pc >> tc.memptr;
    }
    {
        std::istringstream iss(block[regIdx + 1]);
        unsigned iVal = 0, rVal = 0;
        iss >> std::hex >> iVal >> rVal >> tc.iff1 >> tc.iff2 >> tc.im >> tc.halted >> std::dec >> tc.tstates;
        tc.i = static_cast<uint8_t>(iVal);
        tc.r = static_cast<uint8_t>(rVal);
    }

    tc.memBlocks.clear();
    for (int i = regIdx + 2; i < static_cast<int>(block.size()); ++i) {
        std::istringstream iss(block[i]);
        int addr = -1;
        iss >> std::hex >> addr;
        if (addr < 0)
            break;
        FuseTestCase::MemBlock mb;
        mb.addr = static_cast<uint16_t>(addr);
        int byteVal = -1;
        while (iss >> std::hex >> byteVal) {
            if (byteVal == -1)
                break;
            mb.data.push_back(static_cast<uint8_t>(byteVal));
        }
        tc.memBlocks.push_back(std::move(mb));
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path fixtureDir = (argc > 1) ? argv[1] : "fixtures";
    std::ifstream inFile(fixtureDir / "tests.in");
    std::ifstream expFile(fixtureDir / "tests.expected");
    if (!inFile || !expFile) {
        std::println(stderr, "Cannot open FUSE Z80 test files in '{}'", fixtureDir.string());
        return 1;
    }

    std::vector<FuseTestCase> inputs;
    std::unordered_map<std::string, FuseTestCase> expectedMap;

    FuseTestCase tc;
    while (parseTestInput(inFile, tc))
        inputs.push_back(tc);
    while (parseTestExpected(expFile, tc))
        expectedMap[tc.name] = tc;

    Clock clock;
    Mmu mmu;
    const BankId ramBank = mmu.registerBank("ram64k", 64 * 1024, true, true);
    ContentionModel contention;
    IoBus ioBus;
    Im2Controller im2;

    // FUSE coretest expects port reads to return the high byte of the port address
    ioBus.setFloatingBusCallback([](uint16_t port) -> uint8_t {
        return static_cast<uint8_t>(port >> 8);
    });

    Z80Cpu cpu(clock, mmu, contention, ioBus, im2);

    int passed = 0;
    int failed = 0;

    for (const auto &in : inputs) {
        auto it = expectedMap.find(in.name);
        if (it == expectedMap.end()) {
            std::println(stderr, "Missing expected entry for '{}'", in.name);
            ++failed;
            continue;
        }
        const auto &exp = it->second;

        mmu.bank(ramBank).fill(0x00);
        mmu.mapPages1k(0x0000, ramBank, 0, 64, true, true, false);
        for (uint32_t addr = 0; addr < 0x10000; addr += 4) {
            mmu.poke(static_cast<uint16_t>(addr), 0xDE);
            mmu.poke(static_cast<uint16_t>(addr + 1), 0xAD);
            mmu.poke(static_cast<uint16_t>(addr + 2), 0xBE);
            mmu.poke(static_cast<uint16_t>(addr + 3), 0xEF);
        }
        for (const auto &mb : in.memBlocks) {
            for (std::size_t i = 0; i < mb.data.size(); ++i)
                mmu.poke(static_cast<uint16_t>(mb.addr + i), mb.data[i]);
        }

        clock.reset();
        cpu.reset(true);
        auto &r = cpu.registers();
        r.AF  = in.af;  r.BC  = in.bc;  r.DE  = in.de;  r.HL  = in.hl;
        r.AF_ = in.af2; r.BC_ = in.bc2; r.DE_ = in.de2; r.HL_ = in.hl2;
        r.IX  = in.ix;  r.IY  = in.iy;  r.SP  = in.sp;  r.PC  = in.pc;
        r.WZ  = in.memptr;
        r.I   = in.i;
        r.setFullR(in.r);
        r.IFF1 = static_cast<uint8_t>(in.iff1);
        r.IFF2 = static_cast<uint8_t>(in.iff2);
        r.IM   = static_cast<uint8_t>(in.im);
        r.halted = (in.halted != 0);

        while (static_cast<int>(clock.totalTstates()) < in.tstates) {
            cpu.stepInstruction();
        }

        bool ok = true;
        if (r.AF != exp.af || r.BC != exp.bc || r.DE != exp.de || r.HL != exp.hl ||
            r.AF_ != exp.af2 || r.BC_ != exp.bc2 || r.DE_ != exp.de2 || r.HL_ != exp.hl2 ||
            r.IX != exp.ix || r.IY != exp.iy || r.SP != exp.sp || r.PC != exp.pc ||
            r.WZ != exp.memptr || r.I != exp.i || r.fullR() != exp.r ||
            r.IFF1 != exp.iff1 || r.IFF2 != exp.iff2 || r.IM != exp.im ||
            static_cast<int>(r.halted) != exp.halted ||
            static_cast<int>(clock.totalTstates()) != exp.tstates) {
            ok = false;
        }

        for (const auto &mb : exp.memBlocks) {
            for (std::size_t i = 0; i < mb.data.size(); ++i) {
                if (mmu.peek(static_cast<uint16_t>(mb.addr + i)) != mb.data[i]) {
                    ok = false;
                    break;
                }
            }
        }

        if (ok) {
            ++passed;
        } else {
            ++failed;
            if (failed <= 10) {
                std::println(stderr, "FAIL [{}]: AF={:04X}(exp {:04X}) BC={:04X}({:04X}) DE={:04X}({:04X}) HL={:04X}({:04X}) PC={:04X}({:04X}) WZ={:04X}({:04X}) ts={}({})",
                             in.name, r.AF, exp.af, r.BC, exp.bc, r.DE, exp.de, r.HL, exp.hl, r.PC, exp.pc, r.WZ, exp.memptr, clock.totalTstates(), exp.tstates);
            }
        }
    }

    std::println("FUSE Z80 Compliance Suite: {}/{} passed ({} failed)", passed, inputs.size(), failed);
    return (failed == 0 && passed == 1356) ? 0 : 1;
}
