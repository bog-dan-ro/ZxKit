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

// The CoBra 1's floppy disk board (the Z80 CTC and the 8272 in non-DMA mode) and its
// machine: the keyboard's sixth column, the configurations, the 80 characters screen. The
// hardware tests program the ports directly; integration tests boot the bundled ROMs and disk.

#include "RomTestSupport.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <print>
#include <tuple>
#include <vector>

using namespace zxkit;

namespace {

// ── Z80 CTC ───────────────────────────────────────────────────────────────

struct CtcEvents {
    std::vector<std::size_t> zeroCounts;
    std::vector<std::tuple<std::size_t, uint8_t, bool>> interrupts;
};

void connect(Z80Ctc &ctc, CtcEvents &events)
{
    ctc.onZeroCount = [&events](std::size_t channel) { events.zeroCounts.push_back(channel); };
    ctc.onInterrupt = [&events](std::size_t channel, uint8_t vector, bool pending) {
        events.interrupts.emplace_back(channel, vector, pending);
    };
}

void testCtcTimer()
{
    Z80Ctc ctc;
    CtcEvents events;
    connect(ctc, events);
    ctc.reset();
    events.interrupts.clear();

    // Timer, prescaler 16, time constant 10: a zero count every 160 clocks, then it reloads
    ctc.write(1, 0x05);
    ctc.write(1, 10);
    ctc.tick(159);
    assert(events.zeroCounts.empty() && ctc.read(1) == 1);
    ctc.tick(1);
    assert(events.zeroCounts == std::vector<std::size_t>{1} && ctc.read(1) == 10);
    ctc.tick(160 * 3);
    assert(events.zeroCounts.size() == 4);
    // No interrupt: not enabled
    assert(std::ranges::none_of(events.interrupts, [](const auto &e) { return std::get<2>(e); }));

    // Prescaler 256, time constant 0 counts 256
    events.zeroCounts.clear();
    ctc.write(2, 0x25);
    ctc.write(2, 0);
    ctc.tick(256 * 256 - 1);
    assert(events.zeroCounts.empty() || events.zeroCounts.front() != 2);
    ctc.tick(1);
    assert(std::ranges::count(events.zeroCounts, 2) == 1);

    // A timer started by its trigger (falling edge): nothing counts until the edge
    events.zeroCounts.clear();
    ctc.write(0, 0x0D);
    ctc.write(0, 1);
    ctc.tick(1000);
    assert(std::ranges::count(events.zeroCounts, 0) == 0);
    ctc.setTrigger(0, true);
    ctc.tick(1000);
    assert(std::ranges::count(events.zeroCounts, 0) == 0); // a rising edge: not the programmed one
    ctc.setTrigger(0, false);
    ctc.tick(16);
    assert(std::ranges::count(events.zeroCounts, 0) == 1);

    // A channel reset stops it
    events.zeroCounts.clear();
    ctc.write(1, 0x03);
    ctc.tick(10'000);
    assert(std::ranges::count(events.zeroCounts, 1) == 0);
}

void testCtcCounterAndInterrupts()
{
    Z80Ctc ctc;
    CtcEvents events;
    connect(ctc, events);
    ctc.reset();
    events.interrupts.clear();

    // The vector word (channel 0, bit 0 clear): bits 1-2 are the channel's number
    ctc.write(0, 0x98);
    assert(ctc.vector(0) == 0x98 && ctc.vector(3) == 0x9E);

    // Counter, rising edge, interrupts, time constant 3
    events.interrupts.clear();
    ctc.write(3, 0xD5);
    ctc.write(3, 3);
    for (int i = 0; i < 2; ++i) {
        ctc.setTrigger(3, true);
        ctc.setTrigger(3, false);
    }
    assert(events.interrupts.empty() && ctc.read(3) == 1);
    ctc.setTrigger(3, true);
    assert(events.interrupts.size() == 1);
    assert(events.interrupts.back() == std::make_tuple(std::size_t{3}, uint8_t{0x9E}, true));
    assert(ctc.read(3) == 3);
    // The timer ticks don't move a counter
    ctc.tick(100'000);
    assert(ctc.read(3) == 3);

    // Disabling its interrupts withdraws a pending one
    ctc.write(3, 0x51);
    assert(events.interrupts.back() == std::make_tuple(std::size_t{3}, uint8_t{0x9E}, false));

    // A software reset withdraws a pending interrupt even when bit 7 stays enabled.
    ctc.write(3, 0xD7);
    ctc.write(3, 1);
    ctc.setTrigger(3, false);
    ctc.setTrigger(3, true);
    assert(std::get<2>(events.interrupts.back()));
    ctc.write(3, 0xD3);
    assert(events.interrupts.back() == std::make_tuple(std::size_t{3}, uint8_t{0x9E}, false));

    // The state survives a save and a load
    StateWriter w(256);
    ctc.write(1, 0xC5);
    ctc.write(1, 7);
    ctc.saveState(w);
    Z80Ctc copy;
    const auto bytes = w.takeBuffer();
    StateReader r(bytes);
    copy.loadState(r);
    assert(copy.vector(1) == 0x9A && copy.read(1) == 7);
}

// ── The 8272 / uPD765 in non-DMA mode ─────────────────────────────────────

void testUpd765NonDmaInterrupt()
{
    Upd765Fdc fdc;
    fdc.reset();
    fdc.drive(0).formatBlank(40, 1, 9, 512, 1);
    for (std::size_t i = 0; i < 512; ++i)
        fdc.drive(0).findSector(0, 0, 1)->get().data[i] = static_cast<uint8_t>(i);

    uint64_t now = 0;
    const auto command = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            assert(fdc.readMainStatus(now) & 0x80);
            fdc.writeData(b, now++);
        }
    };

    // SPECIFY with ND: non-DMA, the INT pin requests each byte
    command({0x03, 0xDF, 0x03});
    assert(!fdc.interruptLine(now));

    // RECALIBRATE: the seek interrupt holds INT until SENSE INTERRUPT STATUS
    command({0x07, 0x00});
    assert(!fdc.interruptLine(now));
    now += 1000;
    assert(fdc.interruptLine(now));
    command({0x08});
    assert(fdc.readData(now) == 0x20 && fdc.readData(now) == 0x00);
    assert(!fdc.interruptLine(now));

    // READ DATA: the first byte requests once the sector is under the head, not at once
    command({0x46, 0x00, 0, 0, 1, 2, 1, 0x2A, 0xFF});
    assert((fdc.readMainStatus(now) & 0x20) && !fdc.interruptLine(now));
    now += 1000;
    assert(fdc.interruptLine(now));
    for (int i = 0; i < 511; ++i) {
        assert(fdc.interruptLine(now));
        assert(fdc.readData(now) == static_cast<uint8_t>(i));
        now += 40;
    }
    // A hardware TC with the sector's last byte presented: that byte still moves
    fdc.terminalCount();
    assert(fdc.interruptLine(now) && fdc.readData(now) == 0xFF);
    const uint64_t lastByte = now;
    // The controller reads the CRC: busy executing, no request, INT low
    assert(fdc.readMainStatus(now) == 0x30 && !fdc.interruptLine(now));
    now += 200;
    assert(fdc.readMainStatus(now) == 0x30 && !fdc.interruptLine(now));
    // ... then its result phase interrupts until the first result byte is read
    now = lastByte + 2 * 112 + 32;
    assert(fdc.readMainStatus(now) == 0xD0 && fdc.interruptLine(now));
    std::vector<uint8_t> result;
    while ((fdc.readMainStatus(now) & 0xD0) == 0xD0) {
        result.push_back(fdc.readData(now));
        assert(!fdc.interruptLine(now));
    }
    // Normal termination thanks to TC: the next sector is C+1, R=1 (R was EOT)
    assert((result == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x02}));
    assert(fdc.readMainStatus(now) == 0x80);

    // A TC between the bytes of a sector ends it after the presented one
    command({0x46, 0x00, 0, 0, 1, 2, 1, 0x2A, 0xFF});
    now += 1000;
    for (int i = 0; i < 9; ++i)
        fdc.readData(now);
    fdc.terminalCount();
    assert(fdc.readData(now) == 9);
    assert(!fdc.interruptLine(now) && (fdc.readMainStatus(now) & 0x90) == 0x10);
    now += 300;
    assert(fdc.interruptLine(now) && fdc.readMainStatus(now) == 0xD0);
}

// ── The CoBra 1 ───────────────────────────────────────────────────────────

constexpr uint16_t kCtc0 = 0x00E3;
constexpr uint16_t kCtc1 = 0x00EB;
constexpr uint16_t kCtc2 = 0x00F3;
constexpr uint16_t kCtc3 = 0x00FB;
constexpr uint16_t kFdcStatus = 0x00F5;
constexpr uint16_t kFdcData = 0x00FD;

CobraMachine &cobra(Emulator &emulator)
{
    return dynamic_cast<CobraMachine &>(emulator.machine());
}

// Leaves the startup configuration like the boot ROM does: O6 chooses CP/M (1) or BASIC (0),
// R7 falls, and the new map applies after the next opcode fetch, the JP (HL)
void switchConfiguration(Emulator &emulator, bool cpm, uint16_t target)
{
    auto &io = emulator.ioBus();
    io.write(0x00DF, 0x92); // the 8255: A and B in, C out
    io.write(0x00FE, cpm ? 0x40 : 0x00);
    // At 8000 (DRAM#0 in the startup configuration): LD A,$80 ; LD R,A ; XOR A ; LD R,A ; JP (HL)
    constexpr std::array<uint8_t, 8> code = {0x3E, 0x80, 0xED, 0x4F, 0xAF, 0xED, 0x4F, 0xE9};
    for (std::size_t i = 0; i < code.size(); ++i)
        emulator.mmu().poke(static_cast<uint16_t>(0x8000 + i), code[i]);
    auto &regs = emulator.cpu().registers();
    regs.PC = 0x8000;
    regs.HL = target;
    for (int i = 0; i < 5; ++i)
        emulator.cpu().stepInstruction();
    assert(regs.PC == target);
    assert(cobra(emulator).poFlipFlop() == 0 && cobra(emulator).lo6FlipFlop() == (cpm ? 1 : 0));
}

void testCobraKeyboard()
{
    Emulator emulator({.machineId = "cobra1_80k"});
    auto &io = emulator.ioBus();
    assert((io.read(0xFEFE) & 0x3F) == 0x3F);
    // The sixth column (bit 5): CTRL on KA8, F4 on KA15
    emulator.keyboard().setKeyState(ZxKey::Ctrl, true);
    assert((io.read(0xFEFE) & 0x3F) == 0x1F);
    assert((io.read(0x7FFE) & 0x3F) == 0x3F);
    emulator.keyboard().setKeyState(ZxKey::Ctrl, false);
    emulator.keyboard().setKeyState(ZxKey::F4, true);
    assert((io.read(0x7FFE) & 0x3F) == 0x1F);
    // ... and the five Spectrum columns as usual
    emulator.keyboard().setKeyState(ZxKey::F4, false);
    emulator.keyboard().setKeyState(ZxKey::A, true);
    assert((io.read(0xFDFE) & 0x3F) == 0x3E);

    constexpr std::array keys{ZxKey::Ctrl, ZxKey::NoScroll, ZxKey::LineFeed, ZxKey::Esc,
                              ZxKey::F1, ZxKey::F2, ZxKey::F3, ZxKey::F4};
    for (const auto machine : {"cobra1_64k", "cobra1_80k"}) {
        Emulator computer({.machineId = machine});
        for (std::size_t row = 0; row < keys.size(); ++row) {
            computer.setKeyState(keys[row], true);
            for (std::size_t selected = 0; selected < keys.size(); ++selected) {
                const auto port = static_cast<uint16_t>(((0xFF ^ (1u << selected)) << 8) | 0xFE);
                assert((computer.ioBus().read(port) & 0x3F) == (selected == row ? 0x1F : 0x3F));
            }
            computer.setKeyState(keys[row], false);
        }
    }
    // The extra column must not change the Spectrum's fixed bit 5.
    Emulator spectrum;
    spectrum.setKeyState(ZxKey::Ctrl, true);
    assert((spectrum.ioBus().read(0xFEFE) & 0x3F) == 0x3F);
}

void testCobraFrameInterrupt()
{
    // The 50Hz interrupt reaches the CPU in the BASIC configuration only. The program at 0100:
    // EI ; HALT, and a HALT at 0038 (the IM 1 handler). DRAM#0 is at 0000 in both CP/M and BASIC.
    for (const bool cpm : {true, false}) {
        Emulator emulator({.machineId = "cobra1_80k"});
        emulator.mmu().poke(0x8100, 0xFB);
        emulator.mmu().poke(0x8101, 0x76);
        emulator.mmu().poke(0x8038, 0x76);
        switchConfiguration(emulator, cpm, 0x0100);
        assert(emulator.mmu().read(0x0101) == 0x76);
        emulator.runFrames(3);
        const auto &regs = emulator.cpu().registers();
        assert(regs.halted);
        assert(regs.PC == (cpm ? 0x0101 : 0x0038));
        assert(cobra(emulator).isSpectrumModeLocked() == !cpm);
    }
}

void testCobraVerticalSyncCounter()
{
    // CTC channel 3 counts the frames (NVS): CP/M's clock and keyboard scan
    Emulator emulator({.machineId = "cobra1_80k"});
    auto &io = emulator.ioBus();
    io.write(kCtc0, 0x98);
    io.write(kCtc3, 0xC5);
    io.write(kCtc3, 2);
    emulator.runFrames(1);
    assert(emulator.im2().pendingMask() == 0);
    emulator.runFrames(1);
    // vector 9E: index 15 of the daisy chain, CTC channel 0 being the highest priority
    assert(emulator.im2().pendingMask() == 0x8000);
    assert(emulator.im2().acknowledge() == 0x9E);
}

void testCobraFloppyRead()
{
    // The CP/M loaders' setup: CTC0 counts the 8272's INT edges (each one a byte request) and
    // interrupts for each; CTC1 counts CTC0's zero counts (the bytes), CTC2 CTC1's (the sectors),
    // and CTC2's zero count is the 8272's TC
    Emulator emulator({.machineId = "cobra1_80k"});
    assert(emulator.diskDriveCount() == 4);
    DiskImage &disk = emulator.diskDrive(0)->get();
    disk.formatBlank(80, 2, 9, 512, 1);
    for (std::size_t i = 0; i < 512; ++i)
        disk.findSector(0, 0, 1)->get().data[i] = static_cast<uint8_t>(0x40 + i);

    auto &io = emulator.ioBus();
    auto &im2 = emulator.im2();
    io.write(kCtc0, 0x98);
    io.write(kCtc0, 0xD5);
    io.write(kCtc0, 1);
    io.write(kCtc1, 0x55);
    io.write(kCtc1, 4);
    io.write(kCtc2, 0x55);
    io.write(kCtc2, 1);

    const auto command = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            assert(io.read(kFdcStatus) & 0x80);
            io.write(kFdcData, b);
        }
    };
    command({0x03, 0xDF, 0x03}); // SPECIFY, non-DMA
    command({0x46, 0x00, 0, 0, 1, 2, 1, 0x2A, 0xFF});
    assert((io.read(kFdcStatus) & 0xF0) == 0x70 && im2.pendingMask() == 0);

    // The first byte request: CTC0's interrupt (vector 98, index 12)
    emulator.clock().advance(1000);
    io.read(kFdcStatus);
    assert(im2.pendingMask() == 0x1000);
    assert(im2.acknowledge() == 0x98);
    im2.onReti();

    // Four bytes: each moves, the next one requests. The fourth request zeroes CTC1, then CTC2:
    // TC with the fourth byte presented, the transfer ends after it.
    for (int i = 0; i < 4; ++i) {
        assert(io.read(kFdcData) == 0x40 + i);
        emulator.clock().advance(50);
        assert(im2.pendingMask() == (i < 3 ? 0x1000 : 0));
        if (im2.pendingMask()) {
            im2.acknowledge();
            im2.onReti();
        }
    }
    // The CRC goes by (busy, executing), then the result phase interrupts
    assert((io.read(kFdcStatus) & 0xF0) == 0x30);
    emulator.clock().advance(300);
    assert((io.read(kFdcStatus) & 0xD0) == 0xD0);
    assert(im2.pendingMask() == 0x1000);
    std::vector<uint8_t> result;
    while ((io.read(kFdcStatus) & 0xD0) == 0xD0)
        result.push_back(io.read(kFdcData));
    assert((result == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x02}));
}

uint32_t pixel(const VideoFramePtr &frame, uint32_t x, uint32_t y)
{
    return frame.readLock()->scanline(y)[x];
}

void testCobraWideScreen()
{
    // The 80 characters modification: in CP/M, O5 latches a 320 pixel wide screen. The 4 cells
    // left of the screen show the video RAM's upper half columns 28-31, the 4 right columns 0-3.
    Emulator emulator({.machineId = "cobra1_80k"});
    emulator.mmu().poke(0x8100, 0xF3); // DI
    emulator.mmu().poke(0x8101, 0x76); // HALT
    switchConfiguration(emulator, true, 0x0100);
    // The video RAM is at 4000 in CP/M (O6 = 1): its upper half at 6000
    for (uint16_t column = 0; column < 32; ++column) {
        emulator.mmu().poke(static_cast<uint16_t>(0x6000 + column), 0xFF);                     // pixel line 0
        emulator.mmu().poke(static_cast<uint16_t>(0x7800 + column), column < 4 ? 0x02 : 0x06); // red / yellow ink
    }
    const auto palette = emulator.ula().palette();
    constexpr uint32_t y = UlaRenderer::kBorderTopLines;
    constexpr uint32_t left = UlaRenderer::kBorderLeftPx - 32;
    constexpr uint32_t right = UlaRenderer::kBorderLeftPx + UlaRenderer::kScreenWidthPx;

    auto frame = emulator.runFrames(1);
    assert(pixel(frame, left, y) == palette[0] && pixel(frame, right, y) == palette[0]); // the border

    emulator.ioBus().write(0x00FE, 0x60); // O5
    emulator.ioBus().write(0x00FE, 0x40); // the latch holds in CP/M
    frame = emulator.runFrames(1);
    assert(pixel(frame, left, y) == palette[6] && pixel(frame, left + 31, y) == palette[6]);
    assert(pixel(frame, right, y) == palette[2] && pixel(frame, right + 31, y) == palette[2]);
    assert(pixel(frame, left, y + 1) == palette[0]); // pixel line 1 is paper

    // The state survives a rewind: the configuration, the latch, the CTC
    emulator.ioBus().write(kCtc0, 0x98);
    emulator.ioBus().write(kCtc3, 0xC5);
    emulator.ioBus().write(kCtc3, 5);
    emulator.setRewindEnabled(true);
    emulator.runFrames(1); // a snapshot at the frame's end: CTC3 counted one frame
    emulator.reset();
    assert(cobra(emulator).poFlipFlop() == 1);
    emulator.rewind(1);
    assert(cobra(emulator).poFlipFlop() == 0 && cobra(emulator).lo6FlipFlop() == 1);
    assert(emulator.mmu().read(0x7800) == 0x02);
    assert(pixel(emulator.refreshFrame(), right, y) == palette[2]);
    emulator.runFrames(3);
    assert(emulator.im2().pendingMask() == 0);
    emulator.runFrames(1);
    assert(emulator.im2().pendingMask() == 0x8000);
}

// ── The CoBra 2 (Turbo Spectrum) ──────────────────────────────────────────

void testCobra2Modes()
{
    // Any IN loads F5's bits 5 (EEPROM disabled) and 6 (Spectrum mode) into the mode flops
    Emulator emulator({.machineId = "cobra2"});
    auto &io = emulator.ioBus();
    auto &mmu = emulator.mmu();
    auto &ram = mmu.bank(emulator.machine().ramBankId());
    ram.span16k(2)[0] = 0xA5; // DRAM 8000, the CPU's 0000 outside the Spectrum mode
    ram.span16k(0)[0] = 0x5A; // DRAM 0000, the Spectrum mode's read-only "ROM"
    ram.span16k(1)[0] = 0x11; // the video RAM: C000 outside the Spectrum mode, 4000 in it

    // EEPROM mode after a reset (no ROM loaded: zeros)
    assert(mmu.read(0x0000) == 0x00 && mmu.read(0xC000) == 0x11);
    // OUT alone changes nothing, the next IN (any port) disables the EEPROM: 64K DRAM mode
    io.write(0x00FE, 0x20);
    assert(mmu.read(0x0000) == 0x00);
    io.read(0x1235);
    assert(mmu.read(0x0000) == 0xA5);
    mmu.write(0x0001, 0x77);
    assert(ram.span16k(2)[1] == 0x77);
    // ... and back to the EEPROM (the boot copies the Spectrum ROM from it)
    io.write(0x00FE, 0x00);
    io.read(0x00FE);
    assert(mmu.read(0x0000) == 0x00);

    // The Spectrum mode: A15 is no longer inverted, the first 16K are read-only
    io.write(0x00FE, 0x60);
    io.read(0x60FE);
    assert(mmu.read(0x0000) == 0x5A && mmu.read(0x4000) == 0x11);
    mmu.write(0x0000, 0x00);
    assert(mmu.read(0x0000) == 0x5A);
    // ... until a reset: the Spectrum ROM's OUTs and keyboard reads don't leave it
    io.write(0x00FE, 0x07);
    io.read(0xFEFE);
    assert(mmu.read(0x0000) == 0x5A);
    emulator.reset();
    assert(mmu.read(0x0000) == 0x00);
}

void testCobra2Rewind()
{
    Emulator emulator({.machineId = "cobra2"});
    auto &io = emulator.ioBus();
    auto &mmu = emulator.mmu();
    auto &ram = mmu.bank(emulator.machine().ramBankId());
    for (std::size_t page = 0; page < 4; ++page)
        ram.span16k(page)[0] = static_cast<uint8_t>(0x10 + page);
    ram.span16k(0)[0x100] = 0x76; // HALT at 8100 in the DRAM map
    io.write(0x00FE, 0x20);
    io.read(0x1235);
    emulator.cpu().registers().PC = 0x8100;
    emulator.setRewindEnabled(true);
    emulator.runFrames(1);

    io.write(0x00FE, 0x60);
    io.read(0x1235);
    assert(mmu.read(0x4000) == 0x11);
    emulator.rewind(1);
    // All four windows must return to the saved map, including writable DRAM at 0000.
    assert(mmu.read(0x0000) == 0x12 && mmu.read(0x4000) == 0x13);
    assert(mmu.read(0x8000) == 0x10 && mmu.read(0xC000) == 0x11);
    mmu.write(0x0000, 0xA5);
    assert(mmu.read(0x0000) == 0xA5);
}

void testCobraBasicRewind()
{
    Emulator emulator({.machineId = "cobra1_80k"});
    emulator.mmu().poke(0x8100, 0x76);
    switchConfiguration(emulator, false, 0x0100);
    emulator.mmu().poke(0x6000, 0xFF);
    emulator.mmu().poke(0x7800, 0x02);
    emulator.setRewindEnabled(true);
    emulator.runFrames(1);

    emulator.reset();
    emulator.mmu().poke(0x8100, 0x76);
    switchConfiguration(emulator, true, 0x0100);
    emulator.mmu().poke(0x6000, 0xFF);
    emulator.mmu().poke(0x7800, 0x02);
    emulator.ioBus().write(0x00FE, 0x60);
    constexpr uint32_t x = UlaRenderer::kBorderLeftPx + UlaRenderer::kScreenWidthPx;
    constexpr uint32_t y = UlaRenderer::kBorderTopLines;
    assert(pixel(emulator.refreshFrame(), x, y) == emulator.ula().palette()[2]);
    emulator.rewind(1);
    assert(cobra(emulator).isSpectrumModeLocked());
    // A BASIC snapshot must stop drawing the CP/M screen's extra border cells.
    assert(pixel(emulator.refreshFrame(), x, y) == emulator.ula().palette()[0]);
}

// ── The real ROMs and disk (tests/fixtures) ──────────────────────────────

bool hasCpmFile(Emulator &emulator, std::string_view name83)
{
    // A directory entry of user 0: the 8.3 name, space padded ("X       COM")
    const DiskImage &disk = emulator.diskDrive(0)->get();
    for (uint8_t cylinder = 0; cylinder < 80; ++cylinder) {
        for (uint8_t side = 0; side < 2; ++side) {
            for (uint8_t id = 1; id <= 9; ++id) {
                const auto sector = disk.findSector(cylinder, side, id);
                if (!sector)
                    continue;
                const auto &data = sector->get().data;
                for (std::size_t entry = 0; entry + 32 <= data.size(); entry += 32) {
                    if (data[entry] == 0 && std::equal(name83.begin(), name83.end(), data.begin() + entry + 1))
                        return true;
                }
            }
        }
    }
    return false;
}

void testCobraDiskCpm(const std::filesystem::path &fixtures)
{
    // The 80K standard boot (ENTER at the boot manager), then D: CP/M from the KRYS80 system disk
    Emulator emulator(zxkit::test::romConfig(fixtures / "roms", "cobra1_80k"));
    emulator.insertDisk(0, zxkit::test::readFile(fixtures / "cobra" / "KRYS80_CL_SYSDISK.img"), ".img");
    emulator.runFrames(100);
    zxkit::test::tap(emulator, {ZxKey::Enter});
    emulator.runFrames(200);
    zxkit::test::tap(emulator, {ZxKey::D});
    emulator.runFrames(1200);
    // The BIOS is in: its warm boot jump at 0000, the CTC's interrupts in IM 2
    assert(emulator.mmu().read(0x0000) == 0xC3 && emulator.cpu().registers().IM == 2);
    assert(emulator.im2().inServiceMask() == 0);
    // The CCP saves a file: the disk's directory gets it
    assert(!hasCpmFile(emulator, "X       COM"));
    zxkit::test::type(emulator, "save 1 x.com\n");
    emulator.runFrames(300);
    assert(hasCpmFile(emulator, "X       COM"));
}

void testCobra64DiskLoader(const std::filesystem::path &fixtures)
{
    // No preserved 64K CP/M system disk is available. Exercise the original ROM's
    // SYS.COM loader with a diagnostic transient in both of its supported formats.
    for (const bool mfm : {false, true}) {
        Emulator emulator(test::romConfig(fixtures / "roms", "cobra1_64k"));
        auto &disk = emulator.diskDrive(0)->get();
        disk.formatBlank(77, 1, mfm ? 10 : 26, mfm ? 512 : 128);
        for (uint8_t track = 0; track < 77; ++track) {
            for (const auto &reference : disk.trackSectors(track, 0))
                disk.findSector(track, 0, reference.get().sectorId)->get().mfm = mfm;
        }
        auto &directory = disk.findSector(0, 0, 1)->get().data;
        std::fill_n(directory.begin(), 32, 0);
        constexpr std::string_view name = "SYS     COM";
        std::ranges::copy(name, directory.begin() + 1);
        directory[15] = 16;
        directory[16] = 2;
        directory[17] = 3;
        constexpr std::array<uint8_t, 10> mfmXlt{1, 4, 7, 10, 3, 6, 9, 2, 5, 8};
        constexpr std::array<uint8_t, 26> fmXlt{1, 7, 13, 19, 25, 5, 11, 17, 23, 3, 9, 15, 21,
                                               2, 8, 14, 20, 26, 6, 12, 18, 24, 4, 10, 16, 22};
        // DI; LD SP,8000; LD A,5A; LD (0120),A; HALT at the CP/M entry 0100.
        constexpr std::array<uint8_t, 10> program{0xF3, 0x31, 0, 0x80, 0x3E, 0x5A, 0x32, 0x20, 1, 0x76};
        for (unsigned record = 16; record < 32; record += mfm ? 4 : 1) {
            const unsigned logicalSector = mfm ? record / 4 : record;
            const unsigned perTrack = mfm ? 10 : 26;
            const auto id = mfm ? mfmXlt[logicalSector % perTrack] : fmXlt[logicalSector % perTrack];
            auto &data = disk.findSector(logicalSector / perTrack, 0, id)->get().data;
            std::ranges::fill(data, 0);
            if (record == 16)
                std::ranges::copy(program, data.begin());
        }
        emulator.runFrames(100);
        test::tap(emulator, {ZxKey::Enter});
        emulator.runFrames(200);
        test::tap(emulator, {ZxKey::D});
        emulator.runFrames(600);
        assert(cobra(emulator).poFlipFlop() == 0 && cobra(emulator).lo6FlipFlop() == 1);
        assert(emulator.cpu().registers().halted && emulator.cpu().registers().PC == 0x0109);
        assert(emulator.mmu().read(0x0120) == 0x5A);
    }
}

void testCobraBasicBoot(const std::filesystem::path &fixtures)
{
    for (const auto machine : {"cobra1_64k", "cobra1_80k"}) {
        Emulator emulator(test::romConfig(fixtures / "roms", machine));
        emulator.runFrames(100);
        test::tap(emulator, {ZxKey::B});
        emulator.runFrames(200);
        assert(cobra(emulator).isSpectrumModeLocked());
        assert((emulator.mmu().read(23635) | (emulator.mmu().read(23636) << 8)) == 23755);
    }
}

void testCobraRomCpm(const std::filesystem::path &fixtures)
{
    // CTRL+0 at the boot manager: the ROM's CP/M with an empty disk in drive A
    Emulator emulator(zxkit::test::romConfig(fixtures / "roms", "cobra1_80k"));
    emulator.insertDisk(0, std::vector<uint8_t>(80 * 2 * 9 * 512, 0xE5), ".img");
    emulator.runFrames(100);
    zxkit::test::tap(emulator, {ZxKey::Ctrl, ZxKey::Num0});
    emulator.runFrames(300);
    zxkit::test::type(emulator, "save 2 y.com\n");
    emulator.runFrames(300);
    assert(hasCpmFile(emulator, "Y       COM"));
}

void testDevilCatalog(const std::filesystem::path &fixtures)
{
    Emulator emulator(test::romConfig(fixtures / "roms", "cobra1_80k"));
    emulator.insertDisk(0, test::readFile(fixtures / "cobra/UTILS1.TD0"), ".td0");
    emulator.runFrames(100);
    test::tap(emulator, {ZxKey::D});
    emulator.runFrames(200);
    test::tap(emulator, {ZxKey::CapsShift, ZxKey::SymbolShift});
    test::tap(emulator, {ZxKey::SymbolShift, ZxKey::Num9}); // extended keyword CAT
    test::tap(emulator, {ZxKey::Enter});
    emulator.runFrames(500);
    const auto font = test::readFile(fixtures / "roms/cobra1_sys-2.rom");
    std::string text;
    for (unsigned row = 0; row < 24; ++row) {
        for (unsigned col = 0; col < 32; ++col) {
            char found = '?';
            for (unsigned c = 32; c < 127; ++c) {
                bool same = true;
                for (unsigned y = 0; y < 8; ++y) {
                    const unsigned sy = row * 8 + y;
                    const auto address = uint16_t(0x4000 | ((sy & 0xC0) << 5) | ((sy & 7) << 8) | ((sy & 0x38) << 2) | col);
                    same &= emulator.mmu().read(address) == font[0x3D00 + (c - 32) * 8 + y];
                }
                if (same) { found = char(c); break; }
            }
            text += found;
        }
        text += '\n';
    }
    assert(text.contains("Program: STAT"));
    assert(text.contains("Program: ARTSTUDIO"));
    assert(text.contains("Program: COPY/86M"));
    assert(text.contains("0 OK"));
}

void testCobra2Boot(const std::filesystem::path &fixtures)
{
    Emulator emulator(zxkit::test::romConfig(fixtures / "roms", "cobra2"));
    emulator.runFrames(150);
    // The 64K DRAM mode: 0000-3FFF is DRAM (page 2 at the CPU's 0000, A15 inverted), where the
    // boot keeps TURBO's shifted bitmaps, not the EEPROM; the logo is on the screen's first third
    // (the video RAM at C000)
    const auto &ram = emulator.mmu().bank(emulator.machine().ramBankId());
    bool shifted = false;
    for (uint16_t i = 0; i < 0x3000; ++i) {
        const uint8_t byte = emulator.mmu().read(i);
        assert(byte == ram.span16k(2)[i]);
        shifted = shifted || byte != 0;
    }
    assert(shifted);
    for (unsigned row = 2; row < 6; ++row)
        assert(zxkit::test::screenRowHasPixels(emulator, 0xC000, row));
    // A key (not T or S) copies the Spectrum ROM to DRAM and enters the Spectrum mode: BASIC
    // with the read-only ROM copy at 0000
    zxkit::test::tap(emulator, {ZxKey::Enter});
    emulator.runFrames(200);
    auto &mmu = emulator.mmu();
    assert(mmu.read(0x0000) == 0xF3);
    mmu.write(0x0000, 0x00);
    assert(mmu.read(0x0000) == 0xF3);
    assert((mmu.read(23635) | (mmu.read(23636) << 8)) == 23755); // PROG: BASIC's system variables
}

} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path fixtures = argc > 1 ? argv[1] : ZXKIT_TEST_FIXTURES_DIR;
    testCtcTimer();
    testCtcCounterAndInterrupts();
    testUpd765NonDmaInterrupt();
    testCobraKeyboard();
    testCobraFrameInterrupt();
    testCobraVerticalSyncCounter();
    testCobraFloppyRead();
    testCobraWideScreen();
    testCobraBasicRewind();
    testCobra2Modes();
    testCobra2Rewind();
    testCobraDiskCpm(fixtures);
    testCobra64DiskLoader(fixtures);
    testCobraBasicBoot(fixtures);
    testCobraRomCpm(fixtures);
    testDevilCatalog(fixtures);
    testCobra2Boot(fixtures);
    std::println("test_cobra: all tests passed");
    return 0;
}
