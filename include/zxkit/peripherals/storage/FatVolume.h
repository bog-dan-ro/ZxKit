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

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

/// A FAT12/16/32 file system on a disk image (e.g. the ZX Next SD card): reading files (the Next's
/// ROMs) and, with a block writer, writing them (a host adding files to the card). Understands MBR
/// partitioned and unpartitioned images, and long file names. Paths use '/' (or '\'), names are
/// UTF-8 and lookups are case-insensitive (ASCII).
class FatVolume
{
public:
    using BlockReader = std::function<bool(uint64_t lba, std::span<uint8_t, 512> out)>;
    using BlockWriter = std::function<bool(uint64_t lba, std::span<const uint8_t, 512> in)>;

    struct DirEntry
    {
        std::string name; ///< the long name, or the 8.3 one
        uint32_t size = 0;
        bool isDirectory = false;
        uint8_t attributes = 0;

        bool operator==(const DirEntry &) const = default;
    };

    /// Mounts the first FAT partition, read-only without a `writer`.
    /// Throws FormatException if there is none.
    explicit FatVolume(BlockReader reader, BlockWriter writer = {});

    uint8_t fatBits() const noexcept
    {
        return m_fatBits;
    }
    bool isWritable() const noexcept
    {
        return static_cast<bool>(m_write);
    }
    uint64_t totalBytes() const noexcept;
    /// Counts the free clusters (reads the whole FAT).
    uint64_t freeBytes() const;

    /// The entries of a directory ("/" is the root), without "." and "..". nullopt if missing.
    std::optional<std::vector<DirEntry>> list(std::string_view path) const;
    /// Reads a whole file, e.g. "/machines/next/enNextZX.rom". nullopt if missing.
    std::optional<std::vector<uint8_t>> readFile(std::string_view path) const;

    // Writing. These throw IoException (a read-only volume, a full disk or root directory, a block
    // that can't be read or written) or InvalidArgumentException (a missing parent directory, a
    // file where a directory should be or the other way around, a name FAT can't hold).

    /// Creates the file, or replaces it. Its directory must exist.
    void writeFile(std::string_view path, std::span<const uint8_t> data);
    /// Creates the directory and the missing ones above it (nothing if it exists).
    void makeDirectory(std::string_view path);
    /// Removes a file or an empty directory.
    void remove(std::string_view path);
    /// Change the R/H/S/A bits (directory and volume bits are preserved).
    void setFileAttributes(std::string_view path, uint8_t attributes);
    /// Move a file or directory; the destination parent must already exist.
    void rename(std::string_view path, std::string_view destination);

private:
    using Sector = std::array<uint8_t, 512>;
    using ShortName = std::array<uint8_t, 11>;
    using RawEntry = std::array<uint8_t, 32>;

    struct Entry
    {
        ShortName rawName{};
        std::string shortName;
        std::string longName;
        uint32_t firstCluster = 0;
        uint32_t size = 0;
        bool isDir = false;
        uint8_t attributes = 0;
        std::size_t slot = 0; // the 32-byte slot of the 8.3 entry in its directory
        std::size_t firstSlot = 0; // its first long name slot (`slot` without a long name)
    };

    struct Directory
    {
        uint32_t cluster = 0; // 0: the root
        std::vector<uint64_t> sectors; // in order
        std::vector<Entry> entries;
        std::vector<bool> used; // per slot
        std::size_t end = 0; // the slot of the end marker (all slots from it on are free)
    };

    bool mountAt(uint64_t lba);
    uint32_t fatEntry(uint32_t cluster) const;
    bool isEndOfChain(uint32_t cluster) const noexcept;
    uint64_t clusterLba(uint32_t cluster) const noexcept;
    uint32_t clusterBytes() const noexcept
    {
        return m_sectorsPerCluster * 512;
    }
    std::vector<uint32_t> chain(uint32_t firstCluster) const;
    std::vector<uint8_t> readChain(uint32_t firstCluster, std::size_t maxBytes) const;
    Directory readDirectory(uint32_t cluster) const;
    std::optional<Directory> openDirectory(std::span<const std::string_view> path) const;
    static std::optional<Entry> find(const Directory &dir, std::string_view name);

    // The FAT sectors an operation reads are cached for its duration only (the emulated system may
    // change the card between two operations); the ones it changes go to every FAT copy at the end
    class Operation;
    void commit();
    Sector readBlock(uint64_t lba) const;
    void writeBlock(uint64_t lba, std::span<const uint8_t, 512> data);
    const Sector &fatSector(uint64_t index) const;
    Sector &fatSectorForWrite(uint64_t index);
    void setFatEntry(uint32_t cluster, uint32_t value);
    uint32_t endOfChain() const noexcept;
    std::vector<uint32_t> allocate(std::size_t count);
    bool hasFreeClusters(std::size_t count) const;
    void freeChain(uint32_t firstCluster);
    void writeClusters(std::span<const uint32_t> clusters, std::span<const uint8_t> data);
    std::vector<RawEntry> makeEntries(const Directory &dir, std::string_view name,
                                      uint8_t attributes, uint32_t firstCluster,
                                      uint32_t size) const;
    void addEntries(Directory &dir, std::span<const RawEntry> entries);
    void removeEntry(const Directory &dir, const Entry &entry);

    BlockReader m_read;
    BlockWriter m_write;
    uint8_t m_fatBits = 0;
    uint32_t m_sectorsPerCluster = 0;
    uint64_t m_fatLba = 0; // the FAT in use (the first one when they are mirrored)
    uint32_t m_fatSectors = 0; // per FAT
    uint32_t m_fatCopies = 1; // written by the write operations
    uint64_t m_rootDirLba = 0; // FAT12/16 fixed root directory
    uint32_t m_rootDirSectors = 0;
    uint32_t m_rootCluster = 0; // FAT32 root directory cluster
    uint64_t m_dataLba = 0;
    uint32_t m_clusterCount = 0;
    uint64_t m_fsInfoLba = 0; // FAT32: the free cluster count and hint (0: none)
    uint32_t m_nextFree = 2; // where to look for free clusters

    struct CachedSector
    {
        Sector data;
        bool dirty = false;
    };
    mutable std::map<uint64_t, CachedSector> m_fat; // by sector index in the FAT
    int64_t m_freeDelta = 0; // clusters freed minus allocated, for the FSInfo
};

} // namespace zxkit
