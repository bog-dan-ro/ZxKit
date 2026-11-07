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

#pragma once

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/bus/IoBus.h"
#include "zxkit/bus/Peripheral.h"
#include "zxkit/bus/RomcsStack.h"
#include "zxkit/core/Clock.h"
#include "zxkit/core/Events.h"
#include "zxkit/core/Scheduler.h"
#include "zxkit/core/Shared.h"
#include "zxkit/cpu/Im2Controller.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/debug/Debugger.h"
#include "zxkit/debug/Disassembler.h"
#include "zxkit/input/KeyboardMatrix.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/media/MediaRegistry.h"
#include "zxkit/media/RzxFormat.h"
#include "zxkit/media/TapeDeck.h"
#include "zxkit/memory/ContentionModel.h"
#include "zxkit/memory/Mmu.h"
#include "zxkit/peripherals/storage/FatVolume.h"
#include "zxkit/peripherals/storage/FloppyDisk.h"
#include "zxkit/peripherals/storage/SdCard.h"
#include "zxkit/video/UlaRenderer.h"
#include "zxkit/video/VideoFrame.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

class ZxNextMachine;

struct EmulatorConfig
{
    std::string machineId = "48k";
    uint32_t audioSampleRate = 48'000;
    RomResolverFn romResolver;
    /// The machines `selectMachine(id)` can create: the built-in ones by default.
    MachineRegistry machines;
    /// Extra (or overriding) media formats for `loadMedia()` / `saveMedia()`.
    MediaRegistry mediaFormats;
    /// Include the full scanned border rather than the traditional 32-pixel crop.
    /// Hosts can crop the published frame using VideoFrame::paperRect().
    bool fullBorder = false;
    /// Optional machine-scoped ROM override. A nonempty result takes precedence
    /// over romResolver and Next SD firmware; empty uses the normal fallback;
    /// the machine id also follows switches made by snapshot loading. Called
    /// with the emulator locked: preload host files and do not acquire its lock.
    std::function<std::vector<uint8_t>(std::string_view machineId, std::string_view romName,
                                       std::size_t expectedSize)>
            machineRomResolver;
};

/// A media file's content, ready to be loaded. Reading (and unzipping) it is the
/// slow part: do it before taking the emulator's lock.
struct MediaFile
{
    std::vector<uint8_t> data;
    std::string extension; ///< ".tap", ".z80", ... (lower case, the zip entry's for zip files)

    static MediaFile read(const std::filesystem::path &path);
    /// Unzips `data` if it is a zip file, and normalizes the extension.
    static MediaFile fromBytes(std::vector<uint8_t> data, std::string_view extension);
};

/// What `Emulator::runScanline()` got to.
enum class RunStatus : uint8_t {
    LineDone, ///< the scanline ran, the frame goes on
    FrameDone, ///< the frame's last scanline ran: `lastFrame()` / `audioSamples()` hold the frame's output
    Stopped ///< the CPU stopped in the middle of the scanline (breakpoint, watchpoint), see `takePauseEvent()`
};

/// A complete emulated computer: CPU, memory, ULA, machine, peripherals, media and debugger.
///
/// The class itself is single threaded and runs only when it is called, so it can be used
/// directly (tests, tools, a host's own loop). To share it with an `Engine` running it on its
/// own thread, own it through `Shared<Emulator>` and access it through `writeLock()` /
/// `readLock()`: the Engine holds the lock for one scanline at a time.
///
/// Instances are independent: as many emulators as needed can live in the same process.
class Emulator
{
public:
    explicit Emulator(EmulatorConfig config = {});
    ~Emulator();

    Emulator(const Emulator &) = delete;
    Emulator &operator=(const Emulator &) = delete;

    // ── Machine & peripherals ─────────────────────────────────────────────
    MachineRegistry &machines() noexcept
    {
        return m_config.machines;
    }
    const MachineRegistry &machines() const noexcept
    {
        return m_config.machines;
    }
    MediaRegistry &mediaFormats() noexcept
    {
        return m_config.mediaFormats;
    }

    /// Replaces the machine. When the new one fails to start (e.g. a missing ROM)
    /// the previous machine is restored and the error is rethrown.
    void selectMachine(std::string_view machineId);
    void selectMachine(std::unique_ptr<MachineProfile> machine);
    MachineProfile &machine() noexcept
    {
        return *m_machine;
    }
    const MachineProfile &machine() const noexcept
    {
        return *m_machine;
    }

    void attachPeripheral(std::unique_ptr<Peripheral> peripheral);
    void detachPeripheral(std::string_view peripheralId);
    OptionalRef<Peripheral> findPeripheral(std::string_view peripheralId) noexcept;
    /// The floppy drives of the machine's own controller, or of the first attached disk interface.
    std::size_t diskDriveCount() const noexcept;
    OptionalRef<DiskImage> diskDrive(std::size_t drive) noexcept;

    void reset(ResetKind kind = ResetKind::Soft);
    void triggerNmi() noexcept;
    void setKeyState(ZxKey key, bool pressed) noexcept;
    void setNextKeyState(NextKey key, bool pressed) noexcept;
    /// Whether the requested Next button can currently acquire NMI; false on other machines.
    bool canTriggerNextNmi(bool multiface) const noexcept;
    bool triggerNextNmi(bool multiface);

    // ── Running ───────────────────────────────────────────────────────────
    /// Runs the CPU to the end of the current scanline. Frames are resumable: after a
    /// stop the next call continues exactly where the CPU stopped.
    RunStatus runScanline();
    /// Runs to the end of the current frame, or until something stops the CPU.
    RunStatus runFrame();
    /// Runs `count` frames (stops early when the CPU stops) and returns the last complete frame.
    VideoFramePtr runFrames(uint32_t count = 1);

    /// The last complete frame. Frames are never written again once published, so
    /// hosts can keep them for as long as they need (they are recycled afterwards).
    const VideoFramePtr &lastFrame() const noexcept
    {
        return m_lastFrame;
    }
    /// Renders the current machine state into a new frame and publishes it as `lastFrame()`
    /// (e.g. to show a loaded snapshot while the emulation is paused).
    const VideoFramePtr &refreshFrame();
    /// The stereo samples of the last complete frame, valid until the next `runScanline()`.
    std::span<const int16_t> audioSamples() const noexcept
    {
        return m_audioSamples;
    }
    uint32_t audioSampleRate() const noexcept
    {
        return m_audioMixer.sampleRate();
    }
    double frameRateHz() const noexcept;

    /// Why the CPU stopped (breakpoint, watchpoint, magic breakpoint), once.
    std::optional<PauseEvent> takePauseEvent() noexcept
    {
        return std::exchange(m_pendingPause, std::nullopt);
    }

    void setRewindEnabled(bool enabled, std::size_t maxFrames = 300);
    bool isRewindEnabled() const noexcept
    {
        return m_rewindEnabled;
    }
    void rewind(std::size_t framesBack = 1);

    // ── Media ─────────────────────────────────────────────────────────────
    /// Loads any supported media: snapshots, tapes, disks, RZX recordings, SCR screens.
    void loadMedia(const MediaFile &file);
    void loadMediaFile(const std::filesystem::path &path)
    {
        loadMedia(MediaFile::read(path));
    }
    /// Saves a snapshot (.sna, .z80, .szx), a Next program (.nex), the tape (.tap, .tzx) or a screenshot (.png).
    std::vector<uint8_t> saveMedia(std::string_view extension);
    void saveMediaFile(const std::filesystem::path &path);

    void loadSnapshot(std::span<const uint8_t> data, std::string_view extension);
    std::vector<uint8_t> saveSnapshot(std::string_view extension = ".szx");
    void loadTape(std::span<const uint8_t> data, std::string_view extension);
    /// Explicit host actions: reset and schedule ROM key presses on emulated frames.
    /// Return false for unsupported models. Never run frames synchronously on the caller's thread.
    bool autoLoadTape(std::size_t block = 0);
    /// Boots drive A using TR-DOS or the +3 Loader. Other drives/models are untouched.
    bool autoBootDisk();
    /// Inserts a floppy image (.dsk, .trd, .scl, .img, ...) into `drive`, see `diskDrive()`.
    void insertDisk(std::size_t drive, std::span<const uint8_t> data, std::string_view extension);
    void ejectDisk(std::size_t drive);
    /// Inserts a raw SD card image (DivMMC, ZX Spectrum Next). The image is used in place:
    /// the emulated machine writes to the file.
    void insertSdCard(const std::filesystem::path &image, bool readOnly = false);
    void ejectSdCard();
    /// The SD card's file system (its first FAT partition), to list, read and write files on the
    /// card; read-only for a read-only card. It works on the card directly: use it while the
    /// emulation doesn't run (under the Engine's write lock) and reset the machine after writing, as
    /// the running system (NextZXOS, esxDOS) caches the file system. Valid while the card stays in.
    /// Throws PeripheralException without a card, FormatException without a FAT partition.
    FatVolume sdCardVolume();
    std::vector<uint8_t> screenshotPng();
    /// The tape's state, and the disks and SD card accessed in the last frames.
    MediaActivity mediaActivity() const noexcept;

    // ── Debugger ──────────────────────────────────────────────────────────
    Z80Registers registers() const noexcept
    {
        return m_cpu.registers();
    }
    void setRegisters(const Z80Registers &regs) noexcept;
    std::vector<uint8_t> readMemory(uint16_t address, std::size_t length) const;
    void writeMemory(uint16_t address, std::span<const uint8_t> data,
                     bool allowRomWrite = false) noexcept;
    void writeMemory(uint16_t address, std::initializer_list<uint8_t> data,
                     bool allowRomWrite = false) noexcept
    {
        writeMemory(address, std::span<const uint8_t>(data.begin(), data.size()), allowRomWrite);
    }

    /// Executes one instruction, even the one a breakpoint stopped at.
    Z80Registers stepInstruction();
    /// Steps over CALLs, RSTs and repeated block instructions: returns true when it set a
    /// temporary breakpoint after the instruction, i.e. the emulation must run to reach it.
    bool stepOver();
    /// Runs to the return from this stack frame (nested calls/interrupts are skipped).
    bool stepOut();
    /// Runs to the currently mapped instruction at address; false if already there.
    bool runTo(uint16_t address);
    void cancelRunControl() noexcept
    {
        m_debugRun.reset();
    }
    bool hasRunControl() const noexcept
    {
        return m_debugRun.has_value();
    }
    uint32_t addBreakpoint(Breakpoint breakpoint);
    bool removeBreakpoint(uint32_t id);
    bool setBreakpointEnabled(uint32_t id, bool enabled);
    void clearBreakpoints();
    std::vector<DisasmInstruction> disassemble(uint16_t address, std::size_t count) const;
    std::vector<uint8_t> assembleAndPatch(uint16_t address, std::string_view mnemonic,
                                          bool allowRomWrite = false);

    std::size_t pokeFinderReset();
    std::vector<PokeCandidate> pokeFinderSearch(uint8_t value);
    std::vector<PokeCandidate> pokeFinderSearchIncremented();
    std::vector<PokeCandidate> pokeFinderSearchDecremented();

    // ── Components ────────────────────────────────────────────────────────
    Clock &clock() noexcept
    {
        return m_clock;
    }
    const Clock &clock() const noexcept
    {
        return m_clock;
    }
    Scheduler &scheduler() noexcept
    {
        return m_scheduler;
    }
    Mmu &mmu() noexcept
    {
        return m_mmu;
    }
    const Mmu &mmu() const noexcept
    {
        return m_mmu;
    }
    ContentionModel &contention() noexcept
    {
        return m_contention;
    }
    IoBus &ioBus() noexcept
    {
        return m_ioBus;
    }
    RomcsStack &romcs() noexcept
    {
        return m_romcs;
    }
    Im2Controller &im2() noexcept
    {
        return m_im2;
    }
    Z80Cpu &cpu() noexcept
    {
        return m_cpu;
    }
    const Z80Cpu &cpu() const noexcept
    {
        return m_cpu;
    }
    AudioMixer &audioMixer() noexcept
    {
        return m_audioMixer;
    }
    KeyboardMatrix &keyboard() noexcept
    {
        return m_keyboard;
    }
    UlaRenderer &ula() noexcept
    {
        return m_ula;
    }
    TapeDeck &tapeDeck() noexcept
    {
        return m_tapeDeck;
    }
    const TapeDeck &tapeDeck() const noexcept
    {
        return m_tapeDeck;
    }
    void startRzxRecording();
    std::vector<uint8_t> stopRzxRecording();
    void stopRzxPlayback() noexcept;
    /// Host pauses the Engine before seeking. Advance in small batches to keep its UI responsive.
    void beginRzxSeekFrame(std::size_t frame);
    void beginRzxSeek(uint64_t milliseconds);
    bool advanceRzxSeek(uint32_t maxVideoFrames = 2);
    bool isRzxSeeking() const noexcept
    {
        return m_rzxSeekTarget.has_value();
    }
    void cancelRzxSeek() noexcept
    {
        m_rzxSeekTarget.reset();
    }
    void resumeRzxRecording();
    uint64_t rzxPositionMilliseconds() const;
    uint64_t rzxDurationMilliseconds() const;

    RzxStream &rzx() noexcept
    {
        return m_rzx;
    }
    const RzxStream &rzx() const noexcept
    {
        return m_rzx;
    }
    Debugger &debugger() noexcept
    {
        return m_debugger;
    }
    const Debugger &debugger() const noexcept
    {
        return m_debugger;
    }
    SdCard &sdCard() noexcept
    {
        return m_sdCard;
    }
    const SdCard &sdCard() const noexcept
    {
        return m_sdCard;
    }
    MachineContext &context() noexcept
    {
        return m_context;
    }

private:
    void installMachine(std::unique_ptr<MachineProfile> machine);
    void beginFrame();
    RunStatus runPlaybackScanline();
    void advanceMediaBoot() noexcept;
    uint16_t m_mediaBootFrame = 0;
    bool m_mediaBoot48k = false;
    void applyRzxCommands();
    void finishPlaybackPresentation();
    void captureRzxCheckpoint(bool force = false);
    void clearRzxCheckpoints();
    bool m_applyingRzxSnapshot = false;
    bool m_recordingBoundary = false;
    bool m_snapshotStandardAy = false;
    void endFrame();
    void requestPause(PauseReason reason, uint16_t address = 0) noexcept;
    void updateMediaActivity();
    void updateDebuggerHook() noexcept;
    VideoFramePtr acquireFrame();
    BorderMargins effectiveBorderMargins() const noexcept;
    std::vector<uint8_t> serializeState() const;
    void deserializeState(std::span<const uint8_t> bytes);

    EmulatorConfig m_config;

    Clock m_clock;
    Scheduler m_scheduler;
    Mmu m_mmu;
    ContentionModel m_contention;
    IoBus m_ioBus;
    RomcsStack m_romcs;
    Im2Controller m_im2;
    Z80Cpu m_cpu;
    AudioMixer m_audioMixer;
    KeyboardMatrix m_keyboard;
    UlaRenderer m_ula;
    TapeDeck m_tapeDeck;
    RzxStream m_rzx;
    struct RzxCheckpoint
    {
        std::size_t frame = 0, command = 0, bytes = 0;
        std::string machine;
        std::vector<std::string> peripherals;
        std::vector<uint8_t> state;
        TapeDeck tape;
        Scheduler scheduler;
        uint16_t line = 0;
        bool frameStarted = false, lineStarted = false, recordingBoundary = false;
        bool standardAy = false, beforeInterrupt = false, inhibitInterrupt = false;
    };
    std::deque<RzxCheckpoint> m_rzxCheckpoints;
    std::size_t m_rzxCheckpointBytes = 0;
    std::optional<std::size_t> m_rzxSeekTarget;
    bool m_rzxSeekInterrupt = false, m_rzxSeekInhibit = false;
    double m_rzxTimelineHz = 50.0;
    Debugger m_debugger;
    SdCard m_sdCard;
    MachineContext m_context;

    std::unique_ptr<MachineProfile> m_machine;
    std::vector<std::unique_ptr<Peripheral>> m_peripherals;

    // Frame in progress: frames are resumable at any instruction
    uint16_t m_line = 0;
    bool m_frameStarted = false;
    bool m_lineStarted = false;

    // Video output: the frame being rendered, the last published one, and the recycled ones
    VideoFramePtr m_frame;
    VideoFramePtr m_lastFrame;
    std::vector<VideoFramePtr> m_framePool;
    std::span<const int16_t> m_audioSamples;

    // Media activity: the devices' access counts seen at the last frame end, and until when they are busy
    static constexpr std::size_t kMaxActivityDrives = 8;
    std::array<uint64_t, kMaxActivityDrives> m_diskAccesses{};
    std::array<uint64_t, kMaxActivityDrives> m_diskBusyUntil{};
    uint64_t m_sdAccesses = 0;
    uint64_t m_sdBusyUntil = 0;
    MediaActivity m_mediaActivity;

    struct DebugRun
    {
        enum class Kind { Target, Out, Returning } kind = Kind::Target;
        uint16_t address = 0;
        std::optional<uint16_t> stack;
        BankId bank = 0;
        uint16_t page1k = 0;
        uint32_t depth = 0;
    };
    std::optional<DebugRun> m_debugRun;
    ZxNextMachine *m_nextDebugMachine = nullptr; // machine owns it; replaced on installation
    uint32_t m_debugObserverTypes = ~uint32_t(0);
    bool m_nextDebugArmed = false;
    bool m_interruptDebugArmed = false;
    bool checkRunControl(uint16_t pc);
    std::optional<PauseEvent> m_pendingPause;
    std::optional<uint16_t>
            m_stoppedAtBreakpoint; // the next instruction there runs without breaking again
    bool m_rewindEnabled = false;
};

} // namespace zxkit
