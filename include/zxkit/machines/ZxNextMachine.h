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

#include "zxkit/input/KeyboardMatrix.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/peripherals/next/Copper.h"
#include "zxkit/peripherals/next/NextReg.h"
#include "zxkit/peripherals/next/NextRtc.h"
#include "zxkit/peripherals/next/NextUart.h"
#include "zxkit/peripherals/next/ZxnCtc.h"
#include "zxkit/peripherals/next/ZxnDma.h"
#include "zxkit/video/next/NextVideo.h"

namespace zxkit {

/// ZX Spectrum Next (TBBlue) Machine Profile
/// Integrates T80N (Z80N) @ 3.5/7/14/28 MHz, 8x8K MMU slots over 2MB SRAM,
/// NextREG control plane, Layer 2 / Sprites / Tilemap / Copper video compositor,
/// ZXN-DMA, 4-channel CTC, 14-vector IM2 daisy-chain, 3x AY TurboSound, and 4x DACs.
///
/// NextZXOS is booted directly, like CSpect does: instead of running the
/// TBBLUE.FW firmware, the machine takes the NextZXOS (enNextZX.rom), DivMMC
/// (enNxtmmc.rom) and alt (enAltZX.rom) ROMs from /machines/next on the SD card
/// (`Emulator::insertSdCard`), sets the NextREGs the way the firmware hands them
/// over and starts at 0x0000. The built-in DivMMC (port 0xE3, NR 0xB8..0xBB
/// automap entry points) and the SPI SD card interface (ports 0xE7 / 0xEB) give
/// NextZXOS its SD card.
class ZxNextMachine : public MachineProfile {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    FrameTimings timings() const noexcept override;
    ContentionPattern contentionPattern() const noexcept override;
    bool hasFloatingBus() const noexcept override;
    bool supportsZ80n() const noexcept override;

    void initialize(MachineContext &ctx, UlaRenderer &ula) override;
    void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) override;

    void onFrameStart(MachineContext &ctx) override;
    void onScanlineStart(uint16_t line, MachineContext &ctx) override;
    void renderScanline(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula, VideoFrame &frame) override;
    void renderFullFrame(MachineContext &ctx, UlaRenderer &ula, VideoFrame &frame) override;
    uint32_t frameWidth() const noexcept override;
    uint32_t frameHeight() const noexcept override;
    void onFrameEnd(MachineContext &ctx, UlaRenderer &ula) override;

    uint8_t lastPort7ffd() const noexcept override;
    bool is48kMode() const noexcept override { return (m_port7ffd & 0x20) != 0; }
    uint8_t lastPort1ffd() const noexcept override;
    void setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula) override;

    NextReg &nextReg() noexcept;
    const NextReg &nextReg() const noexcept;
    Copper &copper() noexcept;
    const Copper &copper() const noexcept;
    ZxnDma &dma() noexcept;
    ZxnCtc &ctc() noexcept;
    NextVideo &nextVideo() noexcept;
    const NextVideo &nextVideo() const noexcept;

    NextUart &uart() noexcept { return m_uart; }
    NextRtc &rtc() noexcept { return m_rtc; }
    void setExtendedKey(NextKey key, bool pressed) noexcept;
    bool canRequestNmi(bool multiface) const noexcept;
    /// Physical buttons respect NextREG 06 enables; software requests set NR02 causes.
    bool requestNmi(MachineContext &ctx, bool multiface, bool button = true);
    uint64_t uartTime(const MachineContext &ctx) const noexcept;

    bool isDivMmcMapped() const noexcept { return m_divMmcMapped; }

    void saveState(StateWriter &w) const override;
    void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula) override;

private:
  void armMultifaceEntry(MachineContext &ctx);
  void rebuildExtendedMatrix() noexcept;
  void wireMultiface(MachineContext &ctx, UlaRenderer &ula);
  void loadRoms(MachineContext &ctx);
  void wireNextRegs(MachineContext &ctx, UlaRenderer &ula);
  void wirePorts(MachineContext &ctx, UlaRenderer &ula);
  void wireDivMmc(MachineContext &ctx);
  void handOverState(bool hard);

  // Memory
  void updateMemoryMap(MachineContext &ctx, UlaRenderer &ula);
  void mapSlot(MachineContext &ctx, uint8_t slot);
  void mapRomSlot(MachineContext &ctx, uint8_t slot);
  void updateLayer2MemoryMapping(MachineContext &ctx);
  void updateLegacyPaging(MachineContext &ctx, UlaRenderer &ula, bool rebuildTop = true);
  uint8_t machineType() const noexcept { return m_machineType; }
  uint8_t romNumber() const noexcept;
  uint8_t altRomHalf() const noexcept;
  bool isRom3Paged() const noexcept;
  void writeNr8e(MachineContext &ctx, UlaRenderer &ula, uint8_t val);
  uint8_t readNr8e() const noexcept;

  // DivMMC
  bool divMmcEntry(uint16_t pc, bool instant) const noexcept;
  void updateDivMmc(MachineContext &ctx);

  // SPI
  bool isSdCardSelected() const noexcept;

  void scheduleReset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind);
  void raiseInterrupt(MachineContext &ctx, uint8_t im2Vector);

  NextReg m_nextReg;
  NextUart m_uart;
  NextRtc m_rtc;
  uint16_t m_extendedKeys = 0;
  std::array<uint8_t, 8> m_extendedRows{0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
  BankId m_mfRomBank = 0, m_mfRamBank = 0;
  bool m_mfActive = false, m_mfMapped = false, m_mfInvisible = true;
  bool m_mfAwaitingEntry = false;

  Copper m_copper;
  ZxnDma m_dma;
  ZxnCtc m_ctc;
  NextVideo m_nextVideo;
  BankId m_altRomBank = 0; // two 16KB alt ROMs (enAltZX.rom), writable through NR 0x8C
  BankId m_divRomBank = 0; // DivMMC 8KB ROM (enNxtmmc.rom)
  BankId m_divRamBank = 0; // DivMMC 128KB RAM (16 x 8KB)

  uint8_t m_machineType = 3; // NR 0x03: 1 = 48K, 2 = 128K, 3 = +3, 4 = Pentagon
  uint8_t m_timing = 3;
  uint8_t m_resetType = 0x02; // NR 0x02 read: 2 = hard, 1 = soft reset
  uint8_t m_port7ffd = 0;
  uint8_t m_port1ffd = 0;
  uint8_t m_portDffd = 0;
  bool m_specialPaging = false;

  uint8_t m_portE3 = 0;
  bool m_divAutomap = false;
  bool m_divNmiLatched = false;
  bool m_divMmcMapped = false;

  uint8_t m_spiCs = 0xFF;
  uint8_t m_spiLatch = 0xFF;
};

} // namespace zxkit
