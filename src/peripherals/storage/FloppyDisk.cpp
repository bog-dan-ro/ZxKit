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

#include "zxkit/peripherals/storage/FloppyDisk.h"

#include "zxkit/core/Exception.h"
#include "zxkit/cpu/Z80Cpu.h"

#include <algorithm>
#include <cstring>

namespace zxkit {

void DiskImage::clear()
{
    m_sectors.clear();
    m_modified = false;
    m_writeProtected = false;
}

void DiskImage::formatBlank(uint8_t tracks, uint8_t sides, uint8_t sectorsPerTrack, uint16_t sectorSize, uint8_t firstId)
{
    clear();
    uint8_t sizeCode = 2;
    if (sectorSize == 128) sizeCode = 0;
    else if (sectorSize == 256) sizeCode = 1;
    else if (sectorSize == 512) sizeCode = 2;
    else if (sectorSize == 1024) sizeCode = 3;

    for (uint8_t t = 0; t < tracks; ++t) {
        for (uint8_t s = 0; s < sides; ++s) {
            for (uint8_t sec = 0; sec < sectorsPerTrack; ++sec) {
                m_sectors.push_back(Sector{
                    .physTrack = t,
                    .physSide = s,
                    .track = t,
                    .side = s,
                    .sectorId = static_cast<uint8_t>(firstId + sec),
                    .sizeCode = sizeCode,
                    .data = std::vector<uint8_t>(sectorSize, 0xE5)
                });
            }
        }
    }
}

namespace {

bool startsWith(std::span<const uint8_t> bytes, std::string_view magic) noexcept
{
    return bytes.size() >= magic.size()
           && std::ranges::equal(bytes.first(magic.size()), magic,
                                 [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); });
}

} // namespace

void DiskImage::loadFromBytes(std::span<const uint8_t> raw, std::string_view ext)
{
    clear();
    if (ext == ".td0" || startsWith(raw, "TD") || startsWith(raw, "td")) {
        parseTeledisk(raw);
        return;
    }
    if (raw.size() >= 256 && (startsWith(raw, "MV - CPC") || startsWith(raw, "EXTENDED"))) {
        parseDsk(raw);
        return;
    }
    if (ext == ".scl" && raw.size() >= 9 && startsWith(raw, "SINCLAIR")) {
        parseScl(raw);
        return;
    }
    // Raw sector images, geometry guessed from the extension and size
    if (ext == ".trd") {
        // TR-DOS: the disk type in the specification sector (track 0, sector 9) gives the geometry
        uint8_t tracks = 80;
        uint8_t sides = 2;
        if (raw.size() > 8 * 256 + 0xE3) {
            switch (raw[8 * 256 + 0xE3]) {
            case 0x17: tracks = 40; break;
            case 0x18: sides = 1; break;
            case 0x19: tracks = 40; sides = 1; break;
            default: break;
            }
        }
        loadRawGeometry(raw, tracks, sides, 16, 256, 1);
    } else if (raw.size() == 737280) {
        loadRawGeometry(raw, 80, 2, 9, 512, 1);   // 3.5" 720K (HC CP/M .img)
    } else if (raw.size() == 368640) {
        loadRawGeometry(raw, 40, 2, 9, 512, 1);   // 5.25" 360K
    } else if (raw.size() == 655360 && ext != ".mgt") {
        loadRawGeometry(raw, 80, 2, 16, 256, 1);  // TR-DOS
    } else {
        loadRawGeometry(raw, 80, 2, 10, 512, 1);  // +D / Opus .mgt
    }
}

OptionalRef<DiskImage::Sector> DiskImage::findSector(uint8_t track, uint8_t side, uint8_t sectorId) noexcept
{
    for (auto &s : m_sectors) {
        if (s.physTrack == track && s.physSide == side && s.sectorId == sectorId)
            return s;
    }
    return std::nullopt;
}

OptionalRef<const DiskImage::Sector> DiskImage::findSector(uint8_t track, uint8_t side, uint8_t sectorId) const noexcept
{
    for (const auto &s : m_sectors) {
        if (s.physTrack == track && s.physSide == side && s.sectorId == sectorId)
            return s;
    }
    return std::nullopt;
}

OptionalRef<const DiskImage::Sector> DiskImage::firstSectorOnTrack(uint8_t track, uint8_t side) const noexcept
{
    for (const auto &s : m_sectors) {
        if (s.physTrack == track && s.physSide == side)
            return s;
    }
    return std::nullopt;
}

std::vector<std::reference_wrapper<const DiskImage::Sector>> DiskImage::trackSectors(uint8_t track, uint8_t side) const
{
    std::vector<std::reference_wrapper<const Sector>> out;
    for (const auto &s : m_sectors) {
        if (s.physTrack == track && s.physSide == side)
            out.push_back(s);
    }
    return out;
}

OptionalRef<DiskImage::Sector> DiskImage::findSectorById(uint8_t track, uint8_t side, uint8_t c, uint8_t h, uint8_t r, uint8_t n) noexcept
{
    for (auto &s : m_sectors) {
        if (s.physTrack == track && s.physSide == side && s.track == c && s.side == h && s.sectorId == r && s.sizeCode == n)
            return s;
    }
    return std::nullopt;
}

void DiskImage::replaceTrack(uint8_t track, uint8_t side, std::vector<Sector> sectors)
{
    std::erase_if(m_sectors, [track, side](const Sector &s) { return s.physTrack == track && s.physSide == side; });
    const auto pos = std::ranges::find_if(m_sectors, [track, side](const Sector &s) {
        return s.physTrack > track || (s.physTrack == track && s.physSide > side);
    });
    for (auto &s : sectors) {
        s.physTrack = track;
        s.physSide = side;
    }
    m_sectors.insert(pos, std::make_move_iterator(sectors.begin()), std::make_move_iterator(sectors.end()));
    m_modified = true;
}

bool DiskImage::empty() const noexcept
{
    return m_sectors.empty();
}

uint8_t DiskImage::trackCount() const noexcept
{
    uint8_t n = 0;
    for (const auto &s : m_sectors)
        n = std::max<uint8_t>(n, static_cast<uint8_t>(s.physTrack + 1));
    return n;
}

uint8_t DiskImage::sideCount() const noexcept
{
    uint8_t n = 0;
    for (const auto &s : m_sectors)
        n = std::max<uint8_t>(n, static_cast<uint8_t>(s.physSide + 1));
    return n;
}

bool DiskImage::isModified() const noexcept
{
    return m_modified;
}

void DiskImage::setModified(bool mod) noexcept
{
    m_modified = mod;
}

bool DiskImage::isWriteProtected() const noexcept
{
    return m_writeProtected;
}

void DiskImage::setWriteProtected(bool wp) noexcept
{
    m_writeProtected = wp;
}

void DiskImage::loadRawGeometry(std::span<const uint8_t> raw, uint8_t tracks, uint8_t sides, uint8_t spt, uint16_t secSize, uint8_t firstId)
{
    formatBlank(tracks, sides, spt, secSize, firstId);
    std::size_t offset = 0;
    for (auto &sec : m_sectors) {
        if (offset >= raw.size())
            break;
        const std::size_t n = std::min<std::size_t>(secSize, raw.size() - offset);
        std::ranges::copy(raw.subspan(offset, n), sec.data.begin());
        offset += n;
    }
}

void DiskImage::parseScl(std::span<const uint8_t> raw)
{
    // An SCL holds TR-DOS files: lay them out on an 80 track double sided disk, after the
    // catalog (track 0, sectors 1-8) and the disk specification (sector 9)
    formatBlank(80, 2, 16, 256, 1);
    for (auto &sec : m_sectors) {
        if (sec.physTrack == 0 && sec.physSide == 0)
            std::ranges::fill(sec.data, 0);
    }
    const uint8_t fileCount = raw[8];
    std::size_t dataOffset = 9 + static_cast<std::size_t>(fileCount) * 14;
    if (dataOffset > raw.size())
        throw FormatException("Corrupt SCL file header");

    // Logical tracks: cylinder * 2 + side
    unsigned track = 1;
    unsigned sector = 0;
    const unsigned files = std::min<unsigned>(fileCount, 128);
    for (unsigned i = 0; i < files; ++i) {
        const auto entry = raw.subspan(9 + i * 14, 14);
        if (auto dirSec = findSector(0, 0, static_cast<uint8_t>(1 + i / 16))) {
            const auto dst = std::span(dirSec->get().data).subspan((i % 16) * 16, 16);
            std::ranges::copy(entry, dst.begin());
            dst[14] = static_cast<uint8_t>(sector);
            dst[15] = static_cast<uint8_t>(track);
        }
        for (uint8_t s = 0; s < entry[13]; ++s) {
            auto sec = findSector(static_cast<uint8_t>(track / 2), static_cast<uint8_t>(track % 2),
                                  static_cast<uint8_t>(sector + 1));
            if (sec && dataOffset + 256 <= raw.size()) {
                std::ranges::copy(raw.subspan(dataOffset, 256), sec->get().data.begin());
                dataOffset += 256;
            }
            if (++sector == 16) {
                sector = 0;
                ++track;
            }
        }
    }
    if (auto spec = findSector(0, 0, 9)) {
        auto &d = spec->get().data;
        const unsigned freeSectors = 160 * 16 - (track * 16 + sector);
        d[0xE1] = static_cast<uint8_t>(sector);
        d[0xE2] = static_cast<uint8_t>(track);
        d[0xE3] = 0x16; // 80 tracks, double sided
        d[0xE4] = static_cast<uint8_t>(files);
        d[0xE5] = static_cast<uint8_t>(freeSectors);
        d[0xE6] = static_cast<uint8_t>(freeSectors >> 8);
        d[0xE7] = 0x10; // TR-DOS
        std::fill(d.begin() + 0xEA, d.begin() + 0xF3, ' ');
        std::fill(d.begin() + 0xF5, d.begin() + 0xFD, ' ');
    }
}

void DiskImage::parseDsk(std::span<const uint8_t> raw)
{
    const bool isExtended = startsWith(raw, "EXTENDED");
    const uint8_t tracks = raw[0x30];
    const uint8_t sides  = raw[0x31];
    const uint16_t fixedTrackSize = static_cast<uint16_t>(raw[0x32] | (raw[0x33] << 8));

    std::size_t pos = 0x100;
    std::size_t trackTableIdx = 0x34;
    for (uint8_t t = 0; t < tracks; ++t) {
        for (uint8_t s = 0; s < sides; ++s) {
            const std::size_t trkSize = isExtended
                ? (static_cast<std::size_t>(raw[trackTableIdx++]) * 256u)
                : static_cast<std::size_t>(fixedTrackSize);
            if (trkSize == 0)
                continue;
            if (pos + 0x100 > raw.size())
                return;

            const auto trkHdr = raw.subspan(pos, 0x100);
            if (!startsWith(trkHdr, "Track-Info")) {
                pos += trkSize;
                continue;
            }
            const uint8_t sectorCount = trkHdr[0x15];
            const uint16_t defaultSecSize = static_cast<uint16_t>(128u << (trkHdr[0x14] & 0x07));
            std::size_t dataCursor = pos + 0x100;

            for (uint8_t i = 0; i < sectorCount; ++i) {
                const auto sInfo = trkHdr.subspan(0x18 + i * 8, 8);
                uint16_t actualSize = isExtended
                    ? static_cast<uint16_t>(sInfo[6] | (sInfo[7] << 8))
                    : defaultSecSize;
                if (dataCursor + actualSize > raw.size())
                    break;

                m_sectors.push_back(Sector{
                    .physTrack = t,
                    .physSide = s,
                    .track = sInfo[0],
                    .side = sInfo[1],
                    .sectorId = sInfo[2],
                    .sizeCode = sInfo[3],
                    .st1 = sInfo[4],
                    .st2 = sInfo[5],
                    .mfm = trkHdr[0x13] != 1,
                    .data = std::vector<uint8_t>(std::from_range, raw.subspan(dataCursor, actualSize))
                });
                dataCursor += actualSize;
            }
            pos += trkSize;
        }
    }
}

// ── uPD765 ────────────────────────────────────────────────────────────────

namespace {

constexpr uint8_t kMsrRqm = 0x80;
constexpr uint8_t kMsrDio = 0x40;
constexpr uint8_t kMsrExm = 0x20;
constexpr uint8_t kMsrCb  = 0x10;

constexpr uint8_t kSt0AbnormalTermination = 0x40;
constexpr uint8_t kSt0InvalidCommand      = 0x80;
constexpr uint8_t kSt0SeekEnd             = 0x20;
constexpr uint8_t kSt0NotReady            = 0x08;
constexpr uint8_t kSt1EndOfCylinder       = 0x80;
constexpr uint8_t kSt1NotWritable         = 0x02;
constexpr uint8_t kSt1NoData              = 0x04;
constexpr uint8_t kSt1MissingAddressMark  = 0x01;
constexpr uint8_t kSt1Overrun             = 0x10;

// A data byte every 32 us (250 kbit/s MFM), in 3.5 MHz T-states. The CPU must move each byte within a
// few byte times, and gets 20 ms for the first one: the sector has to come under the head first.
constexpr uint64_t kByteTstates = 112;
constexpr uint64_t kOverrunTstates = 4 * kByteTstates;
constexpr uint64_t kFirstByteTstates = 70'000;
// After the last data byte the controller reads the CRC, then interrupts for the result phase
constexpr uint64_t kResultIntTstates = 2 * kByteTstates + 32;
// The first data byte interrupts once the sector is under the head: not at once (CoBra's loaders
// enable the interrupts, then switch registers to the buffer pointer)
constexpr uint64_t kFirstByteIntTstates = 1000;

} // namespace

void Upd765Fdc::reset() noexcept
{
    m_msr = kMsrRqm;
    m_driveBusy = 0;
    m_pendingInts = 0;
    m_cmdBuf.clear();
    m_resBuf.clear();
    m_resPos = 0;
    m_dataBuf.clear();
    m_dataPos = 0;
    m_phase = PhaseCommand;
    m_tcCanNormalize = false;
    m_transferTimed = false;
    m_readTrack = false;
    m_nonDma = false;
    m_resultInt = false;
    m_tcStop = SIZE_MAX;
}

DiskImage &Upd765Fdc::drive(std::size_t idx) noexcept
{
    return m_drives[idx & 3];
}

const DiskImage &Upd765Fdc::drive(std::size_t idx) const noexcept
{
    return m_drives[idx & 3];
}

void Upd765Fdc::checkOverrun(uint64_t now) noexcept
{
    if (!m_transferTimed || (m_phase != PhaseExecRead && m_phase != PhaseExecWrite) || now <= m_transferDeadline)
        return;
    // Nobody moved the byte in time: the controller gives up the transfer (copy protections rely on it)
    std::size_t idx = 0;
    while (idx + 1 < m_sectorEnds.size() && m_dataPos >= m_sectorEnds[idx])
        ++idx;
    const uint8_t hu = static_cast<uint8_t>((m_xferSide << 2) | m_xferDrive);
    setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), static_cast<uint8_t>(m_xferSt1 | kSt1Overrun),
              m_xferSt2, m_xferC, m_xferH, m_sectorIds.empty() ? 0 : m_sectorIds[idx], m_xferN);
    m_transferTimed = false;
}

uint8_t Upd765Fdc::readMainStatus(uint64_t now) noexcept
{
    checkOverrun(now);
    if (m_nonDma && (m_phase == PhaseExecRead || m_phase == PhaseExecWrite || m_phase == PhaseExecFormat)
        && now < m_execIntFrom)
        return static_cast<uint8_t>((m_msr & ~kMsrRqm) | m_driveBusy);
    // After the last data byte the controller still reads the CRC: it is busy executing until
    // the result phase interrupts (CoBra's loaders HALT until EXM clears)
    if (m_phase == PhaseResult && m_resultInt && now < m_resultIntFrom)
        return static_cast<uint8_t>(kMsrCb | (m_nonDma ? kMsrExm : 0) | m_driveBusy);
    return static_cast<uint8_t>(m_msr | m_driveBusy);
}

uint8_t Upd765Fdc::headUnit() const noexcept
{
    return static_cast<uint8_t>(m_cmdBuf[1] & 0x07);
}

uint8_t Upd765Fdc::readData(uint64_t now)
{
    m_lastAccess = now;
    checkOverrun(now);
    if ((m_nonDma && m_phase == PhaseExecRead && now < m_execIntFrom)
        || (m_phase == PhaseResult && m_resultInt && now < m_resultIntFrom))
        return 0xFF;
    if (m_phase == PhaseExecRead) {
        m_transferDeadline = now + kOverrunTstates;
        const uint8_t b = m_dataBuf[m_dataPos++];
        if (m_dataPos >= m_tcStop)
            finishTransfer(true);
        else if (m_dataPos >= m_dataBuf.size())
            finishTransfer(false);
        return b;
    }
    if (m_phase == PhaseResult) {
        m_tcCanNormalize = false;
        m_resultInt = false;
        const uint8_t b = m_resBuf[m_resPos++];
        if (m_resPos >= m_resBuf.size()) {
            m_phase = PhaseCommand;
            m_msr = kMsrRqm;
        }
        return b;
    }
    return 0xFF;
}

void Upd765Fdc::writeData(uint8_t val, uint64_t now)
{
    m_lastAccess = now;
    checkOverrun(now);
    if (m_nonDma && (m_phase == PhaseExecWrite || m_phase == PhaseExecFormat) && now < m_execIntFrom)
        return;
    if (m_phase == PhaseExecWrite || m_phase == PhaseExecFormat) {
        m_transferDeadline = now + kOverrunTstates;
        m_dataBuf[m_dataPos++] = val;
        if (m_phase == PhaseExecWrite) {
            // Commit each sector as soon as it is complete
            if (std::ranges::find(m_sectorEnds, m_dataPos) != m_sectorEnds.end())
                commitWriteSector();
            if (m_dataPos >= m_tcStop) {
                finishTransfer(true);
                return;
            }
        }
        if (m_dataPos >= m_dataBuf.size()) {
            if (m_phase == PhaseExecFormat)
                commitFormat();
            else
                finishTransfer(false);
        }
        return;
    }

    if (m_phase != PhaseCommand)
        return;

    m_cmdBuf.push_back(val);
    m_msr = kMsrRqm | kMsrCb;
    if (m_cmdBuf.size() >= commandLength(m_cmdBuf[0] & 0x1F)) {
        executeCommand(m_cmdBuf[0] & 0x1F);
        m_cmdBuf.clear();
        // A transfer starts: its first byte comes once the sector is under the head
        m_transferTimed = m_phase == PhaseExecRead || m_phase == PhaseExecWrite;
        m_transferDeadline = now + kFirstByteTstates;
    }
}

void Upd765Fdc::terminalCount() noexcept
{
    if (m_phase == PhaseExecRead || m_phase == PhaseExecWrite) {
        // In the middle of a sector the byte already presented still moves
        if (m_dataPos == 0 || std::ranges::find(m_sectorEnds, m_dataPos) != m_sectorEnds.end())
            finishTransfer(true);
        else
            m_tcStop = m_dataPos + 1;
    } else if (m_phase == PhaseExecFormat) {
        commitFormat();
    } else if (m_phase == PhaseResult && m_tcCanNormalize) {
        // TC raised right after the last byte: the controller still sees it
        // before the sector ends, so the command terminates normally.
        m_resBuf[0] &= static_cast<uint8_t>(~kSt0AbnormalTermination);
        m_resBuf[1] &= static_cast<uint8_t>(~kSt1EndOfCylinder);
        m_tcCanNormalize = false;
    }
}

std::size_t Upd765Fdc::commandLength(uint8_t cmd) noexcept
{
    switch (cmd) {
    case 0x03: return 3; // SPECIFY
    case 0x04: return 2; // SENSE DRIVE STATUS
    case 0x07: return 2; // RECALIBRATE
    case 0x08: return 1; // SENSE INTERRUPT STATUS
    case 0x0A: return 2; // READ ID
    case 0x0D: return 6; // FORMAT TRACK
    case 0x0F: return 3; // SEEK
    case 0x02:           // READ TRACK
    case 0x05:           // WRITE DATA
    case 0x06:           // READ DATA
    case 0x09:           // WRITE DELETED DATA
    case 0x0C:           // READ DELETED DATA
    case 0x11:           // SCAN EQUAL
    case 0x19:           // SCAN LOW OR EQUAL
    case 0x1D:           // SCAN HIGH OR EQUAL
        return 9;
    default:   return 1;
    }
}

void Upd765Fdc::enterResultPhase() noexcept
{
    m_phase = PhaseResult;
    m_resPos = 0;
    m_msr = kMsrRqm | kMsrDio | kMsrCb;
    // The transfer commands interrupt at their result phase (not SENSE DRIVE STATUS, SENSE
    // INTERRUPT STATUS or an invalid command)
    switch (m_execCmd.empty() ? 0 : m_execCmd[0] & 0x1F) {
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0A: case 0x0C: case 0x0D: case 0x11: case 0x19: case 0x1D:
        m_resultInt = true;
        // the controller reads the sector's CRC (two bytes, about 64us) before it interrupts
        m_resultIntFrom = m_lastAccess + kResultIntTstates;
        break;
    default:
        m_resultInt = false;
        break;
    }
}

bool Upd765Fdc::interruptLine(uint64_t now) const noexcept
{
    switch (m_phase) {
    case PhaseExecRead:
        return m_nonDma && now >= m_execIntFrom && m_dataPos < m_dataBuf.size();
    case PhaseExecWrite:
    case PhaseExecFormat:
        return m_nonDma && now >= m_execIntFrom;
    case PhaseResult:
        return m_resultInt && now >= m_resultIntFrom;
    default:
        for (unsigned unit = 0; unit < 4; ++unit) {
            if ((m_pendingInts & (1u << unit)) && now >= m_seekIntFrom[unit])
                return true;
        }
        return false;
    }
}

void Upd765Fdc::setResult(uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    m_resBuf = {st0, st1, st2, c, h, r, n};
    enterResultPhase();
}

void Upd765Fdc::executeCommand(uint8_t cmd)
{
    m_execCmd = m_cmdBuf;
    m_resBuf.clear();
    m_tcCanNormalize = false;
    m_resultInt = false;
    m_tcStop = SIZE_MAX;
    // The commands naming a drive (all but SPECIFY, SENSE INTERRUPT STATUS and VERSION)
    if (cmd != 0x03 && cmd != 0x08 && cmd != 0x10 && m_cmdBuf.size() > 1)
        m_drives[(m_cmdBuf[1] & 0x03) & m_unitMask].noteAccess();
    switch (cmd) {
    case 0x03: // SPECIFY
        m_nonDma = m_execCmd.size() > 2 && (m_execCmd[2] & 1);
        m_phase = PhaseCommand;
        m_msr = kMsrRqm;
        break;
    case 0x04: { // SENSE DRIVE STATUS
        const uint8_t hu = headUnit();
        const auto &disk = m_drives[hu & m_unitMask];
        uint8_t st3 = hu;
        if (!disk.empty())
            st3 |= 0x20 | (disk.sideCount() > 1 ? 0x08 : 0x00); // ready, two-sided
        if (m_currentTrack[hu & m_unitMask] == 0)
            st3 |= 0x10;
        if (disk.isWriteProtected())
            st3 |= 0x40;
        m_resBuf = {st3};
        enterResultPhase();
        break;
    }
    case 0x07:   // RECALIBRATE
    case 0x0F: { // SEEK
        const uint8_t us = m_cmdBuf[1] & 0x03;
        m_currentTrack[us & m_unitMask] = (cmd == 0x07) ? 0 : m_cmdBuf[2];
        m_driveBusy |= static_cast<uint8_t>(1u << us);
        m_pendingInts |= static_cast<uint8_t>(1u << us);
        // Leave time for the CPU to enter its wait loop. In particular the original
        // CoBra 64K boot follows the last parameter with HALT, rather than polling.
        m_seekIntFrom[us] = m_lastAccess + kFirstByteIntTstates;
        m_phase = PhaseCommand;
        m_msr = kMsrRqm;
        break;
    }
    case 0x08: { // SENSE INTERRUPT STATUS
        if (m_pendingInts) {
            uint8_t us = 0;
            while (!(m_pendingInts & (1u << us)))
                ++us;
            m_pendingInts &= static_cast<uint8_t>(~(1u << us));
            m_driveBusy &= static_cast<uint8_t>(~(1u << us));
            m_resBuf = {static_cast<uint8_t>(kSt0SeekEnd | us), m_currentTrack[us & m_unitMask]};
        } else {
            m_resBuf = {kSt0InvalidCommand};
        }
        enterResultPhase();
        break;
    }
    case 0x0A: { // READ ID: returns the next sector ID passing under the head
        const uint8_t hu = headUnit();
        const uint8_t unit = hu & m_unitMask;
        const uint8_t hd = (hu >> 2) & 1;
        const auto &disk = m_drives[unit];
        auto onTrack = disk.trackSectors(m_currentTrack[unit], hd);
        const bool mfm = (m_cmdBuf[0] & 0x40) != 0;
        std::erase_if(onTrack, [mfm](const auto &sector) { return sector.get().mfm != mfm; });
        if (onTrack.empty()) {
            setResult(static_cast<uint8_t>(kSt0AbnormalTermination | (disk.empty() ? kSt0NotReady : 0) | hu),
                      kSt1MissingAddressMark, 0, m_currentTrack[unit], hd, 1, 2);
        } else {
            const DiskImage::Sector &sec = onTrack[m_readIdIndex[unit]++ % onTrack.size()];
            setResult(hu, 0, 0, sec.track, sec.side, sec.sectorId, sec.sizeCode);
        }
        break;
    }
    case 0x02:   // READ TRACK
    case 0x06:   // READ DATA
    case 0x0C:   // READ DELETED DATA
        startRead(cmd);
        break;
    case 0x05:   // WRITE DATA
    case 0x09:   // WRITE DELETED DATA
        startWrite(cmd);
        break;
    case 0x0D: { // FORMAT TRACK
        const uint8_t hu = headUnit();
        m_xferDrive = hu & m_unitMask;
        m_xferSide = (hu >> 2) & 1;
        m_xferTrack = m_currentTrack[m_xferDrive];
        if (m_drives[m_xferDrive].isWriteProtected()) {
            setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), kSt1NotWritable, 0, 0, 0, 0, m_cmdBuf[2]);
            break;
        }
        m_dataBuf.assign(static_cast<std::size_t>(m_cmdBuf[3]) * 4, 0);
        m_dataPos = 0;
        m_phase = PhaseExecFormat;
        m_execIntFrom = m_lastAccess + kFirstByteIntTstates;
        m_msr = kMsrRqm | kMsrExm | kMsrCb;
        if (m_dataBuf.empty())
            commitFormat();
        break;
    }
    default: // invalid or unsupported (SCAN) command
        if (commandLength(cmd) == 9)
            setResult(static_cast<uint8_t>(kSt0AbnormalTermination | headUnit()), kSt1NoData, 0,
                      m_cmdBuf[2], m_cmdBuf[3], m_cmdBuf[4], m_cmdBuf[5]);
        else {
            m_resBuf = {kSt0InvalidCommand};
            enterResultPhase();
        }
        break;
    }
}

void Upd765Fdc::startRead(uint8_t cmd)
{
    const uint8_t hu = headUnit();
    const bool mt = (m_cmdBuf[0] & 0x80) != 0;
    m_xferDrive = hu & m_unitMask;
    m_xferTrack = m_currentTrack[m_xferDrive];
    m_xferSide = (hu >> 2) & 1;
    m_xferC = m_cmdBuf[2];
    m_xferH = m_cmdBuf[3];
    m_xferN = m_cmdBuf[5];
    const uint8_t r = m_cmdBuf[4];
    const uint8_t eot = m_cmdBuf[6];
    auto &disk = m_drives[m_xferDrive];

    m_dataBuf.clear();
    m_sectorEnds.clear();
    m_sectorIds.clear();
    m_xferSt1 = 0;
    m_xferSt2 = 0;

    if (disk.empty()) {
        setResult(static_cast<uint8_t>(kSt0AbnormalTermination | kSt0NotReady | hu), 0, 0, m_xferC, m_xferH, r, m_xferN);
        return;
    }

    // A sector's ST2 bit 6 (as a dump stores it) says its data mark is a deleted one. The Control
    // Mark flag tells a mark the command doesn't look for: SK skips such sectors, without SK the
    // sector is read and the command ends after it.
    const bool wantDeleted = cmd == 0x0C;
    const bool skip = (m_cmdBuf[0] & 0x20) != 0;
    bool markMismatch = false;
    bool weakCopies = false;
    const auto addSector = [&](DiskImage::Sector &sec) {
        const bool deleted = (sec.st2 & 0x40) != 0;
        if (cmd != 0x02 && deleted != wantDeleted) {
            m_xferSt2 |= 0x40;
            if (skip)
                return;
            markMismatch = true;
        }
        // A weak sector stored as several copies: each read gets the next one
        const std::size_t size = std::size_t{128} << std::min<uint8_t>(sec.sizeCode, 7);
        auto begin = sec.data.begin();
        auto end = sec.data.end();
        if (sec.data.size() > size && sec.data.size() % size == 0) {
            begin += static_cast<std::ptrdiff_t>((sec.reads % (sec.data.size() / size)) * size);
            end = begin + static_cast<std::ptrdiff_t>(size);
            weakCopies = true;
        }
        ++sec.reads;
        m_dataBuf.insert(m_dataBuf.end(), begin, end);
        m_sectorEnds.push_back(m_dataBuf.size());
        m_sectorIds.push_back(sec.sectorId);
        m_xferSt1 |= sec.st1 & 0x25; // keep data CRC / no data / missing mark flags
        m_xferSt2 |= sec.st2 & 0x21;
    };

    m_readTrack = cmd == 0x02;
    if (m_readTrack) {
        // READ TRACK: EOT sectors in physical order from the index hole, whatever their ids. An id
        // that isn't the one the command expects (its R goes up with each sector) is No Data, the
        // transfer goes on (FUSE's upd_fdc). N says how many bytes of each sector go to the CPU.
        const std::size_t length = std::size_t{128} << std::min<uint8_t>(m_xferN, 7);
        uint8_t expectR = r;
        for (const DiskImage::Sector &sec : disk.trackSectors(m_xferTrack, m_xferSide)) {
            if (sec.mfm != ((m_cmdBuf[0] & 0x40) != 0))
                continue;
            if (m_sectorIds.size() >= eot)
                break;
            if (sec.track != m_xferC || sec.side != m_xferH || sec.sectorId != expectR)
                m_xferSt1 |= kSt1NoData;
            if (sec.track != m_xferC)
                m_xferSt2 |= 0x10; // Wrong Cylinder
            std::vector<uint8_t> bytes(sec.data.begin(), sec.data.begin() + static_cast<std::ptrdiff_t>(std::min(length, sec.data.size())));
            bytes.resize(length, 0x4E); // a short sector: the gap follows
            m_dataBuf.insert(m_dataBuf.end(), bytes.begin(), bytes.end());
            m_sectorEnds.push_back(m_dataBuf.size());
            m_sectorIds.push_back(expectR++);
            m_xferSt1 |= sec.st1 & 0x20;
            m_xferSt2 |= sec.st2 & 0x20;
        }
        if (m_dataBuf.empty()) {
            setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), kSt1MissingAddressMark, 0, m_xferC, m_xferH, r, m_xferN);
            return;
        }
    } else {
        uint8_t side = m_xferSide;
        uint8_t h = m_xferH;
        for (uint8_t id = r;; ++id) {
            const auto sec = disk.findSectorById(m_xferTrack, side, m_xferC, h, id, m_xferN);
            if (!sec || sec->get().mfm != ((m_cmdBuf[0] & 0x40) != 0))
                break;
            addSector(*sec);
            if (markMismatch)
                break;
            if (id == eot) {
                if (!mt || side == 1)
                    break;
                side = 1; // multi-track: continue on the second side
                h ^= 1;
                id = 0;
            }
        }
    }

    if (m_dataBuf.empty()) {
        setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), kSt1NoData, 0, m_xferC, m_xferH, r, m_xferN);
        return;
    }

    // Speedlock's weak sector (FUSE's hack): it reads track 0 sector 2 again and again, and wants
    // the data to differ, as its weak bits do on a real disk
    if (!m_readTrack && !weakCopies) {
        const uint32_t sector = (m_xferH & 0x01) + (uint32_t{m_xferC} << 1) + (uint32_t{r} << 8);
        if (r == eot && sector == 0x200) {
            if (sector == m_lastSectorRead) {
                ++m_speedlock;
            } else {
                m_speedlock = 0;
                m_lastSectorRead = sector;
            }
        } else {
            m_lastSectorRead = 0;
            m_speedlock = 0;
        }
        if (m_speedlock > 0) {
            for (std::size_t offset = 1; offset <= m_dataBuf.size(); ++offset) {
                uint8_t &byte = m_dataBuf[offset - 1];
                if (offset < 64 && byte != 0xE5)
                    m_speedlock = 2; // W.E.C. Le Mans style
                else if ((m_speedlock > 1 || offset < 64) && offset % 29 == 0)
                    byte ^= static_cast<uint8_t>(offset);
            }
        }
    }
    m_dataPos = 0;
    m_phase = PhaseExecRead;
    m_execIntFrom = m_lastAccess + kFirstByteIntTstates;
    m_msr = kMsrRqm | kMsrDio | kMsrExm | kMsrCb;
}

void Upd765Fdc::startWrite(uint8_t /*cmd*/)
{
    m_readTrack = false;
    const uint8_t hu = headUnit();
    m_xferDrive = hu & m_unitMask;
    m_xferTrack = m_currentTrack[m_xferDrive];
    m_xferSide = (hu >> 2) & 1;
    m_xferC = m_cmdBuf[2];
    m_xferH = m_cmdBuf[3];
    m_xferN = m_cmdBuf[5];
    const uint8_t r = m_cmdBuf[4];
    const uint8_t eot = m_cmdBuf[6];
    auto &disk = m_drives[m_xferDrive];

    m_dataBuf.clear();
    m_sectorEnds.clear();
    m_sectorIds.clear();
    m_xferSt1 = 0;
    m_xferSt2 = 0;

    if (disk.empty()) {
        setResult(static_cast<uint8_t>(kSt0AbnormalTermination | kSt0NotReady | hu), 0, 0, m_xferC, m_xferH, r, m_xferN);
        return;
    }
    if (disk.isWriteProtected()) {
        setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), kSt1NotWritable, 0, m_xferC, m_xferH, r, m_xferN);
        return;
    }
    std::size_t total = 0;
    for (uint8_t id = r;; ++id) {
        const auto sec = disk.findSectorById(m_xferTrack, m_xferSide, m_xferC, m_xferH, id, m_xferN);
        if (!sec || sec->get().mfm != ((m_cmdBuf[0] & 0x40) != 0))
            break;
        total += sec->get().data.size();
        m_sectorEnds.push_back(total);
        m_sectorIds.push_back(id);
        if (id == eot)
            break;
    }
    if (total == 0) {
        setResult(static_cast<uint8_t>(kSt0AbnormalTermination | hu), kSt1NoData, 0, m_xferC, m_xferH, r, m_xferN);
        return;
    }
    m_dataBuf.assign(total, 0);
    m_dataPos = 0;
    m_phase = PhaseExecWrite;
    m_execIntFrom = m_lastAccess + kFirstByteIntTstates;
    m_msr = kMsrRqm | kMsrExm | kMsrCb; // DIO = 0: CPU -> FDC
}

void Upd765Fdc::commitWriteSector()
{
    const auto it = std::ranges::find(m_sectorEnds, m_dataPos);
    if (it == m_sectorEnds.end())
        return;
    const std::size_t idx = static_cast<std::size_t>(it - m_sectorEnds.begin());
    const std::size_t begin = idx == 0 ? 0 : m_sectorEnds[idx - 1];
    if (auto sec = m_drives[m_xferDrive].findSectorById(m_xferTrack, m_xferSide, m_xferC, m_xferH, m_sectorIds[idx], m_xferN)) {
        std::copy(m_dataBuf.begin() + static_cast<std::ptrdiff_t>(begin),
                  m_dataBuf.begin() + static_cast<std::ptrdiff_t>(*it), sec->get().data.begin());
        m_drives[m_xferDrive].setModified(true);
    }
}

void Upd765Fdc::finishTransfer(bool byTerminalCount)
{
    if (m_readTrack) {
        // READ TRACK ends after its EOT sectors: errors don't abort it
        std::size_t done = 0;
        while (done < m_sectorEnds.size() && m_dataPos >= m_sectorEnds[done])
            ++done;
        const uint8_t hu = static_cast<uint8_t>((m_xferSide << 2) | m_xferDrive);
        const uint8_t firstR = m_sectorIds.empty() ? 1 : m_sectorIds.front();
        setResult(hu, m_xferSt1, m_xferSt2, m_xferC, m_xferH, static_cast<uint8_t>(firstR + done), m_xferN);
        return;
    }

    // Last sector touched by the transfer: the one the data pointer is in
    std::size_t idx = 0;
    while (idx + 1 < m_sectorEnds.size() && m_dataPos > m_sectorEnds[idx])
        ++idx;
    const bool reachedEnd = m_dataPos >= m_dataBuf.size();
    const uint8_t eot = m_execCmd.size() > 6 ? m_execCmd[6] : m_sectorIds.back();
    const uint8_t lastId = m_sectorIds[idx];
    const uint8_t hu = static_cast<uint8_t>((m_xferSide << 2) | m_xferDrive);

    uint8_t c = m_xferC;
    uint8_t h = m_xferH;
    uint8_t r = static_cast<uint8_t>(lastId + 1);
    if (lastId == eot) {
        c = static_cast<uint8_t>(c + 1);
        r = 1;
    }

    uint8_t st0 = hu;
    uint8_t st1 = m_xferSt1;
    if (reachedEnd && !byTerminalCount) {
        // Without TC the controller runs past EOT: abnormal termination, End of Cylinder
        st0 |= kSt0AbnormalTermination;
        st1 |= kSt1EndOfCylinder;
    } else if (st1 & (kSt1NoData | kSt1MissingAddressMark | 0x20)) {
        st0 |= kSt0AbnormalTermination;
    }
    if (reachedEnd && m_sectorIds.back() != eot) {
        // The transfer stopped early on a missing sector
        st0 |= kSt0AbnormalTermination;
        st1 = static_cast<uint8_t>((st1 & ~kSt1EndOfCylinder) | kSt1NoData);
        c = m_xferC;
        r = static_cast<uint8_t>(m_sectorIds.back() + 1);
    }
    setResult(st0, st1, m_xferSt2, c, h, r, m_xferN);
    m_tcCanNormalize = reachedEnd && !byTerminalCount && (m_sectorIds.back() == eot) && !(m_xferSt1 & 0x25);
}

void Upd765Fdc::commitFormat()
{
    const uint8_t n = m_execCmd[2];
    const uint8_t filler = m_execCmd[5];
    std::vector<DiskImage::Sector> sectors;
    for (std::size_t i = 0; i + 4 <= m_dataPos; i += 4) {
        sectors.push_back(DiskImage::Sector{
            .track = m_dataBuf[i],
            .side = m_dataBuf[i + 1],
            .sectorId = m_dataBuf[i + 2],
            .sizeCode = m_dataBuf[i + 3],
            .mfm = (m_execCmd[0] & 0x40) != 0,
            .data = std::vector<uint8_t>(std::size_t{128} << std::min<uint8_t>(m_dataBuf[i + 3], 7), filler)
        });
    }
    const uint8_t hu = static_cast<uint8_t>((m_xferSide << 2) | m_xferDrive);
    const DiskImage::Sector last = sectors.empty() ? DiskImage::Sector{} : sectors.back();
    m_drives[m_xferDrive].replaceTrack(m_xferTrack, m_xferSide, std::move(sectors));
    setResult(hu, 0, 0, last.track, last.side, last.sectorId, n);
}

// ── HC Interface 1 FDC ────────────────────────────────────────────────────

std::string_view HcIf1FdcPeripheral::id() const noexcept
{
    return "hc_if1_fdc";
}

std::string_view HcIf1FdcPeripheral::name() const noexcept
{
    return "HC Interface 1 uPD765 FDC";
}

void HcIf1FdcPeripheral::onAttach(MachineContext &ctx)
{
    m_romBank = ctx.mmu.registerBank("hc_if1_fdc_rom", 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("hc_if1_fdc_ram", 1024, true, true);
    if (ctx.romResolver)
        ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("hc-if1.rom", 16 * 1024));
    m_fdc.setUnitMask(1);

    ctx.romcs.registerProvider(
        std::string(id()),
        40,
        [this](Mmu &mmu) {
            mmu.mapOverlay1k(0x0000, m_romBank, 0, 16, false);
            // 1KB RAM decoded when A13 and A11 are set
            for (uint16_t addr : {0x2800, 0x2C00, 0x3800, 0x3C00})
                mmu.mapOverlay1k(addr, m_ramBank, 0, 1, true);
        });

    // Pages in like the Interface 1: M1 at 0x0008 or 0x1708, out after the fetch at 0x0700
    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x0008 || pc == 0x1708)
            ctx.romcs.setActive(id(), true);
    }, std::string(id()));
    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if (pc == 0x0700)
            ctx.romcs.setActive(id(), false);
    }, std::string(id()));

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ctl",
        0x00FD,
        0x0005,
        [this](uint16_t) -> uint8_t { return m_motorOn ? 0xFE : 0xFF; },
        [this, &ctx](uint16_t, uint8_t val) { writeControl(ctx, val); });

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_msr",
        0x00FF,
        0x0085,
        [this, &ctx](uint16_t) -> uint8_t { return m_fdc.readMainStatus(ctx.clock.totalTstates()); },
        nullptr);

    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_data",
        0x00FF,
        0x0087,
        [this, &ctx](uint16_t) -> uint8_t { return m_fdc.readData(ctx.clock.totalTstates()); },
        [this, &ctx](uint16_t, uint8_t val) { m_fdc.writeData(val, ctx.clock.totalTstates()); });
}

void HcIf1FdcPeripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    if (m_motorOffEvent)
        ctx.scheduler.cancel(m_motorOffEvent);
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(std::string(id()) + "_ctl");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_msr");
    ctx.ioBus.unregisterOwner(std::string(id()) + "_data");
}

void HcIf1FdcPeripheral::reset(MachineContext &ctx, ResetKind)
{
    m_fdc.reset();
    m_motorOn = 0;
    if (m_motorOffEvent)
        ctx.scheduler.cancel(m_motorOffEvent);
    ctx.romcs.setActive(id(), false);
    m_motorOffEvent = 0;
}

void HcIf1FdcPeripheral::writeControl(MachineContext &ctx, uint8_t val)
{
    if (!(val & 0x10))
        m_fdc.reset();
    if (val & 0x01)
        m_fdc.terminalCount();

    if (m_motorOffEvent) {
        ctx.scheduler.cancel(m_motorOffEvent);
        m_motorOffEvent = 0;
    }
    if (val & 0x08) {
        m_motorOn = static_cast<uint8_t>(((val & 0x02) ? 1 : 0) | ((val & 0x04) ? 2 : 0));
    } else if (m_motorOn) {
        // A 555 monostable keeps the drives selected for ~1.7 seconds
        const uint64_t delay = static_cast<uint64_t>(ctx.clock.timings().baseClockHz) * 17 / 10;
        m_motorOffEvent = ctx.scheduler.scheduleIn(ctx.clock.totalTstates(), delay, [this](uint64_t) {
            m_motorOn = 0;
            m_motorOffEvent = 0;
        });
    }
}

std::size_t HcIf1FdcPeripheral::diskDriveCount() const noexcept
{
    return kDriveCount;
}

OptionalRef<DiskImage> HcIf1FdcPeripheral::diskDrive(std::size_t idx) noexcept
{
    if (idx < kDriveCount)
        return m_fdc.drive(idx);
    return std::nullopt;
}

Upd765Fdc &HcIf1FdcPeripheral::fdc() noexcept
{
    return m_fdc;
}

const Upd765Fdc &HcIf1FdcPeripheral::fdc() const noexcept
{
    return m_fdc;
}

BankId HcIf1FdcPeripheral::romBankId() const noexcept
{
    return m_romBank;
}

BankId HcIf1FdcPeripheral::ramBankId() const noexcept
{
    return m_ramBank;
}

} // namespace zxkit
