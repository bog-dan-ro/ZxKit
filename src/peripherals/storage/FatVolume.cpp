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

#include "zxkit/peripherals/storage/FatVolume.h"

#include "zxkit/core/Exception.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string_view>
#include <utility>

namespace zxkit {

using namespace std::literals;

namespace {

constexpr std::size_t kSlotsPerSector = 512 / 32;
constexpr uint8_t kAttrDirectory = 0x10;
constexpr uint8_t kAttrArchive = 0x20;
constexpr uint8_t kAttrVolume = 0x08;
constexpr uint8_t kAttrLongName = 0x0F;
// The UTF-16 characters of a long name entry
constexpr std::array<std::size_t, 13> kLongNameOffsets{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

uint16_t le16(std::span<const uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t le32(std::span<const uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void put16(std::span<uint8_t> bytes, std::size_t offset, uint16_t value) noexcept
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::span<uint8_t> bytes, std::size_t offset, uint32_t value) noexcept
{
    put16(bytes, offset, static_cast<uint16_t>(value));
    put16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char upper(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool equalsNoCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

std::vector<std::string_view> splitPath(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t pos = 0;
    while (pos < path.size()) {
        const std::size_t end = std::min(path.find_first_of("/\\", pos), path.size());
        if (end > pos)
            parts.push_back(path.substr(pos, end - pos));
        pos = end + 1;
    }
    return parts;
}

std::u16string utf8ToUtf16(std::string_view text)
{
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07;
            length = 4;
        } else {
            throw InvalidArgumentException("A file name isn't valid UTF-8");
        }
        if (i + length > text.size())
            throw InvalidArgumentException("A file name isn't valid UTF-8");
        for (std::size_t k = 1; k < length; ++k) {
            const auto c = static_cast<unsigned char>(text[i + k]);
            if ((c & 0xC0) != 0x80)
                throw InvalidArgumentException("A file name isn't valid UTF-8");
            cp = (cp << 6) | (c & 0x3F);
        }
        i += length;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf16ToUtf8(std::u16string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t cp = text[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = '_'; // a lone surrogate
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

uint8_t shortNameChecksum(std::span<const uint8_t, 11> name) noexcept
{
    uint8_t sum = 0;
    for (uint8_t c : name)
        sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + c);
    return sum;
}

bool isShortNameChar(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || "!#$%&'()-@^_`{}~"sv.contains(c);
}

// `name` as an 8.3 name, if it is one once in upper case
std::optional<std::array<uint8_t, 11>> exactShortName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    const std::string_view base = name.substr(0, dot);
    const std::string_view ext = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || (dot != std::string_view::npos && ext.empty()))
        return std::nullopt;
    std::array<uint8_t, 11> out;
    out.fill(' ');
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (!isShortNameChar(upper(base[i])))
            return std::nullopt;
        out[i] = static_cast<uint8_t>(upper(base[i]));
    }
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (!isShortNameChar(upper(ext[i])))
            return std::nullopt;
        out[8 + i] = static_cast<uint8_t>(upper(ext[i]));
    }
    return out;
}

// A "BASENA~1.EXT" style name for a long one
template <typename Taken>
std::array<uint8_t, 11> generatedShortName(std::string_view name, Taken taken)
{
    const auto clean = [](std::string_view part, std::size_t max) {
        std::string out;
        for (char c : part) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == ' ' || c == '.' || (byte & 0xC0) == 0x80) // one '_' per UTF-8 character
                continue;
            if (out.size() == max)
                break;
            out.push_back(byte < 0x80 && isShortNameChar(upper(c)) ? upper(c) : '_');
        }
        return out;
    };
    const std::size_t dot = name.rfind('.');
    const bool hasExt = dot != std::string_view::npos && dot > 0;
    std::string base = clean(hasExt ? name.substr(0, dot) : name, 8);
    const std::string ext = hasExt ? clean(name.substr(dot + 1), 3) : std::string{};
    if (base.empty())
        base = "_";
    for (unsigned n = 1; n < 1'000'000; ++n) {
        const std::string tail = std::format("~{}", n);
        const std::string stem = base.substr(0, std::min(base.size(), 8 - tail.size())) + tail;
        std::array<uint8_t, 11> out;
        out.fill(' ');
        std::ranges::copy(stem, out.begin());
        std::ranges::copy(ext, out.begin() + 8);
        if (!taken(out))
            return out;
    }
    throw IoException("No free short file name");
}

// The date and time in FAT's format (UTC: the image has no time zone)
std::pair<uint16_t, uint16_t> fatTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto today = floor<days>(now);
    const year_month_day date{today};
    const hh_mm_ss time{floor<seconds>(now - today)};
    const int year = std::clamp(static_cast<int>(date.year()), 1980, 2107);
    return {static_cast<uint16_t>(((year - 1980) << 9) | (static_cast<unsigned>(date.month()) << 5) |
                                  static_cast<unsigned>(date.day())),
            static_cast<uint16_t>((time.hours().count() << 11) | (time.minutes().count() << 5) |
                                  (time.seconds().count() / 2))};
}

// The name of a new file or directory, as UTF-16
std::u16string checkedName(std::string_view name)
{
    if (name == "." || name == ".." || name.ends_with(' ') || name.ends_with('.'))
        throw InvalidArgumentException(std::format("'{}' isn't a valid FAT file name", name));
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || "\"*/:<>?\\|"sv.contains(c))
            throw InvalidArgumentException(std::format("'{}' isn't a valid FAT file name", name));
    }
    std::u16string utf16 = utf8ToUtf16(name);
    if (utf16.size() > 255)
        throw InvalidArgumentException(std::format("'{}' is too long for a FAT file name", name));
    return utf16;
}

} // namespace

// Every public operation runs in one: it drops the cached FAT sectors when it ends. A write commits
// its FAT changes with commit(); if it throws before, they are dropped.
class FatVolume::Operation {
public:
    explicit Operation(FatVolume &volume, bool write)
        : m_volume(volume)
    {
        if (write && !volume.isWritable())
            throw IoException("The FAT volume is read-only");
        volume.m_fat.clear();
        volume.m_freeDelta = 0;
    }
    explicit Operation(const FatVolume &volume)
        : m_volume(volume)
    {
        volume.m_fat.clear();
    }
    ~Operation() { m_volume.m_fat.clear(); }
    Operation(const Operation &) = delete;
    Operation &operator=(const Operation &) = delete;

private:
    const FatVolume &m_volume;
};

FatVolume::FatVolume(BlockReader reader, BlockWriter writer)
    : m_read(std::move(reader))
    , m_write(std::move(writer))
{
    std::array<uint8_t, 512> mbr;
    if (!m_read(0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
        throw FormatException("Disk image has no boot sector");

    // Unpartitioned ("superfloppy") image?
    if (mountAt(0))
        return;

    for (int i = 0; i < 4; ++i) {
        const auto part = std::span<const uint8_t>(mbr).subspan(446 + i * 16, 16);
        const uint8_t type = part[4];
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E) {
            if (mountAt(le32(part, 8)))
                return;
        }
    }
    throw FormatException("Disk image has no FAT partition");
}

bool FatVolume::mountAt(uint64_t lba)
{
    std::array<uint8_t, 512> bs;
    if (!m_read(lba, bs) || bs[510] != 0x55 || bs[511] != 0xAA)
        return false;
    if (bs[0] != 0xEB && bs[0] != 0xE9)
        return false;

    const uint32_t bytesPerSector = le16(bs, 11);
    m_sectorsPerCluster = bs[13];
    const uint32_t reserved = le16(bs, 14);
    const uint32_t fats = bs[16];
    const uint32_t rootEntries = le16(bs, 17);
    uint32_t totalSectors = le16(bs, 19);
    if (totalSectors == 0)
        totalSectors = le32(bs, 32);
    m_fatSectors = le16(bs, 22);
    if (m_fatSectors == 0)
        m_fatSectors = le32(bs, 36);

    if (bytesPerSector != 512 || m_sectorsPerCluster == 0 || fats == 0 || m_fatSectors == 0)
        return false;

    m_rootDirSectors = (rootEntries * 32 + 511) / 512;
    m_fatLba = lba + reserved;
    m_fatCopies = fats;
    m_rootDirLba = m_fatLba + static_cast<uint64_t>(fats) * m_fatSectors;
    m_dataLba = m_rootDirLba + m_rootDirSectors;
    if (m_dataLba - lba >= totalSectors)
        return false;
    const uint64_t dataSectors = totalSectors - (m_dataLba - lba);
    m_clusterCount = static_cast<uint32_t>(dataSectors / m_sectorsPerCluster);

    // Classify by the FAT type the BPB declares (a FAT32 BPB has no fixed
    // root directory), not only by cluster count: some SD card images are
    // FAT32 with fewer clusters than the spec minimum.
    if (rootEntries == 0)
        m_fatBits = 32;
    else
        m_fatBits = m_clusterCount < 4085 ? 12 : 16;
    m_rootCluster = (m_fatBits == 32) ? le32(bs, 44) : 0;

    if (m_fatBits == 32) {
        // Mirroring off: only the active FAT is used
        const uint16_t flags = le16(bs, 40);
        if (flags & 0x80) {
            m_fatLba += static_cast<uint64_t>(flags & 0x0F) * m_fatSectors;
            m_fatCopies = 1;
        }
        const uint16_t fsInfo = le16(bs, 48);
        Sector info;
        if (fsInfo != 0 && fsInfo != 0xFFFF && m_read(lba + fsInfo, info) && le32(info, 0) == 0x41615252 &&
            le32(info, 484) == 0x61417272) {
            m_fsInfoLba = lba + fsInfo;
            const uint32_t hint = le32(info, 492);
            if (hint >= 2 && hint < m_clusterCount + 2)
                m_nextFree = hint;
        }
    }
    return true;
}

uint64_t FatVolume::totalBytes() const noexcept
{
    return static_cast<uint64_t>(m_clusterCount) * clusterBytes();
}

uint64_t FatVolume::clusterLba(uint32_t cluster) const noexcept
{
    return m_dataLba + static_cast<uint64_t>(cluster - 2) * m_sectorsPerCluster;
}

bool FatVolume::isEndOfChain(uint32_t cluster) const noexcept
{
    if (cluster < 2 || cluster >= m_clusterCount + 2)
        return true;
    switch (m_fatBits) {
    case 12: return cluster >= 0xFF8;
    case 16: return cluster >= 0xFFF8;
    default: return cluster >= 0x0FFFFFF8;
    }
}

uint32_t FatVolume::endOfChain() const noexcept
{
    switch (m_fatBits) {
    case 12: return 0xFFF;
    case 16: return 0xFFFF;
    default: return 0x0FFFFFFF;
    }
}

FatVolume::Sector FatVolume::readBlock(uint64_t lba) const
{
    Sector sector;
    if (!m_read(lba, sector))
        throw IoException(std::format("Can't read sector {} of the FAT volume", lba));
    return sector;
}

void FatVolume::writeBlock(uint64_t lba, std::span<const uint8_t, 512> data)
{
    if (!m_write(lba, data))
        throw IoException(std::format("Can't write sector {} of the FAT volume", lba));
}

const FatVolume::Sector &FatVolume::fatSector(uint64_t index) const
{
    auto it = m_fat.find(index);
    if (it == m_fat.end())
        it = m_fat.emplace(index, CachedSector{readBlock(m_fatLba + index), false}).first;
    return it->second.data;
}

FatVolume::Sector &FatVolume::fatSectorForWrite(uint64_t index)
{
    fatSector(index);
    auto &cached = m_fat.at(index);
    cached.dirty = true;
    return cached.data;
}

uint32_t FatVolume::fatEntry(uint32_t cluster) const
{
    switch (m_fatBits) {
    case 12: {
        // A FAT12 entry may straddle two sectors
        const uint64_t offset = cluster + cluster / 2;
        const auto low = fatSector(offset / 512)[offset % 512];
        const auto high = fatSector((offset + 1) / 512)[(offset + 1) % 512];
        const uint16_t v = static_cast<uint16_t>(low | (high << 8));
        return (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
    }
    case 16: {
        const uint64_t offset = static_cast<uint64_t>(cluster) * 2;
        return le16(fatSector(offset / 512), offset % 512);
    }
    default: {
        const uint64_t offset = static_cast<uint64_t>(cluster) * 4;
        return le32(fatSector(offset / 512), offset % 512) & 0x0FFFFFFF;
    }
    }
}

void FatVolume::setFatEntry(uint32_t cluster, uint32_t value)
{
    switch (m_fatBits) {
    case 12: {
        const uint64_t offset = cluster + cluster / 2;
        uint8_t &low = fatSectorForWrite(offset / 512)[offset % 512];
        uint8_t &high = fatSectorForWrite((offset + 1) / 512)[(offset + 1) % 512];
        if (cluster & 1) {
            low = static_cast<uint8_t>((low & 0x0F) | (value << 4));
            high = static_cast<uint8_t>(value >> 4);
        } else {
            low = static_cast<uint8_t>(value);
            high = static_cast<uint8_t>((high & 0xF0) | ((value >> 8) & 0x0F));
        }
        break;
    }
    case 16: {
        const uint64_t offset = static_cast<uint64_t>(cluster) * 2;
        put16(fatSectorForWrite(offset / 512), offset % 512, static_cast<uint16_t>(value));
        break;
    }
    default: {
        // The top four bits are reserved: keep them
        const uint64_t offset = static_cast<uint64_t>(cluster) * 4;
        Sector &sector = fatSectorForWrite(offset / 512);
        put32(sector, offset % 512, (le32(sector, offset % 512) & 0xF0000000) | (value & 0x0FFFFFFF));
        break;
    }
    }
}

std::vector<uint32_t> FatVolume::chain(uint32_t firstCluster) const
{
    std::vector<uint32_t> clusters;
    std::size_t guard = m_clusterCount + 1;
    for (uint32_t cluster = firstCluster; !isEndOfChain(cluster) && guard--; cluster = fatEntry(cluster))
        clusters.push_back(cluster);
    return clusters;
}

std::vector<uint8_t> FatVolume::readChain(uint32_t firstCluster, std::size_t maxBytes) const
{
    std::vector<uint8_t> out;
    Sector sector;
    for (uint32_t cluster : chain(firstCluster)) {
        const uint64_t lba = clusterLba(cluster);
        for (uint32_t s = 0; s < m_sectorsPerCluster && out.size() < maxBytes; ++s) {
            if (!m_read(lba + s, sector))
                return out;
            out.insert(out.end(), sector.begin(), sector.end());
        }
        if (out.size() >= maxBytes)
            break;
    }
    if (out.size() > maxBytes)
        out.resize(maxBytes);
    return out;
}

FatVolume::Directory FatVolume::readDirectory(uint32_t cluster) const
{
    Directory dir;
    dir.cluster = cluster;
    if (cluster == 0 && m_fatBits != 32) {
        for (uint32_t s = 0; s < m_rootDirSectors; ++s)
            dir.sectors.push_back(m_rootDirLba + s);
    } else {
        for (uint32_t c : chain(cluster == 0 ? m_rootCluster : cluster)) {
            for (uint32_t s = 0; s < m_sectorsPerCluster; ++s)
                dir.sectors.push_back(clusterLba(c) + s);
        }
    }
    dir.used.assign(dir.sectors.size() * kSlotsPerSector, false);
    dir.end = dir.used.size();

    std::u16string longName;
    std::size_t longNameStart = 0;
    uint8_t longNameChecksum = 0;
    int nextPart = -1; // the long name part expected next (they are stored last first)
    Sector sector;
    for (std::size_t s = 0; s < dir.sectors.size(); ++s) {
        if (!m_read(dir.sectors[s], sector)) {
            dir.end = s * kSlotsPerSector;
            return dir;
        }
        for (std::size_t i = 0; i < kSlotsPerSector; ++i) {
            const std::size_t slot = s * kSlotsPerSector + i;
            const auto e = std::span<const uint8_t>(sector).subspan(i * 32, 32);
            if (e[0] == 0x00) {
                dir.end = slot;
                return dir;
            }
            if (e[0] == 0xE5) {
                nextPart = -1;
                continue;
            }
            dir.used[slot] = true;
            if (e[11] == kAttrLongName) {
                const int part = e[0] & 0x1F;
                if (e[0] & 0x40) {
                    longName.clear();
                    longNameStart = slot;
                    longNameChecksum = e[13];
                    nextPart = part;
                }
                if (part == 0 || part != nextPart || e[13] != longNameChecksum) {
                    nextPart = -1;
                    continue;
                }
                std::u16string chars;
                for (std::size_t off : kLongNameOffsets) {
                    const uint16_t ch = le16(e, off);
                    if (ch == 0x0000 || ch == 0xFFFF)
                        break;
                    chars.push_back(static_cast<char16_t>(ch));
                }
                longName.insert(0, chars);
                nextPart = part - 1;
                continue;
            }
            if (e[11] & kAttrVolume) {
                nextPart = -1;
                continue;
            }

            Entry entry;
            std::ranges::copy(e.first(11), entry.rawName.begin());
            if (entry.rawName[0] == 0x05) // a name starting with 0xE5
                entry.rawName[0] = 0xE5;
            std::string base(entry.rawName.begin(), entry.rawName.begin() + 8);
            std::string ext(entry.rawName.begin() + 8, entry.rawName.end());
            base.erase(base.find_last_not_of(' ') + 1);
            ext.erase(ext.find_last_not_of(' ') + 1);
            // Windows NT's flags: an all lower case base name or extension
            if (e[12] & 0x08)
                std::ranges::transform(base, base.begin(), lower);
            if (e[12] & 0x10)
                std::ranges::transform(ext, ext.begin(), lower);
            entry.shortName = ext.empty() ? base : base + "." + ext;
            const bool hasLongName = nextPart == 0 && !longName.empty() &&
                                     longNameChecksum == shortNameChecksum(e.first<11>());
            if (hasLongName)
                entry.longName = utf16ToUtf8(longName);
            entry.slot = slot;
            entry.firstSlot = hasLongName ? longNameStart : slot;
            entry.firstCluster = (static_cast<uint32_t>(le16(e, 20)) << 16) | le16(e, 26);
            if (m_fatBits != 32)
                entry.firstCluster &= 0xFFFF;
            entry.size = le32(e, 28);
            entry.isDir = (e[11] & kAttrDirectory) != 0;
            dir.entries.push_back(std::move(entry));
            nextPart = -1;
        }
    }
    return dir;
}

std::optional<FatVolume::Entry> FatVolume::find(const Directory &dir, std::string_view name)
{
    const auto it = std::ranges::find_if(dir.entries, [name](const Entry &e) {
        return equalsNoCase(e.shortName, name) || (!e.longName.empty() && equalsNoCase(e.longName, name));
    });
    if (it == dir.entries.end())
        return std::nullopt;
    return *it;
}

std::optional<FatVolume::Directory> FatVolume::openDirectory(std::span<const std::string_view> path) const
{
    uint32_t cluster = 0; // the root
    for (std::string_view name : path) {
        const auto entry = find(readDirectory(cluster), name);
        if (!entry || !entry->isDir)
            return std::nullopt;
        cluster = entry->firstCluster; // ".." of a root subdirectory is 0: the root
    }
    return readDirectory(cluster);
}

std::optional<std::vector<FatVolume::DirEntry>> FatVolume::list(std::string_view path) const
{
    const Operation operation(*this);
    const auto parts = splitPath(path);
    const auto dir = openDirectory(parts);
    if (!dir)
        return std::nullopt;
    std::vector<DirEntry> out;
    for (const Entry &e : dir->entries) {
        if (e.shortName == "." || e.shortName == "..")
            continue;
        out.push_back({.name = e.longName.empty() ? e.shortName : e.longName, .size = e.isDir ? 0 : e.size,
                       .isDirectory = e.isDir});
    }
    return out;
}

std::optional<std::vector<uint8_t>> FatVolume::readFile(std::string_view path) const
{
    const Operation operation(*this);
    const auto parts = splitPath(path);
    if (parts.empty())
        return std::nullopt;
    const auto dir = openDirectory(std::span(parts).first(parts.size() - 1));
    if (!dir)
        return std::nullopt;
    const auto entry = find(*dir, parts.back());
    if (!entry || entry->isDir)
        return std::nullopt;
    return readChain(entry->firstCluster, entry->size);
}

uint64_t FatVolume::freeBytes() const
{
    const Operation operation(*this);
    uint64_t free = 0;
    for (uint32_t cluster = 2; cluster < m_clusterCount + 2; ++cluster) {
        if (fatEntry(cluster) == 0)
            ++free;
    }
    return free * clusterBytes();
}

// ── Writing ──────────────────────────────────────────────────────────────────

std::vector<uint32_t> FatVolume::allocate(std::size_t count)
{
    std::vector<uint32_t> clusters;
    for (uint32_t i = 0; i < m_clusterCount && clusters.size() < count; ++i) {
        const uint32_t cluster = 2 + (m_nextFree - 2 + i) % m_clusterCount;
        if (fatEntry(cluster) == 0)
            clusters.push_back(cluster);
    }
    if (clusters.size() < count)
        throw IoException("The disk is full");
    for (std::size_t i = 0; i < clusters.size(); ++i)
        setFatEntry(clusters[i], i + 1 < clusters.size() ? clusters[i + 1] : endOfChain());
    if (!clusters.empty())
        m_nextFree = 2 + (clusters.back() + 1 - 2) % m_clusterCount;
    m_freeDelta -= static_cast<int64_t>(clusters.size());
    return clusters;
}

bool FatVolume::hasFreeClusters(std::size_t count) const
{
    std::size_t found = 0;
    for (uint32_t cluster = 2; cluster < m_clusterCount + 2 && found < count; ++cluster) {
        if (fatEntry(cluster) == 0)
            ++found;
    }
    return found >= count;
}

void FatVolume::freeChain(uint32_t firstCluster)
{
    for (uint32_t cluster : chain(firstCluster)) {
        setFatEntry(cluster, 0);
        ++m_freeDelta;
    }
}

void FatVolume::writeClusters(std::span<const uint32_t> clusters, std::span<const uint8_t> data)
{
    std::size_t pos = 0;
    Sector sector;
    for (uint32_t cluster : clusters) {
        for (uint32_t s = 0; s < m_sectorsPerCluster; ++s) {
            sector.fill(0);
            if (pos < data.size())
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(pos), std::min<std::size_t>(512, data.size() - pos),
                            sector.begin());
            writeBlock(clusterLba(cluster) + s, sector);
            pos += 512;
        }
    }
}

void FatVolume::commit()
{
    for (const auto &[index, cached] : m_fat) {
        if (!cached.dirty)
            continue;
        for (uint32_t copy = 0; copy < m_fatCopies; ++copy)
            writeBlock(m_fatLba + static_cast<uint64_t>(copy) * m_fatSectors + index, cached.data);
    }
    m_fat.clear();

    // FAT32's free cluster count and next free hint
    if (m_fsInfoLba != 0) {
        Sector info = readBlock(m_fsInfoLba);
        const uint32_t freeCount = le32(info, 488);
        if (freeCount != 0xFFFFFFFF) {
            const int64_t updated = static_cast<int64_t>(freeCount) + m_freeDelta;
            put32(info, 488, updated >= 0 && updated <= m_clusterCount ? static_cast<uint32_t>(updated) : 0xFFFFFFFF);
        }
        put32(info, 492, m_nextFree);
        writeBlock(m_fsInfoLba, info);
    }
    m_freeDelta = 0;
}

std::vector<FatVolume::RawEntry> FatVolume::makeEntries(const Directory &dir, std::string_view name,
                                                        uint8_t attributes, uint32_t firstCluster,
                                                        uint32_t size) const
{
    const std::u16string utf16 = checkedName(name);

    const auto taken = [&dir](const ShortName &shortName) {
        return std::ranges::any_of(dir.entries, [&shortName](const Entry &e) { return e.rawName == shortName; });
    };
    // An 8.3 name in upper case needs no long name; in lower or mixed case its upper case version
    // is the short name, as Windows does
    ShortName shortName;
    bool longName = true;
    if (const auto exact = exactShortName(name); exact && !taken(*exact)) {
        shortName = *exact;
        longName = std::ranges::any_of(name, [](char c) { return c >= 'a' && c <= 'z'; });
    } else {
        shortName = generatedShortName(name, taken);
    }

    std::vector<RawEntry> entries;
    const uint8_t checksum = shortNameChecksum(shortName);
    if (longName) {
        const std::size_t parts = (utf16.size() + 12) / 13;
        for (std::size_t part = parts; part >= 1; --part) {
            RawEntry e{};
            e[0] = static_cast<uint8_t>(part | (part == parts ? 0x40 : 0x00));
            e[11] = kAttrLongName;
            e[13] = checksum;
            for (std::size_t j = 0; j < kLongNameOffsets.size(); ++j) {
                const std::size_t index = (part - 1) * 13 + j;
                const uint16_t ch = index < utf16.size() ? utf16[index] : (index == utf16.size() ? 0x0000 : 0xFFFF);
                put16(e, kLongNameOffsets[j], ch);
            }
            entries.push_back(e);
        }
    }

    RawEntry e{};
    std::ranges::copy(shortName, e.begin());
    if (e[0] == 0xE5)
        e[0] = 0x05;
    e[11] = attributes;
    const auto [date, time] = fatTimestamp();
    put16(e, 14, time); // created
    put16(e, 16, date);
    put16(e, 18, date); // accessed
    put16(e, 20, static_cast<uint16_t>(firstCluster >> 16));
    put16(e, 22, time); // written
    put16(e, 24, date);
    put16(e, 26, static_cast<uint16_t>(firstCluster));
    put32(e, 28, size);
    entries.push_back(e);
    return entries;
}

void FatVolume::addEntries(Directory &dir, std::span<const RawEntry> entries)
{
    // A run of free slots, growing the directory if there is none (not FAT12/16's root)
    const auto freeRun = [&dir, &entries]() -> std::optional<std::size_t> {
        std::size_t run = 0;
        for (std::size_t slot = 0; slot < dir.used.size(); ++slot) {
            if (dir.used[slot] && slot < dir.end) {
                run = 0;
            } else if (++run == entries.size()) {
                return slot + 1 - run;
            }
        }
        return std::nullopt;
    };
    auto start = freeRun();
    while (!start) {
        if (dir.cluster == 0 && m_fatBits != 32)
            throw IoException("The root directory is full");
        const auto clusters = chain(dir.cluster == 0 ? m_rootCluster : dir.cluster);
        if (clusters.empty())
            throw IoException("A directory has no clusters");
        const auto added = allocate(1);
        setFatEntry(clusters.back(), added.front());
        writeClusters(added, {});
        for (uint32_t s = 0; s < m_sectorsPerCluster; ++s)
            dir.sectors.push_back(clusterLba(added.front()) + s);
        dir.used.resize(dir.sectors.size() * kSlotsPerSector, false);
        start = freeRun();
    }

    // Past the end marker, the slot after the new entries must end the directory again
    const std::size_t end = *start + entries.size();
    const bool needsEndMarker = end > dir.end && end < dir.used.size();
    std::size_t sectorIndex = SIZE_MAX;
    Sector sector;
    const auto flush = [&] {
        if (sectorIndex != SIZE_MAX)
            writeBlock(dir.sectors[sectorIndex], sector);
    };
    for (std::size_t slot = *start; slot < end + (needsEndMarker ? 1 : 0); ++slot) {
        if (slot / kSlotsPerSector != sectorIndex) {
            flush();
            sectorIndex = slot / kSlotsPerSector;
            sector = readBlock(dir.sectors[sectorIndex]);
        }
        const auto target = std::span(sector).subspan((slot % kSlotsPerSector) * 32, 32);
        if (slot < end) {
            std::ranges::copy(entries[slot - *start], target.begin());
            dir.used[slot] = true;
        } else {
            std::ranges::fill(target, 0); // the end marker
        }
    }
    flush();
    dir.end = std::max(dir.end, end);
}

void FatVolume::removeEntry(const Directory &dir, const Entry &entry)
{
    freeChain(entry.firstCluster);
    std::size_t sectorIndex = SIZE_MAX;
    Sector sector;
    for (std::size_t slot = entry.firstSlot; slot <= entry.slot; ++slot) {
        if (slot / kSlotsPerSector != sectorIndex) {
            if (sectorIndex != SIZE_MAX)
                writeBlock(dir.sectors[sectorIndex], sector);
            sectorIndex = slot / kSlotsPerSector;
            sector = readBlock(dir.sectors[sectorIndex]);
        }
        sector[(slot % kSlotsPerSector) * 32] = 0xE5;
    }
    writeBlock(dir.sectors[sectorIndex], sector);
}

void FatVolume::writeFile(std::string_view path, std::span<const uint8_t> data)
{
    const Operation operation(*this, true);
    const auto parts = splitPath(path);
    if (parts.empty())
        throw InvalidArgumentException("No file name");
    if (data.size() > 0xFFFFFFFF)
        throw InvalidArgumentException("FAT files are at most 4 GB");
    auto dir = openDirectory(std::span(parts).first(parts.size() - 1));
    if (!dir)
        throw InvalidArgumentException(std::format("No directory for '{}'", path));

    checkedName(parts.back());

    // Nothing changes before it is sure the file fits (a replaced file's clusters are reused)
    const std::size_t needed = (data.size() + clusterBytes() - 1) / clusterBytes();
    const auto existing = find(*dir, parts.back());
    if (existing && existing->isDir)
        throw InvalidArgumentException(std::format("'{}' is a directory", path));
    const std::size_t reused = existing ? chain(existing->firstCluster).size() : 0;
    if (needed > reused && !hasFreeClusters(needed - reused))
        throw IoException("The disk is full");
    if (existing) {
        removeEntry(*dir, *existing);
        dir = readDirectory(dir->cluster);
    }
    const auto clusters = allocate(needed);
    writeClusters(clusters, data);
    const auto entries = makeEntries(*dir, parts.back(), kAttrArchive, clusters.empty() ? 0 : clusters.front(),
                                     static_cast<uint32_t>(data.size()));
    addEntries(*dir, entries);
    commit();
}

void FatVolume::makeDirectory(std::string_view path)
{
    const Operation operation(*this, true);
    uint32_t parent = 0; // the root
    for (std::string_view name : splitPath(path)) {
        Directory dir = readDirectory(parent);
        if (const auto existing = find(dir, name)) {
            if (!existing->isDir)
                throw InvalidArgumentException(std::format("'{}' is a file", name));
            parent = existing->firstCluster;
            continue;
        }

        checkedName(name);
        const auto clusters = allocate(1);
        // "." and ".." (0 for the root, even on FAT32)
        std::array<uint8_t, 64> dots{};
        const auto [date, time] = fatTimestamp();
        for (std::size_t i = 0; i < 2; ++i) {
            const auto e = std::span(dots).subspan(i * 32, 32);
            std::ranges::fill(e.first(11), ' ');
            e[0] = '.';
            if (i == 1)
                e[1] = '.';
            e[11] = kAttrDirectory;
            const uint32_t cluster = i == 0 ? clusters.front() : parent;
            put16(e, 14, time);
            put16(e, 16, date);
            put16(e, 18, date);
            put16(e, 20, static_cast<uint16_t>(cluster >> 16));
            put16(e, 22, time);
            put16(e, 24, date);
            put16(e, 26, static_cast<uint16_t>(cluster));
        }
        writeClusters(clusters, dots);
        addEntries(dir, makeEntries(dir, name, kAttrDirectory, clusters.front(), 0));
        parent = clusters.front();
    }
    commit();
}

void FatVolume::remove(std::string_view path)
{
    const Operation operation(*this, true);
    const auto parts = splitPath(path);
    if (parts.empty())
        throw InvalidArgumentException("The root directory can't be removed");
    const auto dir = openDirectory(std::span(parts).first(parts.size() - 1));
    const auto entry = dir ? find(*dir, parts.back()) : std::nullopt;
    if (!entry || entry->shortName == "." || entry->shortName == "..")
        throw InvalidArgumentException(std::format("No '{}' to remove", path));
    if (entry->isDir) {
        const auto content = readDirectory(entry->firstCluster);
        if (std::ranges::any_of(content.entries, [](const Entry &e) { return e.shortName != "." && e.shortName != ".."; }))
            throw InvalidArgumentException(std::format("The directory '{}' isn't empty", path));
    }
    removeEntry(*dir, *entry);
    commit();
}

} // namespace zxkit
