/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include <zxkit/zxkit.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <print>
#include <thread>

using namespace zxkit;
using namespace std::chrono_literals;
namespace {
void setup(Emulator &e, uint16_t pc = 0x8000, uint16_t sp = 0xFF00)
{
    auto r = e.registers();
    r.PC = pc;
    r.SP = sp;
    r.IFF1 = r.IFF2 = 0;
    r.IM = 1;
    r.AF = 0x0040;
    e.setRegisters(r);
}
PauseEvent stop(Emulator &e)
{
    for (unsigned i = 0; i < 10000; ++i) {
        e.runScanline();
        if (auto event = e.takePauseEvent())
            return *event;
    }
    assert(false && "Debugger operation did not stop");
    return {};
}
void word(Emulator &e, uint16_t address, uint16_t value)
{
    const std::array<uint8_t, 2> data{ uint8_t(value), uint8_t(value >> 8) };
    e.writeMemory(address, data);
}
void runControl()
{
    Emulator e;
    setup(e);
    // A nested path visits the return PC with the wrong stack depth first.
    e.writeMemory(0x8000, { 0xCD, 0x10, 0x80, 0x3C, 0xC9 });
    e.writeMemory(0x8010, { 0xC3, 0x03, 0x80 });
    assert(e.stepOver());
    auto event = stop(e);
    assert(event.reason == PauseReason::StepComplete && event.pc == 0x8003);
    assert(e.registers().A() == 1 && e.registers().SP == 0xFF00 && !e.hasRunControl());
    assert(!e.runTo(0x8003));
    assert(e.stepInstruction().PC == 0x8004);
    assert(e.takePauseEvent()->reason == PauseReason::StepComplete);

    // Step-out started after PUSH locals: nested CALL/conditional RET/prefixes.
    setup(e, 0x8010, 0xFEFC);
    word(e, 0xFEFC, 0x1234);
    word(e, 0xFEFE, 0x8100);
    e.writeMemory(0x8010, { 0xC1, 0xC4, 0x20, 0x80, 0xCD, 0x20, 0x80, 0xDD, 0xC8 });
    e.writeMemory(0x8020, { 0xC0, 0xDD, 0xC9 });
    e.stepOut();
    event = stop(e);
    assert(event.reason == PauseReason::StepComplete && event.pc == 0x8100);
    assert(e.registers().SP == 0xFF00 && e.registers().BC == 0x1234);

    setup(e, 0x8010, 0xFFFE);
    word(e, 0xFFFE, 0x8100);
    e.writeMemory(0x8010, { 0xC3, 0x20, 0x80 });
    e.writeMemory(0x8020, { 0xC9 });
    e.stepOut();
    event = stop(e);
    assert(event.pc == 0x8100 && e.registers().SP == 0); // tail call and stack wrap

    setup(e);
    e.writeMemory(0x8000, { 0x18, 0xFE });
    e.runTo(0x9000);
    e.addBreakpoint(Breakpoint{ .address = 0x8000 });
    e.runScanline();
    event = stop(e); // current BP is skipped once, then loop stops
    assert(event.reason == PauseReason::ExecutionBreakpoint && !e.hasRunControl());
    e.clearBreakpoints();
    e.stepOut();
    e.reset();
    assert(!e.hasRunControl());
}
void memoryAndPorts()
{
    Emulator e;
    setup(e);
    e.writeMemory(0x8000, { 0x3E, 0x42, 0x32, 0x05, 0x90, 0xD3, 0x1F, 0x76 });
    const auto memory = e.addBreakpoint(Breakpoint{ .type = BreakpointType::MemoryWrite,
                                                    .temporary = true,
                                                    .address = 0x9000,
                                                    .mask = 0xFF00 });
    auto event = stop(e);
    assert(event.reason == PauseReason::MemoryWatchpoint && event.address == 0x9005);
    assert(e.mmu().peek(0x9005) == 0x42 && e.debugger().breakpoints().empty());
    assert(!e.removeBreakpoint(memory));
    e.addBreakpoint(Breakpoint{ .type = BreakpointType::PortWrite,
                                .temporary = true,
                                .address = 0x001F,
                                .mask = 0x00FF });
    event = stop(e);
    assert(event.reason == PauseReason::PortWatchpoint && event.address == 0x421F);
    assert(e.debugger().breakpoints().empty());

    Emulator banked({ .machineId = "128k" });
    const auto ram = banked.machine().ramBankId();
    const auto id = banked.addBreakpoint(Breakpoint{ .type = BreakpointType::MemoryRead,
                                                     .address = 7,
                                                     .mask = 0x03FF,
                                                     .page1k = 0,
                                                     .bankId = ram });
    banked.mmu().read(7);
    assert(!banked.takePauseEvent()); // ROM page zero is not RAM page zero
    banked.mmu().read(0xC007);
    event = *banked.takePauseEvent();
    assert(event.address == 0xC007);
    banked.ioBus().write(0x7FFD, 1);
    banked.mmu().read(0xC007);
    assert(!banked.takePauseEvent());
    banked.mmu().map16k(0x8000, ram, 0);
    banked.mmu().read(0x8007);
    assert(banked.takePauseEvent()->address == 0x8007);
    assert(banked.setBreakpointEnabled(id, false));
    banked.mmu().read(0x8007);
    assert(!banked.takePauseEvent());
    assert(banked.setBreakpointEnabled(id, true));
    const auto hits = banked.debugger().breakpoints().breakpoints()[0].hitCount;
    banked.readMemory(0x8000, 256);
    banked.writeMemory(0x8007, { 0x12 });
    assert(!banked.takePauseEvent()
           && banked.debugger().breakpoints().breakpoints()[0].hitCount == hits);
    banked.clearBreakpoints();
}
void nextAndRaster()
{
    Emulator e({ .machineId = "next" });
    setup(e);
    e.writeMemory(0x8000, { 0xED, 0x91, 0x50, 0x10, 0x76 });
    e.addBreakpoint(
            Breakpoint{ .type = BreakpointType::NextRegWrite, .temporary = true, .address = 0x50 });
    auto event = stop(e);
    assert(event.reason == PauseReason::NextRegWatchpoint && event.address == 0x50);
    auto &next = static_cast<ZxNextMachine &>(e.machine());
    assert(next.nextReg().peek(0x50) == 0x10 && e.debugger().breakpoints().empty());
    e.addBreakpoint(
            Breakpoint{ .type = BreakpointType::NextRegWrite, .temporary = true, .address = 0x51 });
    e.ioBus().write(0x243B, 0x51);
    e.ioBus().write(0x253B, 0x11);
    assert(e.takePauseEvent()->reason == PauseReason::NextRegWatchpoint);
    e.addBreakpoint(
            Breakpoint{ .type = BreakpointType::NextRegWrite, .temporary = true, .address = 0x52 });
    next.nextReg().write(0x52, 0x12); // Copper and peripheral writes use the same NextReg dispatch
    assert(e.takePauseEvent()->address == 0x52);
    e.clearBreakpoints();

    Emulator target({ .machineId = "next" });
    setup(target);
    target.writeMemory(0x8000, { 0xED, 0x91, 0x56, 1, 0xC3, 0x00, 0xC0 });
    target.writeMemory(0x8100, { 0xED, 0x91, 0x56, 0, 0xC3, 0x00, 0xC0 });
    auto wrong = target.mmu().bank(target.machine().ramBankId()).span8k(1);
    wrong[0] = 0xC3;
    wrong[1] = 0;
    wrong[2] = 0x81;
    target.writeMemory(0xC000, { 0x76 });
    target.runTo(0xC000);
    event = stop(target);
    assert(event.reason == PauseReason::StepComplete && event.pc == 0xC000);
    assert(static_cast<ZxNextMachine &>(target.machine()).nextReg().peek(0x56) == 0);

    Emulator raster;
    setup(raster);
    raster.writeMemory(0x8000, { 0x18, 0xFE });
    raster.addBreakpoint(
            Breakpoint{ .type = BreakpointType::RasterLine, .temporary = true, .address = 3 });
    event = stop(raster);
    assert(event.reason == PauseReason::RasterBreakpoint && event.address == 3);
    assert(raster.clock().frameBaseTstates() >= 3 * raster.clock().timings().tstatesPerLine);
    assert(raster.debugger().breakpoints().empty());
    raster.runFrames(1);
    assert(!raster.takePauseEvent());
}
void interruptEvents()
{
    Emulator e;
    setup(e);
    e.writeMemory(0x8000, { 0xFB, 0x00, 0xC9 });
    e.writeMemory(0x0038, { 0xC9 }, true);
    word(e, 0xFF00, 0x8100);
    e.stepOut();
    auto event = stop(e);
    assert(event.reason == PauseReason::StepComplete && event.pc == 0x8100); // skips ISR's RET
    Emulator irq;
    setup(irq);
    irq.writeMemory(0x8000, { 0xFB, 0x00, 0x76 });
    irq.addBreakpoint(Breakpoint{ .type = BreakpointType::Event,
                                  .temporary = true,
                                  .address = uint16_t(DebugEvent::Interrupt) });
    event = stop(irq);
    assert(event.reason == PauseReason::EventTrap && event.pc == 0x0038 && event.address == 0);
    assert(irq.debugger().breakpoints().empty());
    Emulator nmi;
    setup(nmi);
    nmi.addBreakpoint(Breakpoint{ .type = BreakpointType::Event,
                                  .temporary = true,
                                  .address = uint16_t(DebugEvent::Nmi) });
    nmi.cpu().requestNmi();
    event = stop(nmi);
    assert(event.reason == PauseReason::EventTrap && event.pc == 0x0066 && event.address == 1);
}
void engineControls()
{
    std::atomic<unsigned> pauses{ 0 };
    std::atomic<PauseReason> reason{ PauseReason::Error };
    CallbackHandle<PauseCallback> callback([&](const PauseEvent &event) {
        reason = event.reason;
        ++pauses;
    });
    std::atomic_bool started{ false };
    EngineConfig config;
    config.threadHooks.started = [&](Emulator &) { started = true; };
    config.startPaused = true;
    config.turbo = true;
    config.callbacks.paused = callback.weak();
    Engine engine(EmulatorConfig{}, config);
    auto wait = [](auto predicate) {
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        assert(predicate());
    };
    wait([&] { return started.load(); });
    {
        auto core = engine.emulator().writeLock();
        setup(*core);
        core->writeMemory(0x8000, { 0x00, 0x18, 0xFD });
    }
    for (unsigned i = 0; i < 10; ++i) {
        auto before = pauses.load();
        engine.runTo(0x8001);
        wait([&] { return pauses.load() > before; });
        assert(reason == PauseReason::StepComplete
               && engine.emulator().readLock()->registers().PC == 0x8001);
        engine.stepOut();
        wait([&] { return !engine.isPaused(); });
        before = pauses.load();
        engine.setPaused(true);
        wait([&] { return pauses.load() > before; });
        assert(!engine.emulator().readLock()->hasRunControl());
        engine.setPaused(true);
        engine.runTo(0x8000); // a fresh command supersedes any queued cancellation
        wait([&] { return engine.isPaused(); });
    }
}
} // namespace
int main()
{
    runControl();
    memoryAndPorts();
    nextAndRaster();
    interruptEvents();
    engineControls();
    std::println("test_debugger: all tests passed");
}
