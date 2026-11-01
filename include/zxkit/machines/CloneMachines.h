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

#include "zxkit/core/Clock.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/peripherals/storage/Beta128.h"

#include <memory>

namespace zxkit {

inline constexpr FrameTimings kTimingsPentagon = {
    .baseClockHz = 3'584'000,
    .leftBorderTstates = 36,
    .screenTstates = 128,
    .rightBorderTstates = 28,
    .retraceTstates = 32,
    .tstatesPerLine = 224,
    .topBorderLines = 80,
    .screenLines = 192,
    .bottomBorderLines = 48,
    .linesPerFrame = 320,
    .interruptLength = 36
};

inline constexpr FrameTimings kTimingsScorpion = {
    .baseClockHz = 3'500'000,
    .leftBorderTstates = 24,
    .screenTstates = 128,
    .rightBorderTstates = 32,
    .retraceTstates = 40,
    .tstatesPerLine = 224,
    .topBorderLines = 64,
    .screenLines = 192,
    .bottomBorderLines = 56,
    .linesPerFrame = 312,
    .interruptLength = 36
};

// FUSE's Timex SCLD frames: the 48K's and the 60Hz ones, the first pixel 15 T-states earlier
inline constexpr FrameTimings kTimingsTimex50 = {
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
    .interruptLength = 32,
    .screenStartOffset = -15
};

inline constexpr FrameTimings kTimingsTimex60 = {
    .baseClockHz = 3'528'000,
    .leftBorderTstates = 24,
    .screenTstates = 128,
    .rightBorderTstates = 24,
    .retraceTstates = 48,
    .tstatesPerLine = 224,
    .topBorderLines = 41,
    .screenLines = 192,
    .bottomBorderLines = 29,
    .linesPerFrame = 262,
    .interruptLength = 32,
    .screenStartOffset = -15
};

/// Timex TC2048 / TC2068 / TS2068 and the ZX Spectrum SE, as FUSE models them.
///
/// The SCLD: port 0xFF selects the screen mode (bits 0-2), the hi-res colours (3-5), disables
/// the frame interrupt (6) and chooses the EXROM or the DOCK (7); port 0xF4 pages an 8K chunk
/// of that bank over the home bank for each bit. The TC2068 and TS2068 have a 16K ROM, an 8K
/// EXROM (seen in every chunk), an empty DOCK (no cartridges yet) and the AY on 0xF5/0xF6; the
/// TC2048 has neither. The SE adds the 128K's 0x7FFD paging (without its lock) over nine RAM
/// pages, 64K of DOCK and 64K of EXROM RAM, and the AY on 0xFFFD/0xBFFD too.
class TimexMachine : public MachineProfile {
public:
    enum class Variant { Tc2048, Tc2068, Ts2068, SpecSe };

    explicit TimexMachine(Variant variant = Variant::Tc2048) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;
    void onFrameStart(MachineContext &ctx) override;

    uint8_t lastPort7ffd() const noexcept override;
    void setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula) override;

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

private:
    bool isSe() const noexcept { return m_variant == Variant::SpecSe; }
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    Variant m_variant = Variant::Tc2048;
    BankId m_dockBank = 0;
    BankId m_exromBank = 0;
    uint8_t m_portF4 = 0;   // the horizontal select
    uint8_t m_portFf = 0;   // the SCLD's mode register (the ULA renders its screen modes)
    uint8_t m_port7ffd = 0; // the SE's paging
};

/// Pentagon 128K / 512K / 1024K
class PentagonMachine : public MachineProfile {
public:
    enum class Variant { Pentagon128, Pentagon512, Pentagon1024 };

    explicit PentagonMachine(Variant variant = Variant::Pentagon128) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    uint8_t lastPort7ffd() const noexcept override;
    bool is48kMode() const noexcept override { return m_locked; }

    /// The built-in Beta 128: four drives and TR-DOS (the Pentagon starts in it)
    std::size_t diskDriveCount() const noexcept override { return Wd1793::kDriveCount; }
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override { return m_beta->diskDrive(idx); }
    Beta128Peripheral &beta() noexcept { return *m_beta; }

private:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    Variant m_variant = Variant::Pentagon128;
    uint8_t m_port7ffd = 0;
    uint8_t m_portEff7 = 0;
    bool m_locked = false;
    std::unique_ptr<Beta128Peripheral> m_beta;
};

/// Scorpion ZS 256
class ScorpionMachine : public MachineProfile {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    bool is48kMode() const noexcept override { return m_locked; }

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    /// The built-in Beta 128: four drives and TR-DOS
    std::size_t diskDriveCount() const noexcept override { return Wd1793::kDriveCount; }
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override { return m_beta->diskDrive(idx); }
    Beta128Peripheral &beta() noexcept { return *m_beta; }

private:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    uint8_t m_port7ffd = 0;
    uint8_t m_port1ffd = 0;
    bool m_locked = false;
    std::unique_ptr<Beta128Peripheral> m_beta;
};

} // namespace zxkit
