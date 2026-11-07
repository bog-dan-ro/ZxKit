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

#include "zxkit/cpu/Z80Ctc.h"
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
class HcMachine : public MachineProfile
{
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
    HcIf1FdcPeripheral &fdcInterface() noexcept
    {
        return m_fdc;
    }

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
/// The 50Hz interrupt reaches the CPU only in the Spectrum configuration.
///
/// The floppy disk board: an i8272 (status 0xF5, data 0xFD: A1 = 0 selects it, A3 the register)
/// without DMA, and a Z80 CTC (A2 = 0 selects it, A3-A4 the channel: 0xE3, 0xEB, 0xF3, 0xFB)
/// in IM2: the 8272's INT clocks channel 0 (an interrupt for each byte), channel 1 counts
/// channel 0's zero counts and channel 2 channel 1's, whose zero count is the 8272's Terminal
/// Count (the end of a sector). Channel 3 counts the vertical sync (CP/M's 50Hz interrupt).
/// The keyboard has a sixth column (bit 5): CTRL, NO SCROLL, LINE FEED, ESC, F1-F4.
///
/// The 80KB model (mainboard v1.3) has the "80 characters per line in CP/M" modification: in
/// the CP/M configuration, once O5 has been set, the border's 4 character cells left and right
/// of the screen show the upper half of the video RAM (columns 28-31 and 0-3 of each row), a
/// 320x192 screen for CP/M's 80 columns of 4 pixel wide characters.
class CobraMachine : public MachineProfile
{
public:
    explicit CobraMachine(bool mode80k = true) noexcept;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;
    void onFrameStart(MachineContext &ctx) override;
    void onScanlineStart(uint16_t line, MachineContext &ctx) override;
    void renderScanline(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula,
                        VideoFrame &frame) override;
    void renderFullFrame(MachineContext &ctx, UlaRenderer &ula, VideoFrame &frame) override;
    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

    std::size_t diskDriveCount() const noexcept override
    {
        return 4;
    }
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override;

    uint8_t poFlipFlop() const noexcept;
    uint8_t lo6FlipFlop() const noexcept;
    bool isSpectrumModeLocked() const noexcept;

private:
    void reset8255PortC() noexcept;
    void write8255PortC(uint8_t val, MachineContext &ctx, UlaRenderer &ula);
    void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);
    // The 8272's INT line into the CTC; `byteMoved`: a data byte moved, the line fell and rises
    // again for the next one
    void updateFdcInterrupt(uint64_t now, bool byteMoved);
    bool basicConfiguration() const noexcept
    {
        return m_po == 0 && m_lo6 == 0;
    }
    // The 80 characters screen's cells in the border, from the video RAM's upper half
    void renderWideCells(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula,
                         VideoFrame &frame) const;

    Upd765Fdc m_fdc;
    Z80Ctc m_ctc;
    bool m_terminalCount = false; // channel 2 reached zero: the 8272's TC
    bool m_wideLatch = false; // the 80 characters modification's latch (set by O5 in CP/M)
    bool m_wide = false; // ... and the screen is 320 pixels wide
    bool m_mode80k = true;
    uint8_t m_8255Ctrl = 0x9B;
    uint8_t m_8255PortC = 0x00;
    uint8_t m_po = 1;
    uint8_t m_lo6 = 1;
    bool m_locked = false;
};

/// The CoBra 2, the Turbo Spectrum (Sorin Cismas, Brasov, the late 1980s; specification, sources
/// and binaries at https://cobrasov.org/download/): 64KB of DRAM, no ULA chip, a 16KB EEPROM
/// with the boot, its TURBO SPECTRUM logo and a compacted Spectrum ROM. An OUT to any even
/// port stores the F5 register (border, tape, speaker; Q5 and Q6 feed the CE and SM flops),
/// any IN loads the flops:
/// - CE = 0, SM = 0: EEPROM mode, the EEPROM at 0x0000, the video RAM at 0xC000
/// - CE = 1, SM = 0: 64KB DRAM mode, DRAM at 0x0000 (A15 inverted: the video RAM stays at 0xC000)
/// - SM = 1: Spectrum mode (CE set too, until a reset), the read-only DRAM copy of the ROM at 0x0000
class Cobra2Machine : public MachineProfile
{
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

    // F5's Q5 and Q6, the inputs of the CE (EEPROM disabled) and SM (Spectrum mode) flops
    static constexpr uint8_t kEepromDisabled = 0x20;
    static constexpr uint8_t kSpectrumMode = 0x40;

    uint8_t m_portFe = 0; // F5 register
    uint8_t m_latched = 0; // the CE / SM flops (kEepromDisabled, kSpectrumMode), loaded by any IN
};

} // namespace zxkit
