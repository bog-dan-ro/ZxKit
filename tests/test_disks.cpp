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

// Disk tests: the +3's uPD765 (through its ports), the WD1793, the Beta 128 and TR-DOS images.

#include <zxkit/zxkit.h>

#include <cassert>
#include <cstdlib>
#include <print>
#include <string_view>
#include <vector>

using namespace zxkit;

namespace {

// ── The +3's uPD765, through its ports ────────────────────────────────────

class Plus3Fdc {
public:
    explicit Plus3Fdc(Emulator &emulator)
        : m_emulator(emulator)
    {}

    uint8_t status() { return m_emulator.ioBus().read(0x2FFD); }
    void command(std::initializer_list<uint8_t> bytes)
    {
        for (uint8_t b : bytes) {
            assert(status() & 0x80); // RQM
            m_emulator.ioBus().write(0x3FFD, b);
        }
    }
    void ready() {
        for (unsigned i = 0; !(status() & 0x80) && i < 200000; ++i) wait(16);
        assert(status() & 0x80);
    }
    uint8_t read() { ready(); return m_emulator.ioBus().read(0x3FFD); }
    std::vector<uint8_t> readBytes(std::size_t n)
    {
        std::vector<uint8_t> out;
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(read());
        return out;
    }
    std::vector<uint8_t> result()
    {
        // After the last data byte the controller reads the CRC: busy, not ready
        for (int i = 0; i < 200000 && (status() & 0x90) == 0x10; ++i)
            wait(16);
        std::vector<uint8_t> out;
        while ((status() & 0xD0) == 0xD0) // RQM, DIO, busy: a result byte
            out.push_back(read());
        return out;
    }
    void wait(uint32_t tstates) { m_emulator.clock().advance(tstates); }

private:
    Emulator &m_emulator;
};

DiskImage &plus3Disk(Emulator &emulator)
{
    auto disk = emulator.diskDrive(0);
    assert(disk);
    disk->get().formatBlank(40, 1, 9, 512, 1);
    return disk->get();
}

void testPlus3Fdc()
{
    Emulator emulator({.machineId = "plus3"});
    assert(emulator.diskDriveCount() == 2);
    assert(!Emulator({.machineId = "plus2a"}).diskDrive(0));

    DiskImage &disk = plus3Disk(emulator);
    for (std::size_t i = 0; i < 512; ++i)
        disk.findSector(0, 0, 1)->get().data[i] = static_cast<uint8_t>(i);
    Plus3Fdc fdc(emulator);

    // READ DATA: the sector, then (the +3 has no TC) End of Cylinder
    fdc.command({0x46, 0x00, 0, 0, 1, 2, 1, 0x2A, 0xFF});
    assert(fdc.status() & 0x20); // execution
    const auto data = fdc.readBytes(512);
    assert(data[0] == 0 && data[511] == 0xFF);
    auto res = fdc.result();
    assert(res.size() == 7 && res[0] == 0x40 && res[1] == 0x80);

    // Overrun: a CPU that stops reading ends the transfer
    fdc.command({0x46, 0x00, 0, 0, 1, 2, 1, 0x2A, 0xFF});
    fdc.readBytes(10);
    fdc.wait(2000);
    res = fdc.result();
    assert(res.size() == 7 && res[0] == 0x40 && (res[1] & 0x10));
    assert(!(fdc.status() & 0x20));

    // READ DELETED DATA: a deleted sector reads without Control Mark, a normal one with it
    disk.findSector(0, 0, 2)->get().st2 = 0x40;
    fdc.command({0x4C, 0x00, 0, 0, 2, 2, 2, 0x2A, 0xFF});
    fdc.readBytes(512);
    res = fdc.result();
    assert(!(res[2] & 0x40));
    fdc.command({0x4C, 0x00, 0, 0, 3, 2, 3, 0x2A, 0xFF});
    fdc.readBytes(512);
    res = fdc.result();
    assert(res[2] & 0x40);
    // ... and READ DATA on the deleted one sees the Control Mark
    fdc.command({0x46, 0x00, 0, 0, 2, 2, 2, 0x2A, 0xFF});
    fdc.readBytes(512);
    res = fdc.result();
    assert(res[2] & 0x40);

    // READ TRACK: from the index whatever the ids, No Data for an unexpected one, R counts the sectors
    std::vector<DiskImage::Sector> odd;
    for (uint8_t i = 0; i < 4; ++i)
        odd.push_back({.track = 7, .side = 0, .sectorId = static_cast<uint8_t>(0xB1 + i), .sizeCode = 2,
                       .data = std::vector<uint8_t>(512, static_cast<uint8_t>(0x10 + i))});
    disk.replaceTrack(7, 0, odd);
    fdc.command({0x0F, 0x00, 7});
    fdc.wait(700000);
    fdc.command({0x08});
    fdc.result();
    fdc.command({0x42, 0x00, 7, 0, 1, 2, 1, 0x2A, 0xFF});
    assert(fdc.readBytes(512)[0] == 0x10);
    res = fdc.result();
    assert(res[0] == 0x00 && (res[1] & 0x04) && res[5] == 2);

    // A weak sector stored as several copies: each read gets the next one
    auto &weak = disk.findSector(0, 0, 4)->get();
    weak.data.assign(3 * 512, 0);
    for (std::size_t i = 0; i < 3; ++i)
        std::fill(weak.data.begin() + static_cast<std::ptrdiff_t>(i * 512),
                  weak.data.begin() + static_cast<std::ptrdiff_t>((i + 1) * 512), static_cast<uint8_t>(0xA0 + i));
    fdc.command({0x0F, 0x00, 0});
    fdc.wait(700000);
    fdc.command({0x08});
    fdc.result();
    for (uint8_t copy = 0; copy < 4; ++copy) {
        fdc.command({0x46, 0x00, 0, 0, 4, 2, 4, 0x2A, 0xFF});
        const auto bytes = fdc.readBytes(512);
        fdc.result();
        assert(bytes[0] == 0xA0 + copy % 3 && bytes[511] == 0xA0 + copy % 3);
    }
}

// ── The WD1793 ────────────────────────────────────────────────────────────

void testWd1793()
{
    Wd1793 fdc;
    DiskImage &disk = fdc.drive(0);
    disk.formatBlank(80, 2, 16, 256, 1);
    for (uint8_t i = 0; i < 255; ++i)
        disk.findSector(5, 1, 3)->get().data[i] = i;
    fdc.selectDrive(0);
    fdc.selectSide(1);
    uint64_t now = 1'000'000;
    const auto idle = [&] {
        for (unsigned i = 0; fdc.busy() && i < 200000; ++i) fdc.update(now += 32);
        assert(!fdc.busy());
    };
    const auto ready = [&] {
        for (unsigned i = 0; !fdc.drq() && fdc.busy() && i < 200000; ++i) fdc.update(now += 16);
        assert(fdc.drq());
    };
    const auto read = [&] { ready(); return fdc.readData(); };
    const auto write = [&](uint8_t value) { ready(); fdc.writeData(value); };

    // RESTORE, then SEEK: type I status, INTRQ
    fdc.writeCommand(0x08, now);
    assert(fdc.intrq());
    assert(fdc.readStatus(now) & 0x24); // track 0, head loaded
    assert(!fdc.intrq());               // reading the status acknowledges it
    fdc.writeData(5);
    fdc.writeCommand(0x18, now);
    assert(fdc.busy() && !fdc.intrq() && fdc.readTrack() == 0);
    fdc.update(now + 5 * 21000 - 1);
    assert(fdc.busy());
    now += 5 * 21000;
    fdc.update(now);
    assert(fdc.readTrack() == 5 && !(fdc.readStatus(now) & 0x04));

    // READ SECTOR
    fdc.writeSector(3);
    fdc.writeCommand(0x80, now);
    assert(!fdc.drq() && fdc.busy());
    std::vector<uint8_t> data;
    for (int i = 0; i < 256; ++i)
        data.push_back(read());
    assert(data[0] == 0 && data[254] == 254);
    assert(fdc.intrq() && !fdc.busy() && (fdc.readStatus(now) & 0x7F) == 0);

    // A missing sector: Record Not Found
    fdc.writeSector(17);
    fdc.writeCommand(0x80, now);
    assert(fdc.busy() && !fdc.intrq());
    idle();
    assert(fdc.intrq() && (fdc.readStatus(now) & 0x10));

    // Missing a presented byte must raise Lost Data; a busy controller ignores another read.
    fdc.writeSector(3);
    fdc.writeCommand(0x80, now);
    ready();
    assert(fdc.busy());
    fdc.update(now += 112);
    assert(fdc.intrq() && !fdc.busy() && (fdc.readStatus(now) & 4));

    // WRITE SECTOR
    fdc.writeSector(4);
    fdc.writeCommand(0xA0, now);
    for (int i = 0; i < 256; ++i)
        write(0x5A);
    assert(fdc.intrq() && disk.findSector(5, 1, 4)->get().data[100] == 0x5A);

    // READ ADDRESS: the next id, and its track goes to the sector register
    fdc.writeCommand(0xC0, now);
    const uint8_t c = read();
    read();
    const uint8_t r = read();
    assert(read() == 1); // 256 bytes
    read();
    read();
    assert(c == 5 && r >= 1 && r <= 16 && fdc.readSector() == 5 && fdc.intrq());

    // FORCE INTERRUPT ends a transfer
    fdc.writeSector(1);
    fdc.writeCommand(0x80, now);
    fdc.writeCommand(0xD0, now);
    assert(!fdc.busy() && !fdc.drq());

    // WRITE TRACK without data ends at the index pulse with Lost Data (TR-DOS relies on it)
    fdc.writeCommand(0xF0, now);
    assert(fdc.busy() && !fdc.intrq());
    now += 700'000;
    fdc.update(now);
    assert(fdc.intrq() && (fdc.readStatus(now) & 0x04));

    // WRITE TRACK with a TR-DOS style format: the sectors come back from the marks
    fdc.writeCommand(0xF0, now);
    std::vector<uint8_t> track(80, 0x4E);
    for (uint8_t id = 1; id <= 16; ++id) {
        track.insert(track.end(), 12, 0x00);
        track.insert(track.end(), 3, 0xF5);
        track.insert(track.end(), {0xFE, 5, 0, id, 1, 0xF7});
        track.insert(track.end(), 22, 0x4E);
        track.insert(track.end(), 12, 0x00);
        track.insert(track.end(), 3, 0xF5);
        track.push_back(0xFB);
        track.insert(track.end(), 256, id);
        track.push_back(0xF7);
        track.insert(track.end(), 54, 0x4E);
    }
    track.resize(6250, 0x4E);
    for (uint8_t b : track)
        write(b);
    assert(fdc.intrq());
    const auto sectors = disk.trackSectors(5, 1);
    assert(sectors.size() == 16 && sectors[15].get().sectorId == 16 && sectors[15].get().data[0] == 16);
}

// ── The Beta 128 ──────────────────────────────────────────────────────────

void testBeta128Paging()
{
    Emulator emulator({.machineId = "48k"});
    emulator.attachPeripheral(std::make_unique<KempstonJoystickPeripheral>());
    emulator.attachPeripheral(std::make_unique<Beta128Peripheral>());
    auto &beta = static_cast<Beta128Peripheral &>(emulator.findPeripheral("beta128")->get());
    auto &kempston = static_cast<KempstonJoystickPeripheral &>(emulator.findPeripheral("kempston")->get());
    assert(emulator.diskDriveCount() == 4);
    kempston.setButtons(JoystickButtons::Fire1);

    // Paged out: port 0x1F is the Kempston's
    assert(!beta.isPaged());
    assert(emulator.ioBus().read(0x001F) == 0x10);

    // An instruction fetched at 0x3Dxx pages TR-DOS in (the blank ROM runs NOPs)
    Z80Registers regs = emulator.registers();
    regs.PC = 0x3D00;
    emulator.setRegisters(regs);
    emulator.stepInstruction();
    assert(beta.isPaged());
    assert(emulator.ioBus().read(0x00FF) == 0x00);       // the system port: no INTRQ, no DRQ
    assert((emulator.ioBus().read(0x001F) & 0x80) != 0); // the status: no disk, not ready

    // ... and one above the ROM pages it out
    regs = emulator.registers();
    regs.PC = 0x8000;
    emulator.setRegisters(regs);
    emulator.stepInstruction();
    assert(!beta.isPaged());
}

void testPentagonBeta()
{
    // The Pentagon has four drives, and starts in the 128 ROM, as in FUSE
    Emulator emulator({.machineId = "pentagon"});
    assert(emulator.diskDriveCount() == 4);
    auto &pentagon = static_cast<PentagonMachine &>(emulator.machine());
    assert(!pentagon.beta().isPaged());
    // TR-DOS pages in on a jump to 0x3D00 from any ROM (here the blank 128 ROM)
    Z80Registers regs = emulator.registers();
    regs.PC = 0x3D00;
    emulator.setRegisters(regs);
    emulator.stepInstruction();
    assert(pentagon.beta().isPaged());
}

// ── TR-DOS images ─────────────────────────────────────────────────────────

void testTrDosImages()
{
    // An SCL with a BASIC program (1 sector) and a code block (2 sectors)
    std::vector<uint8_t> scl{'S', 'I', 'N', 'C', 'L', 'A', 'I', 'R', 2};
    const auto entry = [&scl](std::string_view name, char type, uint8_t sectors) {
        for (char c : name)
            scl.push_back(static_cast<uint8_t>(c));
        scl.push_back(static_cast<uint8_t>(type));
        scl.insert(scl.end(), {0, 0, 0, 0, sectors});
    };
    entry("game    ", 'B', 1);
    entry("code    ", 'C', 2);
    for (uint8_t s = 0; s < 3; ++s)
        scl.insert(scl.end(), 256, static_cast<uint8_t>(0x40 + s));
    scl.insert(scl.end(), 4, 0); // the checksum

    DiskImage disk;
    disk.loadFromBytes(scl, ".scl");
    const auto &catalog = disk.findSector(0, 0, 1)->get().data;
    assert(std::string_view(reinterpret_cast<const char *>(catalog.data()), 8) == "game    ");
    assert(catalog[14] == 0 && catalog[15] == 1);             // track 1, sector 0
    assert(catalog[16 + 14] == 1 && catalog[16 + 15] == 1);   // the next file follows
    const auto &spec = disk.findSector(0, 0, 9)->get().data;
    assert(spec[0xE7] == 0x10 && spec[0xE3] == 0x16 && spec[0xE4] == 2);
    assert(spec[0xE1] == 3 && spec[0xE2] == 1);               // the first free sector
    assert((spec[0xE5] | spec[0xE6] << 8) == 2560 - 16 - 3);
    assert(disk.findSector(0, 1, 2)->get().data[0] == 0x41);  // logical track 1 is cylinder 0, side 1

    // The boot loader runs the first BASIC program, once
    assert(insertTrDosBootLoader(disk));
    assert(!insertTrDosBootLoader(disk));
    const auto &updated = disk.findSector(0, 0, 9)->get().data;
    assert(updated[0xE4] == 3 && updated[0xE1] == 4);
    const auto &bootEntry = disk.findSector(0, 0, 1)->get().data;
    assert(std::string_view(reinterpret_cast<const char *>(bootEntry.data() + 32), 9) == "boot    B");
    const auto &program = disk.findSector(0, 1, 4)->get().data;
    assert(std::string_view(reinterpret_cast<const char *>(program.data() + 22), 8) == "game    ");

    // A TRD's geometry comes from its disk type: 40 tracks, single sided
    std::vector<uint8_t> trd(40 * 16 * 256, 0);
    trd[8 * 256 + 0xE3] = 0x19;
    trd[8 * 256 + 0xE7] = 0x10;
    trd[17 * 256] = 0x77; // logical track 1, sector 1: cylinder 1 on a single sided disk
    DiskImage single;
    single.loadFromBytes(trd, ".trd");
    assert(single.trackCount() == 40 && single.sideCount() == 1);
    assert(single.findSector(1, 0, 2)->get().data[0] == 0x77);
}

} // namespace

int main()
{
    testPlus3Fdc();
    testWd1793();
    testBeta128Paging();
    testPentagonBeta();
    testTrDosImages();
    std::println("test_disks: all tests passed");
    return EXIT_SUCCESS;
}
