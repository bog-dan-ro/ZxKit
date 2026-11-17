/* Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later */
#include <zxkit/zxkit.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace zxkit;
using namespace std::literals;

enum class Observer {
    None,
    Read,
    Watch,
    Trap,
    Port,
    PreM1,
    PostM1,
    Tape,
    Irq,
    Nmi,
    ClockSpeed,
    ClockPosition,
    ReadClock
};

struct CpuFixture
{
    Clock clock{ kTimings48k };
    Mmu mmu;
    ContentionModel contention;
    IoBus ports;
    Im2Controller interrupts;
    Z80Cpu cpu{ clock, mmu, contention, ports, interrupts };
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t fetches = 0;

    CpuFixture(const std::vector<uint8_t> &program, uint16_t origin, Observer observer,
               uint32_t start, CpuSpeed speed, unsigned contended, bool wrap)
    {
        const auto ram = mmu.registerBank("ram"s, 65536, true);
        mmu.mapPages1k(0, ram, 0, 64);
        auto bytes = mmu.bank(ram).bytes();
        std::copy(program.begin(), program.end(), bytes.begin() + origin);
        bytes[0xc000] = 20;
        bytes[0xc001] = 19;
        bytes[0x38] = 0xfb;
        bytes[0x39] = 0xc9;
        bytes[0x66] = 0xed;
        bytes[0x67] = 0x45;
        contention.configure(ContentionPattern::Sinclair48K, kTimings48k);
        if (contended == 1)
            mmu.setContendedRange(0x8000, 16, true);
        if (contended == 2)
            mmu.setContendedRange(0xc000, 16, true);
        if (contended == 3)
            mmu.setContendedRange(0x4000, 16, true);
        auto &registers = cpu.registers();
        registers.PC = origin;
        registers.AF = registers.AF_ = 0x1237;
        registers.BC = 0x11ff;
        registers.DE = 0xc000;
        registers.HL = 0xc001;
        registers.SP = 0xbffe;
        registers.R = 0x7e;
        registers.R7 = 0x80;
        registers.I = contended == 3 ? 0x40 : 0x3f;
        registers.Q = 0x37;
        registers.lastQ = 0x29;
        registers.IM = 1;
        clock.setFrameTstates(start);
        clock.setSpeed(speed);
        if (wrap) {
            StateWriter saved;
            saved.write(std::numeric_limits<uint64_t>::max() - 100);
            saved.write(clock.frameTstates());
            saved.write(clock.frameBaseTstates());
            saved.write(uint64_t(19));
            saved.write(static_cast<uint8_t>(speed));
            StateReader reader(saved.data());
            clock.loadState(reader);
        }
        mmu.setVideoWriteCallback(ram, [this] { ++writes; });
        if (observer == Observer::Read || observer == Observer::Trap
            || observer == Observer::ReadClock) {
            mmu.setCustomSlotHandlers(0xc000 >> 10,
                                      [this, observer](uint16_t, uint8_t value) {
                                          if (++reads == 3 && observer == Observer::Trap)
                                              cpu.trap();
                                          if (reads == 1 && observer == Observer::ReadClock) {
                                              const auto position = clock.frameBaseTstates();
                                              clock.setSpeed(CpuSpeed::Mhz3_5);
                                              clock.setFrameTstates(position + 64);
                                          }
                                          return value;
                                      },
                                      {});
        }
        if (observer == Observer::Watch) {
            mmu.setWatchpointCallback([this](uint16_t, uint8_t, bool) { ++reads; });
            mmu.setSlotWatchFlags(0xc000 >> 10, true, false);
        }
        if (observer == Observer::Port || observer == Observer::ClockSpeed
            || observer == Observer::ClockPosition) {
            ports.registerSimpleHandler("input"s, 0x00ff, 0x001f,
                                        [this, observer](uint16_t) {
                                            ++reads;
                                            if (observer == Observer::ClockSpeed)
                                                clock.setSpeed((reads & 1) ? CpuSpeed::Mhz28
                                                                           : CpuSpeed::Mhz3_5);
                                            if (reads == 1 && observer == Observer::ClockPosition) {
                                                const auto position = clock.frameBaseTstates();
                                                clock.setSpeed(CpuSpeed::Mhz3_5);
                                                clock.setFrameTstates(position + 64);
                                            }
                                            return uint8_t(0);
                                        },
                                        {});
        }
        if (observer == Observer::PreM1)
            cpu.addPreM1Hook([this](uint16_t) { ++fetches; });
        if (observer == Observer::PostM1)
            cpu.addPostM1Hook([this](uint16_t, uint8_t) { ++fetches; });
        if (observer == Observer::Tape)
            cpu.setTapeTrapHook([this](uint16_t, bool) {
                ++reads;
                return false;
            });
        if (observer == Observer::Irq) {
            registers.IFF1 = registers.IFF2 = 1;
            cpu.requestInterrupt();
        }
        if (observer == Observer::Nmi)
            cpu.requestNmi();
    }

    std::vector<uint8_t> state()
    {
        StateWriter saved;
        clock.saveState(saved);
        cpu.saveState(saved);
        mmu.saveState(saved);
        return saved.takeBuffer();
    }
};

int main()
{
    const std::vector<std::vector<uint8_t>> programs{ { 0x3a, 0x00, 0xc0, 0x96, 0xfe, 4, 0x38,
                                                        0xf8 },
                                                      { 0x18, 0xfe },
                                                      { 0xc3, 0x00, 0x90 },
                                                      { 0x76 },
                                                      { 0x05, 0x20, 0xfd, 0x76 },
                                                      { 0x34, 0x18, 0xfd },
                                                      { 0x77, 0x18, 0xfd },
                                                      { 0xdb, 0x1f, 0x18, 0xfc },
                                                      { 0xfb, 0x18, 0xfd },
                                                      { 0xed, 0x5f, 0x18, 0xfc },
                                                      { 0xdd, 0x18, 0xfd },
                                                      { 0x08, 0x18, 0xfd } };
    for (const auto &program : programs) {
        for (const auto observer :
             { Observer::None, Observer::Read, Observer::Watch, Observer::Trap, Observer::Port,
               Observer::PreM1, Observer::PostM1, Observer::Tape, Observer::Irq, Observer::Nmi,
               Observer::ClockSpeed, Observer::ClockPosition, Observer::ReadClock }) {
            const uint16_t origin = observer == Observer::Tape ? 0x04d0 : 0x9000;
            for (const uint32_t length : { 1, 4, 7, 12, 19, 36, 63, 127, 228, 457, 1001, 4096 }) {
                for (unsigned variant = 0; variant < 5; ++variant) {
                    const auto speed = variant == 4 ? CpuSpeed::Mhz28 : CpuSpeed::Mhz3_5;
                    const uint32_t start = kTimings48k.firstScreenTstate() - 3 + length % 8;
                    const bool wrap = length == 1001;
                    CpuFixture bulk(program, origin, observer, start, speed, variant % 4, wrap);
                    CpuFixture stepped(program, origin, observer, start, speed, variant % 4, wrap);
                    const uint32_t target = start + length;
                    bulk.cpu.runUntil(target);
                    while (stepped.clock.frameBaseTstates() < target && !stepped.cpu.isTrapped())
                        stepped.cpu.stepInstruction();
                    assert(bulk.state() == stepped.state());
                    assert(bulk.reads == stepped.reads);
                    assert(bulk.writes == stepped.writes);
                    assert(bulk.fetches == stepped.fetches);
                }
            }
        }
    }
}
