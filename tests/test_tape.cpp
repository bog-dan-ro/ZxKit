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

// Tape tests: TZX parsing, the signal of every block kind, the timeline in a running
// machine, and (when the ROMs are given) loading through the ROM, trapped and in real time.
//
//   test_tape [ROM_DIR]

#include <zxkit/zxkit.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

using namespace zxkit;

namespace {

using Transition = TapeEdge::Transition;
using Flags = TapeEdge::Flags;

// ── A TZX writer ──────────────────────────────────────────────────────────

class Tzx {
public:
    Tzx() { m_data = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20}; }

    Tzx &u8(uint32_t v)
    {
        m_data.push_back(static_cast<uint8_t>(v));
        return *this;
    }
    Tzx &u16(uint32_t v) { return u8(v).u8(v >> 8); }
    Tzx &u24(uint32_t v) { return u16(v).u8(v >> 16); }
    Tzx &u32(uint32_t v) { return u16(v).u16(v >> 16); }
    Tzx &bytes(std::span<const uint8_t> b)
    {
        m_data.insert(m_data.end(), b.begin(), b.end());
        return *this;
    }
    Tzx &text(std::string_view s)
    {
        for (char c : s)
            u8(static_cast<uint8_t>(c));
        return *this;
    }

    Tzx &standard(std::vector<uint8_t> data, uint16_t pauseMs)
    {
        return u8(0x10).u16(pauseMs).u16(static_cast<uint32_t>(data.size())).bytes(data);
    }

    const std::vector<uint8_t> &data() const { return m_data; }

private:
    std::vector<uint8_t> m_data;
};

std::vector<TapeEdge> allEdges(std::span<const TapeBlock> blocks, std::size_t limit = 1'000'000)
{
    TapeCursor cursor;
    std::vector<TapeEdge> edges;
    for (std::size_t i = 0; i < limit; ++i) {
        edges.push_back(cursor.next(blocks));
        if (edges.back().flags & Flags::TapeEnd)
            break;
    }
    return edges;
}

// Appends a bit's two pulses
void expectBits(std::vector<uint32_t> &lengths, uint8_t byte, int bits, uint32_t zero, uint32_t one)
{
    for (int i = 0; i < bits; ++i) {
        const uint32_t length = ((byte << i) & 0x80) ? one : zero;
        lengths.push_back(length);
        lengths.push_back(length);
    }
}

// ── Parsing and the signal ────────────────────────────────────────────────

void testTapFile()
{
    const std::vector<uint8_t> tap{3, 0, 0xFF, 0x12, 0xED, 2, 0, 0x00, 0x00};
    const auto blocks = TapeFormats::parseTap(tap);
    assert(blocks.size() == 2);
    assert(blocks[0].isRomBlock() && blocks[0].pilotPulses == 3223); // a data block
    assert(blocks[1].pilotPulses == 8063);                           // a header (flag < 0x80)
    assert(TapeFormats::writeTap(blocks) == tap);
}

void testStandardBlockSignal()
{
    const auto blocks = TapeFormats::parseTzx(Tzx().standard({0xFF, 0x80, 0x7F}, 1000).data());
    assert(blocks.size() == 1 && blocks[0].type == TapeBlockType::StandardData);
    const auto edges = allEdges(blocks);

    std::vector<uint32_t> expected(3223, 2168);
    expected.push_back(667);
    expected.push_back(735);
    for (uint8_t byte : {0xFF, 0x80, 0x7F})
        expectBits(expected, byte, 8, 855, 1710);
    expected.push_back(1000 * 3500);
    assert(edges.size() == expected.size());

    // A tape starts low (like libspectrum), then every edge toggles
    assert(edges[0].transition == Transition::Low && !edges[0].level);
    bool level = false;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        assert(edges[i].length == expected[i]);
        if (i > 0) {
            assert(edges[i].transition == Transition::Toggle);
            level = !level;
        }
        assert(edges[i].level == level);
        // The data pulses tell the acceleration what they are
        const bool data = i >= 3225 && i < edges.size() - 1;
        if (data)
            assert(edges[i].flags & (edges[i].length == 855 ? Flags::ShortPulse : Flags::LongPulse));
        else
            assert(!(edges[i].flags & (Flags::ShortPulse | Flags::LongPulse)));
    }
    assert(edges.back().flags & Flags::BlockEnd);
    assert(edges.back().flags & Flags::Stop);
    assert(edges.back().flags & Flags::TapeEnd);
}

void testTurboAndPureData()
{
    Tzx tzx;
    // Turbo: pilot 1000 x 4, sync 300/400, bits 200/500, 3 bits of the last byte, 5 ms pause
    tzx.u8(0x11).u16(1000).u16(300).u16(400).u16(200).u16(500).u16(4).u8(3).u16(5).u24(2).u8(0x0F).u8(0xA0);
    // Pure data: bits 100/150, 2 bits, no pause
    tzx.u8(0x14).u16(100).u16(150).u8(2).u16(0).u24(1).u8(0x40);
    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 2);
    assert(!blocks[0].isRomBlock());
    const auto edges = allEdges(blocks);

    std::vector<uint32_t> expected(4, 1000);
    expected.push_back(300);
    expected.push_back(400);
    expectBits(expected, 0x0F, 8, 200, 500);
    expectBits(expected, 0xA0, 3, 200, 500);
    expected.push_back(5 * 3500);
    expectBits(expected, 0x40, 2, 100, 150);
    expected.push_back(0); // the end of the tape closes the last pulse
    assert(edges.size() == expected.size());
    for (std::size_t i = 0; i < edges.size(); ++i)
        assert(edges[i].length == expected[i]);

    // After the turbo block's pause the pure data starts low
    const std::size_t pureStart = 4 + 2 + 22 + 1;
    assert(edges[pureStart - 1].flags & Flags::BlockEnd);
    assert(edges[pureStart].transition == Transition::Low);
    assert(!edges[pureStart].level);
    assert(edges.back().flags & Flags::TapeEnd);
}

void testToneAndPulses()
{
    Tzx tzx;
    tzx.u8(0x12).u16(2000).u16(3);                 // pure tone
    tzx.u8(0x13).u8(3).u16(10).u16(20).u16(30);    // pulse sequence
    tzx.u8(0x30).u8(4).text("note");               // a description: no signal
    tzx.u8(0x12).u16(77).u16(0);                   // an empty tone
    tzx.u8(0x12).u16(50).u16(1);
    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 5);
    assert(blocks[2].description == "note" && blocks[2].isMetadata());
    const auto edges = allEdges(blocks);
    const std::vector<uint32_t> expected{2000, 2000, 2000, 10, 20, 30, 50};
    assert(edges.size() == expected.size());
    for (std::size_t i = 0; i < edges.size(); ++i) {
        assert(edges[i].length == expected[i]);
        assert(edges[i].transition == (i == 0 ? Transition::Low : Transition::Toggle));
    }
    assert(edges[2].flags & Flags::BlockEnd);
    assert(edges[5].flags & Flags::BlockEnd);
    assert(edges[6].flags & Flags::TapeEnd);
}

void testDirectRecording()
{
    Tzx tzx;
    // 79 T-states per sample: 1111 0000 | 11 00 (4 bits used in the last byte)
    tzx.u8(0x15).u16(79).u16(0).u8(4).u24(2).u8(0xF0).u8(0xC0);
    const auto edges = allEdges(TapeFormats::parseTzx(tzx.data()));
    assert(edges.size() == 5);
    assert(edges[0].length == 4 * 79 && edges[0].transition == Transition::High && edges[0].level);
    assert(edges[1].length == 4 * 79 && edges[1].transition == Transition::Low && !edges[1].level);
    assert(edges[2].length == 2 * 79 && edges[2].level);
    assert(edges[3].length == 2 * 79 && !edges[3].level);
    assert(edges[4].length == 0 && (edges[4].flags & Flags::TapeEnd));
}

void testCswRecording()
{
    // RLE pulses at 35 kHz (100 T-states a sample): 10, 20, and a long 300 one
    const std::vector<uint8_t> rle{10, 20, 0, 0x2C, 0x01, 0x00, 0x00};
    Tzx tzx;
    tzx.u8(0x18).u32(10 + static_cast<uint32_t>(rle.size())).u16(2).u24(35'000).u8(1).u32(3).bytes(rle);
    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 1 && blocks[0].pulses.size() == 3);
    const auto edges = allEdges(blocks);
    assert(edges.size() == 4);
    assert(edges[0].length == 1000 && edges[0].transition == Transition::None); // keeps the level
    assert(edges[1].length == 2000 && edges[1].transition == Transition::Toggle);
    assert(edges[2].length == 30000);
    assert(edges[3].length == 2 * 3500 && edges[3].transition == Transition::Low); // the pause is low

    // Z-RLE: the same pulses, compressed
    const auto packed = zlibCompress(rle);
    Tzx ztzx;
    ztzx.u8(0x18).u32(10 + static_cast<uint32_t>(packed.size())).u16(0).u24(35'000).u8(2).u32(3).bytes(packed);
    assert(TapeFormats::parseTzx(ztzx.data())[0].pulses == blocks[0].pulses);
}

void testGeneralizedData()
{
    Tzx body;
    body.u16(0);                            // no pause
    body.u32(2).u8(2).u8(2);                // pilot stream: 2 runs, 2 pulses a symbol, 2 symbols
    body.u32(8).u8(2).u8(2);                // data: 8 symbols, 2 pulses each, 2 symbols
    body.u8(0).u16(2168).u16(0);            // pilot symbol 0: one pulse
    body.u8(0).u16(667).u16(735);           // pilot symbol 1: the sync pulses
    body.u8(0).u16(3).u8(1).u16(1);         // 3 x symbol 0, 1 x symbol 1
    body.u8(0).u16(855).u16(855);           // data symbol 0
    body.u8(3).u16(1710).u16(1710);         // data symbol 1 starts high
    body.u8(0xA5);                          // 8 symbols, a bit each
    std::vector<uint8_t> payload(body.data().begin() + 10, body.data().end());
    Tzx tzx;
    tzx.u8(0x19).u32(static_cast<uint32_t>(payload.size())).bytes(payload);

    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 1);
    const TapeBlock &block = blocks[0];
    assert(block.pilotSymbols.size() == 2 && block.pilotSymbols[0].pulses.size() == 1);
    assert(block.dataSymbols.size() == 2 && block.symbolBits() == 1 && block.dataSymbolCount == 8);

    const auto edges = allEdges(blocks);
    std::vector<uint32_t> expected{2168, 2168, 2168, 667, 735};
    expectBits(expected, 0xA5, 8, 855, 1710);
    expected.push_back(0);
    assert(edges.size() == expected.size());
    for (std::size_t i = 0; i < edges.size(); ++i)
        assert(edges[i].length == expected[i]);
    // A '1' forces the level high at its first pulse
    assert(edges[5].transition == Transition::High && edges[5].level);
    assert(edges[7].transition == Transition::Toggle);
}

void testControlBlocks()
{
    Tzx tzx;
    tzx.u8(0x24).u16(3);              // 0: loop 3 times
    tzx.u8(0x12).u16(100).u16(1);     // 1
    tzx.u8(0x25);                     // 2: loop end
    tzx.u8(0x23).u16(2);              // 3: jump over the next block
    tzx.u8(0x12).u16(999).u16(1);     // 4: skipped
    tzx.u8(0x26).u16(2).u16(3).u16(5); // 5: call blocks 8 and 10
    tzx.u8(0x20).u16(0);              // 6: stop the tape
    tzx.u8(0x23).u16(4);              // 7: jump to the end
    tzx.u8(0x12).u16(200).u16(1).u8(0x27); // 8, 9: tone, return
    tzx.u8(0x12).u16(300).u16(1).u8(0x27); // 10, 11: tone, return
    tzx.u8(0x2A).u32(0);              // 12: stop in 48K mode
    tzx.u8(0x2B).u32(1).u8(1);        // 13: level high
    tzx.u8(0x12).u16(400).u16(1);     // 14
    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 15);

    TapeCursor cursor;
    const auto next = [&cursor, &blocks] { return cursor.next(blocks); };
    for (int i = 0; i < 3; ++i)
        assert(next().length == 100);
    assert(next().length == 200);
    assert(next().length == 300);
    const TapeEdge stop = next();
    assert((stop.flags & Flags::Stop) && stop.transition == Transition::None);
    assert(cursor.blockIndex() == 7);
    const TapeEdge stop48 = next();
    assert((stop48.flags & Flags::Stop48k) && !(stop48.flags & Flags::Stop));
    const TapeEdge high = next();
    assert(high.length == 0 && high.transition == Transition::High && high.level);
    const TapeEdge last = next();
    assert(last.length == 400 && !last.level && (last.flags & Flags::TapeEnd));

    // Jumping backwards forever must not hang
    Tzx loop;
    loop.u8(0x30).u8(1).text("x").u8(0x23).u16(0xFFFF);
    const auto loopBlocks = TapeFormats::parseTzx(loop.data());
    TapeCursor stuck;
    assert(stuck.next(loopBlocks).flags & Flags::TapeEnd);
}

void testMetadataAndTruncation()
{
    Tzx tzx;
    tzx.u8(0x21).u8(5).text("Group").u8(0x22);
    tzx.u8(0x31).u8(5).u8(3).text("Hi!");
    tzx.u8(0x32).u16(8).u8(1).u8(0).u8(5).text("Title");
    tzx.u8(0x33).u8(1).u8(0).u8(1).u8(0);
    tzx.u8(0x35).text("CUSTOM INFO     ").u32(2).u16(0);
    tzx.u8(0x5A).text("XTape!").u8(0x1A).u8(1).u8(20);
    tzx.u8(0x4B).u32(3).u8(1).u8(2).u8(3);  // an unsupported block, skipped by its length
    tzx.standard({0x00, 1, 2}, 100);
    const auto blocks = TapeFormats::parseTzx(tzx.data());
    assert(blocks.size() == 9);
    assert(blocks[0].description == "Group: Group");
    assert(blocks[2].description == "Hi!");
    assert(blocks[3].description == "Archive info: Title");
    assert(blocks[7].description.starts_with("Unsupported block"));
    assert(blocks[8].isRomBlock());

    // A truncated last block: the blocks before it stay
    auto truncated = tzx.data();
    truncated.push_back(0x10);
    truncated.push_back(0x00);
    assert(TapeFormats::parseTzx(truncated).size() == 9);
    bool threw = false;
    try {
        TapeFormats::parseTzx(std::vector<uint8_t>{'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20, 0x10, 0x00});
    } catch (const FormatException &) {
        threw = true;
    }
    assert(threw);
}

// ── The timeline in a running machine ─────────────────────────────────────

// At 0x8000: counts the 40 T-state iterations of a loop between two EAR edges, and stores
// the counts at 0x9000 (16 bit each)
constexpr std::array<uint8_t, 27> kEdgeTimer{
    0xF3,             // DI
    0x21, 0x00, 0x90, // LD HL,9000h
    0x11, 0x00, 0x00, // LD DE,0
    0x0E, 0x00,       // LD C,0
    0x13,             // loop: INC DE        6
    0xDB, 0xFE,       //       IN A,(FEh)   11
    0xE6, 0x40,       //       AND 40h       7
    0xB9,             //       CP C          4
    0x28, 0xF8,       //       JR Z,loop    12
    0x4F,             // LD C,A
    0x73, 0x23,       // LD (HL),E; INC HL
    0x72, 0x23,       // LD (HL),D; INC HL
    0x11, 0x00, 0x00, // LD DE,0
    0x18, 0xEE,       // JR loop
};

std::vector<uint32_t> measurePulses(Emulator &emulator, std::size_t count)
{
    const auto memory = emulator.readMemory(0x9000, count * 2);
    std::vector<uint32_t> out;
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(memory[i * 2] | (memory[i * 2 + 1] << 8));
    return out;
}

void testTimeline(CpuSpeed speed)
{
    // The Pentagon: no contention, and a 3.584 MHz clock the pulses are scaled to
    Emulator emulator({.machineId = "pentagon"});
    emulator.writeMemory(0x8000, kEdgeTimer);
    Z80Registers regs = emulator.registers();
    regs.PC = 0x8000;
    regs.SP = 0x7FF0;
    emulator.setRegisters(regs);
    emulator.clock().setSpeed(speed);

    Tzx tzx;
    tzx.u8(0x13).u8(6).u16(3000).u16(1000).u16(4000).u16(2000).u16(6000).u16(5000);
    tzx.u8(0x20).u16(0);
    emulator.loadTape(tzx.data(), ".tzx");
    emulator.tapeDeck().setOptions({.romTraps = false, .autoPlay = false, .accelerateLoaders = false});
    emulator.tapeDeck().setPlaying(true);
    emulator.runFrames(5);
    assert(!emulator.tapeDeck().isPlaying()); // the stop block stopped it

    const double cpuPerTape = 3.584 / 3.5 * (1u << static_cast<uint8_t>(speed));
    const auto counts = measurePulses(emulator, 6);
    const std::array<uint32_t, 6> pulses{3000, 1000, 4000, 2000, 6000, 5000};
    // The tape starts low at the first read: the first change ends pulse 0, count i measures pulse i
    assert(std::abs(counts[0] * 40.0 - pulses[0] * cpuPerTape) <= 60.0);
    for (std::size_t i = 1; i < pulses.size(); ++i) {
        // An edge costs 47 T-states more than a loop iteration
        const double measured = counts[i] * 40.0 + 47.0;
        const double expected = pulses[i] * cpuPerTape;
        assert(std::abs(measured - expected) <= 45.0);
    }
}

void testLoadingSound()
{
    Emulator emulator({.machineId = "pentagon"});
    Tzx tzx;
    tzx.u8(0x12).u16(3500).u16(400); // a 500 Hz tone for 0.4 s
    emulator.loadTape(tzx.data(), ".tzx");
    emulator.tapeDeck().setOptions({.romTraps = false, .autoPlay = false, .accelerateLoaders = false});
    emulator.tapeDeck().setPlaying(true);
    emulator.runFrames(2);
    const auto samples = emulator.audioSamples();
    int16_t lo = 0;
    int16_t hi = 0;
    for (int16_t s : samples) {
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    assert(hi - lo > 4000); // the tone is heard

    emulator.tapeDeck().setOptions({.romTraps = false, .autoPlay = false, .accelerateLoaders = false,
                                    .loadingSound = false});
    emulator.runFrames(3);
    const auto quiet = emulator.audioSamples();
    lo = hi = quiet[quiet.size() / 2];
    for (std::size_t i = quiet.size() / 2; i < quiet.size(); ++i) {
        lo = std::min(lo, quiet[i]);
        hi = std::max(hi, quiet[i]);
    }
    assert(hi - lo < 200); // and then it isn't
}

// The ROM trap, against a blank ROM holding only the LD-BYTES signature
void testRomTrap()
{
    Emulator emulator({.machineId = "48k"});
    emulator.writeMemory(0x0562, {0xDB, 0xFE, 0x1F, 0xE6, 0x20, 0xF6, 0x02, 0x4F, 0xBF, 0xC0}, true);
    emulator.loadTape(std::vector<uint8_t>{5, 0, 0xFF, 1, 2, 3, 0xFF ^ 1 ^ 2 ^ 3}, ".tap");

    emulator.writeMemory(0x7FF0, {0x00, 0x90}); // the return address
    Z80Registers regs = emulator.registers();
    regs.PC = 0x056B;
    regs.SP = 0x7FF0;
    regs.IX = 0xA000;
    regs.DE = 3;
    regs.AF_ = 0xFF01; // flag 0xFF, load
    emulator.setRegisters(regs);
    emulator.stepInstruction();

    const auto after = emulator.registers();
    assert(after.PC == 0x9000);
    assert(after.F() & Z80Flags::C); // loaded
    assert(after.IX == 0xA003 && after.DE == 0);
    assert(emulator.readMemory(0xA000, 3) == (std::vector<uint8_t>{1, 2, 3}));
    assert(emulator.tapeDeck().currentBlockIndex() == 1);

    // The wrong flag: no carry, nothing loaded
    emulator.tapeDeck().rewind();
    regs.AF_ = 0x0001;
    regs.IX = 0xB000;
    emulator.setRegisters(regs);
    emulator.stepInstruction();
    assert(!(emulator.registers().F() & Z80Flags::C));
    assert(emulator.readMemory(0xB000, 1)[0] == 0);
}

// ── Media activity ────────────────────────────────────────────────────────

void testMediaActivity()
{
    // The HC-2000 has two floppy drives
    Emulator emulator({.machineId = "hc2000"});
    assert(emulator.diskDriveCount() == 2);
    assert(emulator.mediaActivity() == MediaActivity{});

    emulator.diskDrive(1)->get().noteAccess();
    emulator.runFrames(1);
    assert(emulator.mediaActivity().busyDisks == 0x02);
    emulator.runFrames(5);
    assert(emulator.mediaActivity().busyDisks == 0x02); // held for a while
    emulator.runFrames(10);
    assert(emulator.mediaActivity().busyDisks == 0);

    // The SD card
    const auto image = std::filesystem::temp_directory_path() / "zxkit_test_tape_sd.img";
    {
        std::ofstream f(image, std::ios::binary);
        const std::vector<char> zeros(64 * 1024, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    emulator.insertSdCard(image, true);
    std::array<uint8_t, SdCard::kBlockSize> block{};
    emulator.sdCard().readBlock(3, block);
    emulator.runFrames(1);
    assert(emulator.mediaActivity().sdCardBusy);
    emulator.ejectSdCard();
    std::filesystem::remove(image);

    // The tape
    emulator.loadTape(Tzx().standard({0xFF, 1, 2}, 0).standard({0xFF, 3, 4}, 0).data(), ".tzx");
    emulator.tapeDeck().seekBlock(1);
    emulator.tapeDeck().setPlaying(true);
    const MediaActivity activity = emulator.mediaActivity();
    assert(activity.tapePlaying && activity.tapeBlock == 1);

    // The Engine reports the changes
    std::atomic<int> reports{0};
    std::atomic<bool> playing{false};
    CallbackHandle<MediaActivityCallback> onActivity([&](const MediaActivity &a) {
        playing = a.tapePlaying;
        ++reports;
    });
    Engine engine({.machineId = "48k"}, {.callbacks = {.mediaActivity = onActivity.weak()}});
    engine.emulator().writeLock()->loadTape(Tzx().standard({0xFF, 1, 2}, 0).data(), ".tzx");
    engine.emulator().writeLock()->tapeDeck().setPlaying(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!playing && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(playing && reports == 1);
    engine.emulator().writeLock()->tapeDeck().setPlaying(false);
    while (playing && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!playing && reports == 2);
}

// ── Loading through the real ROM ──────────────────────────────────────────

void tap(Emulator &emulator, std::initializer_list<ZxKey> keys)
{
    for (ZxKey k : keys)
        emulator.setKeyState(k, true);
    emulator.runFrames(4);
    for (ZxKey k : keys)
        emulator.setKeyState(k, false);
    emulator.runFrames(4);
}

std::vector<uint8_t> basicProgram(std::size_t remLength)
{
    // 10 REM xxxx...
    std::vector<uint8_t> line{0xEA};
    for (std::size_t i = 0; i < remLength; ++i)
        line.push_back(static_cast<uint8_t>('A' + i % 26));
    line.push_back(0x0D);
    std::vector<uint8_t> program{0x00, 0x0A, static_cast<uint8_t>(line.size()), static_cast<uint8_t>(line.size() >> 8)};
    program.insert(program.end(), line.begin(), line.end());
    return program;
}

std::vector<uint8_t> romBlock(uint8_t flag, std::span<const uint8_t> payload)
{
    std::vector<uint8_t> block{flag};
    block.insert(block.end(), payload.begin(), payload.end());
    uint8_t parity = 0;
    for (uint8_t b : block)
        parity ^= b;
    block.push_back(parity);
    return block;
}

std::vector<uint8_t> programTap(std::span<const uint8_t> program)
{
    std::vector<uint8_t> header{0x00};
    for (char c : std::string_view("tapetest  "))
        header.push_back(static_cast<uint8_t>(c));
    const auto length = static_cast<uint16_t>(program.size());
    header.insert(header.end(), {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), 0x00, 0x80,
                                 static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8)});
    std::vector<uint8_t> tap;
    for (const auto &block : {romBlock(0x00, header), romBlock(0xFF, program)}) {
        tap.push_back(static_cast<uint8_t>(block.size()));
        tap.push_back(static_cast<uint8_t>(block.size() >> 8));
        tap.insert(tap.end(), block.begin(), block.end());
    }
    return tap;
}

// Types LOAD "" on a 48K, and returns the frames it took to load the program
uint32_t loadThroughRom(const std::filesystem::path &romDir, const TapeOptions &options,
                        std::span<const uint8_t> program)
{
    EmulatorConfig config;
    config.machineId = "48k";
    config.romResolver = [romDir](std::string_view name, std::size_t) {
        std::ifstream f(romDir / name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    };
    Emulator emulator(std::move(config));
    emulator.tapeDeck().setOptions(options);
    emulator.runFrames(100);
    emulator.loadTape(programTap(program), ".tap");
    tap(emulator, {ZxKey::J});
    tap(emulator, {ZxKey::SymbolShift, ZxKey::P});
    tap(emulator, {ZxKey::SymbolShift, ZxKey::P});
    emulator.setKeyState(ZxKey::Enter, true);
    emulator.runFrames(4);
    emulator.setKeyState(ZxKey::Enter, false);

    uint32_t frames = 0;
    const auto loaded = [&] {
        const auto prog = emulator.readMemory(0x5C53, 2);
        const auto at = static_cast<uint16_t>(prog[0] | (prog[1] << 8));
        return emulator.readMemory(at, program.size()) == std::vector<uint8_t>(program.begin(), program.end());
    };
    while (frames < 5000 && !(loaded() && !emulator.tapeDeck().isPlaying())) {
        emulator.runFrames(10);
        frames += 10;
    }
    assert(loaded());
    // The ROM reported "0 OK": the error number is 0xFF
    emulator.runFrames(50);
    assert(emulator.readMemory(0x5C3A, 1)[0] == 0xFF);
    assert(emulator.tapeDeck().currentBlockIndex() == 2);
    return frames;
}

void testRomLoading(const std::filesystem::path &romDir)
{
    const auto program = basicProgram(1500);
    const uint32_t trapped = loadThroughRom(romDir, {}, program);
    const uint32_t accelerated = loadThroughRom(romDir, {.romTraps = false}, program);
    const uint32_t realTime = loadThroughRom(romDir, {.romTraps = false, .accelerateLoaders = false}, program);
    std::println("  ROM loads: trapped {} frames, accelerated {}, real time {}", trapped, accelerated, realTime);
    assert(trapped <= 10);
    assert(accelerated < realTime);
    // Real time: two pilots, a header and 1.5 KB at the ROM's speed, and a second of pause
    assert(realTime > 500 && realTime < 1000);
}

} // namespace

void testBootActions(const std::filesystem::path &romDir)
{
    const auto resolver = [romDir](std::string_view name, std::size_t) {
        std::ifstream file(romDir / name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    };
    const auto program = basicProgram(32);
    auto tape = TapeFormats::parseTap(programTap(program));
    tape.insert(tape.begin(), TapeBlock::standard({0xFF,0xFF}));
    EmulatorConfig config; config.machineId = "48k"; config.romResolver = resolver;
    Emulator emulator(config);
    emulator.tapeDeck().load(std::move(tape));
    assert(emulator.autoLoadTape(1));
    assert(emulator.clock().frameCount() == 0 && emulator.tapeDeck().currentBlockIndex() == 1);
    emulator.runFrames(250);
    const auto pointers = emulator.readMemory(0x5C53,2);
    const auto at = static_cast<uint16_t>(pointers[0] | (pointers[1] << 8));
    assert(emulator.readMemory(at,program.size()) == program);
    assert(emulator.tapeDeck().currentBlockIndex() == 3);

    config.machineId = "plus3";
    Emulator plus3(config);
    assert(!plus3.autoBootDisk());
    plus3.diskDrive(0)->get().formatBlank(40,1,9,512,1);
    assert(plus3.autoBootDisk());
    assert(plus3.clock().frameCount() == 0);
    bool accessed = false;
    for (unsigned i = 0; i < 220; ++i) {
        plus3.runFrames(1);
        accessed |= plus3.mediaActivity().busyDisks != 0;
    }
    assert(accessed); // The actual +3 ROM's Loader accessed drive A without host-side frame execution.

    Emulator pentagon({.machineId="pentagon"});
    pentagon.diskDrive(0)->get().formatBlank(80,2,16,256,1);
    assert(pentagon.autoBootDisk());
    assert(pentagon.registers().PC == 0 && pentagon.machine().lastPort7ffd() == 0x10);
    assert(static_cast<PentagonMachine &>(pentagon.machine()).beta().isPaged());
    assert(pentagon.clock().frameCount() == 0);
    std::println("  Selected-block tape auto-load and +3/TR-DOS boot actions: OK");
}

// Compare the physical signal, coalescing zero-time transitions and equal-level runs.
std::vector<std::pair<uint64_t, bool>> waveform(std::span<const TapeBlock> blocks)
{
    std::vector<std::pair<uint64_t, bool>> runs;
    for (const auto &edge : allEdges(blocks)) {
        if (!edge.length) continue;
        if (!runs.empty() && runs.back().second == edge.level) runs.back().first += edge.length;
        else runs.emplace_back(edge.length, edge.level);
    }
    return runs;
}

void testPzxAndCsw()
{
    StateWriter pzx;
    const auto chunk = [&](std::string_view id, std::initializer_list<uint8_t> data) {
        pzx.writeTag(id); pzx.write<uint32_t>(data.size()); pzx.writeBytes(data);
    };
    chunk("PZXT", {1, 0});
    chunk("PULS", {3, 0x80, 10, 0, 0, 0, 20, 0}); // 3x10, zero, 20.
    chunk("DATA", {2, 0, 0, 0x80, 7, 0, 2, 2, 5, 0, 0, 0, 0, 0, 9, 0, 0x40});
    chunk("PAUS", {30, 0, 0, 0});
    chunk("STOP", {1, 0});
    auto blocks = TapeFormats::parsePzx(pzx.data());
    const std::vector<std::pair<uint64_t, bool>> expected{{10,false},{10,true},{30,false},
                                                        {5,true},{9,false},{7,true},{30,false}};
    assert(waveform(blocks) == expected);
    const auto exported = TapeFormats::parseTzx(TapeFormats::writeTzx(blocks));
    assert(waveform(exported) == expected);
    assert(exported.back().type == TapeBlockType::StopIf48k);
    auto bad = pzx.takeBuffer(); bad.pop_back();
    bool failed = false;
    try { (void)TapeFormats::parsePzx(bad); } catch (const FormatException &) { failed = true; }
    assert(failed);

    for (const uint8_t compression : {1, 2}) {
        StateWriter csw;
        csw.writeTag(std::string_view("Compressed Square Wave\x1A", 23));
        csw.write<uint8_t>(2); csw.write<uint8_t>(0); csw.write<uint32_t>(44100);
        csw.write<uint32_t>(3); csw.write(compression); csw.write<uint8_t>(1); csw.write<uint8_t>(0);
        csw.writeTag(std::string(16, '\0'));
        const std::vector<uint8_t> rle{1, 2, 0, 0x2C, 1, 0, 0}; // 1, 2, 300 samples.
        csw.writeBytes(compression == 1 ? rle : zlibCompress(rle));
        const auto tape = TapeFormats::parseCsw(csw.data());
        const std::vector<std::pair<uint64_t, bool>> signal{{79,true},{159,false},{23809,true}};
        assert(waveform(tape) == signal); // Cumulative conversion, without per-pulse rounding drift.
        assert(waveform(TapeFormats::parseTzx(TapeFormats::writeTzx(tape))) == signal);
        Emulator emu;
        emu.loadTape(csw.data(), ".csw");
        assert(emu.tapeDeck().blocks().size() == 2);
        auto malformed = csw.takeBuffer(); malformed[29] = 4;
        failed = false;
        try { (void)TapeFormats::parseCsw(malformed); } catch (const FormatException &) { failed = true; }
        assert(failed);
    }
    // Truncated RLE and compression bombs fail explicitly, never a partial tape.
    failed = false;
    try { (void)zlibDecompressBounded(zlibCompress(std::vector<uint8_t>(10000, 0)), 100); }
    catch (const FormatException &) { failed = true; }
    assert(failed);
    failed = false;
    try { (void)TapeFormats::writeTap(blocks); } catch (const FormatException &) { failed = true; }
    assert(failed);
    std::println("  PZX/CSW waveforms, polarity, export and invalid inputs: OK");
}

void testTzxExport()
{
    Tzx source;
    source.u8(0x21).u8(4).text("Test")
          .u8(0x24).u16(2)
          .u8(0x12).u16(100).u16(3)
          .u8(0x25)
          .u8(0x22)
          .u8(0x30).u8(4).text("Text")
          .standard({0xFF, 0x42, 0xBD}, 1000);
    const auto tape = TapeFormats::parseTzx(source.data());
    assert(TapeFormats::writeTzx(tape) == source.data());
    assert(waveform(TapeFormats::parseTzx(TapeFormats::writeTzx(tape))) == waveform(tape));
    // A waveform expands into three blocks; relative jumps still reach the original target.
    TapeBlock jump; jump.type = TapeBlockType::JumpTo; jump.value = 2;
    TapeBlock pulses; pulses.type = TapeBlockType::PzxPulses; pulses.pulses = {12,3};
    TapeBlock tone; tone.type = TapeBlockType::PureTone; tone.pilotPulse = 33; tone.pilotPulses = 2;
    const std::vector<TapeBlock> control{jump, pulses, tone};
    const auto roundtrip = TapeFormats::parseTzx(TapeFormats::writeTzx(control));
    assert(roundtrip[0].value == 4);
    assert(waveform(roundtrip) == waveform(control));
    std::println("  TZX signal, control targets and metadata export: OK");
}

int main(int argc, char **argv)
{
    testPzxAndCsw();
    testTzxExport();
    testTapFile();
    testStandardBlockSignal();
    testTurboAndPureData();
    testToneAndPulses();
    testDirectRecording();
    testCswRecording();
    testGeneralizedData();
    testControlBlocks();
    testMetadataAndTruncation();
    testTimeline(CpuSpeed::Mhz3_5);
    testTimeline(CpuSpeed::Mhz7);
    testLoadingSound();
    testRomTrap();
    testMediaActivity();

    const std::filesystem::path romDir = argc > 1 ? argv[1] : ZXKIT_TEST_ROM_DIR;
    assert(std::filesystem::file_size(romDir / "48.rom") == 16384);
    testBootActions(romDir);
    testRomLoading(romDir);

    std::println("test_tape: all tests passed");
    return EXIT_SUCCESS;
}
