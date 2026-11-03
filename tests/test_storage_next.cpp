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

#include <zxkit/zxkit.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>

#include <zlib.h>

using namespace zxkit;

// Unlike CHECK(), always evaluated: these checks drive the hardware under test
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::println(stderr, "{}:{}: CHECK({}) failed", __FILE__, __LINE__, #cond); \
            std::abort(); \
        } \
    } while (false)

namespace {

std::filesystem::path tempPath(std::string_view name)
{
    return std::filesystem::temp_directory_path() / std::format("zxkit_test_{}_{}", ::getpid(), name);
}

void writeFile(const std::filesystem::path &path, std::span<const uint8_t> data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void put16(std::vector<uint8_t> &v, std::size_t at, uint16_t x)
{
    v[at] = static_cast<uint8_t>(x);
    v[at + 1] = static_cast<uint8_t>(x >> 8);
}

void put32(std::vector<uint8_t> &v, std::size_t at, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}

// Sends one SPI command frame and returns the R1 response
uint8_t sdCommand(SdCard &card, uint8_t cmd, uint32_t arg)
{
    card.transfer(static_cast<uint8_t>(0x40 | cmd));
    for (int shift = 24; shift >= 0; shift -= 8)
        card.transfer(static_cast<uint8_t>(arg >> shift));
    card.transfer(0x95);
    for (int i = 0; i < 16; ++i) {
        const uint8_t r1 = card.transfer(0xFF);
        if (r1 != 0xFF)
            return r1;
    }
    return 0xFF;
}

void testSdCard()
{
    const auto image = tempPath("sd.img");
    std::vector<uint8_t> data(1024 * 1024);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i / 512 + i);
    writeFile(image, data);

    SdCard card;
    card.insert(image);
    card.setSelected(true);
    CHECK(sdCommand(card, 0, 0) == 0x01);          // idle
    CHECK(sdCommand(card, 8, 0x1AA) == 0x01);      // R7 follows
    for (int i = 0; i < 4; ++i)
        card.transfer(0xFF);
    CHECK(sdCommand(card, 55, 0) == 0x01);
    CHECK(sdCommand(card, 41, 0x40000000) == 0x00); // host supports SDHC
    CHECK(sdCommand(card, 58, 0) == 0x00);
    CHECK((card.transfer(0xFF) & 0x40) != 0);       // CCS: block addressed

    // READ_SINGLE_BLOCK 3
    CHECK(sdCommand(card, 17, 3) == 0x00);
    uint8_t token = 0xFF;
    while (token == 0xFF)
        token = card.transfer(0xFF);
    CHECK(token == 0xFE);
    for (std::size_t i = 0; i < 512; ++i)
        CHECK(card.transfer(0xFF) == data[3 * 512 + i]);
    card.transfer(0xFF);
    card.transfer(0xFF);

    // WRITE_BLOCK 5
    CHECK(sdCommand(card, 24, 5) == 0x00);
    card.transfer(0xFF);
    card.transfer(0xFE);
    for (int i = 0; i < 512; ++i)
        card.transfer(0xA5);
    card.transfer(0xFF);
    card.transfer(0xFF);
    uint8_t response = 0xFF;
    while (response == 0xFF)
        response = card.transfer(0xFF);
    CHECK((response & 0x1F) == 0x05); // data accepted
    card.setSelected(false);
    card.eject();

    const auto written = readFile(image);
    CHECK(written[5 * 512] == 0xA5 && written[6 * 512 - 1] == 0xA5);
    CHECK(written[4 * 512] == data[4 * 512]);
    std::filesystem::remove(image);
}

// A tiny FAT12 "superfloppy": /DIR/LongName.rom, 600 bytes, two clusters
std::vector<uint8_t> makeFat12Image()
{
    std::vector<uint8_t> img(64 * 512, 0);
    img[0] = 0xEB;
    img[1] = 0x3C;
    img[2] = 0x90;
    put16(img, 11, 512); // bytes per sector
    img[13] = 1;         // sectors per cluster
    put16(img, 14, 1);   // reserved sectors
    img[16] = 1;         // FATs
    put16(img, 17, 16);  // root entries (1 sector)
    put16(img, 19, 64);  // total sectors
    put16(img, 22, 1);   // sectors per FAT
    img[510] = 0x55;
    img[511] = 0xAA;

    // FAT (sector 1): cluster 2 = dir (EOC), 3 -> 4 -> EOC
    const std::size_t fat = 512;
    const auto setFat = [&](uint16_t cluster, uint16_t value) {
        const std::size_t off = fat + cluster + cluster / 2;
        if (cluster & 1) {
            img[off] = static_cast<uint8_t>((img[off] & 0x0F) | (value << 4));
            img[off + 1] = static_cast<uint8_t>(value >> 4);
        } else {
            img[off] = static_cast<uint8_t>(value);
            img[off + 1] = static_cast<uint8_t>((img[off + 1] & 0xF0) | ((value >> 8) & 0x0F));
        }
    };
    setFat(0, 0xFF8);
    setFat(1, 0xFFF);
    setFat(2, 0xFFF);
    setFat(3, 0x004);
    setFat(4, 0xFFF);

    // Root directory (sector 2): DIR -> cluster 2
    const std::size_t root = 2 * 512;
    std::memcpy(&img[root], "DIR        ", 11);
    img[root + 11] = 0x10;
    put16(img, root + 26, 2);

    // Data starts at sector 3 (cluster 2). DIR: an LFN entry + LONGFI~1.ROM -> cluster 3
    const std::size_t dir = 3 * 512;
    const std::string_view lfn = "LongName.rom"; // fits one LFN fragment
    img[dir] = 0x41; // last (and first) LFN fragment
    img[dir + 11] = 0x0F;
    uint8_t checksum = 0; // of the 8.3 name it belongs to
    for (char ch : std::string_view("LONGFI~1ROM"))
        checksum = static_cast<uint8_t>(((checksum & 1) << 7) + (checksum >> 1) + static_cast<uint8_t>(ch));
    img[dir + 13] = checksum;
    std::size_t c = 0;
    for (int off : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
        const uint16_t ch = c < lfn.size() ? static_cast<uint8_t>(lfn[c]) : (c == lfn.size() ? 0x0000 : 0xFFFF);
        put16(img, dir + static_cast<std::size_t>(off), ch);
        ++c;
    }
    std::memcpy(&img[dir + 32], "LONGFI~1ROM", 11);
    put16(img, dir + 32 + 26, 3);
    put32(img, dir + 32 + 28, 600);

    for (std::size_t i = 0; i < 600; ++i)
        img[4 * 512 + i] = static_cast<uint8_t>(i * 7);
    return img;
}

void testFatVolume()
{
    const auto img = makeFat12Image();
    FatVolume fat([&img](uint64_t lba, std::span<uint8_t, 512> out) {
        if ((lba + 1) * 512 > img.size())
            return false;
        std::memcpy(out.data(), img.data() + lba * 512, 512);
        return true;
    });
    const auto byLong = fat.readFile("/dir/longname.ROM");
    CHECK(byLong && byLong->size() == 600);
    CHECK((*byLong)[599] == static_cast<uint8_t>(599 * 7));
    const auto byShort = fat.readFile("DIR/LONGFI~1.ROM");
    CHECK(byShort && *byShort == *byLong);
    CHECK(!fat.readFile("/dir/missing.rom"));
    CHECK(!fat.readFile("/dir"));
}

// A blank FAT volume, laid out as mkfs.fat does (unpartitioned, two FATs, FAT32 with an FSInfo)
std::vector<uint8_t> makeBlankFat(int bits, uint32_t totalSectors, uint8_t sectorsPerCluster)
{
    std::vector<uint8_t> img(std::size_t{totalSectors} * 512, 0);
    img[0] = 0xEB;
    img[1] = 0x3C;
    img[2] = 0x90;
    put16(img, 11, 512);
    img[13] = sectorsPerCluster;
    const uint16_t reserved = bits == 32 ? 32 : 1;
    put16(img, 14, reserved);
    img[16] = 2;
    const uint16_t rootEntries = bits == 32 ? 0 : 512;
    put16(img, 17, rootEntries);
    if (totalSectors < 65536)
        put16(img, 19, static_cast<uint16_t>(totalSectors));
    else
        put32(img, 32, totalSectors);
    img[21] = 0xF8;
    const uint32_t maxClusters = totalSectors / sectorsPerCluster + 2;
    const uint32_t fatBytes = bits == 12 ? maxClusters * 3 / 2 + 1 : maxClusters * static_cast<uint32_t>(bits / 8);
    const uint32_t fatSectors = (fatBytes + 511) / 512;
    if (bits == 32) {
        put32(img, 36, fatSectors);
        put32(img, 44, 2); // the root directory
        put16(img, 48, 1); // the FSInfo sector
    } else {
        put16(img, 22, static_cast<uint16_t>(fatSectors));
    }
    img[510] = 0x55;
    img[511] = 0xAA;
    for (uint32_t copy = 0; copy < 2; ++copy) {
        const std::size_t fat = (reserved + copy * fatSectors) * std::size_t{512};
        if (bits == 12) {
            img[fat] = 0xF8;
            img[fat + 1] = 0xFF;
            img[fat + 2] = 0xFF;
        } else if (bits == 16) {
            put16(img, fat, 0xFFF8);
            put16(img, fat + 2, 0xFFFF);
        } else {
            put32(img, fat, 0x0FFFFFF8);
            put32(img, fat + 4, 0x0FFFFFFF);
            put32(img, fat + 8, 0x0FFFFFFF); // the root directory's cluster
        }
    }
    if (bits == 32) {
        const uint32_t dataSectors = totalSectors - reserved - 2 * fatSectors;
        put32(img, 512, 0x41615252);
        put32(img, 512 + 484, 0x61417272);
        put32(img, 512 + 488, dataSectors / sectorsPerCluster - 1);
        put32(img, 512 + 492, 3);
        img[1022] = 0x55;
        img[1023] = 0xAA;
    }
    return img;
}

std::vector<uint8_t> pattern(std::size_t size)
{
    std::vector<uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i)
        data[i] = static_cast<uint8_t>(i * 31 + i / 511);
    return data;
}

template <typename Exception, typename Fn>
bool throws(Fn fn)
{
    try {
        fn();
    } catch (const Exception &) {
        return true;
    }
    return false;
}

#define kTara "\xC8\x9A" "ar\xC4\x83.tap" // "Țară.tap" in UTF-8

void testFatWriting(int bits, uint32_t totalSectors)
{
    auto img = makeBlankFat(bits, totalSectors, 1);
    const auto reader = [&img](uint64_t lba, std::span<uint8_t, 512> out) {
        if ((lba + 1) * 512 > img.size())
            return false;
        std::memcpy(out.data(), img.data() + lba * 512, 512);
        return true;
    };
    FatVolume fat(reader, [&img](uint64_t lba, std::span<const uint8_t, 512> in) {
        if ((lba + 1) * 512 > img.size())
            return false;
        std::memcpy(img.data() + lba * 512, in.data(), 512);
        return true;
    });
    CHECK(fat.fatBits() == bits && fat.isWritable());
    const uint64_t blank = fat.freeBytes();

    // Files with 8.3 names, long names, lower case and non-ASCII names, an empty one, a directory tree
    fat.makeDirectory("/games/new");
    fat.makeDirectory("games"); // exists: nothing to do
    fat.writeFile("/games/new/My Long Game Name.tzx", pattern(70000));
    fat.writeFile("/readme.txt", pattern(1000));
    fat.writeFile("/GAME.TAP", pattern(5));
    fat.writeFile("/empty.bin", {});
    fat.writeFile("\\games\\" kTara, pattern(3000));
    CHECK(fat.readFile("/GAMES/NEW/my long game name.TZX") == pattern(70000));
    CHECK(fat.readFile("/games/new/MYLONG~1.TZX") == pattern(70000));
    CHECK(fat.readFile("/README.TXT") == pattern(1000));
    CHECK(fat.readFile("/empty.bin")->empty());
    const auto root = *fat.list("/");
    const std::vector<FatVolume::DirEntry> expected{{"games", 0, true}, {"readme.txt", 1000, false},
                                                    {"GAME.TAP", 5, false}, {"empty.bin", 0, false}};
    CHECK(root == expected);
    const auto games = *fat.list("/games");
    CHECK(games.size() == 2 && games[1].name == kTara && games[1].size == 3000);
    CHECK(!fat.list("/missing"));

    // Replacing a file, and a fresh read-only volume sees it all
    fat.writeFile("/readme.txt", pattern(10));
    CHECK(fat.readFile("/readme.txt") == pattern(10));
    FatVolume readOnly(reader);
    CHECK(!readOnly.isWritable() && readOnly.readFile("/games/new/my long game name.tzx") == pattern(70000));
    CHECK(throws<IoException>([&] { readOnly.writeFile("/x", pattern(1)); }));

    // The two FATs stay the same
    const std::size_t fatStart = bits == 32 ? 32 * 512 : 512;
    const std::size_t fatSize = bits == 32 ? (img[36] | img[37] << 8) * std::size_t{512} : (img[22] | img[23] << 8) * std::size_t{512};
    CHECK(std::equal(img.begin() + static_cast<std::ptrdiff_t>(fatStart),
                     img.begin() + static_cast<std::ptrdiff_t>(fatStart + fatSize),
                     img.begin() + static_cast<std::ptrdiff_t>(fatStart + fatSize)));

    // Mistakes
    CHECK(throws<InvalidArgumentException>([&] { fat.writeFile("/nodir/x.bin", pattern(1)); }));
    CHECK(throws<InvalidArgumentException>([&] { fat.writeFile("/games", pattern(1)); }));
    CHECK(throws<InvalidArgumentException>([&] { fat.writeFile("/bad:name", pattern(1)); }));
    CHECK(throws<InvalidArgumentException>([&] { fat.makeDirectory("/readme.txt/sub"); }));
    CHECK(throws<InvalidArgumentException>([&] { fat.remove("/games"); })); // not empty
    CHECK(throws<InvalidArgumentException>([&] { fat.remove("/missing"); }));
    const uint64_t before = fat.freeBytes();
    CHECK(throws<IoException>([&] { fat.writeFile("/big.bin", pattern(before + 1)); }));
    CHECK(fat.freeBytes() == before && !fat.readFile("/big.bin"));

    // Replacing a file reuses its clusters, and a replacement that doesn't fit leaves it alone
    const uint64_t room = fat.freeBytes();
    fat.writeFile("/fill.bin", pattern(room - 1024)); // two clusters left
    CHECK(throws<IoException>([&] { fat.writeFile("/fill.bin", pattern(room + 1)); }));
    CHECK(fat.readFile("/fill.bin") == pattern(room - 1024));
    fat.writeFile("/fill.bin", pattern(room));
    CHECK(fat.readFile("/fill.bin") == pattern(room) && fat.freeBytes() == 0);
    fat.remove("/fill.bin");
    CHECK(fat.freeBytes() == room);

    // A directory grows past its first cluster (one sector: 16 entries)
    fat.makeDirectory("/many");
    for (int i = 0; i < 40; ++i)
        fat.writeFile(std::format("/many/a long file name {}.bin", i), pattern(static_cast<std::size_t>(i)));
    const auto many = *fat.list("/many");
    CHECK(many.size() == 40 && many[39].name == "a long file name 39.bin" && many[39].size == 39);
    CHECK(fat.readFile("/many/a long file name 33.bin") == pattern(33));

    // Removing everything gives the space back
    for (int i = 0; i < 40; ++i)
        fat.remove(std::format("/many/a long file name {}.bin", i));
    for (std::string_view path : {"/many", "/games/new/My Long Game Name.tzx", "/games/new", "/games/" kTara,
                                  "/games", "/readme.txt", "/GAME.TAP", "/empty.bin"})
        fat.remove(path);
    CHECK(fat.list("/")->empty());
    if (bits == 32) {
        // The FSInfo free count follows the allocations
        const uint32_t freeCount = img[512 + 488] | img[512 + 489] << 8 | img[512 + 490] << 16 | img[512 + 491] << 24;
        CHECK(uint64_t{freeCount} * 512 == fat.freeBytes());
    }
    CHECK(fat.freeBytes() == blank);

    // FAT12/16: the root directory has a fixed size
    if (bits != 32) {
        bool full = false;
        for (int i = 0; i < 200 && !full; ++i)
            full = throws<IoException>([&] { fat.writeFile(std::format("/root file {}.bin", i), pattern(1)); });
        CHECK(full);
    }
}

// Builds a one entry deflated zip archive
std::vector<uint8_t> makeZip(std::string_view name, std::span<const uint8_t> data)
{
    std::vector<uint8_t> deflated(compressBound(static_cast<uLong>(data.size())) + 64);
    z_stream strm{};
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    strm.next_in = const_cast<Bytef *>(data.data());
    strm.avail_in = static_cast<uInt>(data.size());
    strm.next_out = deflated.data();
    strm.avail_out = static_cast<uInt>(deflated.size());
    deflate(&strm, Z_FINISH);
    deflated.resize(strm.total_out);
    deflateEnd(&strm);
    const uint32_t crc = computeCrc32(data);

    std::vector<uint8_t> zip(30 + name.size());
    put32(zip, 0, 0x04034B50);
    put16(zip, 8, 8);
    put32(zip, 14, crc);
    put32(zip, 18, static_cast<uint32_t>(deflated.size()));
    put32(zip, 22, static_cast<uint32_t>(data.size()));
    put16(zip, 26, static_cast<uint16_t>(name.size()));
    std::memcpy(&zip[30], name.data(), name.size());
    zip.insert(zip.end(), deflated.begin(), deflated.end());

    const std::size_t cdOffset = zip.size();
    std::vector<uint8_t> cd(46 + name.size());
    put32(cd, 0, 0x02014B50);
    put16(cd, 10, 8);
    put32(cd, 16, crc);
    put32(cd, 20, static_cast<uint32_t>(deflated.size()));
    put32(cd, 24, static_cast<uint32_t>(data.size()));
    put16(cd, 28, static_cast<uint16_t>(name.size()));
    std::memcpy(&cd[46], name.data(), name.size());
    zip.insert(zip.end(), cd.begin(), cd.end());

    std::vector<uint8_t> eocd(22);
    put32(eocd, 0, 0x06054B50);
    put16(eocd, 8, 1);
    put16(eocd, 10, 1);
    put32(eocd, 12, static_cast<uint32_t>(cd.size()));
    put32(eocd, 16, static_cast<uint32_t>(cdOffset));
    zip.insert(zip.end(), eocd.begin(), eocd.end());
    return zip;
}

void testZipStreaming()
{
    std::vector<uint8_t> data(3 * 1024 * 1024 + 123);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>((i * 31) ^ (i >> 9));
    const auto zipPath = tempPath("test.zip");
    const auto outPath = tempPath("out.img");
    writeFile(zipPath, makeZip("dir/Card.IMG", data));

    uint64_t lastDone = 0;
    extractZipEntryToFile(zipPath, "card.img", outPath, [&](uint64_t done, uint64_t total) {
        CHECK(total == data.size() && done >= lastDone);
        lastDone = done;
        return true;
    });
    CHECK(lastDone == data.size());
    CHECK(readFile(outPath) == data);

    // Cancelling removes the partial output
    bool threw = false;
    try {
        extractZipEntryToFile(zipPath, "card.img", outPath, [](uint64_t, uint64_t) { return false; });
    } catch (const IoException &) {
        threw = true;
    }
    CHECK(threw && !std::filesystem::exists(outPath));

    threw = false;
    try {
        extractZipEntryToFile(zipPath, "missing.img", outPath);
    } catch (const FormatException &) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(zipPath);
}

void testZxNextMemoryAndDivMmc()
{
    EmulatorConfig cfg;
    cfg.machineId = "next";
    Emulator emulator(cfg);
    {
        auto &eng = emulator;
        auto &next = dynamic_cast<ZxNextMachine &>(eng.machine());
        auto &mmu = eng.mmu();
        auto &io = eng.ioBus();
        auto &nr = next.nextReg();

        // Identification NextZXOS checks
        CHECK(nr.read(0x00) == 0x0A);
        CHECK(nr.read(0x01) == 0x32 && nr.read(0x0E) == 0x03);
        CHECK(nr.read(0x03) == 0x33);

        // MMU: pages >= 0xE0 are unmapped in slots 2..7 (read 0xFF, writes dropped)
        io.writeNextRegOpcode(0x52, 0xE5);
        mmu.write(0x4000, 0x12);
        CHECK(mmu.read(0x4000) == 0xFF);
        io.writeNextRegOpcode(0x52, 0x0A);
        mmu.write(0x4000, 0x34);
        CHECK(mmu.read(0x4000) == 0x34);

        // Port 0x7FFD bank 3 -> MMU6/7 = 6/7; NR 0x8E reads it back
        io.write(0x7FFD, 0x03);
        CHECK(nr.read(0x56) == 6 && nr.read(0x57) == 7);
        CHECK((nr.read(0x8E) & 0x78) == 0x38);
        // NR 0x8E: bank 5, ROM 2 (+3 ROM select = 1FFD bit2 : 7FFD bit 4)
        io.writeNextRegOpcode(0x8E, 0x5A);
        CHECK(nr.read(0x56) == 10 && nr.read(0x57) == 11);
        CHECK(next.lastPort1ffd() & 0x04);
        CHECK(!(next.lastPort7ffd() & 0x10));

        // +3 special paging 4,7,6,3
        io.write(0x1FFD, 0x07);
        CHECK(nr.read(0x50) == 8 && nr.read(0x52) == 14 && nr.read(0x56) == 6);
        io.write(0x1FFD, 0x00);
        CHECK(nr.read(0x50) == 0xFF && nr.read(0x52) == 0x0A && nr.read(0x54) == 4);

        // DivMMC CONMEM: ROM at 0x0000, RAM bank 2 at 0x2000
        mmu.bank(mmu.findBank("divmmc_rom")).bytes()[0] = 0xD1;
        io.write(0x00E3, 0x82);
        CHECK(next.isDivMmcMapped());
        CHECK(mmu.read(0x0000) == 0xD1);
        mmu.write(0x2000, 0x77);
        CHECK(mmu.bank(mmu.findBank("divmmc_ram")).bytes()[2 * 8192] == 0x77);
        io.write(0x00E3, 0x00);
        CHECK(!next.isDivMmcMapped());

        // SPI with no card: reads are 0xFF, port 0xE7 deselects
        io.write(0x00E7, 0xFE);
        io.write(0x00EB, 0x40);
        CHECK(io.read(0x00EB) == 0xFF);
    }

    // Automap at RST 0x38 (NR 0xB8 bit 7, NR 0xB9 bit 7: always), delayed:
    // the opcode at 0x0038 comes from the ROM, the next one from DivMMC
    {
        auto &eng = emulator;
        auto &next = dynamic_cast<ZxNextMachine &>(eng.machine());
        auto &mmu = eng.mmu();
        eng.ioBus().writeNextRegOpcode(0xB8, 0x80);
        eng.ioBus().writeNextRegOpcode(0xB9, 0x80);
        eng.ioBus().writeNextRegOpcode(0xBA, 0x00);
        auto rom = mmu.bank(mmu.findBank("rom")).bytes();
        auto divRom = mmu.bank(mmu.findBank("divmmc_rom")).bytes();
        rom[0x38] = 0x00;    // NOP in ROM 0
        divRom[0x39] = 0x3C; // INC A in the DivMMC ROM
        rom[0x39] = 0x00;
        auto &r = eng.cpu().registers();
        r.PC = 0x0038;
        r.setA(0);
        eng.cpu().stepInstruction();
        CHECK(next.isDivMmcMapped());
        eng.cpu().stepInstruction();
        CHECK(r.A() == 1);
        // A fetch in 0x1FF8..0x1FFF unmaps (NR 0xBB bit 6)
        r.PC = 0x1FF8;
        eng.cpu().stepInstruction();
        CHECK(!next.isDivMmcMapped());
    }
}

void testTurboTimeline()
{
    // At 28MHz a frame lasts as long as at 3.5MHz, but runs 8x the instructions
    EmulatorConfig cfg;
    cfg.machineId = "next";
    Emulator emulator(cfg);
    emulator.runFrames(1);
    const uint64_t t0 = emulator.clock().totalTstates();
    emulator.runFrames(1);
    const uint64_t t1 = emulator.clock().totalTstates();
    emulator.ioBus().writeNextRegOpcode(0x07, 0x03);
    emulator.runFrames(1);
    const uint64_t t2 = emulator.clock().totalTstates();
    emulator.runFrames(1);
    const uint64_t t3 = emulator.clock().totalTstates();

    const uint64_t slow = t1 - t0;
    const uint64_t fast = t3 - t2;
    CHECK(slow >= kTimings128k.tstatesPerFrame() && slow < kTimings128k.tstatesPerFrame() + 64);
    CHECK(fast >= 8 * kTimings128k.tstatesPerFrame() && fast < 8 * kTimings128k.tstatesPerFrame() + 512);
}

void testZxNextDmaJoystickAndInterrupts()
{
    EmulatorConfig cfg;
    cfg.machineId = "next";
    Emulator emulator(cfg);
    {
        auto &eng = emulator;
        auto &mmu = eng.mmu();
        auto &io = eng.ioBus();
        // Angry Bloaters' fill: 0x3FFF bytes from 0xC000 to 0xC001, the WR2
        // timing byte (0x01) has no prescaler, so WR4 (0xAD) must not be eaten
        mmu.write(0xC000, 0x55);
        for (uint8_t b : {0xC3, 0xC7, 0xCB, 0x7D, 0x00, 0xC0, 0xFF, 0x3F, 0x54, 0x01, 0x50, 0x01,
                          0xAD, 0x01, 0xC0, 0x82, 0xCF, 0xB3, 0x87})
            io.write(0x006B, b);
        CHECK(mmu.read(0xC001) == 0x55 && mmu.read(0xE000) == 0x55 && mmu.read(0xFFFF) == 0x55);
        CHECK((io.read(0x006B) & 0x21) == 0x01); // status: end of block, at least one byte

        // The built-in joystick ports read idle, not floating bus
        CHECK(io.read(0x001F) == 0x00 && io.read(0x0037) == 0x00);

        // RAM in slots 0/1, an IM 1 handler counting interrupts in HL, and EI; JR $
        io.writeNextRegOpcode(0x50, 0x00);
        io.writeNextRegOpcode(0x51, 0x01);
        for (uint8_t i = 0; auto b : {0x23, 0xFB, 0xC9})
            mmu.write(static_cast<uint16_t>(0x0038 + i++), static_cast<uint8_t>(b));
        for (uint8_t i = 0; auto b : {0xFB, 0x18, 0xFE})
            mmu.write(static_cast<uint16_t>(0x8000 + i++), static_cast<uint8_t>(b));
        auto &r = eng.cpu().registers();
        r.PC = 0x8000;
        r.SP = 0xBFF0;
        r.HL = 0;
        r.IM = 1;
        // Line interrupt at line 100 only (NR 0x22 bit 2 turns the ULA interrupt off)
        io.writeNextRegOpcode(0x22, 0x06);
        io.writeNextRegOpcode(0x23, 100);
    }
    const auto interruptsPerFrame = [&emulator](uint8_t nr22) {
        emulator.ioBus().writeNextRegOpcode(0x22, nr22);
        emulator.cpu().registers().HL = 0;
        emulator.runFrames(1);
        return emulator.cpu().registers().HL;
    };
    CHECK(interruptsPerFrame(0x06) == 1); // line only
    CHECK(interruptsPerFrame(0x02) == 2); // line + ULA
    CHECK(interruptsPerFrame(0x04) == 0); // none
    CHECK(interruptsPerFrame(0x00) == 1); // ULA only
}

void testSnapshotExports()
{
    for (const std::string_view id : {"48k","128k","plus2","plus2a","plus3","pentagon"}) {
        EmulatorConfig config; config.machineId = id;
        Emulator source(config);
        const auto bank = source.machine().ramBankId();
        auto &ram = source.mmu().bank(bank);
        for (std::size_t p = 0; p < ram.count16kPages(); ++p) {
            std::vector<uint8_t> pattern(16384);
            for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(p * 29 + i * 13);
            ram.loadData(p * 16384, pattern);
        }
        auto regs = source.registers();
        regs.PC = 0xBEEF; regs.SP = 0x8123; regs.AF = 0xABCD; regs.AF_ = 0x7531;
        regs.BC = 0x1234; regs.DE = 0x5678; regs.HL = 0x9ABC; regs.IX = 0x1357; regs.IY = 0x2468;
        regs.IFF1 = regs.IFF2 = 1; regs.IM = 2; regs.setFullR(0xE5);
        source.setRegisters(regs);
        source.clock().setFrameTstates(23456);
        if (id != "48k") source.ioBus().write(0x7FFD, 0x1B);
        for (const std::string_view format : {".z80", ".szx"}) {
            const auto bytes = source.saveMedia(format);
            CHECK(SnapshotFormats::machineId(bytes, format) == id);
            Emulator target;
            target.loadSnapshot(bytes, format);
            CHECK(target.machine().id() == id);
            CHECK(target.registers().PC == regs.PC && target.registers().SP == regs.SP);
            CHECK(target.registers().AF == regs.AF && target.registers().AF_ == regs.AF_);
            CHECK(target.registers().fullR() == regs.fullR() && target.registers().IFF1 == 1);
            CHECK(target.clock().frameBaseTstates() == source.clock().frameBaseTstates());
            const auto &restored = target.mmu().bank(target.machine().ramBankId());
            for (std::size_t p = 0; p < ram.count16kPages(); ++p)
                CHECK(std::ranges::equal(ram.span16k(p), restored.span16k(p)));
            if (id != "48k") CHECK(target.machine().lastPort7ffd() == 0x1B);
        }
        if (id == "128k" || id == "plus2" || id == "pentagon") {
            for (const uint8_t top : {0, 2, 5, 7}) {
                source.ioBus().write(0x7FFD, top);
                const auto bytes = source.saveSnapshot(".sna");
                CHECK(bytes.size() == ((top == 2 || top == 5) ? 147487 : 131103));
                CHECK(bytes[49179] == 0xEF && bytes[49180] == 0xBE && bytes[49181] == top);
                Emulator target; target.loadSnapshot(bytes, ".sna");
                CHECK(target.registers().PC == regs.PC && target.registers().SP == regs.SP);
                const auto &restored = target.mmu().bank(target.machine().ramBankId());
                for (std::size_t p = 0; p < 8; ++p) CHECK(std::ranges::equal(ram.span16k(p), restored.span16k(p)));
            }
        }
    }
    EmulatorConfig config; config.machineId = "next";
    Emulator next(config);
    next.ioBus().writeNextRegOpcode(0x52, 10); next.ioBus().writeNextRegOpcode(0x53, 11);
    next.ioBus().writeNextRegOpcode(0x54, 4); next.ioBus().writeNextRegOpcode(0x55, 5);
    next.ioBus().writeNextRegOpcode(0x56, 14); next.ioBus().writeNextRegOpcode(0x57, 15);
    auto r = next.registers(); r.PC = 0xC100; r.SP = 0xBFF0; next.setRegisters(r);
    next.writeMemory(0xC100, {0x3E,0x05,0xD3,0xFE,0x18,0xFE}); // LD A,5; OUT (FE),A; JR $
    const auto native = next.saveSnapshot(".szx");
    Emulator restoredNext;
    restoredNext.loadSnapshot(native, ".szx");
    CHECK(restoredNext.machine().id() == "next");
    CHECK(restoredNext.readMemory(0xC100,6) == next.readMemory(0xC100,6));
    CHECK(restoredNext.registers().PC == 0xC100 && restoredNext.registers().SP == 0xBFF0);
    CHECK(restoredNext.saveSnapshot(".szx") == native);
    const auto bytes = next.saveSnapshot(".nex");
    CHECK(std::memcmp(bytes.data(), "NextV1.2", 8) == 0 && bytes[139] == 7);
    Emulator target; target.loadSnapshot(bytes, ".nex");
    CHECK(target.machine().id() == "next");
    CHECK(target.registers().PC == 0xC100 && target.registers().SP == 0xBFF0);
    CHECK(target.readMemory(0xC100,6) == next.readMemory(0xC100,6));
    target.runFrames(1);
    CHECK(target.registers().A() == 5 && target.ula().borderColor() == 5);
    target.ioBus().writeNextRegOpcode(0x15, 0xFF);
    target.loadSnapshot(bytes, ".nex");
    CHECK(static_cast<ZxNextMachine &>(target.machine()).nextReg().peek(0x15) == 0);
    std::println("  Snapshot exports: headers, every RAM bank, paging, CPU and frame position: OK");
}


void testSnapshotJoystickMetadata()
{
    using J = JoystickType;
    Emulator emulator;
    auto original = emulator.saveSnapshot(".szx");
    CHECK(!SnapshotFormats::joystickInfo(original, ".szx").present);
    constexpr std::array types{J::None,J::Kempston,J::Fuller,J::Cursor,J::Sinclair1,J::Sinclair2,J::Timex1,J::Timex2};
    for (auto type : types) {
        auto bytes = original;
        CHECK(SnapshotFormats::setJoystickInfo(bytes, ".szx", type));
        auto info = SnapshotFormats::joystickInfo(bytes, ".szx");
        CHECK(info.present && info.type == type);
        const auto size = bytes.size();
        CHECK(SnapshotFormats::setJoystickInfo(bytes, ".szx", type));
        CHECK(bytes.size() == size); // never duplicate JOY chunks
        Emulator restored;
        restored.loadSnapshot(bytes, ".szx");
        CHECK(restored.registers().PC == emulator.registers().PC);
    }
    auto chunk = [](std::vector<uint8_t> &bytes, const char *id, std::initializer_list<uint8_t> body) {
        bytes.insert(bytes.end(), id, id + 4);
        const auto size = static_cast<uint8_t>(body.size());
        bytes.insert(bytes.end(), {size,0,0,0});
        bytes.insert(bytes.end(), body);
    };
    auto bytes = original;
    chunk(bytes, "KEYB", {1,0,0,0}); // v1.0 has no joystick field
    CHECK(!SnapshotFormats::joystickInfo(bytes, ".szx").present);
    chunk(bytes, "KEYB", {0,0,0,0,5}); // Spectrum+ cursor keys are ordinary keyboard behavior
    CHECK(!SnapshotFormats::joystickInfo(bytes, ".szx").present);
    chunk(bytes, "KEYB", {1,0,0,0,4}); // keyboard-only Sinclair 2
    CHECK(SnapshotFormats::joystickInfo(bytes, ".szx").type == J::Sinclair2);
    chunk(bytes, "JOY\0", {1,0,0,0,8,8}); // deprecated flag is NOT a Kempston declaration
    CHECK(SnapshotFormats::joystickInfo(bytes, ".szx").type == J::Sinclair2);
    chunk(bytes, "JOY\0", {0,0,0,0,4,4}); // same type for two players is unambiguous
    CHECK(SnapshotFormats::joystickInfo(bytes, ".szx").type == J::Sinclair2);
    chunk(bytes, "JOY\0", {0,0,0,0,0,8}); // conflicting interface
    auto info = SnapshotFormats::joystickInfo(bytes, ".szx");
    CHECK(info.present && !info.type);
    CHECK(SnapshotFormats::setJoystickInfo(bytes, ".szx", J::Fuller));
    CHECK(SnapshotFormats::joystickInfo(bytes, ".szx").type == J::Fuller);
    for (uint8_t value : {5,9,255}) {
        auto unknown = original;
        chunk(unknown, "JOY\0", {0,0,0,0,value,0});
        info = SnapshotFormats::joystickInfo(unknown, ".szx");
        CHECK(info.present && !info.type); // no guessing even with one supported player
    }
    bytes = original;
    chunk(bytes, "JOY\0", {0,0,0,0,0});
    bool rejected = false;
    try { SnapshotFormats::joystickInfo(bytes, ".szx"); } catch (const FormatException &) { rejected = true; }
    CHECK(rejected);
    bytes = original; bytes.push_back(0);
    rejected = false;
    try { SnapshotFormats::joystickInfo(bytes, ".szx"); } catch (const FormatException &) { rejected = true; }
    CHECK(rejected);
    CHECK(!SnapshotFormats::joystickInfo({}, ".sna").present);

    // Authored v1 and v2 headers: the left port is Sinclair 2, the right is Sinclair 1.
    for (unsigned version : {1,2,3}) {
        std::vector<uint8_t> header(version == 1 ? 30 : version == 2 ? 55 : 87, 0);
        if (version == 1) header[6] = 1;
        else header[30] = version == 2 ? 23 : 55;
        constexpr std::array expected{J::Cursor,J::Kempston,J::Sinclair2,J::Sinclair1};
        for (uint8_t mode = 0; mode < 4; ++mode) {
            header[29] = static_cast<uint8_t>((mode << 6) | 6);
            info = SnapshotFormats::joystickInfo(header, ".z80");
            CHECK(info.present);
            if (version == 3 && mode == 2) CHECK(!info.type); // arbitrary/unassigned custom mapping
            else CHECK(info.type == expected[mode]);
        }
        for (auto type : {J::Cursor,J::Kempston,J::Sinclair1,J::Sinclair2}) {
            CHECK(SnapshotFormats::setJoystickInfo(header, ".z80", type));
            CHECK((header[29] & 0x3F) == 6); // IM and other flags unchanged
            CHECK(SnapshotFormats::joystickInfo(header, ".z80").type == type);
        }
        const auto before = header;
        CHECK(!SnapshotFormats::setJoystickInfo(header, ".z80", J::Timex1));
        CHECK(header == before);
        if (version == 3) {
            header[63] = 7; // arbitrary programmable interface is not Sinclair 2
            CHECK(!SnapshotFormats::joystickInfo(header, ".z80").type);
        }
    }
    auto exported = emulator.saveSnapshot(".z80");
    CHECK(!SnapshotFormats::joystickInfo(exported, ".z80").type); // core does not invent host preferences
    CHECK(SnapshotFormats::setJoystickInfo(exported, ".z80", J::Sinclair2));
    Emulator restored; restored.loadSnapshot(exported, ".z80");
    CHECK(restored.registers().IM == emulator.registers().IM);
    std::println("  Snapshot joystick metadata: versions, mappings, ambiguity, bounds and exports: OK");
}

} // namespace

int main()
{
    testSnapshotExports();
    testSnapshotJoystickMetadata();
    testSdCard();
    testFatVolume();
    testFatWriting(12, 2880);
    testFatWriting(16, 8192);
    testFatWriting(32, 4096);
    testZipStreaming();
    testZxNextMemoryAndDivMmc();
    testTurboTimeline();
    testZxNextDmaJoystickAndInterrupts();
    std::println("test_storage_next: ALL PASSED");
    return 0;
}
