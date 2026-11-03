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

#include "zxkit/peripherals/storage/FloppyDisk.h"

#include <array>
#include <cstdint>
#include <vector>

namespace zxkit {

/// The WD1793 floppy disk controller (Beta 128 / TR-DOS), emulated at the sector level on
/// `DiskImage`s, with timed seeks, uniform sector rotation, byte requests and lost-data deadlines.
/// `now` is the CPU's T-state (`Clock::totalTstates()`): the index pulses come from it.
class Wd1793 {
public:
    static constexpr std::size_t kDriveCount = 4;

    void reset() noexcept;
    void setClockHz(uint32_t hz) noexcept { m_clockHz = hz; }

    DiskImage &drive(std::size_t idx) noexcept { return m_drives[idx % kDriveCount]; }
    const DiskImage &drive(std::size_t idx) const noexcept { return m_drives[idx % kDriveCount]; }
    void selectDrive(uint8_t drive) noexcept { m_drive = drive % kDriveCount; }
    void selectSide(uint8_t side) noexcept { m_side = side & 1; }

    /// Lets the time pass: a transfer the CPU doesn't feed ends with Lost Data (call it before
    /// every register access).
    void update(uint64_t now) noexcept;
    uint8_t readStatus(uint64_t now) noexcept;
    void writeCommand(uint8_t command, uint64_t now);
    uint8_t readTrack() const noexcept { return m_track; }
    void writeTrack(uint8_t value) noexcept { m_track = value; }
    uint8_t readSector() const noexcept { return m_sector; }
    void writeSector(uint8_t value) noexcept { m_sector = value; }
    uint8_t readData() noexcept;
    void writeData(uint8_t value);

    bool intrq() const noexcept { return m_intrq; }
    bool drq() const noexcept { return m_drq; }
    bool busy() const noexcept { return m_status & kBusy; }

private:
    static constexpr uint8_t kBusy = 0x01;
    static constexpr uint8_t kIndex = 0x02;      // type I
    static constexpr uint8_t kDrq = 0x02;        // types II, III
    static constexpr uint8_t kTrack0 = 0x04;     // type I
    static constexpr uint8_t kLostData = 0x04;   // types II, III
    static constexpr uint8_t kCrcError = 0x08;
    static constexpr uint8_t kSeekError = 0x10;  // type I
    static constexpr uint8_t kNotFound = 0x10;   // types II, III
    static constexpr uint8_t kHeadLoaded = 0x20; // type I
    static constexpr uint8_t kDeleted = 0x20;    // read sector: the record type
    static constexpr uint8_t kWriteProtect = 0x40;
    static constexpr uint8_t kNotReady = 0x80;

    enum class Transfer : uint8_t { None, ReadSector, WriteSector, ReadAddress, ReadTrack, WriteTrack };

    uint64_t rotation() const noexcept { return m_clockHz / 5; }
    uint64_t byteTime() const noexcept { return uint64_t{m_clockHz} * 32 / 1'000'000; }
    uint64_t sectorArrival(uint8_t id) const noexcept;
    void scheduleError(uint8_t status) noexcept;
    void typeOne(uint8_t command);
    void startReadSector();
    void startWriteSector();
    void readAddress();
    void readWholeTrack();
    void commitWriteTrack();
    void finish(uint8_t status) noexcept;
    OptionalRef<DiskImage::Sector> findSector() noexcept;
    DiskImage &disk() noexcept { return m_drives[m_drive]; }

    std::array<DiskImage, kDriveCount> m_drives;
    std::array<uint8_t, kDriveCount> m_cylinder{}; // where each drive's head is
    uint8_t m_drive = 0;
    uint8_t m_side = 0;

    uint8_t m_status = 0;
    uint8_t m_track = 0;
    uint8_t m_sector = 1;
    uint8_t m_data = 0;
    uint8_t m_command = 0;
    bool m_typeOneStatus = true; // the status register shows the type I bits
    int8_t m_direction = 1;
    bool m_intrq = false;
    bool m_drq = false;

    Transfer m_transfer = Transfer::None;
    std::vector<uint8_t> m_buffer;
    std::size_t m_bufferPos = 0;
    std::size_t m_readAddressIndex = 0; // the sector under the head, for READ ADDRESS
    uint32_t m_clockHz = 3'500'000;
    uint64_t m_now = 0;
    uint64_t m_nextDrqAt = 0;
    uint64_t m_nextStepAt = 0, m_stepInterval = 0;
    bool m_finishing = false;
    bool m_seeking = false;
    uint8_t m_finishStatus = 0;
    uint8_t m_targetCylinder = 0, m_targetTrack = 0;
    uint8_t m_forceInterrupt = 0;
    bool m_wasReady = false;
    uint64_t m_deadline = 0;             // a WRITE TRACK without data ends at the index pulse
};

} // namespace zxkit
