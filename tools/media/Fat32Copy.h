// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "MediaImage.h"

namespace zxkit::tool {

inline void formatFat32(const fs::path &path, uint32_t sectors, std::string_view label)
{
    constexpr uint32_t partitionStart = 2048;
    constexpr uint32_t reserved = 32;
    constexpr uint32_t minimumClusters = 65526; // Also accepted by TBBlue's FatFs.
    if (sectors < partitionStart + reserved + minimumClusters || label.size() > 11)
        throw InvalidArgumentException(
                "FAT32 needs a larger image or a label of at most 11 bytes"sv);

    const uint32_t partitionSize = sectors - partitionStart;
    uint8_t sectorsPerCluster = 1;
    while (sectorsPerCluster < 64 && partitionSize / sectorsPerCluster > 262144)
        sectorsPerCluster *= 2;

    // Provision enough entries even before subtracting the space occupied by the FATs.
    const uint32_t fatSectors = (partitionSize / sectorsPerCluster + uint64_t{ 129 }) / 128;
    const uint32_t clusters = (partitionSize - reserved - 2 * fatSectors) / sectorsPerCluster;
    if (clusters < minimumClusters || clusters >= 0x0FFFFFF0)
        throw InvalidArgumentException("Image size cannot be represented as FAT32"sv);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.seekp(std::streamoff(sectors) * 512 - 1);
    out.put(0); // Allocate the image without materializing free sectors.

    std::array<uint8_t, 512> block{};
    const auto put = [&](std::size_t offset, uint32_t value, unsigned width) {
        for (unsigned byte = 0; byte < width; ++byte)
            block[offset + byte] = uint8_t(value >> (byte * 8));
    };
    const auto write = [&](uint32_t lba) {
        out.seekp(std::streamoff(lba) * 512);
        out.write(reinterpret_cast<const char *>(block.data()), block.size());
    };

    // Primary LBA FAT32 partition. The CHS fields mark the partition as LBA-only.
    block[446] = 0x80;
    put(447, 0xFFFFFE, 3);
    block[450] = 0x0C;
    put(451, 0xFFFFFE, 3);
    put(454, partitionStart, 4);
    put(458, partitionSize, 4);
    put(510, 0xAA55, 2);
    write(0);

    block.fill(0);
    put(0, 0x9058EB, 3);
    std::ranges::copy("ZXKIT   "sv, block.begin() + 3);
    put(11, 512, 2);
    block[13] = sectorsPerCluster;
    put(14, reserved, 2);
    block[16] = 2;
    block[21] = 0xF8;
    put(24, 63, 2);
    put(26, 255, 2);
    put(28, partitionStart, 4);
    put(32, partitionSize, 4);
    put(36, fatSectors, 4);
    put(44, 2, 4); // Root directory cluster.
    put(48, 1, 2); // FSInfo.
    put(50, 6, 2); // Backup boot sector.
    block[64] = 0x80;
    block[66] = 0x29;
    put(67, 0x5A584B54, 4);
    std::fill_n(block.begin() + 71, 11, ' ');
    std::ranges::transform(label, block.begin() + 71,
                           [](unsigned char c) { return uint8_t(std::toupper(c)); });
    std::ranges::copy("FAT32   "sv, block.begin() + 82);
    put(510, 0xAA55, 2);
    write(partitionStart);
    write(partitionStart + 6);

    block.fill(0);
    put(0, 0x41615252, 4);
    put(484, 0x61417272, 4);
    put(488, clusters - 1, 4);
    put(492, 3, 4);
    put(508, 0xAA550000, 4);
    write(partitionStart + 1);
    write(partitionStart + 7);

    block.fill(0);
    put(0, 0x0FFFFFF8, 4);
    put(4, 0x0FFFFFFF, 4);
    put(8, 0x0FFFFFFF, 4); // The root is the only allocated cluster.
    write(partitionStart + reserved);
    write(partitionStart + reserved + fatSectors);

    out.close();
    if (!out)
        throw IoException("Cannot format the FAT32 copy"sv);
}

inline void copyFat32(const fs::path &input, const fs::path &output, std::string_view label,
                      bool replace)
{
    const auto destination = imagePath(output);
    if (fs::exists(destination) && fs::equivalent(input, destination))
        throw InvalidArgumentException("FAT32 copy requires a separate destination"sv);

    const auto size = fs::file_size(input);
    if (size % 512 != 0 || size / 512 > UINT32_MAX)
        throw InvalidArgumentException("FAT32 copy requires a whole number of 512-byte sectors"sv);

    FatCardImage source(input, false);
    const auto entries = source.list();
    const ImageLock lock(destination);
    atomicOutput(destination, replace, [&](const fs::path &temporary) {
        formatFat32(temporary, uint32_t(size / 512), label);
        FatCardImage target(temporary, true);
        for (const auto &entry : entries) {
            if (entry.directory) {
                target.makeDirectory(entry.name);
                target.setAttributes(entry.name, entry.attributes);
            } else {
                target.put(entry.name, source.read(entry.name), entry.attributes);
            }
        }
        target.save();
    });
}

} // namespace zxkit::tool
