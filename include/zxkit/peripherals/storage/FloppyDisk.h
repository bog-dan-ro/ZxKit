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
#include "zxkit/core/Scheduler.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

namespace zxkit {

/// Universal Floppy Disk Image container supporting raw sector images (.trd, .scl, .mgt, .img, .opd, .d40, .d80)
/// as well as standard and extended CPC/Spectrum +3/HC-2000/CoBra .dsk / .edsk images.
class DiskImage {
public:
    struct Sector {
        uint8_t physTrack = 0; // where the sector physically is
        uint8_t physSide = 0;
        uint8_t track = 0;     // ID field: C, H, R, N
        uint8_t side = 0;
        uint8_t sectorId = 1;
        uint8_t sizeCode = 2;  // 128 << sizeCode
        uint8_t st1 = 0;
        uint8_t st2 = 0;
        std::vector<uint8_t> data;  ///< several copies of a weak sector (an EDSK can store them)
        uint32_t reads = 0;         ///< reads so far: the copy the next one returns
    };

    void clear();
    void formatBlank(uint8_t tracks = 80, uint8_t sides = 2, uint8_t sectorsPerTrack = 9, uint16_t sectorSize = 512, uint8_t firstId = 1);
    void loadFromBytes(std::span<const uint8_t> raw, std::string_view ext = ".dsk");

    /// Finds sector `sectorId` (R) on physical track `track`, side `side`.
    OptionalRef<Sector> findSector(uint8_t track, uint8_t side, uint8_t sectorId) noexcept;
    OptionalRef<const Sector> findSector(uint8_t track, uint8_t side, uint8_t sectorId) const noexcept;
    OptionalRef<const Sector> firstSectorOnTrack(uint8_t track, uint8_t side) const noexcept;
    /// The sectors of a track in their physical order, from the index hole.
    std::vector<std::reference_wrapper<const Sector>> trackSectors(uint8_t track, uint8_t side) const;
    /// Finds the sector on physical `track`/`side` whose ID field is exactly C/H/R/N,
    /// as the FDC does when reading or writing.
    OptionalRef<Sector> findSectorById(uint8_t track, uint8_t side, uint8_t c, uint8_t h, uint8_t r, uint8_t n) noexcept;
    /// Replaces a whole physical track (used by FORMAT TRACK).
    void replaceTrack(uint8_t track, uint8_t side, std::vector<Sector> sectors);

    bool empty() const noexcept;
    uint8_t trackCount() const noexcept;
    uint8_t sideCount() const noexcept;
    bool isModified() const noexcept;
    void setModified(bool mod) noexcept;
    bool isWriteProtected() const noexcept;
    void setWriteProtected(bool wp) noexcept;

    /// Bumped by the disk controller at every command on this drive (activity LEDs).
    uint64_t accessCount() const noexcept { return m_accesses; }
    void noteAccess() noexcept { ++m_accesses; }

private:
    void loadRawGeometry(std::span<const uint8_t> raw, uint8_t tracks, uint8_t sides, uint8_t spt, uint16_t secSize, uint8_t firstId);
    void parseScl(std::span<const uint8_t> raw);
    void parseDsk(std::span<const uint8_t> raw);

    std::vector<Sector> m_sectors;
    bool m_modified = false;
    bool m_writeProtected = false;
    uint64_t m_accesses = 0;
};

/// NEC uPD765A / Intel 8272 Floppy Disk Controller in non-DMA (polled) mode.
/// Used by ZX Spectrum +3, HC Interface 1 FDC (HC-2000), and CoBra FDC.
class Upd765Fdc {
public:
    void reset() noexcept;

    DiskImage &drive(std::size_t idx) noexcept;
    const DiskImage &drive(std::size_t idx) const noexcept;
    /// Mask applied to the US unit select bits, e.g. 1 when only two drives are
    /// wired (units 2 and 3 then address drives 0 and 1).
    void setUnitMask(uint8_t mask) noexcept { m_unitMask = mask & 3; }

    // `now` is the CPU's T-state (`Clock::totalTstates()`): a CPU too slow to move the
    // bytes of a transfer makes the controller end it with an overrun, like the real one.
    uint8_t readMainStatus(uint64_t now) noexcept;
    uint8_t readData(uint64_t now);
    void writeData(uint8_t val, uint64_t now);
    /// Terminal Count: ends the current read / write / format execution phase.
    void terminalCount() noexcept;

private:
    enum Phase : uint8_t {
        PhaseCommand = 0,
        PhaseExecRead,
        PhaseExecWrite,
        PhaseExecFormat,
        PhaseResult
    };

    static std::size_t commandLength(uint8_t cmd) noexcept;
    void enterResultPhase() noexcept;
    void setResult(uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n);
    void executeCommand(uint8_t cmd);
    void startRead(uint8_t cmd);
    void startWrite(uint8_t cmd);
    void finishTransfer(bool byTerminalCount);
    void commitWriteSector();
    void commitFormat();
    void checkOverrun(uint64_t now) noexcept;
    uint8_t headUnit() const noexcept;

    std::array<DiskImage, 4> m_drives;
    std::array<uint8_t, 4> m_currentTrack{};
    std::array<uint8_t, 4> m_readIdIndex{}; // emulates the disk rotating under the head
    uint8_t m_unitMask = 3;
    uint8_t m_msr = 0x80;
    uint8_t m_driveBusy = 0;     // MSR bits 0..3: seeking until SENSE INTERRUPT
    uint8_t m_pendingInts = 0;   // drives with a pending seek interrupt
    Phase m_phase = PhaseCommand;

    std::vector<uint8_t> m_cmdBuf;
    std::vector<uint8_t> m_execCmd; // the command being executed
    std::vector<uint8_t> m_resBuf;
    std::size_t m_resPos = 0;
    bool m_tcCanNormalize = false; // TC right after the last byte still ends normally
    bool m_readTrack = false;      // the transfer is a READ TRACK
    int m_speedlock = 0;           // FUSE's Speedlock weak sector emulation: re-reads of C0 H0 R2
    uint32_t m_lastSectorRead = 0;

    // Execution phase transfer state
    std::vector<uint8_t> m_dataBuf;
    std::size_t m_dataPos = 0;
    std::vector<std::size_t> m_sectorEnds; // data offsets at which each sector ends
    std::vector<uint8_t> m_sectorIds;
    uint8_t m_xferDrive = 0;
    uint8_t m_xferTrack = 0;
    uint8_t m_xferSide = 0;
    uint8_t m_xferC = 0, m_xferH = 0, m_xferN = 2;
    uint8_t m_xferSt1 = 0, m_xferSt2 = 0;
    uint64_t m_transferDeadline = 0; // the next data byte must move before this T-state
    bool m_transferTimed = false;
};

/// HC Interface 1 uPD765 Floppy Disk Controller (built into the HC-2000).
/// The 16KB "hc-if1.rom" pages in like a ZX Interface 1 (M1 at 0x0008 / 0x1708,
/// out after the fetch at 0x0700), with its 1KB RAM at 0x2800 (mirrored at
/// 0x2C00, 0x3800 and 0x3C00). FDC: status 0x85, data 0x87, control 0x05/0x07
/// (D0 = TC, D1/D2 = drive A/B select + motor, D3 = enable, D4 = /reset).
class HcIf1FdcPeripheral : public Peripheral {
public:
    static constexpr std::size_t kDriveCount = 2;

    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;

    void onAttach(MachineContext &ctx) override;
    void onDetach(MachineContext &ctx) override;
    void reset(MachineContext &ctx, ResetKind kind) override;

    std::size_t diskDriveCount() const noexcept override;
    OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept override;

    Upd765Fdc &fdc() noexcept;
    const Upd765Fdc &fdc() const noexcept;
    BankId romBankId() const noexcept;
    BankId ramBankId() const noexcept;
    bool isMotorOn() const noexcept { return m_motorOn != 0; }

private:
    void writeControl(MachineContext &ctx, uint8_t val);

    BankId m_romBank = 0;
    BankId m_ramBank = 0;
    Upd765Fdc m_fdc;
    uint8_t m_motorOn = 0; // bit per drive
    EventId m_motorOffEvent = 0;
};


} // namespace zxkit
