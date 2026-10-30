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

#include "zxkit/bus/Peripheral.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/peripherals/storage/Wd1793.h"

#include <cstddef>
#include <functional>
#include <optional>

namespace zxkit {

struct Beta128Options {
    /// The TR-DOS ROM as a 16K page of the machine's own ROM bank (Pentagon, Scorpion).
    /// Without it, the interface loads `trdos.rom` into a bank of its own.
    std::optional<BankId> machineRomBank;
    std::size_t machineRomPage = 0;
    /// The ZX 48K BASIC ROM is selected: only then the TR-DOS ROM pages in (an instruction
    /// fetched at 0x3Dxx) and out (an instruction fetched at 0x4000 or above). Unset: always,
    /// as on a 48K machine.
    std::function<bool()> basicRomSelected;
    /// The TR-DOS ROM is paged in after a reset (the Pentagon starts in TR-DOS).
    bool pagedAtReset = false;
};

/// Adds a "boot" program that runs the first BASIC program of a TR-DOS disk without one, as
/// FUSE does when it auto-loads a disk: TR-DOS runs "boot" when it starts. Returns whether it did.
bool insertTrDosBootLoader(DiskImage &disk);

/// The Beta 128 disk interface (TR-DOS): a WD1793, four drives, and the TR-DOS ROM that pages
/// in at 0x3Dxx. Its ports (0x1F, 0x3F, 0x5F, 0x7F and the system port 0xFF) answer only while
/// the TR-DOS ROM is paged in. The Pentagon and the Scorpion have one built in.
class Beta128Peripheral : public Peripheral {
public:
    Beta128Peripheral() = default;
    explicit Beta128Peripheral(Beta128Options options)
        : m_options(std::move(options))
    {}

    std::string_view id() const noexcept override { return "beta128"; }
    std::string_view name() const noexcept override { return "Beta 128 (TR-DOS)"; }

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    std::size_t diskDriveCount() const noexcept override { return Wd1793::kDriveCount; }
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override;

    /// The TR-DOS ROM is paged in (and the ports answer).
    bool isPaged() const noexcept { return m_paged; }
    /// Pages the TR-DOS ROM in or out, e.g. to start TR-DOS: page it in and jump to 0.
    void setPaged(MachineContext &ctx, bool paged);

    Wd1793 &fdc() noexcept { return m_fdc; }
    const Wd1793 &fdc() const noexcept { return m_fdc; }

private:
    bool basicRomSelected() const { return !m_options.basicRomSelected || m_options.basicRomSelected(); }
    static Z80Cpu::PcPages hookPages(bool paged) noexcept;
    void writeSystem(uint8_t value) noexcept;

    Beta128Options m_options;
    Wd1793 m_fdc;
    BankId m_romBank = 0;
    std::size_t m_romPage = 0;
    uint8_t m_system = 0;
    bool m_paged = false;
};

} // namespace zxkit
