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

#include <atomic>
#include <cassert>
#include <chrono>
#include <span>
#include <thread>
#include <print>
#include <vector>

using namespace zxkit;

namespace {

void testZ80nOpcodes()
{
    Clock clock;
    Mmu mmu;
    const BankId ram = mmu.registerBank("ram", 64 * 1024, true, true);
    mmu.mapPages1k(0x0000, ram, 0, 64, true, true, false);
    ContentionModel contention;
    IoBus ioBus;
    Im2Controller im2;
    Z80Cpu cpu(clock, mmu, contention, ioBus, im2);
    cpu.setZ80nEnabled(true);

    // 1. SWAPNIB (ED 23), MIRROR A (ED 24), MUL D,E (ED 30), ADD HL,nn (ED 34), PUSH nn (ED 8A)
    const std::vector<uint8_t> prog = {
        0xED, 0x23,             // SWAPNIB
        0xED, 0x24,             // MIRROR A
        0xED, 0x30,             // MUL D,E
        0xED, 0x34, 0x34, 0x12, // ADD HL, $1234
        0xED, 0x8A, 0xCA, 0xFE  // PUSH $CAFE (big-endian immediate)
    };
    for (std::size_t i = 0; i < prog.size(); ++i)
        mmu.poke(static_cast<uint16_t>(0x8000 + i), prog[i]);

    auto &r = cpu.registers();
    r.PC = 0x8000;
    r.SP = 0xC000;
    r.setA(0x12);
    r.setD(6);
    r.setE(7);
    r.HL = 0x1000;

    cpu.stepInstruction(); // SWAPNIB: 0x12 -> 0x21
    assert(r.A() == 0x21);

    cpu.stepInstruction(); // MIRROR A: 0x21 (0010_0001) -> 0x84 (1000_0100)
    assert(r.A() == 0x84);

    cpu.stepInstruction(); // MUL D,E: 6 * 7 = 42
    assert(r.DE == 42);

    cpu.stepInstruction(); // ADD HL, $1234 -> 0x2234, WZ = 0x1234
    assert(r.HL == 0x2234);
    assert(r.WZ == 0x1234);

    cpu.stepInstruction(); // PUSH $CAFE -> SP = 0xBFFE, (0xBFFE)=0xFE, (0xBFFF)=0xCA
    assert(r.SP == 0xBFFE);
    assert(mmu.peek(0xBFFE) == 0xFE);
    assert(mmu.peek(0xBFFF) == 0xCA);

    // 2. LDIRX (ED B4) block copy with transparency in A
    mmu.poke(0x9000, 0x11);
    mmu.poke(0x9001, 0xE3); // Transparent byte matching A
    mmu.poke(0x9002, 0x22);
    mmu.poke(0xA000, 0xAA);
    mmu.poke(0xA001, 0xBB);
    mmu.poke(0xA002, 0xCC);

    mmu.poke(0x8100, 0xED);
    mmu.poke(0x8101, 0xB4); // LDIRX
    r.PC = 0x8100;
    r.HL = 0x9000;
    r.DE = 0xA000;
    r.BC = 3;
    r.setA(0xE3);

    while (r.BC != 0)
        cpu.stepInstruction();

    assert(mmu.peek(0xA000) == 0x11);
    assert(mmu.peek(0xA001) == 0xBB); // Transparent byte skipped!
    assert(mmu.peek(0xA002) == 0x22);
}

void testRomanianCpmBankingAndIf1Fdc()
{
    // Test HC-2000 CP/M memory banking flips (0x7E, 0xC5/0xC7 A13 pull-high)
    EmulatorConfig cfg;
    cfg.machineId = "hc2000";
    Emulator emulator(cfg);

    {
        auto &eng = emulator;
        auto &mmu = eng.mmu();
        auto &rom = mmu.bank(eng.machine().romBankId());
        auto &ram = mmu.bank(eng.machine().ramBankId());

        // Populate BASIC ROM (page 0) and CP/M ROM (page 1, upper 8K at offset 24K)
        rom.span16k(0)[0x0000] = 0xF3; // BASIC start byte
        rom.span8k(3)[0x0000]  = 0xC9; // CP/M upper 8K ROM signature at 0xE000

        // Populate RAM page 0 and RAM page 3 (lower 8K at 0xC000, upper 8K at 0xE000)
        ram.span8k(0)[0x0000] = 0x42; // RAM page 0 byte
        ram.span8k(6)[0x0010] = 0xAA; // RAM page 3 lower 8K (0xC010)
        ram.span8k(7)[0x0010] = 0x55; // RAM page 3 upper 8K (0xE010)

        // Initially in BASIC mode: 0x0000 reads BASIC ROM
        assert(mmu.read(0x0000) == 0xF3);
        assert(eng.ula().vramPage16k() == 1);

        // Write 0x0B to port 0x7E: Select CP/M ROM (bit 0), CP/M map (bit 1), VRAM at 0xC000 (bit 3)
        eng.ioBus().write(0x007E, 0x0B);

        // Now 0x0000 reads RAM page 0, 0xE000 reads CP/M ROM upper 8K, VRAM is page 3!
        assert(mmu.read(0x0000) == 0x42);
        assert(mmu.read(0xE000) == 0xC9);
        assert(eng.ula().vramPage16k() == 3);
        assert(mmu.read(0xC010) == 0xAA);

        // Strobe port 0xC7: pulls A13 high in 0xC000..0xDFFF so 0xC010 aliases 0xE010 RAM!
        eng.ioBus().write(0x00C7, 0x00);
        assert(mmu.read(0xC010) == 0x55);

        // Strobe port 0xC5: restores normal A13 decoding in 0xC000..0xDFFF
        eng.ioBus().write(0x00C5, 0x00);
        assert(mmu.read(0xC010) == 0xAA);
    }

    // Test CoBra 1 R7 falling-edge flip-flop banking transition
    emulator.selectMachine("cobra1_80k");
    {
        auto &eng = emulator;
        auto &cobra = dynamic_cast<CobraMachine &>(eng.machine());
        assert(cobra.poFlipFlop() == 1);
        assert(cobra.lo6FlipFlop() == 1);

        // Assemble `LD A,$80; LD R,A; LD A,$00; LD R,A` at 0x8000 to generate a falling edge on R7
        eng.mmu().poke(0x8000, 0x3E); eng.mmu().poke(0x8001, 0x80); // LD A, $80
        eng.mmu().poke(0x8002, 0xED); eng.mmu().poke(0x8003, 0x4F); // LD R, A (R7 = 1)
        eng.mmu().poke(0x8004, 0x3E); eng.mmu().poke(0x8005, 0x00); // LD A, $00
        eng.mmu().poke(0x8006, 0xED); eng.mmu().poke(0x8007, 0x4F); // LD R, A (R7 -> 0 falling edge!)

        // Enable 8255 Port C output and set bit 6 = 1 (CP/M mode)
        eng.ioBus().write(0x00DF, 0x80);
        eng.ioBus().write(0x00FE, 0x40);

        eng.cpu().registers().PC = 0x8000;
        eng.cpu().stepInstruction();
        eng.cpu().stepInstruction();
        assert(cobra.poFlipFlop() == 1);
        eng.mmu().poke(0x8008, 0x00);                               // NOP
        eng.cpu().stepInstruction();
        eng.cpu().stepInstruction();
        // R7 fell to 0, but the flip-flop clocks after the next opcode fetch (the ROMs switch
        // with LD R,A ; JP (HL), the jump still fetched from the old map)
        assert(cobra.poFlipFlop() == 1);
        eng.cpu().stepInstruction();
        // R7 fell to 0 while Port C bit 6 was 1 -> PO=0, LO6=1 (CP/M 80K all-RAM mode!)
        assert(cobra.poFlipFlop() == 0);
        assert(cobra.lo6FlipFlop() == 1);
        assert(!cobra.isSpectrumModeLocked());
    }

    // Test the HC-2000 built-in HC Interface 1 uPD765 FDC: 1KB RAM overlay + uPD765 sector read
    emulator.selectMachine("hc2000");
    {
        auto &eng = emulator;
        auto &hc = dynamic_cast<HcMachine &>(eng.machine());
        DiskImage &disk = eng.diskDrive(0).value();
        assert(&disk == &hc.fdcInterface().fdc().drive(0));
        disk.formatBlank(40, 1, 9, 512, 1);
        disk.findSector(0, 0, 2)->get().data[0] = 0x77;

        // Activate /ROMCS for hc_if1_fdc and verify the 1KB RAM at 0x2800 and its 0x3800 mirror
        eng.romcs().setActive("hc_if1_fdc", true);
        eng.mmu().write(0x2805, 0x99);
        assert(eng.mmu().read(0x3805) == 0x99);
        assert(eng.mmu().read(0x2C05) == 0x99);
        eng.romcs().setActive("hc_if1_fdc", false);

        // RECALIBRATE, SENSE INTERRUPT, then READ DATA C=0 H=0 R=2 N=2 EOT=2
        auto &io = eng.ioBus();
        io.write(0x0087, 0x07);
        io.write(0x0087, 0x00);
        eng.clock().advance(1000);
        io.write(0x0087, 0x08);
        assert(io.read(0x0087) == 0x20);
        assert(io.read(0x0087) == 0x00);
        for (uint8_t b : {0x46, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x2A, 0xFF})
            io.write(0x0087, b);
        const auto ready = [&] {
            for (unsigned i = 0; !(io.read(0x0085) & 0x80) && i < 200000; ++i) eng.clock().advance(16);
            assert(io.read(0x0085) & 0x80);
        };
        ready();
        assert((io.read(0x0085) & 0xF0) == 0xF0); // execution phase, FDC -> CPU
        assert(io.read(0x0087) == 0x77);
        for (int i = 1; i < 512; ++i) {
            ready();
            io.read(0x0087);
        }
        io.write(0x0007, 0x19); // Terminal Count, drive A selected
        // the controller reads the sector's CRC, then enters its result phase
        assert((io.read(0x0085) & 0x90) == 0x10);
        eng.clock().advance(300);
        assert((io.read(0x0085) & 0xD0) == 0xD0); // result phase
        assert(io.read(0x0087) == 0x00);          // ST0: normal termination thanks to TC
        assert(io.read(0x0087) == 0x00);          // ST1
        assert(io.read(0x0087) == 0x00);          // ST2
        assert(io.read(0x0087) == 0x01);          // C + 1 (R was EOT)
        io.read(0x0087);
        assert(io.read(0x0087) == 0x01);          // R
        io.read(0x0087);
        assert(io.read(0x0085) == 0x80);
    }

    // A detached peripheral must not leave CPU hooks behind
    emulator.attachPeripheral(std::make_unique<Interface1Peripheral>());
    emulator.detachPeripheral("interface1");
    emulator.runFrames(2);
}

// Custom user-defined peripheral to verify zero-internal-change extensibility
class CustomCounterPeripheral : public Peripheral {
public:
    std::string_view id() const noexcept override { return "custom_counter"; }
    std::string_view name() const noexcept override { return "Custom Counter Peripheral"; }

    void onAttach(MachineContext &ctx) override
    {
        ctx.ioBus.registerSimpleHandler(
            std::string(id()),
            0x00FF,
            0x00AB,
            [this](uint16_t) -> uint8_t { return ++m_counter; },
            [this](uint16_t, uint8_t val) { m_counter = val; });
    }

    void onDetach(MachineContext &ctx) override
    {
        ctx.ioBus.unregisterOwner(id());
    }

    void reset(MachineContext &, ResetKind) override { m_counter = 0; }

private:
    uint8_t m_counter = 0;
};

void testExtensibilityAndZxNextAndDebugger()
{
    EmulatorConfig cfg;
    cfg.machineId = "next";
    Emulator emulator(cfg);

    // 1. Attach custom peripheral dynamically
    emulator.attachPeripheral(std::make_unique<CustomCounterPeripheral>());
    {
        auto &eng = emulator;
        eng.ioBus().write(0x00AB, 10);
        assert(eng.ioBus().read(0x00AB) == 11);
        assert(eng.ioBus().read(0x00AB) == 12);

        // 2. Test ZX Next NextREG, ZXN-DMA, and Copper
        auto &nextMach = dynamic_cast<ZxNextMachine &>(eng.machine());
        // Write NextREG 0x07 = 3 (28 MHz) via Z80N NEXTREG opcode path without disturbing 0x243B select latch
        eng.ioBus().write(0x243B, 0x14);
        eng.ioBus().writeNextRegOpcode(0x07, 0x03);
        assert(eng.ioBus().read(0x243B) == 0x14); // Select latch preserved!
        assert(eng.clock().speed() == CpuSpeed::Mhz28);

        // Test ZXN-DMA memory-to-memory transfer (copy 4 bytes from 0x8000 to 0x9000)
        eng.mmu().write(0x8000, 0xDE);
        eng.mmu().write(0x8001, 0xAD);
        eng.mmu().write(0x8002, 0xBE);
        eng.mmu().write(0x8003, 0xEF);

        const std::vector<uint8_t> dmaProg = {
            0xC3,                   // WR6: Reset DMA
            0x7D, 0x00, 0x80, 0x04, 0x00, // WR0: A->B, Port A = 0x8000, Len = 4
            0x14,                   // WR1: Port A Memory, Increment
            0x10,                   // WR2: Port B Memory, Increment
            0xCD, 0x00, 0x90,       // WR4: Continuous, Port B = 0x9000
            0xCF,                   // WR6: Load
            0x87                    // WR6: Enable DMA
        };
        for (uint8_t b : dmaProg)
            eng.ioBus().write(0x006B, b);

        assert(eng.mmu().read(0x9000) == 0xDE);
        assert(eng.mmu().read(0x9001) == 0xAD);
        assert(eng.mmu().read(0x9002) == 0xBE);
        assert(eng.mmu().read(0x9003) == 0xEF);

        // Test Copper co-processor executing MOVE(0x4A, 0xAA) on scanline 0
        nextMach.copper().setControlLo(0);
        nextMach.copper().setControlHi(0);
        nextMach.copper().writeData8(0x4A); // MOVE NextREG 0x4A
        nextMach.copper().writeData8(0xAA); // Value 0xAA
        nextMach.copper().writeData8(0xFF); // HALT
        nextMach.copper().writeData8(0xFF);
        nextMach.copper().setControlHi(0x40); // Start Copper at PC=0
        nextMach.copper().tickScanline(0, nextMach.nextReg());
        assert(nextMach.nextReg().peek(0x4A) == 0xAA);
    }

    // 3. Test Inline Assembler, Disassembler, Breakpoints, Snapshot Save/Load, Rewind, and PokeFinder
    emulator.selectMachine("48k");
    auto patched = emulator.assembleAndPatch(0x8000, "LD HL, $4000");
    assert((patched == std::vector<uint8_t>{0x21, 0x00, 0x40}));

    auto dis = emulator.disassemble(0x8000, 1);
    assert(dis.size() == 1);
    assert(dis[0].mnemonic == "LD HL,$4000");
    emulator.writeMemory(0x8010, {0xED, 0x79, 0xED, 0x78, 0xED, 0x71, 0xED, 0x70});
    dis = emulator.disassemble(0x8010, 4);
    assert(dis[0].mnemonic == "OUT (C),A" && dis[1].mnemonic == "IN A,(C)");
    assert(dis[2].mnemonic == "OUT (C),0" && dis[3].mnemonic == "IN (C)");

    // Test SZX & SNA snapshot round-trip
    emulator.writeMemory(0x5800, {0x38, 0x47});
    auto szxData = emulator.saveSnapshot(".szx");
    auto snaData = emulator.saveSnapshot(".sna");
    assert(snaData.size() == 49179);

    emulator.writeMemory(0x5800, {0x00, 0x00});
    emulator.loadSnapshot(szxData, ".szx");
    auto restored = emulator.readMemory(0x5800, 2);
    assert((restored == std::vector<uint8_t>{0x38, 0x47}));

    // Test PNG screenshot encoding
    auto pngBytes = emulator.screenshotPng();
    assert(pngBytes.size() > 64);
    assert(pngBytes[0] == 0x89 && pngBytes[1] == 'P' && pngBytes[2] == 'N' && pngBytes[3] == 'G');

    // Test PokeFinder
    emulator.writeMemory(0x9500, {3}); // 3 lives
    emulator.pokeFinderReset();
    emulator.writeMemory(0x9500, {2}); // Decreased to 2 lives
    emulator.pokeFinderSearchDecremented();
    auto candidates = emulator.pokeFinderSearch(2);
    bool foundAddr = false;
    for (const auto &c : candidates) {
        if (c.page16k == 1 && c.offset == 0x1500 && c.currentValue == 2)
            foundAddr = true;
    }
    assert(foundAddr);

    // Test RewindBuffer time-travel
    emulator.setRewindEnabled(true, 10);
    emulator.writeMemory(0xA000, {0x11});
    emulator.runFrames(1);
    emulator.writeMemory(0xA000, {0x99});
    assert(emulator.readMemory(0xA000, 1)[0] == 0x99);
    emulator.rewind(1);
    assert(emulator.readMemory(0xA000, 1)[0] == 0x11);

    // 4. Test .pok file parser & SLD SymbolTable
    const std::string_view pokSample =
        "NInfinite Lives\n"
        "M 8 35000 0 5\n"
        "Z 8 35001 255 0\n"
        "Y\n";
    auto trainers = PokeFinder::parsePokFile(pokSample);
    assert(trainers.size() == 1);
    assert(trainers[0].name == "Infinite Lives");
    assert(trainers[0].patches.size() == 2);
    assert(trainers[0].patches[0].address == 35000);
    assert(trainers[0].patches[1].value == 255);

    SymbolTable sym;
    sym.loadSldText("main.asm|42||0|32768|T|\nmain.asm|42||0|32768|L|,start_game\n");
    assert(sym.findAddress("start_game").value_or(0) == 32768);
    auto loc = sym.findSourceLocation(32768);
    assert(loc.has_value() && loc->file == "main.asm" && loc->line == 42);

    // 5. Test RZX recording and playback round-trip
    IoBus testBus;
    RzxStream rzx;
    rzx.startRecording({0x01, 0x02, 0x03}, ".szx", testBus);
    testBus.registerSimpleHandler("btn", 0x00FF, 0x001F, [](uint16_t) -> uint8_t { return 0x15; }, nullptr);
    assert(testBus.read(0x001F) == 0x15);
    rzx.endRecordedFrame(100);
    auto rzxBytes = rzx.stopRecording(testBus);

    // Change hardware port response to 0x00, then replay RZX and verify it overrides to recorded 0x15
    testBus.unregisterOwner("btn");
    testBus.registerSimpleHandler("btn", 0x00FF, 0x001F, [](uint16_t) -> uint8_t { return 0x00; }, nullptr);
    rzx.loadRzx(rzxBytes, testBus);
    assert(rzx.isPlaying());
    assert(testBus.read(0x001F) == 0x15);
}

} // namespace

void testEngineThreading()
{
    using namespace std::chrono_literals;
    const auto waitFor = [](const auto &condition) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!condition()) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
    };

    std::atomic<int> frames{0};
    std::atomic<int> audioFrames{0};
    std::atomic<int> pauses{0};
    std::atomic<int> resumes{0};
    std::atomic<uint16_t> pausedAt{0};
    std::atomic<PauseReason> pauseReason{PauseReason::Error};

    CallbackHandle<VideoFrameCallback> onFrame([&frames](const VideoFramePtr &frame) {
        const auto pixels = frame.readLock();
        assert(pixels->width() == 320 && pixels->height() == 256);
        ++frames;
    });
    CallbackHandle<AudioCallback> onAudio([&audioFrames](std::span<const int16_t> samples, uint32_t rate, uint8_t channels) {
        assert(rate == 48'000 && channels == 2 && !samples.empty());
        ++audioFrames;
    });
    CallbackHandle<PauseCallback> onPause([&](const PauseEvent &event) {
        pausedAt = event.pc;
        pauseReason = event.reason;
        ++pauses;
    });
    CallbackHandle<ResumeCallback> onResume([&resumes] { ++resumes; });

    EngineConfig config;
    config.turbo = true;
    config.callbacks = {onFrame.weak(), onAudio.weak(), onPause.weak(), onResume.weak(), {}};
    Engine engine(EmulatorConfig{}, config);

    // Frames and audio flow, while other threads use the emulator at the same time
    {
        std::vector<std::jthread> hosts;
        for (int t = 0; t < 3; ++t) {
            hosts.emplace_back([&engine] {
                for (int i = 0; i < 2000; ++i) {
                    const auto regs = engine.emulator().readLock()->registers();
                    (void)regs;
                    engine.emulator().writeLock()->setKeyState(ZxKey::Space, (i & 1) != 0);
                }
            });
        }
    }
    waitFor([&] { return frames > 20 && audioFrames > 20; });

    // A breakpoint pauses the Engine thread and tells why and where
    engine.emulator().writeLock()->addBreakpoint(Breakpoint{.address = 0x0038});
    waitFor([&] { return pauses == 1; });
    assert(engine.isPaused());
    assert(pauseReason == PauseReason::ExecutionBreakpoint && pausedAt == 0x0038);
    assert(engine.emulator().readLock()->registers().PC == 0x0038);

    // Paused means paused: nothing runs
    const int framesWhilePaused = frames;
    std::this_thread::sleep_for(50ms);
    assert(frames == framesWhilePaused);

    // A refresh request still publishes a frame
    engine.refreshFrame();
    waitFor([&] { return frames == framesWhilePaused + 1; });

    // Stepping runs the instruction the breakpoint stopped at
    const auto regs = engine.emulator().writeLock()->stepInstruction();
    assert(regs.PC != 0x0038);

    // Resuming runs on, up to the next interrupt
    engine.emulator().writeLock()->clearBreakpoints();
    engine.emulator().writeLock()->addBreakpoint(Breakpoint{.address = 0x0038});
    engine.setPaused(false);
    waitFor([&] { return pauses == 2; });
    assert(resumes == 1);
    assert(pausedAt == 0x0038);

    // Resuming from a breakpoint doesn't stop at the same breakpoint again
    const uint64_t cycle = engine.emulator().readLock()->clock().totalTstates();
    engine.setPaused(false);
    waitFor([&] { return pauses == 3; });
    assert(engine.emulator().readLock()->clock().totalTstates() > cycle + 60'000);

    // A user pause is reported too; a destroyed callback is simply not called
    engine.emulator().writeLock()->clearBreakpoints();
    engine.setPaused(false);
    waitFor([&] { return resumes == 3; });
    onFrame.reset();
    engine.setPaused(true);
    waitFor([&] { return pauses == 4; });
    assert(pauseReason == PauseReason::UserRequest);
    engine.stop();

    // Real time pacing follows the speed: 4x the speed, about 4x the frames
    std::atomic<int> paced{0};
    CallbackHandle<VideoFrameCallback> onPacedFrame([&paced](const VideoFramePtr &) { ++paced; });
    Engine pacedEngine(EmulatorConfig{}, EngineConfig{.callbacks = {.videoFrame = onPacedFrame.weak()}});
    const auto framesIn = [&paced](std::chrono::milliseconds duration) {
        const int start = paced;
        std::this_thread::sleep_for(duration);
        return paced - start;
    };
    std::this_thread::sleep_for(100ms);
    const int atNormalSpeed = framesIn(400ms); // 50 Hz: ~20
    pacedEngine.setSpeed(400);
    std::this_thread::sleep_for(100ms);
    const int atFourTimes = framesIn(400ms); // ~80
    std::println("paced frames in 400 ms: {} at 100%, {} at 400%", atNormalSpeed, atFourTimes);
    assert(atNormalSpeed >= 12 && atNormalSpeed <= 30);
    assert(atFourTimes > atNormalSpeed * 5 / 2);
    assert(!pacedEngine.isRealTime());
}

void testAssemblerAndDisassembler()
{
    const auto disassemble = [](std::initializer_list<uint8_t> code, bool z80n = true) {
        std::array<uint8_t, 4> bytes{};
        std::ranges::copy(code, bytes.begin());
        return Disassembler::disassembleOne([&bytes](uint16_t a) { return bytes[(a - 0x8000) & 3]; }, 0x8000, z80n);
    };
    const auto assemble = [](std::string_view line, AssemblerOptions options = {}) {
        return Assembler::assemble(line, 0x8000, options);
    };
    const auto bytesOf = [&assemble](std::string_view line, AssemblerOptions options = {}) {
        const auto bytes = assemble(line, options);
        return bytes ? *bytes : std::vector<uint8_t>{};
    };

    // IX / IY: (IX+d), IXH / IXL, HL -> IX, and prefixes without an effect
    assert(disassemble({0xDD, 0x7E, 0x05}).mnemonic == "LD A,(IX+$05)");
    assert(disassemble({0xFD, 0x66, 0xFE}).mnemonic == "LD H,(IY-$02)");
    assert(disassemble({0xDD, 0x26, 0x12}).mnemonic == "LD IXH,$12");
    assert(disassemble({0xDD, 0x36, 0x02, 0x34}).mnemonic == "LD (IX+$02),$34");
    assert(disassemble({0xDD, 0x09}).mnemonic == "ADD IX,BC");
    assert(disassemble({0xFD, 0xE3}).mnemonic == "EX (SP),IY");
    assert(disassemble({0xDD, 0x86, 0x80}).mnemonic == "ADD A,(IX-$80)");
    const auto noEffect = disassemble({0xDD, 0x00});
    assert(noEffect.mnemonic == "DEFB $DD" && noEffect.length == 1);
    assert(disassemble({0xDD, 0xCB, 0x05, 0x06}).mnemonic == "RLC (IX+$05)");
    assert(disassemble({0xDD, 0xCB, 0x05, 0x00}).mnemonic == "RLC (IX+$05),B");
    assert(disassemble({0xFD, 0xCB, 0xFF, 0x7E}).mnemonic == "BIT 7,(IY-$01)");
    assert(disassemble({0xED, 0x4C}).mnemonic == "NEG");
    assert(disassemble({0xED, 0x7E}).mnemonic == "IM 2");
    assert(disassemble({0xED, 0x23}, false).mnemonic == "DEFB $ED,$23");

    // Every instruction the disassembler prints assembles back to the same instruction
    int checked = 0;
    int sameBytes = 0;
    const auto roundTrip = [&](std::array<uint8_t, 4> code) {
        const auto inst = Disassembler::disassembleOne([&code](uint16_t a) { return code[(a - 0x8000) & 3]; }, 0x8000, true);
        if (inst.mnemonic.starts_with("DEFB"))
            return;
        const auto bytes = Assembler::assemble(inst.mnemonic, 0x8000);
        if (!bytes)
            std::println(stderr, "can't assemble '{}': {}", inst.mnemonic, bytes.error());
        assert(bytes.has_value());
        std::array<uint8_t, 4> again{};
        std::ranges::copy(*bytes, again.begin());
        const auto back = Disassembler::disassembleOne([&again](uint16_t a) { return again[(a - 0x8000) & 3]; }, 0x8000, true);
        if (back.mnemonic != inst.mnemonic)
            std::println(stderr, "'{}' assembled as '{}'", inst.mnemonic, back.mnemonic);
        assert(back.mnemonic == inst.mnemonic && back.length == bytes->size());
        ++checked;
        sameBytes += std::ranges::equal(*bytes, std::span(code).first(inst.length));
    };
    for (const auto operands : {std::array<uint8_t, 2>{0x05, 0x40}, std::array<uint8_t, 2>{0xFE, 0x80}}) {
        for (unsigned op = 0; op < 256; ++op) {
            roundTrip({uint8_t(op), operands[0], operands[1], 0});
            roundTrip({0xCB, uint8_t(op), 0, 0});
            roundTrip({0xED, uint8_t(op), operands[0], operands[1]});
            for (const uint8_t prefix : {uint8_t(0xDD), uint8_t(0xFD)}) {
                roundTrip({prefix, uint8_t(op), operands[0], operands[1]});
                roundTrip({prefix, 0xCB, operands[0], uint8_t(op)});
            }
        }
    }
    std::println(stderr, "assembler round trip: {} instructions, {} to the very same bytes", checked, sameBytes);
    assert(checked > 2000);

    // The usual spellings
    assert((bytesOf("ld a, (ix + 5)") == std::vector<uint8_t>{0xDD, 0x7E, 0x05}));
    assert((bytesOf("LD A,(IY)") == std::vector<uint8_t>{0xFD, 0x7E, 0x00}));
    assert((bytesOf("ld (ix-2),%1010") == std::vector<uint8_t>{0xDD, 0x36, 0xFE, 0x0A}));
    assert((bytesOf("add b") == std::vector<uint8_t>{0x80}));
    assert((bytesOf("cp a,c ; compare") == std::vector<uint8_t>{0xB9}));
    assert((bytesOf("ex af,af") == std::vector<uint8_t>{0x08}));
    assert((bytesOf("jp hl") == std::vector<uint8_t>{0xE9}));
    assert((bytesOf("rst 38h") == std::vector<uint8_t>{0xFF}));
    assert((bytesOf("rst 8") == std::vector<uint8_t>{0xCF}));
    assert((bytesOf("jr $8000") == std::vector<uint8_t>{0x18, 0xFE}));
    assert((bytesOf("ld bc,0x1234") == std::vector<uint8_t>{0x01, 0x34, 0x12}));
    assert((bytesOf("ld hl,-1") == std::vector<uint8_t>{0x21, 0xFF, 0xFF}));
    assert((bytesOf("ld a,(bc)") == std::vector<uint8_t>{0x0A}));
    assert((bytesOf("bit 3,(ix-2)") == std::vector<uint8_t>{0xDD, 0xCB, 0xFE, 0x5E}));
    assert((bytesOf("res 1,(iy+3),b") == std::vector<uint8_t>{0xFD, 0xCB, 0x03, 0x88}));
    assert((bytesOf("im 2") == std::vector<uint8_t>{0xED, 0x5E}));
    assert((bytesOf("neg") == std::vector<uint8_t>{0xED, 0x44}));
    assert((bytesOf("out (c),a") == std::vector<uint8_t>{0xED, 0x79}));
    assert((bytesOf("defb 1, 2, $ff") == std::vector<uint8_t>{0x01, 0x02, 0xFF}));
    assert((bytesOf("nextreg $07,3") == std::vector<uint8_t>{0xED, 0x91, 0x07, 0x03}));
    assert((bytesOf("push $1234") == std::vector<uint8_t>{0xED, 0x8A, 0x12, 0x34}));
    assert((bytesOf("mul") == std::vector<uint8_t>{0xED, 0x30}));

    // Base 16 numbers start with a digit, register names are never numbers
    const AssemblerOptions hex{.defaultBase = 16};
    assert((bytesOf("ld a,10", hex) == std::vector<uint8_t>{0x3E, 0x10}));
    assert((bytesOf("ld a,b", hex) == std::vector<uint8_t>{0x78}));
    assert((bytesOf("ld a,0ff", hex) == std::vector<uint8_t>{0x3E, 0xFF}));
    assert((bytesOf("ld a,(bc)", hex) == std::vector<uint8_t>{0x0A}));

    // Errors
    assert(!assemble("swapnib", {.z80n = false}));
    assert(!assemble("djnz $8100"));
    assert(!assemble("ld a,(ix+200)"));
    assert(!assemble("ld a,256"));
    assert(!assemble("ld a,"));
    assert(!assemble("frobnicate"));
    bool threw = false;
    try {
        Assembler::assembleOne("jp");
    } catch (const DebuggerException &) {
        threw = true;
    }
    assert(threw);
}

void testJoysticks()
{
    Emulator emulator;
    emulator.attachPeripheral(std::make_unique<KempstonJoystickPeripheral>());
    emulator.attachPeripheral(std::make_unique<FullerBoxPeripheral>());
    auto &kempston = static_cast<KempstonJoystickPeripheral &>(emulator.findPeripheral("kempston")->get());
    auto &fuller = static_cast<FullerBoxPeripheral &>(emulator.findPeripheral("fuller")->get());

    assert(emulator.ioBus().read(0x001F) == 0x00);
    assert(emulator.ioBus().read(0x007F) == 0xFF);
    kempston.setButtons(JoystickButtons::Up | JoystickButtons::Fire1);
    fuller.setButtons(JoystickButtons::Up | JoystickButtons::Right | JoystickButtons::Fire1);
    assert(emulator.ioBus().read(0x001F) == 0x18);
    assert(emulator.ioBus().read(0x007F) == 0x76); // active low: up 0x01, right 0x08, fire 0x80
    emulator.reset(ResetKind::Hard);
    assert(emulator.ioBus().read(0x007F) == 0x76); // the host's joystick, still pushed
}

int main()
{
    testZ80nOpcodes();
    testRomanianCpmBankingAndIf1Fdc();
    testExtensibilityAndZxNextAndDebugger();
    testEngineThreading();
    testAssemblerAndDisassembler();
    testJoysticks();
    std::println("test_z80n_mmu_machines: ALL PASSED");
    return 0;
}
