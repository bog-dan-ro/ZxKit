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
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace zxkit;
namespace
{
const std::filesystem::path fixtures = ZXKIT_RZX_FIXTURES;
std::vector<uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return { std::istreambuf_iterator<char>(file), {} };
}
EmulatorConfig config(std::string machine = "48k")
{
    EmulatorConfig c;
    c.machineId = machine;
    c.romResolver
        = [](std::string_view name, std::size_t) { return read(std::filesystem::path(ZXKIT_TEST_ROMS) / name); };
    return c;
}
template <class Function> std::string fails(Function fn)
{
    try {
        fn();
    } catch (const FormatException& e) {
        return std::string(e.what());
    }
    assert(false && "Expected an RZX format/playback error");
    return {};
}
void putBlock(StateWriter& out, uint8_t id, const StateWriter& payload)
{
    out.write(id);
    out.write<uint32_t>(payload.size() + 5);
    out.writeBytes(payload.data());
}
std::vector<uint8_t> alteredInputs(
    std::span<const uint8_t> source, uint16_t firstFetches, std::vector<uint8_t> firstInputs)
{
    StateWriter out;
    out.writeBytes(source.first(10));
    StateReader blocks(source.subspan(10));
    bool changed = false;
    while (!blocks.eof()) {
        auto id = blocks.read<uint8_t>();
        auto length = blocks.read<uint32_t>();
        auto data = blocks.readSpan(length - 5);
        StateWriter block;
        if (id == 0x80 && !changed) {
            StateReader header(data);
            auto count = header.read<uint32_t>();
            auto reserved = header.read<uint8_t>();
            auto tstates = header.read<uint32_t>();
            auto flags = header.read<uint32_t>();
            auto bytes = data.subspan(13);
            auto decoded = flags & 2 ? zlibDecompress(bytes) : std::vector<uint8_t>(bytes.begin(), bytes.end());
            StateReader old(decoded);
            old.read<uint16_t>();
            auto n = old.read<uint16_t>();
            old.readSpan(n);
            StateWriter records;
            records.write(firstFetches);
            records.write<uint16_t>(firstInputs.size());
            records.writeBytes(firstInputs);
            records.writeBytes(std::span(decoded).subspan(old.position()));
            block.write(count);
            block.write(reserved);
            block.write(tstates);
            block.write<uint32_t>(0);
            block.writeBytes(records.data());
            changed = true;
        } else
            block.writeBytes(data);
        putBlock(out, id, block);
    }
    return out.takeBuffer();
}
void seekFrame(Emulator &e, std::size_t frame);
void testIndependentFixtures()
{
    struct Expected {
        const char* file;
        uint64_t total;
        uint16_t pc, hl, sp;
        uint8_t r, a;
    };
    for (const auto expected : { Expected { "fetches", 3, 0x8000, 0x9006, 0xfffe, 0x6b, 0x66 },
             Expected { "sequence", 5, 0x8000, 0x9022, 0xfffe, 0xaa, 0xa5 },
             Expected { "interrupts", 4, 0x9200, 0x9000, 0xfffc, 0x38, 0 },
             Expected { "ei-boundary", 3, 0x800c, 0x9000, 0xfffe, 0x16, 0x44 } }) {
        Emulator e(config("128k")); // embedded snapshots must select the real base machine
        e.loadMediaFile(fixtures / (std::string(expected.file) + ".rzx"));
        assert(e.machine().id() == "48k");
        auto p = e.rzx().progress();
        assert(p.state == RzxState::Playing && p.completedFrames == 0 && p.totalFrames == expected.total);
        unsigned calls = 0;
        while (e.rzx().isPlaying()) {
            assert(++calls < 10);
            e.runFrame();
        }
        p = e.rzx().progress();
        assert(
            p.state == RzxState::Completed && p.completedFrames == expected.total && p.totalFrames == expected.total);
        const auto r = e.registers();
        assert(r.PC == expected.pc && r.HL == expected.hl && r.SP == expected.sp && r.fullR() == expected.r
            && r.A() == expected.a);
        const auto memory = e.readMemory(0x9000, 48);
        if (std::string(expected.file) == "fetches")
            assert((std::vector<uint8_t>(memory.begin(), memory.begin() + 6)
                == std::vector<uint8_t> { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }));
        if (std::string(expected.file) == "sequence")
            assert(memory[0] == 0x99 && memory[32] == 0xa5 && memory[33] == 0xa5);
        if (std::string(expected.file) == "interrupts") {
            assert(memory[0] == 2);
            assert(calls == 3);
        }
        for (std::size_t frame=0;frame<=expected.total;++frame) {
            seekFrame(e,frame);
            assert(e.rzx().playbackFrame()==frame);
        }
        assert(e.registers().PC==r.PC && e.registers().fullR()==r.fullR());
        std::cout << expected.file << ": independent CPU/memory reference matches\n";
    }
}
void testDesyncAndStops()
{
    auto valid = read(fixtures / "fetches.rzx");
    for (auto inputs : { std::vector<uint8_t> { 0x11 }, std::vector<uint8_t> { 0x11, 0x22, 0x99 } }) {
        Emulator e(config());
        auto bytes = alteredInputs(valid, 20, inputs);
        e.loadMedia({ bytes, ".rzx" });
        e.ioBus().registerSimpleHandler("live", 0xffff, 0x001f, [](uint16_t) { return uint8_t { 0x66 }; }, nullptr);
        auto error = fails([&] { e.runFrame(); });
        assert(error.find(inputs.size() == 1 ? "overrun" : "underrun") != std::string::npos);
        assert(e.rzx().state() == RzxState::Error && !e.rzx().isPlaying());
        assert(e.rzx().progress().completedFrames == 0 && !e.rzx().progress().error.empty());
        assert(e.ioBus().read(0x001f) == 0x66); // failed playback never keeps supplying FF
    }
    {
        Emulator e(config());
        e.loadMedia({ alteredInputs(valid, 18, { 0x11, 0x22 }), ".rzx" });
        assert(fails([&] { e.runFrame(); }).find("splits") != std::string::npos);
    }
    Emulator e(config());
    e.loadMedia({ valid, ".rzx" });
    e.runFrame();
    assert(e.rzx().playbackFrame() == 1);
    e.stopRzxPlayback();
    assert(e.rzx().state() == RzxState::Stopped && e.rzx().progress().completedFrames == 1);
    e.ioBus().registerSimpleHandler("live", 0xffff, 0x001f, [](uint16_t) { return uint8_t { 0x66 }; }, nullptr);
    assert(e.ioBus().read(0x001f) == 0x66);
    e.loadMedia({ valid, ".rzx" });
    e.reset();
    assert(!e.rzx().isPlaying());
    e.runFrame();
}
void seekFrame(Emulator& e, std::size_t frame)
{
    e.beginRzxSeekFrame(frame);
    unsigned batches = 0;
    while (!e.advanceRzxSeek(2))
        assert(++batches < 1000);
    assert(!e.isRzxSeeking());
}
void setupEditingGame(Emulator& e, uint8_t& input, bool interrupts)
{
    const std::array<uint8_t, 11> program = { 0xdb, 0x1f, 0x32, 0, 0x90, 0x21, 1, 0x90, 0x34, 0x18, 0xf5 };
    e.writeMemory(0x8000, program);
    auto r = e.registers();
    r.PC = 0x8000;
    r.SP = 0xfffe;
    r.IFF1 = r.IFF2 = interrupts;
    r.IM = 2;
    r.I = 0x91;
    e.setRegisters(r);
    if (interrupts) {
        const std::array<uint8_t, 2> vector = { 0, 0x93 }, handler = { 0xfb, 0xc9 };
        e.writeMemory(0x91ff, vector);
        e.writeMemory(0x9300, handler);
    }
    e.ioBus().registerSimpleHandler("editing-input", 0x00ff, 0x001f, [&input](uint16_t) { return input; }, nullptr);
}
void testRecordingEditing(const std::filesystem::path& exportPath)
{
    for (const bool interrupts : { false, true }) {
        uint8_t input = 0x11, referenceInput = 0x11;
        Emulator edited(config()), reference(config());
        setupEditingGame(edited, input, interrupts);
        setupEditingGame(reference, referenceInput, interrupts);
        edited.startRzxRecording();
        reference.startRzxRecording();
        edited.runFrames(53);
        reference.runFrames(53);
        const auto cursor = edited.rzx().frameCount();
        const auto prefixMemory = edited.readMemory(0x4000, 49152);
        input = 0x22;
        edited.runFrames(47);
        const auto full = edited.rzx().frameCount();
        const auto oldMemory = edited.readMemory(0x4000, 49152);
        seekFrame(edited, cursor);
        assert(edited.rzx().isReviewing() && edited.rzx().frameCount() == full);
        const auto replacement=edited.saveSnapshot(".szx");
        fails([&]{edited.loadMedia({replacement,".szx"});});
        fails([&]{edited.reset();});
        assert(edited.rzx().isReviewing() && edited.rzx().frameCount()==full);
        // The replayed cursor includes its boundary INT, unlike the native pre-INT frame end.
        const auto prefixActual = edited.readMemory(0x4000, 49152);
        assert(std::equal(prefixActual.begin(), prefixActual.end() - (interrupts ? 4 : 0), prefixMemory.begin()));
        // Preview the old future even though the live input has changed.
        input = 0x33;
        seekFrame(edited, full);
        assert(edited.readMemory(0x4000, 49152) == oldMemory);
        seekFrame(edited, cursor);
        edited.resumeRzxRecording();
        assert(edited.rzx().isRecording() && edited.rzx().frameCount() == cursor);
        referenceInput = 0x33;
        edited.runFrames(47);
        reference.runFrames(47);
        const auto actual = edited.registers(), expected = reference.registers();
        assert(actual.PC == expected.PC && actual.AF == expected.AF && actual.HL == expected.HL
            && actual.SP == expected.SP && actual.fullR() == expected.fullR());
        const auto branchMemory = edited.readMemory(0x4000, 49152);
        const auto branchReference = reference.readMemory(0x4000, 49152);
        // Playback canonicalises the IRQ boundary, so discarded interrupt-stack bytes can
        // differ from a native uninterrupted run. The exported movie below checks ALL RAM.
        assert(std::equal(branchMemory.begin(), branchMemory.end() - (interrupts ? 4 : 0), branchReference.begin()));
        auto bytes = edited.stopRzxRecording();
        // An RZX player acknowledges the final interval's INT as well.
        if (interrupts) edited.cpu().acceptPlaybackInterrupt(false);
        const auto playbackExpected = edited.registers();
        const auto finalMemory = edited.readMemory(0x4000, 49152);
        if (!exportPath.empty()) {
            const auto path = exportPath.parent_path() / (interrupts ? "edited-int.rzx" : "edited.rzx");
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            assert(file);
            const auto state = edited.saveSnapshot(".szx");
            std::ofstream expectedFile(path.string()+".expected.szx",std::ios::binary);
            expectedFile.write(reinterpret_cast<const char*>(state.data()),state.size());assert(expectedFile);
            std::cout << "Edited " << interrupts << " PC " << std::hex << actual.PC << " AF " << actual.AF << " HL "
                      << actual.HL << " R " << unsigned(actual.fullR()) << std::dec << '\n';
        }
        Emulator replay(config());
        replay.loadMedia({ bytes, ".rzx" });
        while (replay.rzx().isPlaying())
            replay.runFrame();
        assert(replay.readMemory(0x4000, 49152) == finalMemory);
        assert(replay.registers().PC == playbackExpected.PC && replay.registers().fullR() == playbackExpected.fullR());
        // Playback seeks retain the full file and never enter recording mode.
        seekFrame(replay, cursor);
        assert(replay.rzx().state() == RzxState::Playing && replay.rzx().frameCount() == full);
        seekFrame(replay, 0);
        replay.beginRzxSeek(replay.rzxDurationMilliseconds() + 1000);
        while (!replay.advanceRzxSeek()) { }
        assert(replay.rzx().state() == RzxState::Completed && replay.readMemory(0x4000, 49152) == finalMemory);
        // Stop trims at the reviewed cursor, without needing another live frame.
        Emulator trim(config());
        uint8_t trimInput = 0x77;
        setupEditingGame(trim, trimInput, interrupts);
        trim.startRzxRecording();
        trim.runFrames(50);
        const auto cut = trim.rzx().frameCount();
        trim.runFrames(20);
        seekFrame(trim, cut);
        bytes = trim.stopRzxRecording();
        assert(trim.rzx().frameCount() == cut);
        Emulator check(config());
        check.loadMedia({ bytes, ".rzx" });
        assert(check.rzx().frameCount() == cut);
        while (check.rzx().isPlaying())
            check.runFrame();
    }
}

void test48kAyInterfaces()
{
    IoBus bus;
    RzxStream stream;
    const auto fixture = read(fixtures / "fetches.rzx");
    stream.loadRzx(fixture, bus);
    const auto embedded = stream.embeddedSnapshot();
    Emulator original(config());
    for (const bool fuller : { true, false }) {
        std::vector<uint8_t> snapshot(embedded.begin(), embedded.end());
        snapshot[37] = fuller ? 0x44 : 4;
        snapshot[38] = 0;
        snapshot[39] = 0x5a;
        StateWriter file;
        file.writeBytes(std::span(fixture).first(39));
        StateWriter block;
        block.write<uint32_t>(0);
        block.writeTag(std::string_view("z80\0", 4));
        block.write<uint32_t>(snapshot.size());
        block.writeBytes(snapshot);
        putBlock(file, 0x30, block);
        original.loadMedia({ file.takeBuffer(), ".rzx" });
        const uint16_t select = fuller ? 0x3f : 0xfffd;
        const uint16_t data = fuller ? 0x5f : 0xbffd;
        assert(original.ioBus().read(select) == 0x5a);
        original.ioBus().write(select, 0);
        original.ioBus().write(data, 0x3c);
        assert(original.ioBus().read(select) == 0x3c);
        original.startRzxRecording();
        original.runFrame();
        auto bytes = original.stopRzxRecording();
        Emulator replay(config());
        replay.loadMedia({ bytes, ".rzx" });
        assert(rzxSnapshotAyFlags(replay.rzx().embeddedSnapshot(), ".szx") == (fuller ? 1 : 3));
        while (replay.rzx().isPlaying())
            replay.runFrame();
        replay.ioBus().write(select, 0);
        replay.ioBus().write(data, 0x69);
        assert(replay.ioBus().read(select) == 0x69);
    }
}

void testSnapshotMachineHeaders()
{
    assert(!rzxMachineId("next") && !rzxMachineId("scorpion") && !rzxMachineId("pentagon512"));
    std::vector<uint8_t> header(87);
    for (const uint8_t length : { 23, 55 }) {
        header[30] = length;
        for (const auto mode : { 0, 3, 4, 7, 12, 13 }) {
            header[34] = mode;
            header[37] = 0;
            const auto base = rzxSnapshotMachine(header, ".z80");
            header[37] = 0x80;
            const auto modified = rzxSnapshotMachine(header, ".z80");
            assert(modified == (base == "48k" ? "16k" : base == "128k" ? "plus2" : base == "plus3" ? "plus2a" : base));
        }
    }
}

void testLatePositionError()
{
    const auto valid = read(fixtures / "fetches.rzx");
    StateWriter file;
    file.writeBytes(valid);
    StateWriter block;
    block.write<uint32_t>(0);
    block.write<uint8_t>(0);
    block.write<uint32_t>(999999);
    block.write<uint32_t>(0);
    putBlock(file, 0x80, block);
    Emulator e(config());
    e.loadMedia({ file.takeBuffer(), ".rzx" });
    assert(fails([&] {
        while (e.rzx().isPlaying())
            e.runFrame();
    }).find("position")
        != std::string::npos);
    assert(e.rzx().state() == RzxState::Error);
}

void testParserAndLifetime()
{
    const auto valid = read(fixtures / "fetches.rzx");
    IoBus bus;
    bus.registerSimpleHandler("live", 0, 0, [](uint16_t) { return uint8_t { 0x66 }; }, nullptr);
    {
        RzxStream stream;
        stream.loadRzx(valid, bus);
        for (std::size_t size : { std::size_t(0), std::size_t(9), valid.size() - 1 })
            fails([&] { stream.loadRzx(std::span(valid).first(size), bus); });
        assert(stream.isPlaying() && bus.read(0x1f) == 0x11); // failed replacement preserves the old stream
        stream.clear();
        assert(bus.read(0x1f) == 0x66);
        auto bad = valid;
        bad[5] = 11;
        assert(fails([&] { stream.loadRzx(bad, bus); }).find("revision") != std::string::npos);
        // Required creator and block header length validation.
        bad.assign(valid.begin(), valid.begin() + 10);
        bad.insert(bad.end(), valid.begin() + 39, valid.end());
        assert(fails([&] { stream.loadRzx(bad, bus); }).find("creator") != std::string::npos);
        stream.loadRzx(valid, bus);
    }
    assert(bus.read(0x1f) == 0x66); // IoBus can safely outlive its standalone stream
    // Reject unsupported features and hostile declared lengths explicitly.
    const auto withBlock = [&](uint8_t id, const StateWriter& payload) {
        StateWriter file;
        file.writeBytes(std::span(valid).first(39));
        putBlock(file, id, payload);
        return file.takeBuffer();
    };
    for (const uint32_t flags : { 1u, 4u }) {
        StateWriter block;
        block.write<uint32_t>(0);
        block.write<uint8_t>(0);
        block.write<uint32_t>(0);
        block.write(flags);
        RzxStream stream;
        assert(!fails([&] { stream.loadRzx(withBlock(0x80, block), bus); }).empty());
    }
    for (const uint32_t flags : { 0u, 1u }) {
        StateWriter block;
        block.write(flags);
        block.writeTag(std::string_view("z80\0", 4));
        block.write<uint32_t>(16 * 1024 * 1024 + 1);
        RzxStream stream;
        assert(!fails([&] { stream.loadRzx(withBlock(0x30, block), bus); }).empty());
    }
    StateWriter file;
    file.writeBytes(std::span(valid).first(39));
    StateWriter payload;
    payload.write<uint32_t>(1);
    payload.write<uint8_t>(0);
    payload.write<uint32_t>(0);
    payload.write<uint32_t>(0);
    payload.write<uint16_t>(10);
    payload.write<uint16_t>(65535);
    putBlock(file, 0x80, payload);
    RzxStream stream;
    assert(fails([&] { stream.loadRzx(file.data(), bus); }).find("predecessor") != std::string::npos);
}
void testRecordRoundTrip(const std::filesystem::path& exportPath)
{
    Emulator original(config());
    original.loadMediaFile(fixtures / "fetches.rzx");
    while (original.rzx().isPlaying())
        original.runFrame();
    auto r = original.registers();
    r.PC = 0x8000;
    r.HL = 0x9000;
    r.setFullR(0);
    original.setRegisters(r);
    original.ioBus().registerSimpleHandler(
        "constant-input", 0x00ff, 0x1f, [](uint16_t) { return uint8_t { 0x5a }; }, nullptr);
    original.startRzxRecording();
    fails([&] { original.reset(); });
    original.runFrames(2);
    original.stopRzxPlayback(); // playback control must not detach an active recorder
    original.runFrames(3);
    const auto expected = original.registers();
    const auto memory = original.readMemory(0x4000, 49152);
    auto bytes = original.stopRzxRecording();
    assert(original.rzx().frameCount() == 5);
    if (!exportPath.empty()) {
        std::ofstream file(exportPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        assert(file);
    }
    Emulator replay(config("128k"));
    replay.loadMedia({ bytes, ".rzx" });
    assert(replay.machine().id() == "48k");
    while (replay.rzx().isPlaying())
        replay.runFrame();
    const auto actual = replay.registers();
    assert(actual.PC == expected.PC && actual.HL == expected.HL && actual.AF == expected.AF
        && actual.fullR() == expected.fullR());
    assert(replay.readMemory(0x4000, 49152) == memory);
    assert(replay.rzx().frameCount() == 5 && replay.rzx().playbackFrame() == 5);
    std::cout << "Recorded reference: PC " << std::hex << expected.PC << " HL " << expected.HL << " R "
              << unsigned(expected.fullR()) << std::dec << '\n';
}
void testRecordingModelsAndAy()
{
    for (const auto machine : { "16k", "128k", "plus2", "plus3" }) {
        Emulator e(config(machine));
        const uint16_t address = std::string_view(machine) == "16k" ? 0x8000 - 0x4000 : 0x8000;
        const std::array<uint8_t, 4> program = { 0, 0xc3, uint8_t(address), uint8_t(address >> 8) };
        e.writeMemory(address, program);
        auto r = e.registers();
        r.PC = address;
        r.IFF1 = r.IFF2 = 0;
        e.setRegisters(r);
        if (std::string_view(machine) != "16k") {
            e.ioBus().write(0xfffd, 0);
            e.ioBus().write(0xbffd, 0x5a);
        }
        e.startRzxRecording();
        e.runFrames(2);
        auto bytes = e.stopRzxRecording();
        Emulator replay(config());
        replay.loadMedia({ bytes, ".rzx" });
        assert(replay.machine().id() == machine);
        while (replay.rzx().isPlaying())
            replay.runFrame();
        assert(replay.registers().PC == e.registers().PC && replay.registers().fullR() == e.registers().fullR());
        assert(replay.readMemory(address, 4) == e.readMemory(address, 4));
        if (std::string_view(machine) != "16k")
            assert(replay.ioBus().read(0xfffd) == 0x5a);
    }
}
}
int main(int argc, char** argv)
{
    testIndependentFixtures();
    testRecordingEditing(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path {});
    testSnapshotMachineHeaders();
    test48kAyInterfaces();
    testDesyncAndStops();
    testParserAndLifetime();
    testLatePositionError();
    testRecordingModelsAndAy();
    testRecordRoundTrip(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path {});
    std::cout << "RZX parser, execution, progress and recording tests passed\n";
}
