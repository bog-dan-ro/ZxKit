// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "DiskContainer.h"
#include <numeric>
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
constexpr Profile profiles[]{
    { "plus3"sv, "plus3"sv, 40, 1, 9, 512, 1024, 64, 1 },
    { "plus3-720"sv, "plus3"sv, 80, 2, 9, 512, 2048, 256, 1 },
    { "cpm-360"sv, "cpm"sv, 40, 2, 9, 512, 2048, 64, 2, 2 },
    { "cpm-720"sv, "cpm"sv, 80, 2, 9, 512, 2048, 128, 4, 2 },
    { "cobra-cpm"sv, "cpm"sv, 80, 2, 9, 512, 2048, 128, 4, 2 },
    { "hc-basic-40"sv, "hc"sv, 40, 2, 16, 256, 2048, 128, 0, 2 },
    { "hc-basic-80"sv, "hc"sv, 80, 2, 16, 256, 2048, 128, 0, 2 },
    { "cip04"sv, "plus3"sv, 40, 1, 9, 512, 1024, 64, 1 },
    { "cip04-360"sv, "plus3"sv, 80, 1, 9, 512, 2048, 64, 1 },
    { "plus3-pcw"sv, "plus3"sv, 40, 2, 9, 512, 2048, 64, 1 },
    { "plus3-203"sv, "plus3"sv, 42, 1, 10, 512, 1024, 64, 1, 3 },
    { "trdos"sv, "trdos"sv, 80, 2, 16, 256, 256, 128, 0 },
    { "trdos-40"sv, "trdos"sv, 40, 2, 16, 256, 256, 128, 0 },
    { "trdos-ss"sv, "trdos"sv, 80, 1, 16, 256, 256, 128, 0 },
    { "trdos-40ss"sv, "trdos"sv, 40, 1, 16, 256, 256, 128, 0 },
    { "mgt"sv, "mgt"sv, 80, 2, 10, 512, 512, 80, 0 },
    { "opus"sv, "opus"sv, 40, 1, 18, 256, 256, 112, 0, 1, 0 },
    { "opus-720"sv, "opus"sv, 80, 2, 18, 256, 256, 112, 0, 1, 0 },
    { "mdos40"sv, "mdos"sv, 40, 2, 9, 512, 512, 128, 0 },
    { "mdos80"sv, "mdos"sv, 80, 2, 9, 512, 512, 128, 0 },
    { "devil"sv, "devil"sv, 80, 2, 18, 256, 9216, 108, 0 },
};
uint16_t tdcrc(View data)
{
    uint16_t c = 0;
    for (auto b : data) {
        c ^= uint16_t(b) << 8;
        for (unsigned i = 0; i < 8; ++i)
            c = uint16_t((c << 1) ^ ((c & 0x8000) ? 0xA097 : 0));
    }
    return c;
}
uint32_t qmcrc(View data)
{
    // CopyQM masks the lookup index to six bits (the original implementation's quirk).
    uint32_t c = 0;
    for (auto b : data) {
        uint32_t t = (c ^ b) & 63;
        for (unsigned i = 0; i < 8; ++i)
            t = (t >> 1) ^ ((t & 1) ? 0xEDB88320 : 0);
        c = (c >> 8) ^ t;
    }
    return c;
}
std::vector<unsigned> sectorOrder(const Profile &p, unsigned track)
{
    std::vector<unsigned> out;
    std::vector<bool> used(p.sectors);
    unsigned n = 0;
    unsigned skew = track < p.bootTracks ? 1 : p.skew;
    for (unsigned i = 0; i < p.sectors; ++i) {
        while (used[n])
            n = (n + 1) % p.sectors;
        out.push_back(n + 1);
        used[n] = true;
        n = (n + skew) % p.sectors;
    }
    return out;
}
Bytes rawToCqm(View raw, const Profile &p)
{
    Bytes out(133);
    textAt(out, 0, "CQ\x14"sv);
    setWord(out, 3, p.sectorSize);
    setWord(out, 0x10, p.sectors);
    setWord(out, 0x12, p.sides);
    textAt(out, 0x1C, "ZxKit disk image"sv);
    out[0x58] = 1;
    out[0x5A] = out[0x5B] = uint8_t(p.cylinders);
    setDword(out, 0x5C, qmcrc(raw));
    std::fill(out.begin() + 0x60, out.begin() + 0x6B, ' ');
    out[0x74] = 1;
    unsigned sum = 0;
    for (std::size_t i = 0; i < 132; ++i)
        sum += out[i];
    out[132] = uint8_t(-sum);
    for (std::size_t i = 0; i < raw.size();) {
        std::size_t count = std::min<std::size_t>(32767, raw.size() - i);
        auto n = out.size();
        out.resize(n + 2);
        setWord(out, n, count);
        append(out, raw.subspan(i, count));
        i += count;
    }
    return out;
}
Bytes cqmToRaw(View b, Profile &p)
{
    require(b.size() >= 133 && b[0] == 'C' && b[1] == 'Q' && b[2] == 20, "Invalid CQM header"sv);
    unsigned sum = 0;
    for (auto c : b.first(133))
        sum += c;
    require((sum & 255) == 0, "CQM header checksum mismatch"sv);
    require(word(b, 3) == p.sectorSize && word(b, 0x10) == p.sectors && word(b, 0x12) == p.sides
                    && b[0x5B] == p.cylinders && b[0x71] == 0,
            "CQM geometry does not match selected profile"sv);
    std::size_t max = std::size_t(p.cylinders) * p.sides * p.sectors * p.sectorSize;
    Bytes raw;
    std::size_t pos = 133 + word(b, 0x6F);
    std::size_t used = std::size_t(b[0x5A]) * p.sides * p.sectors * p.sectorSize;
    require(used <= max, "CQM used cylinders exceed geometry"sv);
    while (pos < b.size() && raw.size() < used) {
        auto count = int16_t(word(b, pos));
        pos += 2;
        if (!count)
            continue;
        std::size_t n = count < 0 ? -int(count) : count;
        require(n <= used - raw.size(), "CQM data exceeds geometry"sv);
        if (count < 0) {
            raw.insert(raw.end(), n, slice(b, pos, 1)[0]);
            ++pos;
        } else {
            append(raw, slice(b, pos, n));
            pos += n;
        }
    }
    // Missing final tracks in CopyQM are filler, not arbitrary uninitialised bytes.
    require(raw.size() == used, "Truncated CQM data"sv);
    require(!dword(b, 0x5C) || qmcrc(raw) == dword(b, 0x5C), "CQM data checksum mismatch"sv);
    raw.resize(max, 0xF6);
    return raw;
}
} // namespace
std::span<const Profile> diskProfiles()
{
    return profiles;
}
DiskContainer DiskContainer::open(View b, std::string_view extension, const MediaOptions &opts,
                                  bool create)
{
    std::string ext = archive::extension(extension);
    std::string_view selected = opts.filesystem;
    if (ext == ".p3d"sv) {
        DiskContainer result;
        result.ext = ext;
        if (create) {
            result.profile = { "next-cpm"sv, "plus3"sv, 256, 1, 128, 512, 8192, 512, 0 };
            result.prefix.resize(512);
            auto &h = result.prefix;
            textAt(h, 0, "P3D\x1A"sv);
            setWord(h, 4, 512);
            h[6] = 6;
            h[7] = 63;
            h[8] = 3;
            setWord(h, 9, 2047);
            setWord(h, 11, 511);
            h[13] = 0xC0;
            setWord(h, 15, 0x8000);
            h[19] = 2;
            h[20] = 3;
            h[23] = 128;
            setWord(h, 25, 512);
            h[31] = 16;
            h[32] = 3;
            setDword(h, 39, 32767);
            unsigned sum = 0;
            for (unsigned n = 4; n < 48; ++n)
                sum += h[n];
            h[511] = uint8_t(sum);
            result.logical.resize(16 * 1024 * 1024, 0xE5);
        } else {
            require(b.size() >= 512
                            && std::string_view(reinterpret_cast<const char *>(b.data()), 4)
                                    == "P3D\x1A"sv,
                    "Invalid P3D signature"sv);
            unsigned sum = 0;
            for (unsigned n = 4; n < 48; ++n)
                sum += b[n];
            require(uint8_t(sum) == b[511], "P3D checksum mismatch"sv);
            require(b[32] == 3 && b[6] >= 4 && b[6] <= 6 && word(b, 17) == 0,
                    "Unsupported P3D geometry"sv);
            unsigned block = 128u << b[6];
            unsigned count = word(b, 9) + 1;
            unsigned dirs = word(b, 11) + 1;
            require(b.size() == 512 + std::size_t(block) * count && dirs * 32 <= block * 8,
                    "P3D size disagrees with DPB"sv);
            require(b[8] == block * 8 / 16384 - 1, "Invalid P3D extent mask"sv);
            result.profile = { "next-cpm"sv, "plus3"sv, 0, 1, 128, 512, block, dirs, 0 };
            result.prefix.assign(b.begin(), b.begin() + 512);
            result.logical.assign(b.begin() + 512, b.end());
        }
        return result;
    }
    if (selected.empty()) {
        if (ext == ".trd"sv || ext == ".scl"sv) {
            selected = "trdos"sv;
            if (!create && ext == ".trd"sv && b.size() > 8 * 256 + 0xE3) {
                auto type = b[8 * 256 + 0xE3];
                if (type == 0x17)
                    selected = "trdos-40"sv;
                else if (type == 0x18)
                    selected = "trdos-ss"sv;
                else if (type == 0x19)
                    selected = "trdos-40ss"sv;
            }
        } else if (ext == ".mgt"sv)
            selected = "mgt"sv;
        else if (ext == ".opd"sv)
            selected = !create && b.size() == 737280 ? "opus-720"sv : "opus"sv;
        else if (ext == ".d40"sv || ext == ".d80"sv)
            selected = ext == ".d40"sv ? "mdos40"sv : "mdos80"sv;
        else if (ext == ".dsk"sv || ext == ".edsk"sv) {
            selected = "plus3"sv;
            if (!create && b.size() >= 512) {
                if (b[0x30] == 80 && b[0x31] == 2)
                    selected = "plus3-720"sv;
                else if (b[0x30] == 40 && b[0x31] == 2)
                    selected = "plus3-pcw"sv;
                else if (b[0x30] == 42 && b[0x31] == 1)
                    selected = "plus3-203"sv;
                else if (b[0x30] == 80 && b[0x31] == 1)
                    selected = "cip04-360"sv;
            }
        } else
            throw FormatException(
                    "Select a filesystem with --filesystem; raw/TD0/CQM images are ambiguous"sv);
    }
    auto i = std::ranges::find(profiles, selected, &Profile::name);
    require(i != std::end(profiles), "Unknown filesystem profile"sv);
    DiskContainer result;
    result.profile = *i;
    result.ext = ext;
    auto &p = result.profile;
    result.image.formatBlank(uint8_t(p.cylinders), uint8_t(p.sides), uint8_t(p.sectors),
                             uint16_t(p.sectorSize), uint8_t(p.firstSector));
    Bytes decoded;
    if (!create) {
        if (ext == ".cqm"sv) {
            decoded = cqmToRaw(b, p);
            b = decoded;
        }
        const bool dskHeader = b.size() >= 8
                && (std::ranges::equal(b.first(8), "EXTENDED"sv)
                    || std::ranges::equal(b.first(8), "MV - CPC"sv));
        bool container = ext == ".td0"sv || dskHeader || ext == ".scl"sv;
        if (container) {
            if (ext == ".dsk"sv || ext == ".edsk"sv) {
                require(b.size() >= 256 && b[0x30] > 0 && (b[0x31] == 1 || b[0x31] == 2),
                        "Invalid DSK geometry header"sv);
                bool extended = b[0] == 'E';
                std::size_t tracks = std::size_t(b[0x30]) * b[0x31];
                std::size_t pos = 256;
                require(!extended || tracks <= 204, "EDSK track table exceeds header"sv);
                for (std::size_t n = 0; n < tracks; ++n) {
                    std::size_t size = extended ? std::size_t(b[0x34 + n]) * 256 : word(b, 0x32);
                    if (!size)
                        continue;
                    auto track = slice(b, pos, size);
                    require(size >= 256 && track[0x15] <= 29, "Invalid DSK track header"sv);
                    std::size_t used = 256;
                    for (unsigned k = 0; k < track[0x15]; ++k) {
                        auto e = slice(track, 0x18 + k * 8, 8);
                        require(e[3] <= 7, "Invalid DSK sector size"sv);
                        used += extended ? word(e, 6) : (std::size_t{ 128 } << (track[0x14] & 7));
                        require(used <= size, "Truncated DSK sector data"sv);
                    }
                    pos += size;
                }
                require(pos == b.size(), "DSK has unexplained trailing data"sv);
            }
            if (ext == ".scl"sv) {
                require(b.size() >= 13 && b[8] <= 128, "Invalid SCL directory"sv);
                std::size_t expected = 9 + std::size_t(b[8]) * 14 + 4;
                for (unsigned n = 0; n < b[8]; ++n)
                    expected += std::size_t(slice(b, 9 + n * 14, 14)[13]) * 256;
                require(expected == b.size(), "SCL data length mismatch"sv);
                uint32_t sum = 0;
                for (auto c : b.first(b.size() - 4))
                    sum += c;
                require(sum == dword(b, b.size() - 4), "SCL checksum mismatch"sv);
            }
            result.image.loadFromBytes(b, ext);
            require(result.image.trackCount() >= p.cylinders && result.image.sideCount() == p.sides,
                    "Disk geometry differs from selected profile"sv);
        } else {
            Bytes expanded;
            const std::size_t expected =
                    std::size_t(p.cylinders) * p.sides * p.sectors * p.sectorSize;
            if (p.fs == "trdos"sv && b.size() < expected) {
                require(b.size() >= 4096 && b.size() % 256 == 0, "Invalid trimmed TRD size"sv);
                for (unsigned slot = 0; slot < 128; ++slot) {
                    const auto entry = slice(b, slot * 16, 16);
                    if (!entry[0])
                        break;
                    if (entry[0] == 1)
                        continue;
                    const std::size_t end =
                            (std::size_t(entry[15]) * 16 + entry[14] + entry[13]) * 256;
                    require(end <= b.size(), "Trimmed TRD cuts off allocated file data"sv);
                }
                expanded.assign(b.begin(), b.end());
                expanded.resize(expected, 0);
                b = expanded;
            }
            require(b.size() == expected, "Raw disk size differs from selected profile"sv);
            for (unsigned t = 0; t < p.cylinders; ++t)
                for (unsigned h = 0; h < p.sides; ++h)
                    for (unsigned n = 1; n <= p.sectors; ++n) {
                        auto offset = ((t * p.sides + h) * p.sectors + n - 1) * p.sectorSize;
                        // MGT raw images store all of side zero before side one.
                        if (p.fs == "mgt"sv && (ext == ".img"sv || ext == ".raw"sv))
                            offset = ((h * p.cylinders + t) * p.sectors + n - 1) * p.sectorSize;
                        auto &sector = result.image.findSector(t, h, n + p.firstSector - 1)->get();
                        auto data = slice(b, offset, p.sectorSize);
                        sector.data.assign(data.begin(), data.end());
                    }
        }
    }
    for (unsigned a = 0; a < (p.fs == "mgt"sv ? p.sides : p.cylinders); ++a)
        for (unsigned b = 0; b < (p.fs == "mgt"sv ? p.cylinders : p.sides); ++b) {
            unsigned t = p.fs == "mgt"sv ? b : a;
            unsigned h = p.fs == "mgt"sv ? a : b;
            require(result.image.trackSectors(t, h).size() == p.sectors,
                    "Filesystem sector count differs from selected profile"sv);
            for (auto n : sectorOrder(p, t * p.sides + h)) {
                auto sec = result.image.findSector(t, h, n + p.firstSector - 1);
                require(bool(sec), "Filesystem sector missing"sv);
                require(sec->get().data.size() == p.sectorSize,
                        "Filesystem sector size differs (weak/protected sector)"sv);
                require(!(sec->get().st1 & 0x25) && !(sec->get().st2 & 0x25),
                        "Filesystem sector has disk read errors"sv);
                append(result.logical, sec->get().data);
            }
        }
    return result;
}
Bytes DiskContainer::encode(std::string_view extension) const
{
    auto target = extension.empty() ? ext : archive::extension(extension);
    if (!prefix.empty()) {
        require(target == ".p3d"sv, "P3D conversion requires copying logical files"sv);
        Bytes out = prefix;
        append(out, logical);
        return out;
    }
    auto disk = image;
    auto &p = profile;
    std::size_t pos = 0;
    for (unsigned a = 0; a < (p.fs == "mgt"sv ? p.sides : p.cylinders); ++a)
        for (unsigned b = 0; b < (p.fs == "mgt"sv ? p.cylinders : p.sides); ++b) {
            unsigned t = p.fs == "mgt"sv ? b : a;
            unsigned h = p.fs == "mgt"sv ? a : b;
            for (auto n : sectorOrder(p, t * p.sides + h)) {
                auto &s = disk.findSector(t, h, n + p.firstSector - 1)->get();
                auto data = slice(logical, pos, p.sectorSize);
                s.data.assign(data.begin(), data.end());
                pos += p.sectorSize;
            }
        }
    if (target == ".scl"sv) {
        require(p.fs == "trdos"sv, "SCL requires TR-DOS"sv);
        Bytes out(9);
        textAt(out, 0, "SINCLAIR"sv);
        Bytes payload;
        for (std::size_t i = 0; i < 128; ++i) {
            auto e = slice(logical, i * 16, 16);
            if (e[0] < 2)
                continue;
            require(out[8] < 128, "Too many SCL files"sv);
            ++out[8];
            append(out, e.first(14));
            auto start = (e[15] * 16 + e[14]) * 256;
            append(payload, slice(logical, start, std::size_t(e[13]) * 256));
        }
        append(out, payload);
        uint32_t sum = 0;
        for (auto c : out)
            sum += c;
        auto n = out.size();
        out.resize(n + 4);
        setDword(out, n, sum);
        return out;
    }
    if (target == ".td0"sv) {
        Bytes out(12);
        out[0] = 'T';
        out[1] = 'D';
        out[4] = 21;
        out[6] = 3;
        out[9] = uint8_t(p.sides);
        setWord(out, 10, tdcrc(View(out).first(10)));
        for (unsigned t = 0; t < disk.trackCount(); ++t)
            for (unsigned h = 0; h < disk.sideCount(); ++h) {
                auto sectors = disk.trackSectors(t, h);
                require(sectors.size() <= 255, "Too many TD0 sectors"sv);
                Bytes track{ uint8_t(sectors.size()), uint8_t(t), uint8_t(h), 0 };
                track[3] = uint8_t(tdcrc(View(track).first(3)));
                append(out, track);
                for (auto &ref : sectors) {
                    auto &s = ref.get();
                    require(s.data.size() == (std::size_t{ 128 } << s.sizeCode),
                            "TD0 cannot preserve weak-sector copies"sv);
                    Bytes header{ s.track,
                                  s.side,
                                  s.sectorId,
                                  s.sizeCode,
                                  uint8_t((s.st2 & 0x40 ? 4 : 0) | (s.st2 & 0x20 ? 2 : 0)),
                                  uint8_t(tdcrc(s.data)) };
                    append(out, header);
                    auto n = out.size();
                    out.resize(n + 3);
                    setWord(out, n, s.data.size() + 1);
                    append(out, s.data);
                }
            }
        out.push_back(255);
        return out;
    }
    if (target == ".dsk"sv || target == ".edsk"sv) {
        Bytes out(256);
        textAt(out, 0, "EXTENDED CPC DSK File\r\nDisk-Info\r\n"sv);
        textAt(out, 0x22, "ZxKit"sv);
        require(unsigned(disk.trackCount()) * disk.sideCount() <= 204,
                "Too many tracks for EDSK"sv);
        out[0x30] = disk.trackCount();
        out[0x31] = disk.sideCount();
        for (unsigned t = 0; t < disk.trackCount(); ++t)
            for (unsigned h = 0; h < disk.sideCount(); ++h) {
                auto sectors = disk.trackSectors(t, h);
                require(sectors.size() <= 29, "Too many EDSK sectors"sv);
                Bytes track(256);
                textAt(track, 0, "Track-Info\r\n"sv);
                track[0x10] = t;
                track[0x11] = h;
                track[0x14] = p.sectorSize == 256 ? 1 : 2;
                track[0x15] = uint8_t(sectors.size());
                track[0x16] = 0x4E;
                track[0x17] = 0xE5;
                std::size_t n = 0;
                for (auto &ref : sectors) {
                    auto &s = ref.get();
                    auto offset = 0x18 + n++ * 8;
                    track[offset] = s.track;
                    track[offset + 1] = s.side;
                    track[offset + 2] = s.sectorId;
                    track[offset + 3] = s.sizeCode;
                    track[offset + 4] = s.st1;
                    track[offset + 5] = s.st2;
                    setWord(track, offset + 6, s.data.size());
                    append(track, s.data);
                }
                track.resize((track.size() + 255) & ~std::size_t(255));
                require(track.size() / 256 <= 255, "EDSK track too large"sv);
                out[0x34 + t * disk.sideCount() + h] = uint8_t(track.size() / 256);
                append(out, track);
            }
        return out;
    }
    require(target == ".img"sv || target == ".raw"sv || target == ".trd"sv || target == ".mgt"sv
                    || target == ".opd"sv || target == ".d40"sv || target == ".d80"sv
                    || target == ".cqm"sv,
            "Unsuitable disk container conversion"sv);
    if (target == ".trd"sv)
        require(p.fs == "trdos"sv, "TRD requires a TR-DOS filesystem"sv);
    if (target == ".mgt"sv)
        require(p.fs == "mgt"sv, "MGT requires an MGT filesystem"sv);
    if (target == ".opd"sv)
        require(p.fs == "opus"sv, "OPD requires Opus Discovery"sv);
    require(disk.trackCount() == p.cylinders,
            "Raw/CQM output would discard extra recorded tracks; use EDSK or TD0"sv);
    Bytes raw;
    const bool sideOrder = p.fs == "mgt"sv && (target == ".img"sv || target == ".raw"sv);
    for (unsigned a = 0; a < (sideOrder ? p.sides : p.cylinders); ++a)
        for (unsigned b = 0; b < (sideOrder ? p.cylinders : p.sides); ++b)
            for (unsigned n = 1; n <= p.sectors; ++n) {
                auto &s =
                        disk.findSector(sideOrder ? b : a, sideOrder ? a : b, n + p.firstSector - 1)
                                ->get();
                require(!s.st1 && !s.st2 && s.data.size() == p.sectorSize,
                        "Raw output would lose sector metadata"sv);
                append(raw, s.data);
            }
    if (target == ".cqm"sv)
        return rawToCqm(raw, p);
    return raw;
}
} // namespace zxkit::archive
