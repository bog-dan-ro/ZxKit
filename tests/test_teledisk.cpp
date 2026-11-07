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

#include "RomTestSupport.h"

#include <algorithm>
#include <cassert>
#include <print>

using namespace zxkit;

namespace {
uint16_t crc(std::span<const uint8_t> data)
{
    uint16_t result = 0;
    for (uint8_t b : data) {
        result ^= uint16_t(b) << 8;
        for (int bit = 0; bit < 8; ++bit)
            result = uint16_t((result << 1) ^ ((result & 0x8000) ? 0xA097 : 0));
    }
    return result;
}
void word(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(uint8_t(value));
    data.push_back(uint8_t(value >> 8));
}
std::vector<uint8_t> image(uint8_t flags, std::span<const uint8_t> encoded, bool fm = false)
{
    std::vector<uint8_t> bytes{ 'T', 'D', 0, 0, 0x15, 0, 3, 0, 0, 1 };
    bytes.reserve(25 + encoded.size());
    word(bytes, crc(bytes));
    const std::array track{ uint8_t{ 1 }, uint8_t{ 2 }, uint8_t(fm ? 0x80 : 0) };
    bytes.insert(bytes.end(), track.begin(), track.end());
    bytes.push_back(uint8_t(crc(track)));
    // Deliberately different physical and ID addresses.
    const std::vector<uint8_t> sector(128, 0xE5);
    bytes.insert(bytes.end(), { 3, 1, 7, 0, flags, uint8_t(crc(sector)) });
    if (!(flags & 0x30)) {
        word(bytes, uint16_t(encoded.size()));
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    bytes.push_back(0xFF);
    return bytes;
}
void reject(std::span<const uint8_t> bytes)
{
    DiskImage disk;
    bool failed = false;
    try {
        disk.loadFromBytes(bytes, ".td0");
    } catch (const FormatException &) {
        failed = true;
    }
    assert(failed && disk.empty());
}
void testEncodingsAndFlags()
{
    const std::array<uint8_t, 5> repeated{ 1, 64, 0, 0xE5, 0xE5 };
    const std::array<uint8_t, 7> patterns{ 2, 0, 2, 0xE5, 0xE5, 1, 63 };
    std::vector<uint8_t> pattern(patterns.begin(), patterns.end());
    pattern.insert(pattern.end(), { 0xE5, 0xE5 });
    std::vector<uint8_t> literal(129, 0xE5);
    literal[0] = 0;
    for (const auto &encoded :
         { literal, std::vector<uint8_t>(repeated.begin(), repeated.end()), pattern }) {
        DiskImage disk;
        disk.loadFromBytes(image(6, encoded, true), ".td0");
        const auto &sector = disk.findSector(2, 0, 7)->get();
        assert(disk.trackCount() == 3 && disk.sideCount() == 1);
        assert(sector.track == 3 && sector.side == 1 && !sector.mfm);
        assert(sector.st1 == 0x20 && sector.st2 == 0x60);
        assert(sector.data == std::vector<uint8_t>(128, 0xE5));
        assert(!disk.isModified());
    }
    DiskImage missing;
    missing.loadFromBytes(image(0x20, {}));
    assert(missing.findSector(2, 0, 7)->get().st2 == 1);
    DiskImage skipped;
    skipped.loadFromBytes(image(0x10, {}));
    assert(skipped.findSector(2, 0, 7)->get().data.size() == 128);
    DiskImage bogus;
    bogus.loadFromBytes(image(0x40, literal));
    assert(bogus.empty());

    const auto good = image(0, repeated);
    for (std::size_t n = 0; n < good.size(); ++n)
        reject(std::span(good).first(n));
    auto bad = good;
    bad[10] ^= 1;
    reject(bad); // main header CRC
    bad = good;
    bad[15] ^= 1;
    reject(bad); // track CRC
    bad = good;
    bad[21] ^= 1;
    reject(bad); // decoded sector CRC
    bad = good;
    bad[24] = 3;
    reject(bad); // unknown encoding
    bad = good;
    bad[25] = 65;
    reject(bad); // output overrun
    bad = good;
    bad[25] = 63;
    reject(bad); // incomplete sector
}
void testAdvanced(const std::filesystem::path &fixtures)
{
    DiskImage normal, advanced;
    const auto bytes = test::readFile(fixtures / "teledisk/advanced.td0");
    normal.loadFromBytes(test::readFile(fixtures / "teledisk/normal.td0"));
    advanced.loadFromBytes(bytes);
    for (uint8_t track = 0; track < 2; ++track) {
        for (uint8_t id = 1; id <= 32; ++id) {
            const auto &data = advanced.findSector(track, 0, id)->get().data;
            assert(data == normal.findSector(track, 0, id)->get().data);
            assert(data.size() == 1024);
            for (std::size_t i = 0; i < data.size(); ++i) {
                const uint8_t expected = track == 0 && id == 1 && i < 256
                        ? 0
                        : uint8_t(i * 37 + id * 11 + track * 71);
                assert(data[i] == expected);
            }
        }
    }
    // A second instance must not inherit adaptive trees/dictionary state.
    DiskImage second;
    second.loadFromBytes(bytes);
    assert(second.findSector(1, 0, 32)->get().data == advanced.findSector(1, 0, 32)->get().data);
    reject(std::span(bytes).first(bytes.size() / 2));
    assert(classifyMediaFile("archive.TD0") == MediaClass::Disk);
    assert(classifyMediaFile("unknown", bytes) == MediaClass::Disk);
    Emulator emulator({ .machineId = "cobra1_80k" });
    emulator.loadMedia(MediaFile::fromBytes(bytes, ".TD0"));
    assert(emulator.diskDrive(0)->get().trackCount() == 2);
}
void testOriginalDevil(const std::filesystem::path &fixtures)
{
    DiskImage disk;
    disk.loadFromBytes(test::readFile(fixtures / "cobra/UTILS1.TD0"));
    assert(disk.trackCount() == 82 && disk.sideCount() == 2);
    for (uint8_t track = 0; track < 82; ++track) {
        for (uint8_t side = 0; side < 2; ++side) {
            const auto sectors = disk.trackSectors(track, side);
            assert(sectors.size() == 18);
            for (const DiskImage::Sector &sector : sectors)
                assert(sector.data.size() == 256 && sector.mfm);
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path fixtures = argc > 1 ? argv[1] : ZXKIT_TEST_FIXTURES_DIR;
    testEncodingsAndFlags();
    testAdvanced(fixtures);
    testOriginalDevil(fixtures);
    std::println("test_teledisk: all tests passed");
}
