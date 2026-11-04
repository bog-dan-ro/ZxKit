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

#include "zxkit/core/Emulator.h"
#include "zxkit/machines/ZxNextMachine.h"
#include "zxkit/machines/CloneMachines.h"

#include "zxkit/core/Exception.h"
#include "zxkit/debug/Assembler.h"
#include "zxkit/media/Compression.h"
#include "zxkit/media/ScreenshotFormat.h"
#include "zxkit/media/SnapshotFormats.h"
#include "zxkit/peripherals/expansion/ExpansionPeripherals.h"

#include <algorithm>
#include <limits>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace zxkit {

namespace {

// Frames a host may hold on to before the emulator allocates new ones instead of recycling
constexpr std::size_t kMaxPooledFrames = 8;

constexpr std::array kSnapshotExtensions{std::string_view{".sna"}, std::string_view{".z80"},
                                         std::string_view{".szx"}, std::string_view{".nex"}};
constexpr std::array kDiskExtensions{std::string_view{".dsk"}, std::string_view{".trd"}, std::string_view{".scl"},
                                     std::string_view{".img"}, std::string_view{".mgt"}, std::string_view{".opd"},
                                     std::string_view{".td0"}};

// Normalizes a file extension to ".ext" in lower case. Accepts "ext", ".ext",
// a multi-part suffix ("tar.dsk") or a whole file name.
std::string normalizedExtension(std::string_view s)
{
    if (const auto dot = s.rfind('.'); dot != std::string_view::npos)
        s.remove_prefix(dot + 1);
    std::string out = s.empty() ? std::string{} : "." + std::string(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool isZip(std::span<const uint8_t> bytes, std::string_view extension)
{
    return extension == ".zip"
           || (bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 0x03 && bytes[3] == 0x04);
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw IoException(std::format("Cannot open file '{}'", path.string()));
    const auto size = f.tellg();
    if (size < 0)
        throw IoException(std::format("Failed to determine size of '{}'", path.string()));
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    if (!buf.empty() && !f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size())))
        throw IoException(std::format("Failed reading file '{}'", path.string()));
    return buf;
}

void writeFileBytes(const std::filesystem::path &path, std::span<const uint8_t> data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        throw IoException(std::format("Cannot open file '{}' for writing", path.string()));
    if (!data.empty() && !f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())))
        throw IoException(std::format("Failed writing to '{}'", path.string()));
}

} // namespace

// ── MediaFile ─────────────────────────────────────────────────────────────

MediaFile MediaFile::read(const std::filesystem::path &path)
{
    return fromBytes(readFileBytes(path), path.extension().string());
}

MediaFile MediaFile::fromBytes(std::vector<uint8_t> data, std::string_view extension)
{
    MediaFile file{std::move(data), normalizedExtension(extension)};
    if (isZip(file.data, file.extension)) {
        auto entry = extractFirstFromZip(file.data);
        file.data = std::move(entry.data);
        file.extension = normalizedExtension(std::filesystem::path(entry.filename).extension().string());
    }
    return file;
}

// ── Emulator ──────────────────────────────────────────────────────────────

Emulator::Emulator(EmulatorConfig config)
    : m_config(std::move(config))
    , m_romcs(m_mmu)
    , m_cpu(m_clock, m_mmu, m_contention, m_ioBus, m_im2)
    , m_context{
          .clock = m_clock,
          .scheduler = m_scheduler,
          .mmu = m_mmu,
          .contention = m_contention,
          .ioBus = m_ioBus,
          .romcs = m_romcs,
          .cpu = m_cpu,
          .im2 = m_im2,
          .audioMixer = m_audioMixer,
          .keyboard = m_keyboard,
          .sdCard = m_sdCard,
          .tape = m_tapeDeck,
          .romResolver = m_config.romResolver
      }
{
    selectMachine(m_config.machineId);
}

// The machine and the peripherals are declared after the components they use, so they go first
Emulator::~Emulator() = default;

void Emulator::selectMachine(std::string_view machineId)
{
    selectMachine(m_config.machines.create(machineId));
}

void Emulator::selectMachine(std::unique_ptr<MachineProfile> machine)
{
    if (!m_applyingRzxSnapshot) {
        if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("Stop RZX recording before changing machines");
        stopRzxPlayback();
    }
    if (!machine)
        throw InvalidArgumentException("Cannot install a null MachineProfile");

    const std::string previousId = m_machine ? std::string(m_machine->id()) : std::string{};
    try {
        installMachine(std::move(machine));
    } catch (...) {
        // A machine that can't start (e.g. a missing ROM) must not leave the
        // emulator half torn down: go back to the previous machine.
        if (!previousId.empty() && m_config.machines.contains(previousId)) {
            try {
                installMachine(m_config.machines.create(previousId));
            } catch (...) {
            }
        }
        throw;
    }
}

void Emulator::installMachine(std::unique_ptr<MachineProfile> machine)
{
    m_mediaBootFrame = 0;
    m_scheduler.clear();
    m_ioBus.clear();
    m_snapshotStandardAy = false;
    m_romcs.clear();
    m_cpu.clearPeripheralHooks();
    m_cpu.setZ80nEnabled(machine->supportsZ80n());
    m_cpu.setInterruptRetrigger(!machine->supportsZ80n());
    m_cpu.setEvenM1(false);

    m_mmu.clear();
    m_clock.setTimings(machine->timings());
    m_clock.reset();
    m_contention.configure(machine->contentionPattern(), machine->timings());
    m_audioMixer.configure(machine->timings().baseClockHz, m_config.audioSampleRate, machine->timings().tstatesPerFrame());
    m_audioMixer.reset();
    m_audioSamples = {};
    m_keyboard.reset();
    m_ula.reset();
    m_im2.reset();

    if (machine->hasFloatingBus()) {
        m_ioBus.setFloatingBusCallback([this](uint16_t) -> uint8_t {
            return m_ula.readFloatingBus(m_clock, m_mmu);
        });
    }

    m_cpu.setTapeTrapHook([this](uint16_t /*pc*/, bool isLoad) -> bool {
        return m_tapeDeck.handleRomTrap(m_context, isLoad);
    });
    m_tapeDeck.machineChanged();
    m_diskAccesses = {};
    m_diskBusyUntil = {};
    m_mediaActivity = {};

    m_cpu.setDebuggerHook([this](uint16_t pc) -> bool {
        bool skip = false;
        if (m_stoppedAtBreakpoint) {
            skip = *m_stoppedAtBreakpoint == pc;
            m_stoppedAtBreakpoint.reset();
        }
        if (!skip && m_debugger.breakpoints().checkExecute(pc, m_mmu, m_cpu.registers())) {
            m_stoppedAtBreakpoint = pc;
            requestPause(PauseReason::ExecutionBreakpoint, pc);
            return true;
        }
        if (m_debugRun && checkRunControl(pc)) {
            m_stoppedAtBreakpoint = pc;
            requestPause(PauseReason::StepComplete, pc);
            return true;
        }
        if (m_debugger.isTraceEnabled())
            m_debugger.recordTrace(m_clock.totalTstates(), m_cpu.registers());
        return false;
    });

    m_mmu.setWatchpointCallback([this](uint16_t addr, uint8_t /*val*/, bool isWrite) {
        if (m_debugger.breakpoints().checkMemory(addr, isWrite, m_cpu.registers(), &m_mmu)) {
            requestPause(PauseReason::MemoryWatchpoint, addr);
            m_cpu.trap();
        }
    });

    m_ioBus.setWatchpointCallback([this](uint16_t port, uint8_t /*val*/, bool isWrite) {
        if (m_debugger.breakpoints().checkPort(port, isWrite, m_cpu.registers())) {
            requestPause(PauseReason::PortWatchpoint, port);
            m_cpu.trap();
        }
    });

    const bool sameFrameSize = m_machine && m_machine->frameWidth() == machine->frameWidth()
                               && m_machine->frameHeight() == machine->frameHeight();
    m_nextDebugMachine = nullptr;
    m_debugObserverTypes = ~uint32_t(0);
    m_nextDebugArmed = false;
    m_interruptDebugArmed = false;
    m_cpu.setInterruptObserver({});
    m_machine = std::move(machine);
    m_machine->initialize(m_context, m_ula);
    m_nextDebugMachine = dynamic_cast<ZxNextMachine *>(m_machine.get());
    if (m_machine->usesRasterUla()) {
        m_mmu.setVideoWriteCallback(m_machine->ramBankId(), [this] {
            m_ula.renderUntil(m_clock.frameBaseTstates(), m_mmu);
        });
        m_ioBus.addObserver([this](uint16_t, uint8_t, bool isWrite) {
            if (isWrite)
                m_ula.renderUntil(m_clock.frameBaseTstates(), m_mmu);
        });
    }


    for (auto &periph : m_peripherals)
        periph->onAttach(m_context);

    m_cpu.reset(true);
    m_machine->reset(m_context, m_ula, ResetKind::Hard);
    for (auto &periph : m_peripherals)
        periph->reset(m_context, ResetKind::Hard);

    m_debugger.breakpoints().syncMmuWatchFlags(m_mmu);

    m_line = 0;
    m_frameStarted = false;
    m_lineStarted = false;
    m_pendingPause.reset();
    m_stoppedAtBreakpoint.reset();
    cancelRunControl();

    if (!sameFrameSize || !m_frame) {
        m_framePool.clear();
        m_frame = acquireFrame();
        m_lastFrame = acquireFrame();
    }
}

void Emulator::attachPeripheral(std::unique_ptr<Peripheral> peripheral)
{
    if (!peripheral)
        throw PeripheralException("Cannot attach a null Peripheral");
    if (findPeripheral(peripheral->id()))
        throw PeripheralException(std::format("Peripheral '{}' is already attached", peripheral->id()));
    peripheral->onAttach(m_context);
    peripheral->reset(m_context, ResetKind::Hard);
    m_peripherals.push_back(std::move(peripheral));
}

void Emulator::detachPeripheral(std::string_view peripheralId)
{
    const auto it = std::ranges::find_if(m_peripherals, [peripheralId](const auto &p) { return p->id() == peripheralId; });
    if (it == m_peripherals.end())
        throw PeripheralException(std::format("Peripheral '{}' is not attached", peripheralId));
    (*it)->onDetach(m_context);
    m_peripherals.erase(it);
}

OptionalRef<Peripheral> Emulator::findPeripheral(std::string_view peripheralId) noexcept
{
    for (const auto &p : m_peripherals) {
        if (p->id() == peripheralId)
            return *p;
    }
    return std::nullopt;
}

std::size_t Emulator::diskDriveCount() const noexcept
{
    if (const std::size_t count = m_machine->diskDriveCount())
        return count;
    for (const auto &p : m_peripherals) {
        if (const std::size_t count = p->diskDriveCount())
            return count;
    }
    return 0;
}

OptionalRef<DiskImage> Emulator::diskDrive(std::size_t drive) noexcept
{
    if (m_machine->diskDriveCount() > 0)
        return m_machine->diskDrive(drive);
    for (const auto &p : m_peripherals) {
        if (p->diskDriveCount() > 0)
            return p->diskDrive(drive);
    }
    return std::nullopt;
}

void Emulator::reset(ResetKind kind)
{
    if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("Stop RZX recording before resetting the machine");
    stopRzxPlayback();
    m_mediaBootFrame = 0;
    m_keyboard.reset();
    m_ula.endRasterFrame();
    m_clock.reset();
    m_cpu.reset(kind == ResetKind::Hard);
    m_machine->reset(m_context, m_ula, kind);
    for (auto &p : m_peripherals)
        p->reset(m_context, kind);
    m_line = 0;
    m_frameStarted = false;
    m_lineStarted = false;
    m_pendingPause.reset();
    m_stoppedAtBreakpoint.reset();
    cancelRunControl();
}

void Emulator::triggerNmi() noexcept
{
    m_cpu.requestNmi();
}

void Emulator::setNextKeyState(NextKey key, bool pressed) noexcept
{
    if (m_machine->id() == "next")
        static_cast<ZxNextMachine &>(*m_machine).setExtendedKey(key, pressed);
}

bool Emulator::triggerNextNmi(bool multiface)
{
    return m_machine->id() == "next" && static_cast<ZxNextMachine &>(*m_machine).requestNmi(m_context, multiface);
}

void Emulator::setKeyState(ZxKey key, bool pressed) noexcept
{
    m_keyboard.setKeyState(key, pressed);
}

// ── Running ───────────────────────────────────────────────────────────────

void Emulator::requestPause(PauseReason reason, uint16_t address) noexcept
{
    cancelRunControl();
    if (!m_pendingPause)
        m_pendingPause = PauseEvent{reason, m_cpu.registers().PC, m_clock.totalTstates(), address};
}

void Emulator::updateDebuggerHook() noexcept
{
    const auto &breakpoints = m_debugger.breakpoints();
    const bool hook = breakpoints.has(BreakpointType::Execute) || m_debugger.isTraceEnabled()
                      || m_stoppedAtBreakpoint.has_value() || m_debugRun.has_value();
    const bool steppingOut = m_debugRun && m_debugRun->kind != DebugRun::Kind::Target;
    const auto observers = breakpoints.enabledTypes() | (steppingOut ? 0x80000000u : 0u) | (hook ? 0x40000000u : 0u);
    if (m_debugObserverTypes == observers) return;
    m_debugObserverTypes = observers;
    m_cpu.setDebuggerHookEnabled(hook);
    const bool next = m_nextDebugMachine && breakpoints.has(BreakpointType::NextRegWrite);
    if (next != m_nextDebugArmed) {
        m_nextDebugArmed = next;
        if (next) m_nextDebugMachine->nextReg().setWatchCallback([this](uint8_t reg, uint8_t, uint8_t) {
            if (m_debugger.breakpoints().checkNextReg(reg, m_cpu.registers())) {
                requestPause(PauseReason::NextRegWatchpoint, reg);
                m_cpu.trap();
            }
        });
        else if (m_nextDebugMachine) m_nextDebugMachine->nextReg().setWatchCallback({});
    }
    const bool events = breakpoints.has(BreakpointType::Event) || steppingOut;
    if (events != m_interruptDebugArmed) {
        m_interruptDebugArmed = events;
        if (events) m_cpu.setInterruptObserver([this](bool nmi) {
            if (m_debugRun && m_debugRun->kind == DebugRun::Kind::Out) ++m_debugRun->depth;
            if (m_debugger.breakpoints().checkEvent(nmi ? DebugEvent::Nmi : DebugEvent::Interrupt, m_cpu.registers())) {
                requestPause(PauseReason::EventTrap, uint16_t(nmi ? DebugEvent::Nmi : DebugEvent::Interrupt));
                m_cpu.trap();
            }
        });
        else m_cpu.setInterruptObserver({});
    }
}

void Emulator::startRzxRecording()
{
    if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("RZX recording is already active");
    const auto machineId = rzxMachineId(m_machine->id());
    if (!machineId || m_clock.speedMultiplier() != 1)
        throw FormatException("This machine/CPU turbo mode cannot be represented by an RZX snapshot");
    stopRzxPlayback();
    clearRzxCheckpoints();
    if (!m_frameStarted) beginFrame();
    m_cpu.acknowledgePendingInterrupt();
    auto snapshot = saveSnapshot(".szx");
    snapshot[6] = *machineId;
    if (m_machine->id() == "16k" || m_machine->id() == "48k" || m_machine->id() == "48k_ntsc") {
        // Preserve the external AY interface as well as its register contents.
        StateReader chunks(std::span<const uint8_t>(snapshot).subspan(8));
        while (!chunks.eof()) {
            const auto tag = chunks.readSpan(4);
            const auto size = chunks.read<uint32_t>();
            const auto offset = 8 + chunks.position();
            chunks.readSpan(size);
            if (std::memcmp(tag.data(), "AY\0\0", 4) == 0 && size >= 18) {
                const uint8_t flags = (findPeripheral("fuller") ? 1 : 0) | (m_snapshotStandardAy ? 2 : 0);
                snapshot[offset] = flags ? flags : 2;
            }
        }
    }
    m_cpu.resetTrackedFetches();
    m_recordingBoundary = false;
    m_rzx.startRecording(std::move(snapshot), ".szx", m_ioBus, m_clock.frameBaseTstates());
    m_tapeDeck.onFrameStart(m_machine->is48kMode(), true);
    m_rzxTimelineHz = frameRateHz();
    captureRzxCheckpoint(true);
}

std::vector<uint8_t> Emulator::stopRzxRecording()
{
    if (isRzxSeeking()) throw FormatException("Wait for the RZX seek to finish");
    if (!m_rzx.isRecording() && !m_rzx.isReviewing()) throw FormatException("No active RZX recording");
    if (m_rzx.isRecording() && m_cpu.trackedFetches()) {
        // Finish the video interval so that a later replay has an actual INT boundary.
        if (runFrame() != RunStatus::FrameDone)
            throw FormatException("Cannot finish an RZX interval while the CPU is trapped");
    }
    auto bytes = m_rzx.stopRecording(m_ioBus);
    while (!m_rzxCheckpoints.empty() && m_rzxCheckpoints.back().frame > m_rzx.frameCount()) {
        m_rzxCheckpointBytes -= m_rzxCheckpoints.back().bytes;
        m_rzxCheckpoints.pop_back();
    }
    return bytes;
}

void Emulator::stopRzxPlayback() noexcept
{
    if (m_rzx.isRecording() || m_rzx.isReviewing()) return;
    m_rzx.stopPlayback();
    m_ioBus.setRzxCallbacks(nullptr, nullptr);
    m_cpu.resetTrackedFetches();
    m_rzxSeekTarget.reset();
}

void Emulator::clearRzxCheckpoints()
{
    m_rzxCheckpoints.clear();
    m_rzxCheckpointBytes = 0;
    m_rzxSeekTarget.reset();
}

void Emulator::captureRzxCheckpoint(bool force)
{
    const auto frame = m_rzx.playbackFrame();
    if (!m_rzxCheckpoints.empty()) {
        const auto previous = m_rzxCheckpoints.back().frame;
        if (frame <= previous || (!force && m_rzx.ticksAt(frame) - m_rzx.ticksAt(previous) < 50)) return;
    }
    RzxCheckpoint point;
    point.frame = frame; point.command = m_rzx.commandPosition();
    point.machine = m_machine->id(); point.state = serializeState();
    for (const auto &peripheral : m_peripherals) point.peripherals.emplace_back(peripheral->id());
    point.scheduler = m_scheduler;
    point.line = m_line; point.frameStarted = m_frameStarted; point.lineStarted = m_lineStarted;
    point.recordingBoundary = m_recordingBoundary; point.standardAy = m_snapshotStandardAy;
    point.beforeInterrupt = m_rzx.isRecording() && m_recordingBoundary;
    point.inhibitInterrupt = m_cpu.interruptJustEnabled();
    point.bytes = point.state.size() + sizeof(point);
    for (const auto &block : m_tapeDeck.blocks()) {
        point.bytes += sizeof(block) + block.description.capacity() + block.data.capacity()
            + block.metadata.capacity() + block.pulses.capacity() * sizeof(uint32_t) + block.pilotStream.capacity() * sizeof(TapeSymbolRun)
            + block.offsets.capacity() * sizeof(int16_t) + block.labels.capacity() * sizeof(std::string)
            + (block.pilotSymbols.capacity() + block.dataSymbols.capacity()) * sizeof(TapeSymbol);
        for (const auto *symbols : {&block.pilotSymbols, &block.dataSymbols})
            for (const auto &symbol : *symbols) point.bytes += symbol.pulses.capacity() * sizeof(uint16_t);
        for (const auto &label : block.labels) point.bytes += label.capacity();
    }
    // Keep the origin for arbitrary backward seeks; recent checkpoints bound replay work.
    constexpr std::size_t budget = 64 * 1024 * 1024;
    if (point.bytes > budget) {
        if (m_rzxCheckpoints.empty()) m_rzx.fail("initial seek checkpoint exceeds the memory budget");
        return;
    }
    if (!m_rzxCheckpoints.empty() && point.bytes + m_rzxCheckpoints.front().bytes > budget) return;
    point.tape = m_tapeDeck;
    m_rzxCheckpointBytes += point.bytes;
    m_rzxCheckpoints.push_back(std::move(point));
    while (m_rzxCheckpoints.size() > 2 && m_rzxCheckpointBytes > budget) {
        m_rzxCheckpointBytes -= m_rzxCheckpoints[1].bytes;
        m_rzxCheckpoints.erase(m_rzxCheckpoints.begin() + 1);
    }
}

uint64_t Emulator::rzxPositionMilliseconds() const
{
    return static_cast<uint64_t>(std::llround(m_rzx.ticksAt(m_rzx.playbackFrame()) * 1000.0 / m_rzxTimelineHz));
}

uint64_t Emulator::rzxDurationMilliseconds() const
{
    return static_cast<uint64_t>(std::llround(m_rzx.ticksAt(m_rzx.frameCount()) * 1000.0 / m_rzxTimelineHz));
}

void Emulator::beginRzxSeek(uint64_t milliseconds)
{
    const auto tick = milliseconds >= rzxDurationMilliseconds() ? m_rzx.ticksAt(m_rzx.frameCount())
        : static_cast<uint64_t>(std::llround(milliseconds * m_rzxTimelineHz / 1000.0));
    beginRzxSeekFrame(m_rzx.frameAtTick(tick));
}

void Emulator::beginRzxSeekFrame(std::size_t frame)
{
    if (m_rzxCheckpoints.empty() || m_rzx.state() == RzxState::Error || m_rzx.state() == RzxState::Idle)
        throw FormatException("No seekable RZX timeline");
    const bool reviewing = m_rzx.isRecording() || m_rzx.isReviewing();
    if (m_rzx.isRecording()) {
        if (m_cpu.trackedFetches() && runFrame() != RunStatus::FrameDone)
            throw FormatException("Cannot seek while the CPU is trapped");
        captureRzxCheckpoint(true);
    }
    frame = std::min(frame, m_rzx.frameCount());
    const auto it = std::find_if(m_rzxCheckpoints.rbegin(), m_rzxCheckpoints.rend(),
                                 [frame](const auto &point) { return point.frame <= frame; });
    const auto &point = *it;
    m_applyingRzxSnapshot = true;
    try {
        // Fuller can be added automatically by later embedded snapshots.
        const bool fuller = std::ranges::find(point.peripherals, "fuller") != point.peripherals.end();
        std::vector<std::string> current, expected;
        for (const auto &peripheral : m_peripherals)
            if (peripheral->id() != "fuller") current.emplace_back(peripheral->id());
        for (const auto &id : point.peripherals) if (id != "fuller") expected.push_back(id);
        if (current != expected) throw FormatException("Cannot seek across peripheral configuration changes");
        if (fuller && !findPeripheral("fuller")) attachPeripheral(std::make_unique<FullerBoxPeripheral>());
        if (!fuller && findPeripheral("fuller")) detachPeripheral("fuller");
        if (m_machine->id() != point.machine) selectMachine(point.machine);
        if (m_snapshotStandardAy && !point.standardAy) {
            m_ioBus.unregisterOwner(std::string(m_machine->id()) + "_ay_reg");
            m_ioBus.unregisterOwner(std::string(m_machine->id()) + "_ay_data");
        }
        if (point.standardAy && !m_snapshotStandardAy) m_machine->registerStandardAyPorts(m_context);
        m_snapshotStandardAy = point.standardAy;
        deserializeState(point.state);
        m_tapeDeck = point.tape; m_scheduler = point.scheduler;
    } catch (...) { m_applyingRzxSnapshot = false; throw; }
    m_applyingRzxSnapshot = false;
    m_line = point.line; m_frameStarted = point.frameStarted; m_lineStarted = point.lineStarted;
    m_recordingBoundary = point.recordingBoundary;
    m_cpu.resetTrackedFetches(); m_audioSamples = {};
    m_rzx.setPlaybackPosition(point.frame, point.command, reviewing, m_ioBus);
    m_rzxSeekInterrupt = point.beforeInterrupt; m_rzxSeekInhibit = point.inhibitInterrupt;
    m_rzxSeekTarget = frame;
}

bool Emulator::advanceRzxSeek(uint32_t maxVideoFrames)
{
    if (!m_rzxSeekTarget) return true;
    const uint64_t budget = uint64_t(maxVideoFrames) * (m_clock.timings().linesPerFrame + 16);
    for (uint64_t i = 0; i < budget && m_rzx.playbackFrame() < *m_rzxSeekTarget; ++i) {
        if (m_rzxSeekInterrupt) {
            m_cpu.acceptPlaybackInterrupt(m_rzx.playbackFetches() <= 4, m_rzxSeekInhibit);
            m_rzxSeekInterrupt = false;
        }
        if (runScanline() == RunStatus::Stopped) throw FormatException("RZX seek stopped at a debugger trap");
    }
    if (m_rzx.playbackFrame() < *m_rzxSeekTarget) return false;
    m_rzxSeekTarget.reset(); m_audioSamples = {};
    refreshFrame();
    return true;
}

void Emulator::resumeRzxRecording()
{
    if (isRzxSeeking()) throw FormatException("Wait for the RZX seek to finish");
    if (m_rzx.isRecording()) return;
    if (!m_rzx.isReviewing()) throw FormatException("No paused RZX recording");
    const auto frame = m_rzx.playbackFrame();
    while (!m_rzxCheckpoints.empty() && m_rzxCheckpoints.back().frame > frame) {
        m_rzxCheckpointBytes -= m_rzxCheckpoints.back().bytes;
        m_rzxCheckpoints.pop_back();
    }
    m_rzx.resumeRecording(m_ioBus);
    m_recordingBoundary = !m_frameStarted;
    m_rzxSeekInterrupt = false;
}

void Emulator::applyRzxCommands()
{
    const auto commands = m_rzx.pendingCommands();
    if (commands.empty()) return;
    try {
        for (const auto &command : commands) {
            if (command.type == RzxCommand::Type::Snapshot) {
                m_applyingRzxSnapshot = true;
                selectMachine(rzxSnapshotMachine(command.snapshot, command.extension));
                loadSnapshot(command.snapshot, command.extension);
                const auto id = m_machine->id();
                if (id == "16k" || id == "48k" || id == "48k_ntsc") {
                    const auto ayFlags = rzxSnapshotAyFlags(command.snapshot, command.extension);
                    if ((ayFlags & 1) && !findPeripheral("fuller"))
                        attachPeripheral(std::make_unique<FullerBoxPeripheral>());
                    if (ayFlags & 2) {
                        m_machine->registerStandardAyPorts(m_context);
                        m_snapshotStandardAy = true;
                    }
                }
                m_applyingRzxSnapshot = false;
            } else {
                if (command.tstates >= m_clock.timings().tstatesPerFrame())
                    throw FormatException("RZX initial T-state position is outside the machine frame");
                m_clock.setFrameTstates(command.tstates * m_clock.speedMultiplier());
                m_line = static_cast<uint16_t>(command.tstates / m_clock.timings().tstatesPerLine);
                m_frameStarted = command.tstates != 0;
                m_lineStarted = false;
            }
        }
    } catch (const std::exception &error) {
        m_applyingRzxSnapshot = false;
        m_rzx.fail(std::format("snapshot/position setup failed: {}", error.what()));
    }
    m_rzx.consumeCommands(commands.size());
    m_rzx.connectPlayback(m_ioBus); // snapshot loading may have rebuilt the machine's bus
}

void Emulator::finishPlaybackPresentation()
{
    const auto &timings = m_clock.timings();
    // Finish unexecuted raster time without running any unrecorded instructions.
    if (m_clock.frameBaseTstates() < timings.tstatesPerFrame())
        m_clock.advance((timings.tstatesPerFrame() - m_clock.frameBaseTstates()) * m_clock.speedMultiplier());
    m_scheduler.processDue(m_clock.totalTstates());
    m_tapeDeck.update(m_context);
    if (m_machine->usesRasterUla()) m_ula.renderUntil(timings.tstatesPerFrame(), m_mmu);
    for (; m_line < timings.linesPerFrame; ++m_line) {
        const int32_t y = int32_t(m_line) - timings.topBorderLines + m_ula.borderMargins().top;
        if (y >= 0 && y < int32_t(m_ula.outputHeight())) {
            auto frame = m_frame.writeLock();
            if (m_machine->usesRasterUla()) m_ula.copyRasterScanline(y, *frame);
            else m_machine->renderScanline(y, m_context, m_ula, *frame);
        }
    }
    endFrame();
    // A recorded fetch boundary is not an instruction overshooting a raster
    // boundary. Contention differences must not accumulate into the next frame.
    m_clock.setFrameTstates(0);
}

RunStatus Emulator::runPlaybackScanline()
{
    // Fetch/INT boundaries are independent of the machine's raster timing.
    // Short retrigger records share a video frame with the following interval.
    for (unsigned boundaries = 0; boundaries < 1024; ++boundaries) {
        if (!m_frameStarted) beginFrame();
        const auto &timings = m_clock.timings();
        const auto fetches = m_rzx.playbackFetches();
        if (!m_lineStarted) {
            m_lineStarted = true;
            if (m_line < timings.linesPerFrame) m_machine->onScanlineStart(m_line, m_context);
            if (m_debugger.breakpoints().has(BreakpointType::RasterLine)
            && m_debugger.breakpoints().checkRaster(m_line, m_cpu.registers())) requestPause(PauseReason::RasterBreakpoint, m_line);
            if (m_pendingPause) return RunStatus::Stopped;
        }
        const auto target = std::max<uint32_t>((uint32_t(m_line) + 1) * timings.tstatesPerLine,
                                               m_clock.frameBaseTstates() + 1);
        m_cpu.runTrackedUntil(target, fetches, true);
        if (m_cpu.isTrapped()) {
            if (!m_pendingPause) requestPause(PauseReason::MagicBreakpoint);
            return RunStatus::Stopped;
        }
        if (m_cpu.trackedFetches() > fetches) m_rzx.fail("fetch boundary splits a CPU instruction");
        if (m_clock.frameBaseTstates() > timings.tstatesPerFrame() * 16)
            m_rzx.fail("recorded interval exceeds the playback timing limit");
        if (m_cpu.trackedFetches() == fetches) {
            const bool inhibitInterrupt = m_cpu.interruptJustEnabled();
            m_rzx.advancePlaybackFrame(m_ioBus); // validates exact input consumption
            m_cpu.resetTrackedFetches();
            const bool present = fetches > 4 || !m_rzx.isPlaying();
            if (present) finishPlaybackPresentation();
            applyRzxCommands();
            if (m_rzx.isPlaying()) {
                m_cpu.acceptPlaybackInterrupt(m_rzx.playbackFetches() <= 4, inhibitInterrupt);
                m_tapeDeck.onFrameStart(m_machine->is48kMode(), true);
            } else {
                m_cpu.acceptPlaybackInterrupt(false);
                // Rejoin the machine's normal level pulse after the final RZX INT.
                m_cpu.requestInterrupt(0xff, m_clock.frameTstates());
            }
            if (!present && m_rzxSeekTarget && m_rzx.playbackFrame() >= *m_rzxSeekTarget)
                return RunStatus::LineDone;
            if (present) {
                captureRzxCheckpoint();
                if (!m_rzx.isPlaying() && !m_rzx.isReviewing()) requestPause(PauseReason::UserRequest);
                return RunStatus::FrameDone;
            }
            continue;
        }
        m_scheduler.processDue(m_clock.totalTstates());
        m_tapeDeck.update(m_context);
        const int32_t y = int32_t(m_line) - timings.topBorderLines + m_ula.borderMargins().top;
        if (m_line < timings.linesPerFrame && y >= 0 && y < int32_t(m_ula.outputHeight())) {
            auto frame = m_frame.writeLock();
            if (m_machine->usesRasterUla()) {
                m_ula.renderUntil(m_clock.frameBaseTstates(), m_mmu);
                m_ula.copyRasterScanline(y, *frame);
            } else m_machine->renderScanline(y, m_context, m_ula, *frame);
        }
        m_lineStarted = false;
        ++m_line;
        return m_pendingPause ? RunStatus::Stopped : RunStatus::LineDone;
    }
    return RunStatus::LineDone;
}

void Emulator::beginFrame()
{
    if (m_mediaBootFrame) advanceMediaBoot();
    m_frameStarted = true;
    m_ula.setBorderMargins(effectiveBorderMargins());
    if (m_machine->usesRasterUla())
        m_ula.beginRasterFrame(m_clock.timings());
    m_tapeDeck.onFrameStart(m_machine->is48kMode(), m_rzx.isPlaying() || m_rzx.isRecording());
    for (auto &periph : m_peripherals)
        periph->onFrameStart();
    // The ULA frame interrupt (machines may gate or redirect it)
    if (!m_rzx.isPlaying())
        m_machine->onFrameStart(m_context);
    m_frame.writeLock()->setFrameNumber(m_clock.frameCount());
}

RunStatus Emulator::runScanline()
{
    m_pendingPause.reset();
    updateDebuggerHook();

    if (m_rzx.isPlaying())
        return runPlaybackScanline();

    if (!m_frameStarted)
        beginFrame();

    const auto &timings = m_clock.timings();
    if (!m_lineStarted) {
        m_lineStarted = true;
        m_machine->onScanlineStart(m_line, m_context);
        if (m_debugger.breakpoints().has(BreakpointType::RasterLine)
            && m_debugger.breakpoints().checkRaster(m_line, m_cpu.registers())) requestPause(PauseReason::RasterBreakpoint, m_line);
        if (m_pendingPause) return RunStatus::Stopped;
    }

    const uint32_t lineEnd = (static_cast<uint32_t>(m_line) + 1) * timings.tstatesPerLine;
    if (m_rzx.isRecording()) {
        while (m_clock.frameBaseTstates() < lineEnd) {
            if (m_cpu.runTrackedUntil(lineEnd, std::numeric_limits<uint32_t>::max(), false)) {
                if (!m_recordingBoundary || m_cpu.trackedFetches())
                    m_rzx.endRecordedFrame(m_cpu.trackedFetches());
                m_cpu.resetTrackedFetches();
                m_recordingBoundary = false;
            }
            if (m_cpu.isTrapped()) break;
        }
    } else {
        m_cpu.runUntil(lineEnd);
    }
    if (m_cpu.isTrapped()) {
        // Not a breakpoint nor a watchpoint: a magic breakpoint (ED FF / DD 01)
        requestPause(PauseReason::MagicBreakpoint);
        return RunStatus::Stopped;
    }

    m_scheduler.processDue(m_clock.totalTstates());
    m_tapeDeck.update(m_context);

    // Presentation margins never change the machine's scanline timeline.
    const int32_t visY = static_cast<int32_t>(m_line) - static_cast<int32_t>(timings.topBorderLines)
                         + static_cast<int32_t>(m_ula.borderMargins().top);
    if (visY >= 0 && visY < static_cast<int32_t>(m_ula.outputHeight())) {
        auto frame = m_frame.writeLock();
        if (m_machine->usesRasterUla()) {
            m_ula.renderUntil(m_clock.frameBaseTstates(), m_mmu);
            m_ula.copyRasterScanline(static_cast<uint32_t>(visY), *frame);
        } else {
            m_machine->renderScanline(static_cast<uint32_t>(visY), m_context, m_ula, *frame);
        }
    }

    m_lineStarted = false;
    if (++m_line >= timings.linesPerFrame) {
        // A short frame (the TS2068's 262 lines) ends before the window's last rows: the
        // bottom border goes on there
        const int32_t nextY = static_cast<int32_t>(timings.linesPerFrame) - static_cast<int32_t>(timings.topBorderLines)
                              + static_cast<int32_t>(m_ula.borderMargins().top);
        if (nextY < static_cast<int32_t>(m_ula.outputHeight()) && !m_machine->usesRasterUla()) {
            auto frame = m_frame.writeLock();
            for (int32_t y = std::max(nextY, 0); y < static_cast<int32_t>(m_ula.outputHeight()); ++y)
                m_machine->renderScanline(static_cast<uint32_t>(y), m_context, m_ula, *frame);
        }
        endFrame();
        return RunStatus::FrameDone;
    }
    // A watchpoint hit by a scheduled event (e.g. a DMA transfer) stops after the line
    return m_pendingPause ? RunStatus::Stopped : RunStatus::LineDone;
}

void Emulator::endFrame()
{
    m_machine->onFrameEnd(m_context, m_ula);
    for (auto &periph : m_peripherals)
        periph->onFrameEnd();

    if (m_rewindEnabled)
        m_debugger.rewindBuffer().pushSnapshot(serializeState());

    if (m_rzx.isRecording()) {
        m_rzx.endRecordedFrame(m_cpu.trackedFetches());
        m_cpu.resetTrackedFetches();
        m_recordingBoundary = true;
    }

    // Clock carries the last instruction's overshoot into the next video frame.
    // Audio must use the nominal boundary too, or those T-states are counted twice.
    m_audioSamples = m_audioMixer.endFrame(m_clock.timings().tstatesPerFrame());
    m_ula.endRasterFrame();
    m_clock.beginNewFrame();
    updateMediaActivity();

    // Publish the frame; the next one goes into a recycled frame nobody holds anymore
    VideoFramePtr next = acquireFrame();
    m_lastFrame = std::exchange(m_frame, std::move(next));
    m_line = 0;
    m_frameStarted = false;
    if (m_rzx.isRecording()) captureRzxCheckpoint();
}

RunStatus Emulator::runFrame()
{
    for (;;) {
        if (const RunStatus status = runScanline(); status != RunStatus::LineDone)
            return status;
    }
}

VideoFramePtr Emulator::runFrames(uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (runFrame() != RunStatus::FrameDone || m_pendingPause)
            break;
    }
    return m_lastFrame;
}

VideoFramePtr Emulator::acquireFrame()
{
    // A pooled frame nobody else references can't be referenced again behind our back:
    // the pool is private and the frames go out only through the (locked) emulator.
    for (const auto &frame : m_framePool) {
        if (frame.useCount() == 1)
            return frame;
    }
    auto frame = makeShared<VideoFrame>(m_machine->frameWidth(), m_machine->frameHeight());
    if (m_framePool.size() < kMaxPooledFrames)
        m_framePool.push_back(frame);
    return frame;
}

const VideoFramePtr &Emulator::refreshFrame()
{
    VideoFramePtr frame = acquireFrame();
    {
        auto out = frame.writeLock();
        out->setFrameNumber(m_clock.frameCount());
        // A paused refresh may happen mid-frame. Keep its raster intact.
        auto renderer = m_ula;
        renderer.setBorderMargins(effectiveBorderMargins());
        m_machine->renderFullFrame(m_context, renderer, *out);
    }
    m_lastFrame = std::move(frame);
    return m_lastFrame;
}

BorderMargins Emulator::effectiveBorderMargins() const noexcept
{
    if (!m_config.fullBorder)
        return {};
    const auto &t = m_clock.timings();
    // Leave one line before the topmost row for the left-border/fetch timeline.
    // Short-frame machines retain at least the traditional 32-line presentation.
    return {uint16_t(std::min<uint32_t>(128, t.leftBorderTstates * 2)),
            uint16_t(std::clamp<int32_t>(int32_t(t.topBorderLines) - 1, 32, 128)),
            uint16_t(std::min<uint32_t>(128, t.rightBorderTstates * 2)),
            uint16_t(std::clamp<int32_t>(int32_t(t.bottomBorderLines), 32, 128))};
}

double Emulator::frameRateHz() const noexcept
{
    const double hz = m_clock.timings().frameRateHz();
    return hz > 1.0 ? hz : 50.0;
}

void Emulator::setRewindEnabled(bool enabled, std::size_t maxFrames)
{
    m_rewindEnabled = enabled;
    m_debugger.rewindBuffer().setCapacity(maxFrames);
    if (!enabled)
        m_debugger.rewindBuffer().clear();
}

void Emulator::rewind(std::size_t framesBack)
{
    if (m_rzx.isPlaying() || m_rzx.isRecording() || m_rzx.isReviewing())
        throw FormatException("Use RZX seeking instead of debugger rewind during playback/recording");
    deserializeState(m_debugger.rewindBuffer().popStepsBack(framesBack));
    refreshFrame();
}

std::vector<uint8_t> Emulator::serializeState() const
{
    StateWriter w(65536);
    m_clock.saveState(w);
    m_cpu.saveState(w);
    m_im2.saveState(w);
    m_mmu.saveState(w);
    m_ula.saveState(w);
    m_audioMixer.saveState(w);
    m_machine->saveState(w);
    for (const auto &p : m_peripherals)
        p->saveState(w);
    w.write(m_mediaBootFrame); w.writeBool(m_mediaBoot48k);
    return w.takeBuffer();
}

void Emulator::deserializeState(std::span<const uint8_t> bytes)
{
    StateReader r(bytes);
    m_clock.loadState(r);
    m_cpu.loadState(r);
    m_im2.loadState(r);
    m_mmu.loadState(r);
    m_ula.loadState(r);
    m_audioMixer.loadState(r);
    m_machine->loadState(r, m_context, m_ula);
    for (auto &p : m_peripherals)
        p->loadState(r);
    m_mediaBootFrame = r.read<uint16_t>(); m_mediaBoot48k = r.readBool();
    if (!r.eof()) throw FormatException("Trailing native state data");
    // Rewind snapshots are taken at frame ends
    m_line = 0;
    m_frameStarted = false;
    m_lineStarted = false;
    m_pendingPause.reset();
    m_stoppedAtBreakpoint.reset();
}

// ── Media ─────────────────────────────────────────────────────────────────

void Emulator::loadMedia(const MediaFile &file)
{
    const std::string &ext = file.extension;
    if (ext != ".rzx" && !m_rzx.isRecording() && !m_rzx.isReviewing()) {
        stopRzxPlayback();
        m_rzx.clear();
        clearRzxCheckpoints();
    }
    if (m_config.mediaFormats.hasLoader(ext)) {
        m_config.mediaFormats.invokeLoader(ext, *this, file.data);
        return;
    }

    if (std::ranges::contains(kSnapshotExtensions, ext)) {
        loadSnapshot(file.data, ext);
    } else if (ext == ".tap" || ext == ".tzx" || ext == ".pzx" || ext == ".csw") {
        loadTape(file.data, ext);
    } else if (std::ranges::contains(kDiskExtensions, ext)) {
        if (!diskDrive(0))
            throw PeripheralException("This machine has no floppy drive");
        insertDisk(0, file.data, ext);
    } else if (ext == ".rzx") {
        if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("Stop recording before opening an RZX file");
        m_rzx.loadRzx(file.data, m_ioBus);
        applyRzxCommands();
        m_cpu.resetTrackedFetches();
        m_tapeDeck.onFrameStart(m_machine->is48kMode(), m_rzx.isPlaying());
        clearRzxCheckpoints();
        m_rzxTimelineHz = frameRateHz();
        captureRzxCheckpoint(true);
    } else if (ext == ".scr") {
        if (file.data.size() < 6912)
            throw FormatException("SCR file must be at least 6912 bytes");
        for (std::size_t i = 0; i < 6912; ++i)
            m_mmu.poke(static_cast<uint16_t>(0x4000 + i), file.data[i]);
        refreshFrame();
    } else {
        throw FormatException(std::format("Unsupported media file extension '{}'", ext));
    }
}

std::vector<uint8_t> Emulator::saveMedia(std::string_view extension)
{
    const std::string ext = normalizedExtension(extension);
    if (m_config.mediaFormats.hasSaver(ext))
        return m_config.mediaFormats.invokeSaver(ext, *this);
    if (ext == ".sna" || ext == ".szx" || ext == ".z80" || ext == ".nex")
        return saveSnapshot(ext);
    if (ext == ".tap")
        return m_tapeDeck.saveTap();
    if (ext == ".tzx")
        return m_tapeDeck.saveTzx();
    if (ext == ".png")
        return screenshotPng();
    throw FormatException(std::format("Unsupported save file extension '{}'", ext));
}

void Emulator::saveMediaFile(const std::filesystem::path &path)
{
    writeFileBytes(path, saveMedia(path.extension().string()));
}

void Emulator::loadSnapshot(std::span<const uint8_t> data, std::string_view extension)
{
    cancelRunControl();
    if (!m_applyingRzxSnapshot) {
        if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("Stop recording before loading a snapshot");
        stopRzxPlayback();
    }
    const std::string ext = normalizedExtension(extension);
    if (ext == ".szx" && !m_applyingRzxSnapshot && data.size() >= 8 && std::memcmp(data.data(), "ZXST", 4) == 0) {
        StateReader chunks(data.subspan(8));
        while (!chunks.eof()) {
            const auto tag = chunks.readSpan(4);
            const auto chunk = chunks.readSpan(chunks.read<uint32_t>());
            if (std::memcmp(tag.data(), "ZKNX", 4)) continue;
            StateReader body(chunk);
            if (body.read<uint32_t>() != 1) throw FormatException("Unsupported ZxKit Next SZX state version");
            const auto line = body.read<uint16_t>();
            const auto frameStarted = body.readBool(), lineStarted = body.readBool();
            const auto peripherals = body.read<uint32_t>();
            if (peripherals != m_peripherals.size()) throw FormatException("Next SZX requires the same attached peripherals");
            for (const auto &p : m_peripherals)
                if (body.readString() != p->id()) throw FormatException("Next SZX peripheral configuration does not match");
            const auto decoded = zlibDecompressBounded(body.readSpan(body.remaining()), 32 * 1024 * 1024);
            if (m_machine->id() != "next") selectMachine("next");
            m_mediaBootFrame = 0;
            if (line >= m_clock.timings().linesPerFrame) throw FormatException("Invalid Next SZX scanline");
            const auto previous = serializeState();
            const auto previousLine = m_line;
            const bool previousFrameStarted = m_frameStarted, previousLineStarted = m_lineStarted;
            try { deserializeState(decoded); }
            catch (...) {
                deserializeState(previous);
                m_line = previousLine; m_frameStarted = previousFrameStarted; m_lineStarted = previousLineStarted;
                throw;
            }
            m_line = line; m_frameStarted = frameStarted; m_lineStarted = lineStarted;
            refreshFrame();
            return;
        }
    }
    if (!m_applyingRzxSnapshot) {
        const auto id = SnapshotFormats::machineId(data, ext);
        if (id != m_machine->id()) selectMachine(id);
    }
    if (ext == ".nex" && data.size() >= 512 && data[134] == 0)
        reset(ResetKind::Soft); // NEX's default requests a clean device/register state.
    m_mediaBootFrame = 0;
    m_ula.endRasterFrame();
    m_cpu.registers().halted = false;
    m_cpu.registers().eiDelayed = false;
    m_cpu.registers().Q = m_cpu.registers().lastQ = 0;
    m_clock.setFrameTstates(0);
    SnapshotMachineState state{m_context, *m_machine, m_ula};
    if (ext == ".sna")
        SnapshotFormats::loadSna(data, state);
    else if (ext == ".z80")
        SnapshotFormats::loadZ80(data, state);
    else if (ext == ".szx")
        SnapshotFormats::loadSzx(data, state);
    else if (ext == ".nex")
        SnapshotFormats::loadNex(data, state);
    else
        throw FormatException(std::format("Unsupported snapshot format '{}'", ext));

    if (m_machine->usesRasterUla())
        m_ula.beginRasterFrame(m_clock.timings());

    // Continue the frame from wherever the snapshot's clock is
    const auto &timings = m_clock.timings();
    const uint32_t ts = m_clock.frameBaseTstates();
    m_line = static_cast<uint16_t>(std::min<uint32_t>(ts / timings.tstatesPerLine, timings.linesPerFrame - 1u));
    m_frameStarted = ts > 0;
    m_lineStarted = false;
    m_pendingPause.reset();
    m_stoppedAtBreakpoint.reset();
    refreshFrame();
}

std::vector<uint8_t> Emulator::saveSnapshot(std::string_view extension)
{
    const std::string ext = normalizedExtension(extension);
    SnapshotMachineState state{m_context, *m_machine, m_ula};
    if (ext == ".sna")
        return m_mmu.bank(m_machine->ramBankId()).count16kPages() > 3
            ? SnapshotFormats::saveSna128k(state) : SnapshotFormats::saveSna48k(state);
    if (ext == ".z80") return SnapshotFormats::saveZ80(state);
    if (ext == ".nex") return SnapshotFormats::saveNex(state);
    // SZX has no standard Next device blocks. Keep its classic preview/base chunks, and add
    // a versioned ZxKit extension for native Next state. Other readers may ignore this chunk.
    if (ext == ".szx" && m_machine->id() == "next") {
        auto base = SnapshotFormats::saveSzx(state);
        StateWriter body;
        body.write<uint32_t>(1); // ZKNX native state schema; increment when serializeState changes.
        body.write<uint16_t>(m_line);
        body.writeBool(m_frameStarted); body.writeBool(m_lineStarted);
        body.write<uint32_t>(static_cast<uint32_t>(m_peripherals.size()));
        for (const auto &p : m_peripherals) body.writeString(p->id());
        body.writeBytes(zlibCompress(serializeState()));
        StateWriter out;
        out.writeBytes(base); out.writeTag("ZKNX");
        out.write<uint32_t>(static_cast<uint32_t>(body.size())); out.writeBytes(body.data());
        return out.takeBuffer();
    }
    if (ext == ".szx")
        return SnapshotFormats::saveSzx(state);
    throw FormatException(std::format("Unsupported snapshot save format '{}'", ext));
}

void Emulator::loadTape(std::span<const uint8_t> data, std::string_view extension)
{
    const std::string ext = normalizedExtension(extension);
    if (ext == ".tap")
        m_tapeDeck.loadTap(data);
    else if (ext == ".tzx")
        m_tapeDeck.loadTzx(data);
    else if (ext == ".pzx")
        m_tapeDeck.loadPzx(data);
    else if (ext == ".csw")
        m_tapeDeck.loadCsw(data);
    else
        throw FormatException(std::format("Unsupported tape format '{}'", ext));
}

bool Emulator::autoLoadTape(std::size_t block)
{
    const auto id = m_machine->id();
    if (id != "16k" && id != "48k" && id != "48k_ntsc" && id != "128k" && id != "plus2"
        && id != "plus2a" && id != "plus3" && id != "plus3e")
        return false;
    if (block >= m_tapeDeck.blocks().size()) throw InvalidArgumentException("Tape block is outside the tape");
    reset(ResetKind::Soft);
    m_tapeDeck.seekBlock(block);
    m_tapeDeck.setPlaying(false);
    m_mediaBoot48k = id == "16k" || id == "48k" || id == "48k_ntsc";
    m_mediaBootFrame = 1;
    return true;
}

bool Emulator::autoBootDisk()
{
    auto disk = diskDrive(0);
    if (!disk || disk->get().empty()) return false;
    Beta128Peripheral *beta = nullptr;
    if (auto p = findPeripheral("beta128")) beta = dynamic_cast<Beta128Peripheral *>(&p->get());
    else if (auto *p = dynamic_cast<PentagonMachine *>(m_machine.get())) beta = &p->beta();
    else if (auto *p = dynamic_cast<ScorpionMachine *>(m_machine.get())) beta = &p->beta();
    const auto id = m_machine->id();
    if (!beta && id != "plus3" && id != "plus3e") return false;
    // Refuse before modifying the image or keyboard when an RZX edit is active.
    if (m_rzx.isRecording() || m_rzx.isReviewing()) throw FormatException("Stop RZX recording before booting a disk");
    if (beta) {
        insertTrDosBootLoader(disk->get());
        reset(ResetKind::Hard);
        m_ioBus.write(0x7FFD, 0x10);
        beta->setPaged(m_context, true);
        m_cpu.registers().PC = 0;
    } else {
        reset(ResetKind::Soft);
        m_mediaBoot48k = false;
        m_mediaBootFrame = 1; // Enter on the +3's Loader, after the ROM has initialized.
    }
    return true;
}

void Emulator::advanceMediaBoot() noexcept
{
    const uint16_t frame = m_mediaBootFrame++;
    if (frame < 90) return;
    static constexpr std::array<ZxKey,4> keys{ZxKey::J, ZxKey::P, ZxKey::P, ZxKey::Enter};
    const unsigned step = (frame - 90) / 5;
    const unsigned count = m_mediaBoot48k ? 8 : 2;
    if (step >= count) { m_mediaBootFrame = 0; return; }
    const auto key = m_mediaBoot48k ? keys[step / 2] : ZxKey::Enter;
    const bool pressed = (step & 1) == 0;
    setKeyState(key, pressed);
    if (m_mediaBoot48k && (step / 2 == 1 || step / 2 == 2)) setKeyState(ZxKey::SymbolShift, pressed);
}

void Emulator::insertDisk(std::size_t drive, std::span<const uint8_t> data, std::string_view extension)
{
    auto disk = diskDrive(drive);
    if (!disk)
        throw PeripheralException(std::format("No floppy drive {} on this machine", drive));
    disk->get().loadFromBytes(data, normalizedExtension(extension));
}

void Emulator::ejectDisk(std::size_t drive)
{
    if (auto disk = diskDrive(drive))
        disk->get().clear();
}

void Emulator::insertSdCard(const std::filesystem::path &image, bool readOnly)
{
    m_sdCard.insert(image, readOnly);
}

void Emulator::ejectSdCard()
{
    m_sdCard.eject();
}

FatVolume Emulator::sdCardVolume()
{
    if (!m_sdCard.isInserted())
        throw PeripheralException("No SD card");
    FatVolume::BlockWriter writer;
    if (!m_sdCard.isReadOnly()) {
        writer = [&card = m_sdCard](uint64_t lba, std::span<const uint8_t, 512> in) {
            return card.writeBlock(lba, in);
        };
    }
    return FatVolume([&card = m_sdCard](uint64_t lba, std::span<uint8_t, 512> out) { return card.readBlock(lba, out); },
                     std::move(writer));
}

MediaActivity Emulator::mediaActivity() const noexcept
{
    MediaActivity activity = m_mediaActivity;
    activity.tapePlaying = m_tapeDeck.isPlaying();
    activity.tapeBlock = m_tapeDeck.currentBlockIndex();
    return activity;
}

void Emulator::updateMediaActivity()
{
    // An access keeps a device busy for this many frames (0.2 s)
    constexpr uint64_t kHoldFrames = 10;
    const uint64_t frame = m_clock.frameCount();

    uint8_t busyDisks = 0;
    const std::size_t drives = std::min(diskDriveCount(), kMaxActivityDrives);
    for (std::size_t i = 0; i < drives; ++i) {
        if (const auto disk = diskDrive(i)) {
            if (const uint64_t count = disk->get().accessCount(); count != m_diskAccesses[i]) {
                m_diskAccesses[i] = count;
                m_diskBusyUntil[i] = frame + kHoldFrames;
            }
        }
        if (frame < m_diskBusyUntil[i])
            busyDisks |= static_cast<uint8_t>(1u << i);
    }
    if (const uint64_t count = m_sdCard.accessCount(); count != m_sdAccesses) {
        m_sdAccesses = count;
        m_sdBusyUntil = frame + kHoldFrames;
    }
    m_mediaActivity.busyDisks = busyDisks;
    m_mediaActivity.sdCardBusy = frame < m_sdBusyUntil;
}

std::vector<uint8_t> Emulator::screenshotPng()
{
    return encodePng(*refreshFrame().readLock());
}

// ── Debugger ──────────────────────────────────────────────────────────────

void Emulator::setRegisters(const Z80Registers &regs) noexcept
{
    m_cpu.setRegisters(regs);
}

std::vector<uint8_t> Emulator::readMemory(uint16_t address, std::size_t length) const
{
    std::vector<uint8_t> out(length);
    for (std::size_t i = 0; i < length; ++i)
        out[i] = m_mmu.peek(static_cast<uint16_t>(address + i));
    return out;
}

void Emulator::writeMemory(uint16_t address, std::span<const uint8_t> data, bool allowRomWrite) noexcept
{
    for (std::size_t i = 0; i < data.size(); ++i)
        m_mmu.poke(static_cast<uint16_t>(address + i), data[i], allowRomWrite);
}

Z80Registers Emulator::stepInstruction()
{
    if (m_rzx.isPlaying() || m_rzx.isRecording() || m_rzx.isReviewing())
        throw DebuggerException("Stop RZX playback/recording before single-stepping");
    cancelRunControl();
    m_pendingPause.reset();
    m_stoppedAtBreakpoint = m_cpu.registers().PC;
    updateDebuggerHook();
    m_cpu.clearTrap();
    m_cpu.stepInstruction();
    m_cpu.clearTrap();
    if (!m_pendingPause) requestPause(PauseReason::StepComplete);
    return m_cpu.registers();
}

bool Emulator::runTo(uint16_t address)
{
    cancelRunControl();
    if (m_cpu.registers().PC == address) return false;
    const auto &map = m_mmu.effectiveMapping(address >> Mmu::kPageShift);
    m_debugRun = DebugRun{DebugRun::Kind::Target, address, std::nullopt, map.bankId, map.readPage1k};
    m_stoppedAtBreakpoint = m_cpu.registers().PC;
    return true;
}

bool Emulator::stepOver()
{
    const uint16_t pc = m_cpu.registers().PC;
    const auto inst = Disassembler::disassembleOne([this](uint16_t a) { return m_mmu.peek(a); }, pc, m_cpu.isZ80nEnabled());
    if (!inst.isStepOverCandidate) { stepInstruction(); return false; }
    const auto address = static_cast<uint16_t>(pc + inst.length);
    if (!runTo(address)) return false;
    m_debugRun->stack = m_cpu.registers().SP;
    return true;
}

bool Emulator::stepOut()
{
    cancelRunControl();
    m_debugRun = DebugRun{DebugRun::Kind::Out, 0, m_cpu.registers().SP};
    m_stoppedAtBreakpoint = m_cpu.registers().PC;
    return true;
}

bool Emulator::checkRunControl(uint16_t pc)
{
    auto &run = *m_debugRun;
    const auto &regs = m_cpu.registers();
    if (run.kind == DebugRun::Kind::Target) {
        const auto &map = m_mmu.effectiveMapping(pc >> Mmu::kPageShift);
        return pc == run.address && (!run.stack || regs.SP == *run.stack)
            && map.bankId == run.bank && map.readPage1k == run.page1k;
    }
    if (run.kind == DebugRun::Kind::Returning) return regs.SP == *run.stack;
    uint16_t at = pc;
    uint8_t opcode = m_mmu.peek(at++);
    // DD/FD before RET are ignored by the CPU; bound a malformed all-prefix address space.
    for (unsigned prefixes = 0; (opcode == 0xDD || opcode == 0xFD) && prefixes < 65536; ++prefixes)
        opcode = m_mmu.peek(at++);
    const auto flags = regs.F();
    const std::array<bool, 8> conditions{!(flags & 0x40), bool(flags & 0x40), !(flags & 1), bool(flags & 1),
                                       !(flags & 4), bool(flags & 4), !(flags & 0x80), bool(flags & 0x80)};
    if (opcode == 0xCD || ((opcode & 0xC7) == 0xC4 && conditions[(opcode >> 3) & 7]) || (opcode & 0xC7) == 0xC7) {
        ++run.depth;
        return false;
    }
    bool returns = opcode == 0xC9;
    if ((opcode & 0xC7) == 0xC0) returns = conditions[(opcode >> 3) & 7];
    else if (opcode == 0xED) returns = (m_mmu.peek(at) & 0xC7) == 0x45;
    if (returns) {
        if (run.depth) { --run.depth; return false; }
        if (static_cast<int16_t>(uint16_t(regs.SP - *run.stack)) < 0) return false;
        run.kind = DebugRun::Kind::Returning;
        run.stack = uint16_t(regs.SP + 2);
    }
    return false;
}

uint32_t Emulator::addBreakpoint(Breakpoint breakpoint)
{
    const auto id = m_debugger.breakpoints().add(std::move(breakpoint), m_mmu);
    updateDebuggerHook();
    return id;
}
bool Emulator::removeBreakpoint(uint32_t id)
{
    const bool removed = m_debugger.breakpoints().remove(id, m_mmu);
    updateDebuggerHook();
    return removed;
}
bool Emulator::setBreakpointEnabled(uint32_t id, bool enabled)
{
    const bool found = m_debugger.breakpoints().setEnabled(id, enabled, m_mmu);
    updateDebuggerHook();
    return found;
}
void Emulator::clearBreakpoints()
{
    m_debugger.breakpoints().clear(m_mmu);
    updateDebuggerHook();
}

std::vector<DisasmInstruction> Emulator::disassemble(uint16_t address, std::size_t count) const
{
    return Disassembler::disassembleCount([this](uint16_t a) { return m_mmu.peek(a); }, address, count,
                                          m_cpu.isZ80nEnabled());
}

std::vector<uint8_t> Emulator::assembleAndPatch(uint16_t address, std::string_view mnemonic, bool allowRomWrite)
{
    auto bytes = Assembler::assembleOne(mnemonic, address, {.z80n = m_cpu.isZ80nEnabled()});
    writeMemory(address, bytes, allowRomWrite);
    return bytes;
}

std::size_t Emulator::pokeFinderReset()
{
    m_debugger.pokeFinder().reset(m_mmu, m_machine->ramBankId());
    return m_debugger.pokeFinder().count();
}

std::vector<PokeCandidate> Emulator::pokeFinderSearch(uint8_t value)
{
    m_debugger.pokeFinder().search(m_mmu, value);
    return m_debugger.pokeFinder().results();
}

std::vector<PokeCandidate> Emulator::pokeFinderSearchIncremented()
{
    m_debugger.pokeFinder().searchIncremented(m_mmu);
    return m_debugger.pokeFinder().results();
}

std::vector<PokeCandidate> Emulator::pokeFinderSearchDecremented()
{
    m_debugger.pokeFinder().searchDecremented(m_mmu);
    return m_debugger.pokeFinder().results();
}

} // namespace zxkit
