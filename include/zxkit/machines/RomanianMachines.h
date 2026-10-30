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

/// ICE Felix HC-2000 (Romanian Z80 Spectrum & CP/M computer)
///
/// Two 16KB ROMs (0 = BASIC, 1 = CP/M boot & BIOS), 64KB RAM and a built-in
/// HC Interface 1 uPD765 floppy controller (with its own 16KB "hc-if1.rom").
/// `PRINT USR 14446` in BASIC boots CP/M from drive A:.
///
/// 1. System configuration port 0x7E (decoded on A7 = A0 = 0):
///    - D0 (B/OL):  ROM select (0 = BASIC, 1 = CP/M)
///    - D1 (CPMPL): CP/M address decoding: RAM at 0x0000 and the upper 8KB of
///                  the selected ROM at 0xE000..0xFFFF, /ROMCS ignored
///    - D2:         Lockout bit (ignores further writes to port 0x7E until reset)
///    - D3 (CVS):   Video RAM address select (0 = 0x4000 RAM page 1, 1 = 0xC000 RAM page 3)
/// 2. Ports 0xC7 (set) and 0xC5 (clear) the CPM signal, which pulls A13 high
///    for CPU accesses to 0xC000..0xDFFF, mapping the 0xE000 RAM in at 0xC000.
class HcMachine : public MachineProfile {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    std::size_t diskDriveCount() const noexcept override;
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override;

    uint8_t configPort7e() const noexcept;
    bool isCpmA13Active() const noexcept;
    HcIf1FdcPeripheral &fdcInterface() noexcept { return m_fdc; }

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

private:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    HcIf1FdcPeripheral m_fdc;
    uint8_t m_config7e = 0x00;
    bool m_locked = false;
    bool m_cpmA13PullHigh = false;
};

/// Brasov CoBra 1 / CoBra 2 (64KB / 80KB Romanian Z80 & CP/M Computer)
///
/// Models the hardware D-type flip-flops (PO = R7, LO6 = latched 8255 Port C bit 6 on
/// falling edge of R7), 8255 PIA on ports 0xDF (control) and 0xFE (port C),
/// 4 selectable 16KB System ROMs (Opus, BASIC, Devil, CP/M), and 3 operating modes:
/// - Startup/Boot (PO == 1): 32KB ROM (0x0000..0x7FFF) + RAM (0x8000..0xFFFF)
/// - CP/M 64K/80K (PO == 0 && LO6 == 1): All-RAM 64KB mode with selectable VRAM page
/// - Spectrum 48K (PO == 0 && LO6 == 0): Locked 16KB Read-Only RAM at 0x0000 + 48KB RAM
class CobraMachine : public MachineProfile {
public:
    explicit CobraMachine(bool mode80k = true) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    uint8_t poFlipFlop() const noexcept;
    uint8_t lo6FlipFlop() const noexcept;
    bool isSpectrumModeLocked() const noexcept;

private:
    void reset8255PortC() noexcept;
    void write8255PortC(uint8_t val, MachineContext &ctx, UlaRenderer &ula);
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    bool m_mode80k = true;
    uint8_t m_8255Ctrl = 0x9B;
    uint8_t m_8255PortC = 0x00;
    uint8_t m_po = 1;
    uint8_t m_lo6 = 1;
    bool m_locked = false;
};

/// Brasov CoBra 2 (TS) - 64KB, no ULA chip, four 16KB system ROMs (Sinclair
/// BASIC + boot, Devil BASIC, Opus, ITCI BASIC) and a Timex style AY (0xF5/0xF6).
/// An OUT to port 0xFE stores the F5 register: Q3/Q7 select the ROM, Q5/Q6 feed
/// the nCE/SM flip-flops, which are clocked by an IN from port 0x7E:
/// - nCE = 0, SM = 0: boot, ROM at 0x0000, video RAM at 0xC000
/// - nCE = 1, SM = 0: 64KB DRAM (CP/M), RAM page 2 at 0x0000
/// - nCE = 1, SM = 1: Spectrum 48K, read-only RAM page 0 at 0x0000
class Cobra2Machine : public MachineProfile {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

private:
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);

    uint8_t m_portFe = 0;  // F5 register
    uint8_t m_latched = 0; // nCE / SM flip-flops, latched from F5 by IN 0x7E
};

} // namespace zxkit
