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

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <zxkit/peripherals/next/NextRtc.h>
#include <zxkit/peripherals/next/NextUart.h>
#include <zxkit/zxkit.h>

using namespace zxkit;

struct I2c
{
    NextRtc &rtc;
    void start()
    {
        rtc.setScl(false);
        rtc.setSda(true);
        rtc.setScl(true);
        rtc.setSda(false);
        rtc.setScl(false);
    }
    void stop()
    {
        rtc.setScl(false);
        rtc.setSda(false);
        rtc.setScl(true);
        rtc.setSda(true);
    }
    bool send(uint8_t value)
    {
        for (int bit = 7; bit >= 0; --bit) {
            rtc.setSda(value & (1u << bit));
            rtc.setScl(true);
            rtc.setScl(false);
        }
        rtc.setSda(true);
        rtc.setScl(true);
        const bool ack = !rtc.sda();
        rtc.setScl(false);
        return ack;
    }
    uint8_t read(bool ack)
    {
        uint8_t value = 0;
        rtc.setSda(true);
        for (int bit = 0; bit < 8; ++bit) {
            rtc.setScl(true);
            value = uint8_t((value << 1) | rtc.sda());
            rtc.setScl(false);
        }
        rtc.setSda(!ack);
        rtc.setScl(true);
        rtc.setScl(false);
        rtc.setSda(true);
        return value;
    }
    void at(uint8_t reg)
    {
        start();
        assert(send(0xD0));
        assert(send(reg));
        start();
        assert(send(0xD1));
    }
};

void uartTests()
{
    NextUart uart;
    std::vector<std::pair<unsigned, uint8_t>> sent;
    uart.setTransmitHandler([&](unsigned ch, uint8_t byte) { sent.emplace_back(ch, byte); });
    assert(uart.read(0x133B, 0) == 0x10);
    uart.write(0x143B, 0x80, 0);
    uart.write(0x143B, 8, 0); // 8 FPGA clocks/bit => ten base cycles/byte
    uart.write(0x133B, 0xFE, 0);
    assert(sent.empty());
    uart.tick(9);
    assert(sent.empty());
    uart.tick(10);
    assert((sent == std::vector<std::pair<unsigned, uint8_t>>{ { 0, 0xFE } }));
    uart.write(0x153B, 0x50, 10); // select Pi, write prescaler MSB
    assert(uart.read(0x153B, 10) == 0); // FPGA read does not echo UART select
    uart.write(0x143B, 0x80, 10);
    uart.write(0x143B, 8, 10);
    uart.write(0x163B, 0x20, 10); // 5-bit, hardware flow control
    uart.setClearToSend(1, false);
    uart.write(0x133B, 0xFF, 10);
    uart.tick(30);
    assert(sent.size() == 1);
    uart.setClearToSend(1, true);
    uart.tick(30);
    uart.tick(37);
    assert((sent.back() == std::pair<unsigned, uint8_t>{ 1, 31 }));
    const std::array<uint8_t, 3> bytes{ 0, 0xFF, 42 };
    assert(uart.receive(0, bytes, 40) == 3);
    uart.write(0x153B, 0, 40);
    assert(!(uart.read(0x133B, 49) & 1));
    assert(uart.read(0x133B, 50) & 1);
    assert(uart.read(0x143B, 50) == 0);
    assert(uart.read(0x143B, 70) == 0xFF);
    assert(uart.read(0x143B, 70) == 42);
    assert(uart.read(0x143B, 70) == 0);
    std::vector<uint8_t> overflow(513, 0xA5);
    uart.receive(0, overflow, 100);
    uart.tick(6000);
    assert((uart.read(0x133B, 6000) & 0x0D) == 0x0D);
    assert(!(uart.read(0x133B, 6000) & 4));
    uart.write(0x163B, 0x98, 6000); // sticky reset, no traffic accepted
    assert(uart.receive(0, bytes, 6000) == 0);
    uart.write(0x133B, 1, 6000);
    uart.tick(7000);
    assert(uart.read(0x133B, 7000) == 0x10);
    uart.write(0x163B, 0x18, 7000);
    for (int i = 0; i < 65; ++i)
        uart.write(0x133B, uint8_t(i), 7000);
    assert(uart.read(0x133B, 7000) & 2);
    StateWriter state;
    uart.saveState(state);
    NextUart copy;
    StateReader reader(state.data());
    copy.loadState(reader);
    assert(copy.read(0x133B, 7000) == uart.read(0x133B, 7000));
    const auto generation = uart.generation();
    uart.reset(false);
    assert(uart.generation() != generation);
    assert(!uart.active());
}

void rtcTests()
{
    NextRtc rtc;
    int64_t now = 1709251199; // 2024-02-29 23:59:59 UTC
    rtc.setTimeSource([&] { return now; });
    I2c bus{ rtc };
    bus.start();
    assert(!bus.send(0xA0));
    bus.stop(); // No device at 0x50
    bus.at(0);
    assert(bus.read(true) == 0x59);
    assert(bus.read(true) == 0x59);
    assert(bus.read(false) == 0x23);
    bus.stop();
    ++now;
    bus.at(4);
    assert(bus.read(true) == 1);
    assert(bus.read(true) == 3);
    assert(bus.read(false) == 0x24);
    bus.stop();
    bus.start();
    assert(bus.send(0xD0));
    assert(bus.send(0));
    for (auto byte : { 0x85, 0x04, 0x63, 0x02, 0x01, 0x01, 0x25 })
        assert(bus.send(byte)); // halt, 3:04:05 PM, 2025
    bus.stop();
    now += 500;
    bus.at(0);
    assert(bus.read(false) == 0x85);
    bus.stop();
    bus.at(2);
    assert(bus.read(false) == 0x63);
    bus.stop();
    bus.start();
    assert(bus.send(0xD0));
    assert(bus.send(0));
    assert(bus.send(5));
    bus.stop(); // run again
    now += 2;
    bus.at(0);
    assert(bus.read(false) == 7);
    bus.stop();
    bus.start();
    assert(bus.send(0xD0));
    assert(bus.send(63));
    assert(bus.send(0xAA));
    assert(bus.send(9));
    bus.stop(); // wrap to seconds
    bus.at(63);
    assert(bus.read(false) == 0xAA);
    bus.stop();
    rtc.resetBus();
    assert(rtc.nvram().back() == 0xAA);
    StateWriter state;
    rtc.saveState(state);
    NextRtc copy;
    StateReader reader(state.data());
    copy.loadState(reader);
    assert(copy.nvram().back() == 0xAA);
}

void nmiAvailabilityTests()
{
    Emulator em;
    const auto &core = em;
    assert(!core.canTriggerNextNmi(true));
    assert(!core.canTriggerNextNmi(false));
    em.selectMachine("next");
    auto &next = static_cast<ZxNextMachine &>(em.machine());
    auto &nr = next.nextReg();
    // Button enables and device decoders are independent, firmware-controlled gates.
    for (uint8_t buttons : { 0x00, 0x08, 0x10, 0x18 }) {
        nr.write(0x06, buttons);
        for (uint8_t devices : { 0x00, 0x01, 0x02, 0x03 }) {
            nr.write(0x83, devices);
            assert(core.canTriggerNextNmi(true) == bool((buttons & 8) && (devices & 2)));
            assert(core.canTriggerNextNmi(false) == bool((buttons & 0x10) && (devices & 1)));
        }
    }
    // Explicit DivMMC paging blocks Multiface, while its own button remains available.
    em.ioBus().write(0xE3, 0x80);
    assert(!core.canTriggerNextNmi(true));
    assert(core.canTriggerNextNmi(false));
    em.ioBus().write(0xE3, 0);
    assert(core.canTriggerNextNmi(true));
    // A paced DMA owns the bus until disabled.
    for (uint8_t byte : { 0xC3, 0x7D, 0x00, 0x80, 0x04, 0x00, 0x14, 0x50, 0x20, 0xFF, 0xCD, 0x00,
                          0x90, 0xCF, 0x87 })
        em.ioBus().write(0x6B, byte);
    assert(next.dma().isActive());
    assert(!core.canTriggerNextNmi(true));
    assert(!core.canTriggerNextNmi(false));
    assert(!em.triggerNextNmi(true));
    assert(!em.triggerNextNmi(false));
    next.dma().reset();
    assert(core.canTriggerNextNmi(true));
    assert(core.canTriggerNextNmi(false));
}

void machineTests()
{
    Emulator em({ .machineId = "next", .romResolver = [](std::string_view name, size_t size) {
                     std::vector<uint8_t> rom(size, 0);
                     if (name == "enNextMf.rom") {
                         rom[0x66] = 0xED;
                         rom[0x67] = 0x45;
                     } // RETN
                     return rom;
                 } });
    auto &next = static_cast<ZxNextMachine &>(em.machine());
    auto &nr = next.nextReg();
    auto &io = em.ioBus();
    em.setKeyState(ZxKey::CapsShift, true);
    em.setNextKeyState(NextKey::Left, true);
    assert(nr.read(0xB0) == 2);
    assert((io.read(0xF7FE) & 0x10) == 0);
    nr.write(0x68, 0x10);
    assert((io.read(0xF7FE) & 0x10) != 0);
    assert(!(io.read(0xFEFE) & 1));
    em.setNextKeyState(NextKey::Left, false);
    em.setNextKeyState(NextKey::Delete, true);
    assert(nr.read(0xB1) == 0x80);
    nr.write(0xB1, 0);
    assert(nr.read(0xB1) == 0x80);
    nr.write(0x08, 8);
    nr.write(0x84, 0xFF);
    for (auto [port, channel] : { std::pair{ 0x1F, 0 }, { 0xF3, 1 }, { 0xF9, 2 }, { 0x5F, 3 } }) {
        io.write(port, 0x90 + channel);
        assert(em.audioMixer().dac().channel(channel) == 0x90 + channel);
    }
    io.write(0xDF, 0xAA);
    assert(em.audioMixer().dac().channel(0) == 0xAA);
    assert(em.audioMixer().dac().channel(3) == 0xAA);
    assert(em.audioMixer().dac().channel(1) == 0x91);
    io.write(0xFB, 0xBB);
    assert(em.audioMixer().dac().channel(3) == 0xBB);
    assert(em.audioMixer().dac().channel(0) == 0xAA);
    nr.write(0x84, 0x20);
    io.write(0xFB, 0xCC);
    assert(em.audioMixer().dac().channel(0) == 0xCC);
    nr.write(0x2C, 0xDD);
    assert(em.audioMixer().dac().channel(1) == 0xDD);
    assert(nr.read(0x2C) == 0);
    nr.write(0x83, 0);
    assert(io.read(0x133B) == 0xFF);
    io.write(0x163B, 0x80);
    assert(next.uart().framing(0) == 0x18);
    nr.write(0x83, 0xFF);
    assert(io.read(0x133B) == 0x10);
    assert(!em.triggerNextNmi(true));
    nr.write(0x06, 0x18);
    em.cpu().registers().PC = 0x8000;
    em.cpu().registers().SP = 0xFF00;
    assert(em.canTriggerNextNmi(true));
    assert(em.canTriggerNextNmi(false));
    assert(em.triggerNextNmi(true));
    assert(!em.canTriggerNextNmi(true));
    assert(!em.canTriggerNextNmi(false));
    assert(!em.triggerNextNmi(false));
    em.cpu().stepInstruction();
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x8000); // NMI acknowledge, then RETN in MF ROM
    assert(nr.read(0xC2) == 0 && nr.read(0xC3) == 0x80);
    assert(!em.romcs().isActive("next_multiface"));
    assert(em.canTriggerNextNmi(true));
    assert(em.canTriggerNextNmi(false));
    // NextZXOS restores 128K snapshots through a stackless Multiface NMI.
    // Its return address is held in C2/C3 and may be changed by the handler.
    nr.write(0xC0, 0x08);
    em.cpu().registers().PC = 0x8123;
    em.cpu().registers().SP = 0xFF00;
    em.mmu().write(0xFEFE, 0xA5);
    em.mmu().write(0xFEFF, 0x5A);
    nr.write(0x02, 0x08);
    assert(nr.read(0x02) & 0x08);
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x66);
    assert(em.cpu().registers().SP == 0xFEFE);
    assert(em.mmu().read(0xFEFE) == 0xA5 && em.mmu().read(0xFEFF) == 0x5A);
    assert(nr.read(0xC2) == 0x23 && nr.read(0xC3) == 0x81);
    nr.write(0xC2, 0x56);
    nr.write(0xC3, 0x84);
    const auto pendingNmi = em.saveSnapshot();
    assert(!em.canTriggerNextNmi(true));
    assert(!em.canTriggerNextNmi(false));
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x8456);
    assert(em.cpu().registers().SP == 0xFF00);
    assert(!em.romcs().isActive("next_multiface"));
    assert(em.triggerNextNmi(false));
    assert(!em.canTriggerNextNmi(true));
    assert(!em.canTriggerNextNmi(false));
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x66);
    em.cpu().stepInstruction();
    assert(next.isDivMmcMapped());
    em.reset();
    assert(!next.isDivMmcMapped());
    assert(em.canTriggerNextNmi(true));
    assert(em.canTriggerNextNmi(false));

    em.loadSnapshot(pendingNmi, ".szx");
    assert(!em.canTriggerNextNmi(true));
    assert(!em.canTriggerNextNmi(false));
    auto &restored = static_cast<ZxNextMachine &>(em.machine());
    assert(restored.nextReg().read(0xC2) == 0x56 && restored.nextReg().read(0xC3) == 0x84);
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x8456);
    assert(em.cpu().registers().SP == 0xFF00);
    assert(!em.romcs().isActive("next_multiface"));

    // Clearing stackless mode before RETN restores ordinary stack reads.
    em.loadSnapshot(pendingNmi, ".szx");
    static_cast<ZxNextMachine &>(em.machine()).nextReg().write(0xC0, 0);
    em.cpu().stepInstruction();
    assert(em.cpu().registers().PC == 0x5AA5);
    assert(em.cpu().registers().SP == 0xFF00);
}

void ioDispatchTests()
{
    IoBus io;
    int writes = 0;
    io.registerSimpleHandler(
            "fallback", 0xFF, 0xFE, [](uint16_t) { return uint8_t(0xFF); }, nullptr, -1);
    assert(io.read(0xABFE) == 0xFF); // Warm the decoder before attaching devices.
    io.registerHandler(
            "matrix", 0xFF, 0xFE, [](uint16_t) -> IoReadResult { return { 0xFE, 1 }; },
            [&](uint16_t, uint8_t) { ++writes; });
    io.registerHandler(
            "other", 0xFFFF, 0xABFE, [](uint16_t) -> IoReadResult { return { 0xFD, 2 }; },
            [&](uint16_t, uint8_t) { writes += 10; });
    assert(io.read(0xABFE) == 0xFC);
    assert(io.read(0xBCFE) == 0xFE);
    io.write(0xABFE, 0);
    assert(writes == 11);
    // Sorting, vector reallocation and high/low mask overlap must preserve order.
    for (int n = 0; n < 300; ++n)
        io.registerSimpleHandler("unrelated", 0xFFFF, uint16_t(n), nullptr,
                                 [](uint16_t, uint8_t) { });
    assert(io.read(0xABFE) == 0xFC);
    io.unregisterOwner("other");
    assert(io.read(0xABFE) == 0xFE);
    io.registerHandler(
            "exclusive", 0xFFFF, 0xABFE,
            [](uint16_t) -> IoReadResult { return { 0x55, 0xFF, true }; },
            [&](uint16_t, uint8_t) { writes += 100; }, 100, true);
    assert(io.read(0xABFE) == 0x55);
    io.write(0xABFE, 0);
    assert(writes == 111);
    IoBus copied = io;
    io.clear();
    assert(copied.read(0xABFE) == 0x55);
    copied.unregisterOwner("exclusive");
    assert(copied.read(0xABFE) == 0xFE);
    assert(io.read(0xABFE) == 0xFF);
}

void engineTests()
{
    std::thread::id owner;
    std::atomic<unsigned> count = 0;
    std::atomic<bool> stopped = false;
    EngineConfig config;
    config.turbo = true;
    config.threadHooks.started = [&](Emulator &) {
        owner = std::this_thread::get_id();
        ++count;
    };
    config.threadHooks.poll = [&](Emulator &) {
        assert(std::this_thread::get_id() == owner);
        ++count;
    };
    config.threadHooks.stopped = [&](Emulator &) {
        assert(std::this_thread::get_id() == owner);
        stopped = true;
    };
    {
        Engine engine(EmulatorConfig{}, std::move(config));
        while (count.load() < 3)
            std::this_thread::yield();
        engine.stop();
    }
    assert(stopped);
    assert(owner != std::this_thread::get_id());
}
int main()
{
    uartTests();
    rtcTests();
    nmiAvailabilityTests();
    machineTests();
    ioDispatchTests();
    engineTests();
    std::cout << "Next device tests passed\n";
}
