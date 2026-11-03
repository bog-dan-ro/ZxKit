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

#include "zxkit/peripherals/storage/Wd1793.h"

#include <algorithm>

namespace zxkit {

namespace {

// A rotation every 200 ms (300 rpm), with a 4 ms index pulse, in 3.5 MHz T-states
constexpr uint8_t kMaxCylinder = 85;
// An MFM track: the bytes READ TRACK returns and WRITE TRACK takes
constexpr std::size_t kTrackBytes = 6250;

uint16_t crc16(uint16_t crc, uint8_t byte) noexcept
{
    crc ^= static_cast<uint16_t>(byte << 8);
    for (int i = 0; i < 8; ++i)
        crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1));
    return crc;
}

uint16_t crc16(std::span<const uint8_t> bytes) noexcept
{
    uint16_t crc = 0xFFFF;
    for (uint8_t b : bytes)
        crc = crc16(crc, b);
    return crc;
}

std::size_t sectorBytes(uint8_t sizeCode) noexcept
{
    return std::size_t{128} << std::min<uint8_t>(sizeCode, 7);
}

} // namespace

void Wd1793::reset() noexcept
{
    m_status = 0;
    m_track = 0;
    m_sector = 1;
    m_data = 0;
    m_command = 0;
    m_typeOneStatus = true;
    m_direction = 1;
    m_intrq = false;
    m_drq = false;
    m_transfer = Transfer::None;
    m_buffer.clear();
    m_bufferPos = 0;
    m_now = m_deadline = m_nextDrqAt = 0;
    m_finishing = m_seeking = false;
    m_forceInterrupt = 0;
    m_wasReady = !disk().empty();
}

uint64_t Wd1793::sectorArrival(uint8_t id) const noexcept
{
    const auto sectors = m_drives[m_drive].trackSectors(m_cylinder[m_drive], m_side);
    const auto found = std::ranges::find_if(sectors, [id](const auto &s) { return s.get().sectorId == id; });
    const auto position = found == sectors.end() ? 0 : std::distance(sectors.begin(), found);
    const uint64_t settle = (m_command & 4) ? uint64_t{m_clockHz} * 30 / 1000 : 0;
    const uint64_t start = m_now + settle;
    const uint64_t phase = sectors.empty() ? 0 : (rotation() * position / sectors.size() + 1000) % rotation();
    return start + (phase + rotation() - start % rotation()) % rotation();
}

void Wd1793::scheduleError(uint8_t status) noexcept
{
    m_finishing = true;
    m_finishStatus = status;
    m_status = kBusy;
    m_drq = false;
    m_deadline = m_now + 5 * rotation();
}

void Wd1793::update(uint64_t now) noexcept
{
    const auto previous = m_now;
    m_now = now;
    const bool ready = !disk().empty();
    if (((m_forceInterrupt & 1) && !m_wasReady && ready)
        || ((m_forceInterrupt & 2) && m_wasReady && !ready)
        || ((m_forceInterrupt & 4) && now / rotation() != previous / rotation())) m_intrq = true;
    m_wasReady = ready;
    if (m_seeking) {
        while (m_nextStepAt <= now && m_cylinder[m_drive] != m_targetCylinder) {
            const int direction = m_targetCylinder > m_cylinder[m_drive] ? 1 : -1;
            m_cylinder[m_drive] = static_cast<uint8_t>(m_cylinder[m_drive] + direction);
            if ((m_command & 0xF0) == 0x10 || (m_command & 0x10))
                m_track = static_cast<uint8_t>(m_track + direction);
            m_nextStepAt += m_stepInterval;
        }
    }
    if ((m_finishing || m_seeking) && now >= m_deadline) {
        if (m_seeking) { m_cylinder[m_drive] = m_targetCylinder; m_track = m_targetTrack; }
        finish(m_finishStatus);
        return;
    }
    if (m_finishing || m_seeking || m_transfer == Transfer::None) return;
    if (m_drq && now >= m_deadline) { finish(kLostData); return; }
    if (!m_drq && now >= m_nextDrqAt) {
        m_drq = true;
        m_deadline = m_nextDrqAt + byteTime();
        if (now >= m_deadline) finish(kLostData);
    }
}

uint8_t Wd1793::readStatus(uint64_t now) noexcept
{
    update(now);
    m_intrq = false; // reading the status acknowledges the interrupt
    const DiskImage &d = m_drives[m_drive];
    uint8_t status = m_status;
    if (d.empty())
        status |= kNotReady;
    if (m_typeOneStatus) {
        status &= static_cast<uint8_t>(~(kIndex | kTrack0 | kWriteProtect));
        if (m_cylinder[m_drive] == 0)
            status |= kTrack0;
        if (d.isWriteProtected())
            status |= kWriteProtect;
        if (!d.empty() && now % rotation() < m_clockHz / 250)
            status |= kIndex;
    } else if (m_drq) {
        status |= kDrq;
    }
    return status;
}

void Wd1793::finish(uint8_t status) noexcept
{
    m_finishing = m_seeking = false;
    m_status = static_cast<uint8_t>(status & ~kBusy);
    m_drq = false;
    m_intrq = true;
    m_transfer = Transfer::None;
}

void Wd1793::writeCommand(uint8_t command, uint64_t now)
{
    update(now);
    // Force interrupt: ends whatever runs
    if ((command & 0xF0) == 0xD0) {
        m_finishing = m_seeking = false;
        m_forceInterrupt = command & 7;
        m_transfer = Transfer::None;
        m_drq = false;
        m_typeOneStatus = true;
        m_status &= static_cast<uint8_t>(~kBusy);
        m_intrq = (command & 8) != 0;
        m_command = command;
        return;
    }
    if (m_status & kBusy)
        return; // a busy controller ignores the other commands
    m_intrq = false;
    m_forceInterrupt = 0;
    m_command = command;

    if (!(command & 0x80)) {
        typeOne(command);
        return;
    }
    m_typeOneStatus = false;
    m_status = 0;
    switch (command & 0xF0) {
    case 0x80:
    case 0x90:
        startReadSector();
        break;
    case 0xA0:
    case 0xB0:
        startWriteSector();
        break;
    case 0xC0:
        readAddress();
        break;
    case 0xE0:
        readWholeTrack();
        break;
    case 0xF0:
        if (disk().empty()) {
            finish(kNotReady);
        } else if (disk().isWriteProtected()) {
            finish(kWriteProtect);
        } else {
            disk().noteAccess();
            m_buffer.clear();
            m_transfer = Transfer::WriteTrack;
            m_status = kBusy;
            m_drq = true;
            // The next index pulse
            m_deadline = now + (rotation() - now % rotation());
            m_nextDrqAt = m_deadline;
        }
        break;
    default:
        finish(0);
        break;
    }
}

void Wd1793::typeOne(uint8_t command)
{
    m_typeOneStatus = true;
    disk().noteAccess();
    uint8_t &cylinder = m_cylinder[m_drive];
    const auto previousCylinder = cylinder, previousTrack = m_track;
    const auto stepBy = [&cylinder](int delta) {
        cylinder = static_cast<uint8_t>(std::clamp(int{cylinder} + delta, 0, int{kMaxCylinder}));
    };

    switch (command & 0xF0) {
    case 0x00: // RESTORE
        cylinder = 0;
        m_track = 0;
        break;
    case 0x10: { // SEEK to the data register's track
        const int delta = int{m_data} - int{m_track};
        stepBy(delta);
        m_track = m_data;
        break;
    }
    default: { // STEP (0x20), STEP IN (0x40), STEP OUT (0x60); bit 4 updates the track register
        if ((command & 0xE0) == 0x40)
            m_direction = 1;
        else if ((command & 0xE0) == 0x60)
            m_direction = -1;
        stepBy(m_direction);
        if (command & 0x10)
            m_track = static_cast<uint8_t>(m_track + m_direction);
        break;
    }
    }

    uint8_t status = (command & 0x08) ? kHeadLoaded : 0;
    if (command & 0x04) {
        // Verify: an id on this cylinder must carry the track register's number
        const auto sectors = disk().trackSectors(cylinder, m_side);
        const bool found = std::ranges::any_of(sectors, [this](const DiskImage::Sector &s) { return s.track == m_track; });
        if (!found)
            status |= kSeekError;
    }
    m_targetCylinder = cylinder;
    m_targetTrack = m_track;
    const unsigned steps = (command & 0xF0) == 0 ? previousCylinder
        : (command & 0xF0) == 0x10 ? std::abs(int{m_data} - int{previousTrack}) : 1;
    if (!steps && !(command & 4)) { finish(status); return; }
    cylinder = previousCylinder;
    m_track = previousTrack;
    constexpr std::array<unsigned,4> rates{6,12,20,30}; // 1MHz WD1793 clock.
    m_deadline = m_now + uint64_t{m_clockHz} * (steps * rates[command & 3] + ((command & 4) ? 30 : 0)) / 1000;
    m_stepInterval = uint64_t{m_clockHz} * rates[command & 3] / 1000;
    m_nextStepAt = m_now + m_stepInterval;
    m_seeking = true;
    m_finishStatus = status;
    m_status = static_cast<uint8_t>(kBusy | (status & kHeadLoaded));
}

OptionalRef<DiskImage::Sector> Wd1793::findSector() noexcept
{
    // The id's track and sector must match the registers; the side only with the C flag
    const bool compareSide = (m_command & 0x02) != 0;
    const uint8_t side = (m_command & 0x08) ? 1 : 0;
    for (const DiskImage::Sector &s : disk().trackSectors(m_cylinder[m_drive], m_side)) {
        if (s.track == m_track && s.sectorId == m_sector && (!compareSide || s.side == side))
            return disk().findSector(s.physTrack, s.physSide, s.sectorId);
    }
    return std::nullopt;
}

void Wd1793::startReadSector()
{
    if (disk().empty()) {
        finish(kNotReady);
        return;
    }
    disk().noteAccess();
    const auto sector = findSector();
    if (!sector) {
        scheduleError(kNotFound);
        return;
    }
    const DiskImage::Sector &s = *sector;
    m_buffer.assign(s.data.begin(), s.data.begin() + static_cast<std::ptrdiff_t>(std::min(s.data.size(), sectorBytes(s.sizeCode))));
    m_buffer.resize(sectorBytes(s.sizeCode), 0);
    m_bufferPos = 0;
    m_transfer = Transfer::ReadSector;
    m_status = static_cast<uint8_t>(kBusy | ((s.st2 & 0x40) ? kDeleted : 0));
    m_drq = false;
    m_nextDrqAt = sectorArrival(m_sector);
}

void Wd1793::startWriteSector()
{
    if (disk().empty()) {
        finish(kNotReady);
        return;
    }
    if (disk().isWriteProtected()) {
        finish(kWriteProtect);
        return;
    }
    disk().noteAccess();
    const auto sector = findSector();
    if (!sector) {
        scheduleError(kNotFound);
        return;
    }
    m_buffer.assign(sectorBytes(sector->get().sizeCode), 0);
    m_bufferPos = 0;
    m_transfer = Transfer::WriteSector;
    m_status = kBusy;
    m_drq = false;
    m_nextDrqAt = sectorArrival(m_sector);
}

void Wd1793::readAddress()
{
    if (disk().empty()) {
        finish(kNotReady);
        return;
    }
    disk().noteAccess();
    const auto sectors = disk().trackSectors(m_cylinder[m_drive], m_side);
    if (sectors.empty()) {
        scheduleError(kNotFound);
        return;
    }
    // The next id passing under the head
    const auto index = ((m_now % rotation() > 1000 ? m_now % rotation() - 1000 : 0) * sectors.size() + rotation() - 1) / rotation();
    const DiskImage::Sector &s = sectors[index % sectors.size()];
    const std::array<uint8_t, 8> id{0xA1, 0xA1, 0xA1, 0xFE, s.track, s.side, s.sectorId, s.sizeCode};
    const uint16_t crc = crc16(id);
    m_buffer = {s.track, s.side, s.sectorId, s.sizeCode, static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
    m_bufferPos = 0;
    m_sector = s.track; // the WD1793 puts the id's track in the sector register
    m_transfer = Transfer::ReadAddress;
    m_status = kBusy;
    m_drq = false;
    m_nextDrqAt = sectorArrival(s.sectorId);
}

void Wd1793::readWholeTrack()
{
    if (disk().empty()) {
        finish(kNotReady);
        return;
    }
    disk().noteAccess();
    // An MFM track built from the sectors: gaps, sync bytes, address marks, CRCs
    m_buffer.clear();
    const auto put = [this](uint8_t byte, std::size_t count) { m_buffer.insert(m_buffer.end(), count, byte); };
    put(0x4E, 80);
    put(0x00, 12);
    put(0xC2, 3);
    put(0xFC, 1);
    put(0x4E, 50);
    for (const DiskImage::Sector &s : disk().trackSectors(m_cylinder[m_drive], m_side)) {
        put(0x00, 12);
        const std::array<uint8_t, 8> id{0xA1, 0xA1, 0xA1, 0xFE, s.track, s.side, s.sectorId, s.sizeCode};
        m_buffer.insert(m_buffer.end(), id.begin(), id.end());
        const uint16_t idCrc = crc16(id);
        put(static_cast<uint8_t>(idCrc >> 8), 1);
        put(static_cast<uint8_t>(idCrc), 1);
        put(0x4E, 22);
        put(0x00, 12);
        const std::size_t start = m_buffer.size();
        put(0xA1, 3);
        put((s.st2 & 0x40) ? 0xF8 : 0xFB, 1);
        m_buffer.insert(m_buffer.end(), s.data.begin(), s.data.begin() + static_cast<std::ptrdiff_t>(std::min(s.data.size(), sectorBytes(s.sizeCode))));
        const uint16_t dataCrc = crc16(std::span(m_buffer).subspan(start));
        put(static_cast<uint8_t>(dataCrc >> 8), 1);
        put(static_cast<uint8_t>(dataCrc), 1);
        put(0x4E, 54);
    }
    if (m_buffer.size() < kTrackBytes)
        put(0x4E, kTrackBytes - m_buffer.size());
    m_buffer.resize(kTrackBytes);
    m_bufferPos = 0;
    m_transfer = Transfer::ReadTrack;
    m_status = kBusy;
    m_drq = false;
    m_nextDrqAt = m_now + rotation() - m_now % rotation();
}

uint8_t Wd1793::readData() noexcept
{
    if (!m_drq || (m_transfer != Transfer::ReadSector && m_transfer != Transfer::ReadAddress && m_transfer != Transfer::ReadTrack))
        return m_data;
    m_drq = false;
    m_nextDrqAt += byteTime();
    m_data = m_buffer[m_bufferPos++];
    if (m_bufferPos < m_buffer.size())
        return m_data;

    if (m_transfer == Transfer::ReadSector) {
        const auto sector = findSector();
        const bool crcError = sector && (sector->get().st1 & 0x20);
        if (crcError) {
            finish(static_cast<uint8_t>(kCrcError | (m_status & kDeleted)));
        } else if (m_command & 0x10) {
            // Multiple sectors: on to the next one, until there's none
            ++m_sector;
            if (findSector())
                startReadSector();
            else
                finish(kNotFound);
        } else {
            finish(static_cast<uint8_t>(m_status & kDeleted));
        }
    } else {
        finish(0);
    }
    return m_data;
}

void Wd1793::writeData(uint8_t value)
{
    m_data = value;
    if (m_transfer != Transfer::None && !m_drq) return;
    m_drq = false;
    if (m_transfer == Transfer::WriteTrack && m_buffer.empty()) m_nextDrqAt = m_deadline;
    else m_nextDrqAt += byteTime();
    if (m_transfer == Transfer::WriteSector) {
        m_buffer[m_bufferPos++] = value;
        if (m_bufferPos < m_buffer.size())
            return;
        if (auto sector = findSector()) {
            sector->get().data.resize(m_buffer.size());
            std::ranges::copy(m_buffer, sector->get().data.begin());
            sector->get().st1 &= static_cast<uint8_t>(~0x20);
            sector->get().st2 &= static_cast<uint8_t>(~0x60);
            if (m_command & 0x01)
                sector->get().st2 |= 0x40; // a deleted data mark
            disk().setModified(true);
        }
        if (m_command & 0x10) {
            ++m_sector;
            if (findSector())
                startWriteSector();
            else
                finish(kNotFound);
        } else {
            finish(0);
        }
    } else if (m_transfer == Transfer::WriteTrack) {
        m_buffer.push_back(value);
        if (m_buffer.size() >= kTrackBytes)
            commitWriteTrack();
    }
}

void Wd1793::commitWriteTrack()
{
    // Rebuild the sectors from the bytes written: F5 writes a sync byte (A1), FE an id
    // address mark, FB / F8 a (deleted) data mark, F7 the CRC
    std::vector<DiskImage::Sector> sectors;
    std::size_t i = 0;
    const auto afterSync = [this](std::size_t pos) { return pos > 0 && m_buffer[pos - 1] == 0xF5; };
    while (i < m_buffer.size()) {
        if (m_buffer[i] == 0xFE && afterSync(i) && i + 4 < m_buffer.size()) {
            DiskImage::Sector sector;
            sector.track = m_buffer[i + 1];
            sector.side = m_buffer[i + 2];
            sector.sectorId = m_buffer[i + 3];
            sector.sizeCode = m_buffer[i + 4];
            const std::size_t length = sectorBytes(sector.sizeCode);
            i += 5;
            // The data mark follows the id (within the next ~50 bytes)
            for (std::size_t j = i; j < std::min(m_buffer.size(), i + 64); ++j) {
                if ((m_buffer[j] == 0xFB || m_buffer[j] == 0xF8) && afterSync(j)) {
                    const std::size_t begin = j + 1;
                    const std::size_t end = std::min(m_buffer.size(), begin + length);
                    sector.data.assign(m_buffer.begin() + static_cast<std::ptrdiff_t>(begin),
                                       m_buffer.begin() + static_cast<std::ptrdiff_t>(end));
                    sector.data.resize(length, 0);
                    if (m_buffer[j] == 0xF8)
                        sector.st2 = 0x40;
                    i = end;
                    break;
                }
            }
            if (sector.data.empty())
                sector.data.assign(length, 0);
            sectors.push_back(std::move(sector));
            continue;
        }
        ++i;
    }
    disk().replaceTrack(m_cylinder[m_drive], m_side, std::move(sectors));
    finish(0);
}

} // namespace zxkit
