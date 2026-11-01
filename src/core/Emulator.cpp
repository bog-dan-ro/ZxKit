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

#include "zxkit/core/Exception.h"
#include "zxkit/debug/Assembler.h"
#include "zxkit/media/Compression.h"
#include "zxkit/media/ScreenshotFormat.h"
#include "zxkit/media/SnapshotFormats.h"

#include <algorithm>
#include <array>
#include <cctype>
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
    m_scheduler.clear();
    m_ioBus.clear();
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
        // Resuming from (or stepping over) a breakpoint runs its instruction
        if (m_stoppedAtBreakpoint) {
            const bool resuming = *m_stoppedAtBreakpoint == pc;
            m_stoppedAtBreakpoint.reset();
            if (resuming)
                return false;
        }
        if (m_debugger.breakpoints().checkExecute(pc, m_mmu, m_cpu.registers())) {
            m_stoppedAtBreakpoint = pc;
            requestPause(PauseReason::ExecutionBreakpoint);
            return true;
        }
        if (m_debugger.isTraceEnabled())
            m_debugger.recordTrace(m_clock.totalTstates(), m_cpu.registers());
        return false;
    });

    m_mmu.setWatchpointCallback([this](uint16_t addr, uint8_t /*val*/, bool isWrite) {
        if (m_debugger.breakpoints().checkMemory(addr, isWrite, m_cpu.registers())) {
            requestPause(PauseReason::MemoryWatchpoint);
            m_cpu.trap();
        }
    });

    m_ioBus.setWatchpointCallback([this](uint16_t port, uint8_t /*val*/, bool isWrite) {
        if (m_debugger.breakpoints().checkPort(port, isWrite, m_cpu.registers())) {
            requestPause(PauseReason::PortWatchpoint);
            m_cpu.trap();
        }
    });

    const bool sameFrameSize = m_machine && m_machine->frameWidth() == machine->frameWidth()
                               && m_machine->frameHeight() == machine->frameHeight();
    m_machine = std::move(machine);
    m_machine->initialize(m_context, m_ula);
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
}

void Emulator::triggerNmi() noexcept
{
    m_cpu.requestNmi();
}

void Emulator::setKeyState(ZxKey key, bool pressed) noexcept
{
    m_keyboard.setKeyState(key, pressed);
}

// ── Running ───────────────────────────────────────────────────────────────

void Emulator::requestPause(PauseReason reason) noexcept
{
    if (!m_pendingPause)
        m_pendingPause = PauseEvent{reason, m_cpu.registers().PC, m_clock.totalTstates()};
}

void Emulator::updateDebuggerHook() noexcept
{
    // Breakpoints and tracing cost a call per instruction, only pay for them when used
    m_cpu.setDebuggerHookEnabled(!m_debugger.breakpoints().empty() || m_debugger.isTraceEnabled()
                                 || m_stoppedAtBreakpoint.has_value());
}

void Emulator::beginFrame()
{
    m_frameStarted = true;
    if (m_machine->usesRasterUla())
        m_ula.beginRasterFrame(m_clock.timings());
    m_tapeDeck.onFrameStart(m_machine->is48kMode(), m_rzx.isPlaying() || m_rzx.isRecording());
    for (auto &periph : m_peripherals)
        periph->onFrameStart();
    // The ULA frame interrupt (machines may gate or redirect it)
    m_machine->onFrameStart(m_context);
    m_frame.writeLock()->setFrameNumber(m_clock.frameCount());
}

RunStatus Emulator::runScanline()
{
    m_pendingPause.reset();
    updateDebuggerHook();

    if (!m_frameStarted)
        beginFrame();

    const auto &timings = m_clock.timings();
    if (!m_lineStarted) {
        m_lineStarted = true;
        m_machine->onScanlineStart(m_line, m_context);
    }

    m_cpu.runUntil((static_cast<uint32_t>(m_line) + 1) * timings.tstatesPerLine);
    if (m_cpu.isTrapped()) {
        // Not a breakpoint nor a watchpoint: a magic breakpoint (ED FF / DD 01)
        requestPause(PauseReason::MagicBreakpoint);
        return RunStatus::Stopped;
    }

    m_scheduler.processDue(m_clock.totalTstates());
    m_tapeDeck.update(m_context);

    // Map the machine scanline to the visible 256 line window
    const int32_t visY = static_cast<int32_t>(m_line) - static_cast<int32_t>(timings.topBorderLines)
                         + static_cast<int32_t>(UlaRenderer::kBorderTopLines);
    if (visY >= 0 && visY < static_cast<int32_t>(UlaRenderer::kTotalHeightLines)) {
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
                              + static_cast<int32_t>(UlaRenderer::kBorderTopLines);
        if (nextY < static_cast<int32_t>(UlaRenderer::kTotalHeightLines) && !m_machine->usesRasterUla()) {
            auto frame = m_frame.writeLock();
            for (int32_t y = std::max(nextY, 0); y < static_cast<int32_t>(UlaRenderer::kTotalHeightLines); ++y)
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
    const uint32_t executedFrameTs = m_clock.frameBaseTstates();
    m_machine->onFrameEnd(m_context, m_ula);
    for (auto &periph : m_peripherals)
        periph->onFrameEnd();

    if (m_rewindEnabled)
        m_debugger.rewindBuffer().pushSnapshot(serializeState());

    if (m_rzx.isRecording())
        m_rzx.endRecordedFrame();
    else if (m_rzx.isPlaying())
        m_rzx.advancePlaybackFrame(m_ioBus);

    m_audioSamples = m_audioMixer.endFrame(executedFrameTs);
    m_ula.endRasterFrame();
    m_clock.beginNewFrame();
    updateMediaActivity();

    // Publish the frame; the next one goes into a recycled frame nobody holds anymore
    VideoFramePtr next = acquireFrame();
    m_lastFrame = std::exchange(m_frame, std::move(next));
    m_line = 0;
    m_frameStarted = false;
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
        m_machine->renderFullFrame(m_context, m_ula, *out);
    }
    m_lastFrame = std::move(frame);
    return m_lastFrame;
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
    if (m_config.mediaFormats.hasLoader(ext)) {
        m_config.mediaFormats.invokeLoader(ext, *this, file.data);
        return;
    }

    if (std::ranges::contains(kSnapshotExtensions, ext)) {
        loadSnapshot(file.data, ext);
    } else if (ext == ".tap" || ext == ".tzx") {
        loadTape(file.data, ext);
    } else if (std::ranges::contains(kDiskExtensions, ext)) {
        if (!diskDrive(0))
            throw PeripheralException("This machine has no floppy drive");
        insertDisk(0, file.data, ext);
    } else if (ext == ".rzx") {
        m_rzx.loadRzx(file.data, m_ioBus);
        if (!m_rzx.embeddedSnapshot().empty())
            loadSnapshot(m_rzx.embeddedSnapshot(), m_rzx.embeddedSnapshotExt());
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
    if (ext == ".sna" || ext == ".szx")
        return saveSnapshot(ext);
    if (ext == ".tap")
        return m_tapeDeck.saveTap();
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
    m_ula.endRasterFrame();
    const std::string ext = normalizedExtension(extension);
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
        return SnapshotFormats::saveSna48k(state);
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
    else
        throw FormatException(std::format("Unsupported tape format '{}'", ext));
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
    // A step always executes the current instruction, breakpoint or not
    m_stoppedAtBreakpoint = m_cpu.registers().PC;
    updateDebuggerHook();
    m_cpu.clearTrap();
    m_cpu.stepInstruction();
    m_cpu.clearTrap();
    m_pendingPause.reset();
    return m_cpu.registers();
}

bool Emulator::stepOver()
{
    const uint16_t pc = m_cpu.registers().PC;
    const auto inst = Disassembler::disassembleOne([this](uint16_t a) { return m_mmu.peek(a); }, pc,
                                                   m_cpu.isZ80nEnabled());
    if (!inst.isStepOverCandidate) {
        stepInstruction();
        return false;
    }
    Breakpoint after;
    after.type = BreakpointType::Execute;
    after.address = static_cast<uint16_t>(pc + inst.length);
    after.temporary = true;
    m_debugger.breakpoints().add(std::move(after), m_mmu);
    m_stoppedAtBreakpoint = pc;
    return true;
}

uint32_t Emulator::addBreakpoint(Breakpoint breakpoint)
{
    return m_debugger.breakpoints().add(std::move(breakpoint), m_mmu);
}

bool Emulator::removeBreakpoint(uint32_t id)
{
    return m_debugger.breakpoints().remove(id, m_mmu);
}

void Emulator::clearBreakpoints()
{
    m_debugger.breakpoints().clear(m_mmu);
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
