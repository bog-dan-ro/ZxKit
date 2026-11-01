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

#include "zxkit/machines/MachineProfile.h"
#include "zxkit/peripherals/storage/FloppyDisk.h"

namespace zxkit {

inline constexpr FrameTimings kTimings48k = {
    .baseClockHz = 3'500'000,
    .leftBorderTstates = 24,
    .screenTstates = 128,
    .rightBorderTstates = 24,
    .retraceTstates = 48,
    .tstatesPerLine = 224,
    .topBorderLines = 64,
    .screenLines = 192,
    .bottomBorderLines = 56,
    .linesPerFrame = 312,
    .interruptLength = 32
};

inline constexpr FrameTimings kTimings48kNtsc = {
    .baseClockHz = 3'527'500,
    .leftBorderTstates = 24,
    .screenTstates = 128,
    .rightBorderTstates = 24,
    .retraceTstates = 48,
    .tstatesPerLine = 224,
    .topBorderLines = 40,
    .screenLines = 192,
    .bottomBorderLines = 32,
    .linesPerFrame = 264,
    .interruptLength = 32
};

inline constexpr FrameTimings kTimings128k = {
    .baseClockHz = 3'546'900,
    .leftBorderTstates = 24,
    .screenTstates = 128,
    .rightBorderTstates = 24,
    .retraceTstates = 52,
    .tstatesPerLine = 228,
    .topBorderLines = 63,
    .screenLines = 192,
    .bottomBorderLines = 56,
    .linesPerFrame = 311,
    .interruptLength = 36,
    .screenStartOffset = -2
};

/// ZX Spectrum 16K / 48K / 48K NTSC
class Spectrum48kMachine : public MachineProfile {
public:
    enum class Variant { Spec16k, Spec48k, Spec48kNtsc };

    explicit Spectrum48kMachine(Variant variant = Variant::Spec48k) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    bool usesRasterUla() const noexcept override { return true; }

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

private:
    Variant m_variant = Variant::Spec48k;
};

/// ZX Spectrum 128K / +2
class Spectrum128kMachine : public MachineProfile {
public:
    explicit Spectrum128kMachine(bool isPlus2 = false) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    bool usesRasterUla() const noexcept override { return true; }

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    uint8_t lastPort7ffd() const noexcept override;
    bool is48kMode() const noexcept override { return m_locked; }

    void setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula) override;

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

protected:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    bool m_isPlus2 = false;
    uint8_t m_port7ffd = 0;
    bool m_locked = false;
};

/// ZX Spectrum +2A / +3 / +3e
class SpectrumPlus3Machine : public MachineProfile {
public:
    enum class Variant { Plus2A, Plus3, Plus3e };

    explicit SpectrumPlus3Machine(Variant variant = Variant::Plus3) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    bool usesRasterUla() const noexcept override { return true; }
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    uint8_t lastPort7ffd() const noexcept override;
    bool is48kMode() const noexcept override { return m_locked; }
    uint8_t lastPort1ffd() const noexcept override;

    void setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula) override;

    /// The +3 and +3e have two floppy drives (A: and B:) on a uPD765; the +2A has none.
    std::size_t diskDriveCount() const noexcept override;
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override;
    bool isMotorOn() const noexcept { return (m_port1ffd & 0x08) != 0; }

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

private:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);
    bool hasFloppyDrives() const noexcept { return m_variant != Variant::Plus2A; }

    Variant m_variant = Variant::Plus3;
    uint8_t m_port7ffd = 0;
    uint8_t m_port1ffd = 0;
    bool m_locked = false;
    Upd765Fdc m_fdc;
};

} // namespace zxkit
