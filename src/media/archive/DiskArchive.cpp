// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "DiskContainer.h"
#include <set>
#include <charconv>
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
struct Disk final : Backend
{
    DiskContainer storage;
    bool cpm() const
    {
        auto fs = storage.profile.fs;
        return fs == "cpm"sv || fs == "hc"sv || fs == "plus3"sv;
    }
    std::size_t base() const
    {
        auto &p = storage.profile;
        return p.bootTracks * p.sectors * p.sectorSize;
    }
    std::size_t blocks() const
    {
        return (storage.logical.size() - base()) / storage.profile.blockSize;
    }
    std::size_t dirPos(unsigned n) const
    {
        return base() + n * 32;
    }
    bool wide() const
    {
        return storage.profile.fs == "hc"sv || blocks() > 256;
    }
    unsigned extentMask() const
    {
        return storage.profile.blockSize * (wide() ? 8 : 16) / 16384 - 1;
    }
    struct File
    {
        MediaEntry entry;
        Bytes bytes;
        std::vector<unsigned> slots;
        std::string nativeName;
        Bytes nativeHeader;
    };
    std::pair<unsigned, std::string> cpmName(std::string_view path) const
    {
        unsigned user = 0;
        if (path.starts_with("user"sv) && path.size() > 7 && path[6] == '/') {
            require(path.size() > 7 && path[6] == '/', "Use user01/NAME for CP/M user areas"sv);
            auto r = std::from_chars(path.data() + 4, path.data() + 6, user);
            require(r.ec == std::errc{} && r.ptr == path.data() + 6 && user <= 15,
                    "Invalid CP/M user area"sv);
            path.remove_prefix(7);
        }
        auto name = storage.profile.fs == "hc"sv ? std::string(path) : upper(path);
        validName(name, 12);
        auto dot = name.find('.');
        auto stem = name.substr(0, dot);
        auto ext = dot == name.npos ? ""s : name.substr(dot + 1);
        require(stem.size() >= 1 && stem.size() <= 8 && ext.size() <= 3
                        && ext.find('.') == ext.npos,
                "Use CP/M 8.3 names"sv);
        for (char c : name)
            require(std::string_view("<>.,;:=?*[]"sv).find(c) == std::string_view::npos || c == '.',
                    "Invalid CP/M filename"sv);
        std::string field =
                stem + std::string(8 - stem.size(), ' ') + ext + std::string(3 - ext.size(), ' ');
        return { user, field };
    }
    std::string cpmPath(unsigned user, std::string_view field) const
    {
        auto stem = field.substr(0, 8);
        auto ext = field.substr(8, 3);
        while (stem.ends_with(' '))
            stem.remove_suffix(1);
        while (ext.ends_with(' '))
            ext.remove_suffix(1);
        if (storage.profile.fs == "hc"sv && stem.empty() && ext.empty())
            return (user ? std::format("user{:02}/", user) : ""s) + "%20"s;
        return (user ? std::format("user{:02}/", user) : ""s) + std::string(stem)
                + (ext.empty() ? ""s : "."s + std::string(ext));
    }
    std::string canonicalName(std::string_view name) const
    {
        if (!cpm() || storage.profile.fs == "hc"sv)
            return std::string(name);
        auto [user, field] = cpmName(name);
        return cpmPath(user, field);
    }
    std::vector<File> cpmFiles() const
    {
        auto &data = storage.logical;
        auto &p = storage.profile;
        std::map<std::string, File> map;
        bool recognizedDirectory = false;
        for (unsigned n = 0; n < p.dirEntries; ++n) {
            auto e = slice(data, dirPos(n), 32);
            if (e[0] > 15) {
                recognizedDirectory |= e[0] == 0xE5 || e[0] == 0x20 || e[0] == 0x21;
                continue;
            }
            if (std::ranges::all_of(e, [](uint8_t value) { return value == 0; }))
                continue;
            recognizedDirectory = true;
            std::string field;
            for (auto c : e.subspan(1, 11))
                field += char(c & 127);
            const bool hcBlankName =
                    p.fs == "hc"sv && std::ranges::all_of(field, [](char c) { return c == ' '; });
            require(hcBlankName
                            || std::ranges::any_of(field,
                                                   [](char c) { return c != 0 && c != ' '; }),
                    "CP/M directory entry has no filename"sv);
            auto path = cpmPath(e[0], field);
            auto &f = map[path];
            f.nativeName = field;
            f.entry.name = path;
            f.slots.push_back(n);
            f.entry.attributes = { { "user"s, e[0] },
                                   { "readonly"s, bool(e[9] & 128) },
                                   { "system"s, bool(e[10] & 128) },
                                   { "archive"s, bool(e[11] & 128) } };
            std::size_t extent = (e[12] & 31) | ((e[14] & 63) << 5);
            std::size_t start = (extent & ~extentMask()) * 16384;
            std::size_t length = ((extent & extentMask()) * 128 + e[15]) * 128;
            require(e[15] <= 128 && length <= p.blockSize * (wide() ? 8 : 16)
                            && start + length <= 32 * 1024 * 1024,
                    "Invalid CP/M extent length"sv);
            if (f.bytes.size() < start + length)
                f.bytes.resize(start + length, 0x1A);
            std::size_t remaining = length;
            std::size_t offset = start;
            for (unsigned i = 0; i < (wide() ? 8 : 16) && remaining; ++i) {
                unsigned block = wide() ? word(e, 16 + 2 * i) : e[16 + i];
                require(block >= (p.dirEntries * 32 + p.blockSize - 1) / p.blockSize
                                && block < blocks(),
                        "Invalid CP/M allocation block"sv);
                std::size_t count = std::min<std::size_t>(p.blockSize, remaining);
                auto b = slice(data, base() + block * p.blockSize, count);
                std::ranges::copy(b, f.bytes.begin() + offset);
                offset += count;
                remaining -= count;
            }
            require(!remaining, "Incomplete CP/M extent allocation"sv);
        }
        require(recognizedDirectory, "CP/M disk has no recognizable directory"sv);
        std::vector<File> out;
        for (auto &[path, f] : map) {
            if (p.fs == "plus3"sv && f.bytes.size() >= 128
                && std::string_view(reinterpret_cast<const char *>(f.bytes.data()), 8)
                        == "PLUS3DOS"sv) {
                unsigned sum = 0;
                for (auto c : View(f.bytes).first(127))
                    sum += c;
                require(uint8_t(sum) == f.bytes[127], "+3DOS checksum mismatch"sv);
                auto size = dword(f.bytes, 11);
                require(size >= 128 && size <= f.bytes.size(), "Invalid +3DOS size"sv);
                f.entry.attributes["type"s] = f.bytes[15];
                f.entry.attributes["param1"s] = word(f.bytes, 18);
                f.entry.attributes["param2"s] = word(f.bytes, 20);
                f.bytes = Bytes(f.bytes.begin() + 128, f.bytes.begin() + size);
            } else if (p.fs == "hc"sv && f.bytes.size() >= 9 && f.bytes[0] <= 3
                       && word(f.bytes, 1) <= f.bytes.size() - 9) {
                auto size = word(f.bytes, 1);
                require(size <= f.bytes.size() - 9, "Invalid HC BASIC file size"sv);
                f.entry.attributes["type"s] = f.bytes[0];
                f.entry.attributes["param1"s] =
                        f.bytes[0] == 0 ? word(f.bytes, 7) : word(f.bytes, 3);
                f.entry.attributes["param2"s] = word(f.bytes, 5);
                f.bytes = Bytes(f.bytes.begin() + 9, f.bytes.begin() + 9 + size);
            }
            if (p.fs == "hc"sv && !f.entry.attributes.contains("type"sv))
                f.entry.attributes["raw"s] = 1;
            f.entry.size = f.bytes.size();
            out.push_back(std::move(f));
        }
        return out;
    }
    bool matchesCpmProfile() const
    {
        const auto &profile = storage.profile;
        // Container geometry rules out a 40-track interpretation of an 80-track disk.
        // Two additional recorded cylinders are common in archival images.
        if (storage.image.trackCount() > profile.cylinders + 2)
            return false;
        const auto entries = cpmFiles();
        if (entries.empty())
            return false;
        std::set<unsigned> allocated;
        for (const auto &file : entries) {
            std::set<unsigned> extents;
            for (const auto slot : file.slots) {
                const auto entry = slice(storage.logical, dirPos(slot), 32);
                const unsigned extent = (entry[12] & 31) | ((entry[14] & 63) << 5);
                if (!extents.insert(extent / (extentMask() + 1)).second)
                    return false;
                const std::size_t length = ((extent & extentMask()) * 128 + entry[15]) * 128;
                const auto count = (length + profile.blockSize - 1) / profile.blockSize;
                for (std::size_t index = 0; index < count; ++index) {
                    const unsigned block = wide() ? word(entry, 16 + index * 2) : entry[16 + index];
                    if (!allocated.insert(block).second)
                        return false;
                }
            }
            if (*extents.begin() != 0 || *extents.rbegin() + 1 != extents.size())
                return false;
        }
        for (unsigned slot = 0; slot < profile.dirEntries; ++slot) {
            const auto entry = slice(storage.logical, dirPos(slot), 32);
            if (entry[0] == 0xE5 || entry[0] == 0x20 || entry[0] == 0x21
                || std::ranges::all_of(entry, [](uint8_t value) { return value == 0; }))
                continue;
            if (entry[0] > 15 || std::ranges::any_of(entry.subspan(1, 11), [](uint8_t value) {
                    const auto character = value & 127;
                    return character < 32 || character == 127;
                }))
                return false;
        }
        // HC's native nine-byte Spectrum headers distinguish it from arbitrary
        // sector data with the same capacity. Headerless-only images need a profile.
        return profile.fs != "hc"sv || std::ranges::any_of(entries, [](const File &file) {
                   return file.entry.attributes.contains("type"sv);
               });
    }
    std::vector<File> files() const
    {
        if (cpm())
            return cpmFiles();
        auto &b = storage.logical;
        auto &p = storage.profile;
        std::vector<File> out;
        if (p.fs == "trdos"sv) {
            require(b.size() > 2304 && b[8 * 256 + 0xE7] == 0x10,
                    "Invalid TR-DOS service sector"sv);
            for (unsigned i = 0; i < 128; ++i) {
                auto e = slice(b, i * 16, 16);
                if (!e[0])
                    break;
                if (e[0] == 1)
                    continue;
                File f;
                f.slots = { i };
                f.nativeName = std::string(e.begin(), e.begin() + 8);
                f.entry.name = visibleName(e.first(8)) + "."s + visibleName(e.subspan(8, 1));
                std::size_t len = word(e, 11);
                require(len <= std::size_t(e[13]) * 256, "TR-DOS file length exceeds allocation"sv);
                // BASIC's length includes its variables; preserve the trailer in the final sector too.
                auto data =
                        slice(b, (std::size_t(e[15]) * 16 + e[14]) * 256, std::size_t(e[13]) * 256);
                f.bytes.assign(data.begin(), data.end());
                f.entry.size = len ? len : std::size_t(e[13]) * 256;
                f.entry.attributes = { { "type"s, e[8] },
                                       { "param1"s, word(e, 9) },
                                       { "length"s, len } };
                out.push_back(std::move(f));
            }
        } else if (p.fs == "devil"sv) {
            for (unsigned i = 0; i < p.cylinders - 3; ++i) {
                auto e = slice(b, i * 256, 256);
                if (e[0] > 3)
                    continue;
                File f;
                f.nativeHeader.assign(e.begin(), e.end());
                f.nativeName = std::string(e.begin() + 1, e.begin() + 11);
                f.entry.name = visibleName(e.subspan(1, 10));
                if (f.entry.name.empty())
                    f.entry.name = "%20"sv;
                f.slots = { i };
                std::size_t size = word(e, 11);
                f.entry.attributes = { { "type"s, e[0] },
                                       { "param1"s, word(e, 13) },
                                       { "param2"s, word(e, 15) },
                                       { "readonly"s, bool(e[18] & 2) },
                                       { "hidden"s, bool(e[18] & 1) },
                                       { "length"s, size } };
                // Optional headerless blocks follow the standard data; retain their length table.
                if (word(e, 20)) {
                    require(word(e, 20) == size, "DEVIL standard-length copy mismatch"sv);
                    for (unsigned j = 22; j < 256; j += 2) {
                        auto len = word(e, j);
                        if (!len)
                            break;
                        size += len;
                        f.entry.attributes[std::format("tail{}", (j - 22) / 2)] = len;
                    }
                }
                std::size_t tracks = std::max<std::size_t>(1, (size + 9215) / 9216);
                require(tracks && i + tracks <= p.cylinders - 3, "Invalid DEVIL allocation"sv);
                for (unsigned j = 1; j < tracks; ++j) {
                    require(b[(i + j) * 256] == 0xE4, "Missing DEVIL continuation"sv);
                    f.slots.push_back(i + j);
                }
                auto data = slice(b, (i + 3) * 9216, size);
                f.bytes.assign(data.begin(), data.end());
                f.entry.size = size;
                out.push_back(std::move(f));
                i += tracks - 1;
            }
        } else if (p.fs == "opus"sv) {
            auto catalog = slice(b, 256, 1792);
            auto first = word(catalog, 2);
            auto last = word(catalog, 4);
            require(first == 0 && last >= 6 && last < 720, "Invalid Opus catalogue"sv);
            for (unsigned i = 1; i < 112; ++i) {
                auto e = catalog.subspan(i * 16, 16);
                if (word(e, 4) == 65535)
                    break;
                auto start = word(e, 2);
                auto end = word(e, 4);
                if (!start && !end)
                    continue;
                require(end >= start && start >= 7 && end < b.size() / 256 - 1 && word(e, 0) < 256,
                        "Invalid Opus allocation"sv);
                std::size_t size = (end - start) * 256 + word(e, 0) + 1;
                auto data = slice(b, (start + 1) * 256, size);
                require(data.size() >= 7, "Opus header missing"sv);
                File f;
                f.nativeName = std::string(e.begin() + 6, e.begin() + 16);
                f.entry.name = visibleName(e.subspan(6, 10));
                if (f.entry.name.empty())
                    f.entry.name = "%20"sv;
                f.slots = { i };
                f.entry.attributes = { { "type"s, data[0] },
                                       { "param1"s, word(data, 3) },
                                       { "param2"s, word(data, 5) } };
                std::size_t n = word(data, 1);
                require(n <= data.size() - 7, "Opus length exceeds allocation"sv);
                f.bytes.assign(data.begin() + 7, data.begin() + 7 + n);
                f.entry.size = n;
                out.push_back(std::move(f));
            }
        } else if (p.fs == "mgt"sv) {
            for (unsigned i = 0; i < 80; ++i) {
                auto e = slice(b, i * 256, 256);
                if (!(e[0] & 31))
                    continue;
                File f;
                f.nativeHeader.assign(e.begin(), e.end());
                f.slots = { i };
                f.nativeName = std::string(e.begin() + 1, e.begin() + 11);
                f.entry.name = visibleName(e.subspan(1, 10));
                if (f.entry.name.empty())
                    f.entry.name = "%20"sv;
                f.entry.attributes = { { "native-type"s, e[0] & 31 },
                                       { "readonly"s, bool(e[0] & 64) },
                                       { "hidden"s, bool(e[0] & 128) } };
                unsigned track = e[13];
                unsigned sector = e[14];
                std::set<unsigned> seen;
                unsigned count = (e[11] << 8) | e[12];
                require(count <= 1560, "MGT sector count exceeds disk"sv);
                for (unsigned j = 0; j < count; ++j) {
                    unsigned head = track >> 7;
                    unsigned cylinder = track & 127;
                    require(head < 2 && cylinder < 80 && sector >= 1 && sector <= 10,
                            "Invalid MGT sector link"sv);
                    unsigned block = (head * 80 + cylinder) * 10 + sector - 1;
                    require(seen.insert(block).second, "Cyclic MGT file chain"sv);
                    auto data = slice(b, block * 512, 512);
                    append(f.bytes, data.first(510));
                    track = data[510];
                    sector = data[511];
                }
                require(!track && !sector, "MGT chain exceeds declared length"sv);
                if ((e[0] & 31) <= 4) {
                    require(f.bytes.size() >= 9, "MGT file header missing"sv);
                    std::size_t size = word(e, 212);
                    f.entry.attributes["type"s] = e[211];
                    f.entry.attributes["param1"s] = word(e, 214);
                    f.entry.attributes["param2"s] = word(e, 216);
                    f.entry.attributes["autostart"s] = word(e, 218);
                    require(size <= f.bytes.size() - 9, "MGT file length exceeds chain"sv);
                    f.bytes = Bytes(f.bytes.begin() + 9, f.bytes.begin() + 9 + size);
                }
                f.entry.size = f.bytes.size();
                out.push_back(std::move(f));
            }
        }
        std::set<std::string> names;
        for (auto &f : out) {
            validName(f.entry.name, 128);
            require(names.insert(f.entry.name).second,
                    "Duplicate filesystem names require repair"sv);
        }
        return out;
    }
    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Disk>(*this);
    }
    std::vector<MediaEntry> list() const override
    {
        std::vector<MediaEntry> out;
        if (cpm())
            for (unsigned i = 1; i <= 15; ++i)
                out.push_back({ std::format("user{:02}", i), 0, true, {} });
        for (auto &f : files())
            out.push_back(f.entry);
        return out;
    }
    Bytes read(std::string_view name) const override
    {
        const auto canonical = canonicalName(name);
        for (auto &f : files())
            if (f.entry.name == canonical) {
                if (storage.profile.fs == "trdos"sv)
                    f.bytes.resize(f.entry.size);
                return f.bytes;
            }
        throw FormatException("File not found"sv);
    }
    void clearCpm(std::string_view name, bool protect = true)
    {
        const auto canonical = canonicalName(name);
        for (auto &f : cpmFiles())
            if (f.entry.name == canonical) {
                require(!protect || !attr(f.entry.attributes, "readonly"sv),
                        "File is read-only; clear readonly first"sv);
                for (auto n : f.slots)
                    storage.logical[dirPos(n)] = 0xE5;
                return;
            }
    }
    void putCpm(std::string_view name, View bytes, const MediaAttributes &changes)
    {
        checkAttrs(changes,
                   { "user"sv, "readonly"sv, "system"sv, "archive"sv, "type"sv, "param1"sv,
                     "param2"sv, "raw"sv });
        auto entries = cpmFiles();
        const auto existingNative =
                std::ranges::find(entries, name, [](const File &file) { return file.entry.name; });
        auto native = existingNative != entries.end()
                ? std::pair{ unsigned(attr(existingNative->entry.attributes, "user"sv)),
                             existingNative->nativeName }
                : cpmName(name);
        auto &[user, field] = native;
        const auto canonical = cpmPath(user, field);
        name = canonical;
        MediaAttributes attrs;
        auto it = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
        if (it != entries.end()) {
            attrs = it->entry.attributes;
            require(!attr(attrs, "readonly"sv)
                            || (changes.contains("readonly"sv) && !attr(changes, "readonly"sv)),
                    "File is read-only; clear readonly first"sv);
        }
        for (auto &[k, v] : changes)
            attrs[k] = v;
        require(!attrs.contains("user"sv) || attrs["user"s] == user,
                "Change CP/M user by renaming into userNN/"sv);
        for (auto key : { "readonly"sv, "system"sv, "archive"sv })
            require(attr(attrs, key) <= 1, "Boolean attribute out of range"sv);
        Bytes data(bytes.begin(), bytes.end());
        auto &p = storage.profile;
        if ((p.fs == "hc"sv && !attr(attrs, "raw"sv))
            || (p.fs == "plus3"sv && attrs.contains("type"sv))) {
            require(bytes.size() <= 65535 && attr(attrs, "type"sv, 3) <= 3,
                    "Spectrum file size/type out of range"sv);
            if (p.fs == "hc"sv) {
                data.assign(9, 0);
                data[0] = uint8_t(attr(attrs, "type"sv, 3));
                setWord(data, 1, bytes.size());
                setWord(data, 3, attr(attrs, "param1"sv, 32768));
                setWord(data, 5, attr(attrs, "param2"sv, bytes.size()));
                setWord(data, 7, attr(attrs, "param1"sv, 32768));
            } else {
                data.assign(128, 0);
                textAt(data, 0, "PLUS3DOS\x1A"sv);
                data[9] = 1;
                setDword(data, 11, bytes.size() + 128);
                data[15] = uint8_t(attr(attrs, "type"sv, 3));
                setWord(data, 16, bytes.size());
                setWord(data, 18, attr(attrs, "param1"sv, 32768));
                setWord(data, 20, attr(attrs, "param2"sv, bytes.size()));
                unsigned sum = 0;
                for (auto c : View(data).first(127))
                    sum += c;
                data[127] = uint8_t(sum);
            }
            append(data, bytes);
        } else
            require(!attrs.contains("type"sv) && !attrs.contains("param1"sv)
                            && !attrs.contains("param2"sv),
                    "Generic CP/M files have no Spectrum header attributes"sv);
        clearCpm(name, false);
        auto &b = storage.logical;
        std::set<unsigned> used;
        for (unsigned n = 0; n < (p.dirEntries * 32 + p.blockSize - 1) / p.blockSize; ++n)
            used.insert(n);
        for (unsigned n = 0; n < p.dirEntries; ++n) {
            auto e = slice(b, dirPos(n), 32);
            if (e[0] > 15)
                continue;
            for (unsigned k = 0; k < (wide() ? 8 : 16); ++k) {
                auto block = wide() ? word(e, 16 + k * 2) : e[16 + k];
                if (block) {
                    require(block < blocks(), "Invalid CP/M allocation"sv);
                    used.insert(block);
                }
            }
        }
        std::size_t capacity = p.blockSize * (wide() ? 8 : 16);
        std::size_t pos = 0;
        unsigned extentIndex = 0;
        do {
            unsigned slot = 0;
            for (; slot < p.dirEntries; ++slot)
                if (b[dirPos(slot)] == 0xE5)
                    break;
            require(slot < p.dirEntries, "CP/M directory full"sv);
            Bytes e(32);
            e[0] = uint8_t(user);
            textAt(e, 1, field);
            e[9] |= attr(attrs, "readonly"sv) ? 128 : 0;
            e[10] |= attr(attrs, "system"sv) ? 128 : 0;
            e[11] |= attr(attrs, "archive"sv) ? 128 : 0;
            std::size_t count = std::min(capacity, data.size() - pos);
            unsigned records = unsigned((count + 127) / 128);
            unsigned lastExtent =
                    extentIndex * (extentMask() + 1) + (records ? (records - 1) / 128 : 0);
            e[12] = lastExtent & 31;
            e[14] = lastExtent >> 5;
            e[15] = records ? uint8_t((records - 1) % 128 + 1) : 0;
            unsigned alloc = 0;
            std::size_t end = pos + count;
            while (pos < end) {
                unsigned block = 0;
                while (block < blocks() && used.contains(block))
                    ++block;
                require(block < blocks(), "CP/M disk full"sv);
                used.insert(block);
                if (wide())
                    setWord(e, 16 + alloc * 2, block);
                else
                    e[16 + alloc] = uint8_t(block);
                ++alloc;
                std::size_t n = std::min<std::size_t>(p.blockSize, end - pos);
                auto offset = base() + block * p.blockSize;
                std::fill_n(b.begin() + offset, p.blockSize, 0x1A);
                std::copy_n(data.begin() + pos, n, b.begin() + offset);
                pos += n;
            }
            std::ranges::copy(e, b.begin() + dirPos(slot));
            ++extentIndex;
        } while (pos < data.size());
    }
    void rebuild(std::vector<File> entries)
    {
        auto &b = storage.logical;
        auto &p = storage.profile;
        if (p.fs == "trdos"sv) {
            std::fill_n(b.begin(), 2048, 0);
            require(entries.size() <= 128, "TR-DOS directory full"sv);
            unsigned sector = 16;
            for (unsigned i = 0; i < entries.size(); ++i) {
                auto &f = entries[i];
                auto dot = f.entry.name.rfind('.');
                require(!f.nativeName.empty()
                                || (dot != std::string::npos && dot <= 8
                                    && dot + 2 == f.entry.name.size()),
                        "Use TR-DOS NAME.T (8+1) names"sv);
                if (f.nativeName.empty())
                    if (f.nativeName.empty())
                        validName(f.entry.name, 10);
                require(!f.nativeName.empty() || !f.entry.attributes.contains("type"sv)
                                || attr(f.entry.attributes, "type"sv)
                                        == uint8_t(f.entry.name.back()),
                        "TR-DOS type must match the filename extension"sv);
                std::size_t size = attr(f.entry.attributes, "length"sv, f.bytes.size());
                require(size <= 65535 && size <= f.bytes.size(), "TR-DOS length out of range"sv);
                unsigned count = unsigned((f.bytes.size() + 255) / 256);
                require(count <= 255 && sector + count <= b.size() / 256,
                        "TR-DOS disk/file full"sv);
                auto n = i * 16;
                std::fill_n(b.begin() + n, 8, ' ');
                textAt(b, n,
                       f.nativeName.empty() ? std::string_view(f.entry.name).substr(0, dot)
                                            : std::string_view(f.nativeName));
                b[n + 8] =
                        uint8_t(attr(f.entry.attributes, "type"sv, uint8_t(f.entry.name.back())));
                setWord(b, n + 9, attr(f.entry.attributes, "param1"sv, 32768));
                setWord(b, n + 11, size);
                b[n + 13] = uint8_t(count);
                b[n + 14] = sector % 16;
                b[n + 15] = sector / 16;
                auto offset = sector * 256;
                std::fill_n(b.begin() + offset, count * 256, 0);
                std::ranges::copy(f.bytes, b.begin() + offset);
                sector += count;
            }
            auto n = 8 * 256;
            b[n + 0xE1] = sector % 16;
            b[n + 0xE2] = sector / 16;
            b[n + 0xE3] =
                    p.cylinders == 80 ? (p.sides == 2 ? 0x16 : 0x18) : (p.sides == 2 ? 0x17 : 0x19);
            b[n + 0xE4] = uint8_t(entries.size());
            setWord(b, n + 0xE5, b.size() / 256 - sector);
            b[n + 0xE7] = 0x10;
            b[n + 0xF4] = 0;
        } else if (p.fs == "devil"sv) {
            std::fill_n(b.begin(), 3 * 9216, 0xE5);
            unsigned track = 0;
            for (auto &f : entries) {
                if (f.nativeName.empty())
                    validName(f.entry.name, 10);
                std::size_t count = std::max<std::size_t>(1, (f.bytes.size() + 9215) / 9216);
                require(track + count <= p.cylinders - 3, "DEVIL disk full"sv);
                std::size_t n = track * 256;
                std::fill_n(b.begin() + n, count * 256, 0);
                if (!f.nativeHeader.empty())
                    std::ranges::copy(f.nativeHeader, b.begin() + n);
                b[n] = uint8_t(attr(f.entry.attributes, "type"sv, 3));
                require(b[n] <= 3, "Invalid DEVIL type"sv);
                std::fill_n(b.begin() + n + 1, 10, ' ');
                textAt(b, n + 1, f.nativeName.empty() ? f.entry.name : f.nativeName);
                setWord(b, n + 11, attr(f.entry.attributes, "length"sv, f.bytes.size()));
                setWord(b, n + 13, attr(f.entry.attributes, "param1"sv, 32768));
                setWord(b, n + 15, attr(f.entry.attributes, "param2"sv, f.bytes.size()));
                b[n + 18] = (attr(f.entry.attributes, "readonly"sv) ? 2 : 0)
                        | (attr(f.entry.attributes, "hidden"sv) ? 1 : 0);
                if (f.entry.attributes.contains("tail0"sv)) {
                    setWord(b, n + 20, attr(f.entry.attributes, "length"sv));
                    std::size_t total = attr(f.entry.attributes, "length"sv);
                    for (unsigned i = 0; i < 117; ++i) {
                        auto key = std::format("tail{}", i);
                        if (!f.entry.attributes.contains(key))
                            break;
                        auto len = attr(f.entry.attributes, key);
                        require(len > 0 && len <= 65535, "Invalid DEVIL tail length"sv);
                        setWord(b, n + 22 + i * 2, len);
                        total += len;
                    }
                    require(total == f.bytes.size(), "DEVIL tail lengths disagree with data"sv);
                }
                for (std::size_t j = 1; j < count; ++j)
                    b[(track + j) * 256] = 0xE4;
                auto offset = (track + 3) * 9216;
                std::fill_n(b.begin() + offset, count * 9216, 0);
                std::ranges::copy(f.bytes, b.begin() + offset);
                track += count;
            }
        } else if (p.fs == "opus"sv) {
            require(entries.size() <= 110, "Opus directory full"sv);
            auto diskLabel = Bytes(b.begin() + 262, b.begin() + 272);
            std::fill_n(b.begin() + 256, 1792, 0);
            std::ranges::copy(diskLabel, b.begin() + 262);
            setWord(b, 256, 255);
            setWord(b, 258, 0);
            setWord(b, 260, 6);
            unsigned block = 7;
            unsigned index = 1;
            for (auto &f : entries) {
                if (f.nativeName.empty())
                    validName(f.entry.name, 10);
                require(f.bytes.size() <= 65535, "Opus file too large"sv);
                Bytes data(7);
                data[0] = uint8_t(attr(f.entry.attributes, "type"sv, 3));
                setWord(data, 1, f.bytes.size());
                setWord(data, 3, attr(f.entry.attributes, "param1"sv, 32768));
                setWord(data, 5, attr(f.entry.attributes, "param2"sv, 32768));
                append(data, f.bytes);
                unsigned count = unsigned((data.size() + 255) / 256);
                require(block + count <= b.size() / 256 - 1, "Opus disk full"sv);
                std::size_t n = 256 + index++ * 16;
                setWord(b, n, (data.size() - 1) % 256);
                setWord(b, n + 2, block);
                setWord(b, n + 4, block + count - 1);
                std::fill_n(b.begin() + n + 6, 10, ' ');
                textAt(b, n + 6, f.nativeName.empty() ? f.entry.name : f.nativeName);
                std::ranges::copy(data, b.begin() + (block + 1) * 256);
                block += count;
            }
            auto n = 256 + index * 16;
            setWord(b, n + 2, b.size() / 256 - 1);
            setWord(b, n + 4, 65535);
        } else if (p.fs == "mgt"sv) {
            require(entries.size() <= 80, "MGT directory full"sv);
            std::fill_n(b.begin(), 40 * 512, 0);
            unsigned next = 40;
            unsigned index = 0;
            for (auto &f : entries) {
                if (f.nativeName.empty())
                    validName(f.entry.name, 10);
                auto n = index++ * 256;
                auto native = attr(f.entry.attributes, "native-type"sv,
                                   attr(f.entry.attributes, "type"sv, 3) + 1);
                require(native > 0 && native <= 31, "Invalid MGT type"sv);
                if (!f.nativeHeader.empty())
                    std::ranges::copy(f.nativeHeader, b.begin() + n);
                std::fill_n(b.begin() + n + 15, 195, 0);
                b[n] = uint8_t(native | (attr(f.entry.attributes, "readonly"sv) ? 64 : 0)
                               | (attr(f.entry.attributes, "hidden"sv) ? 128 : 0));
                std::fill_n(b.begin() + n + 1, 10, ' ');
                textAt(b, n + 1, f.nativeName.empty() ? f.entry.name : f.nativeName);
                Bytes data = f.bytes;
                if (native <= 4) {
                    require(data.size() <= 65535, "MGT Spectrum file too large"sv);
                    Bytes header(9);
                    header[0] = uint8_t(native - 1);
                    setWord(header, 1, data.size());
                    setWord(header, 3, attr(f.entry.attributes, "param1"sv, 32768));
                    setWord(header, 5, attr(f.entry.attributes, "param2"sv, data.size()));
                    setWord(header, 7, attr(f.entry.attributes, "autostart"sv, 32768));
                    std::ranges::copy(header, b.begin() + n + 211);
                    append(header, data);
                    data = std::move(header);
                }
                unsigned count = std::max<std::size_t>(1, (data.size() + 509) / 510);
                require(next + count <= 1600, "MGT disk full"sv);
                b[n + 11] = uint8_t(count >> 8);
                b[n + 12] = uint8_t(count);
                auto address = [](unsigned lba) {
                    return std::pair{ uint8_t((lba / 10 % 80) | ((lba / 800) << 7)),
                                      uint8_t(lba % 10 + 1) };
                };
                auto [track, sector] = address(next);
                b[n + 13] = track;
                b[n + 14] = sector;
                for (unsigned j = 0; j < count; ++j) {
                    auto [t, s] = address(next + j);
                    unsigned bitmap = (t & 127) * 10 + (t >> 7) * 800 + s - 1 - 40;
                    require(bitmap < 1560, "Invalid MGT bitmap index"sv);
                    b[n + 15 + bitmap / 8] |= uint8_t(1 << (bitmap % 8));
                    std::size_t offset = (next + j) * 512;
                    std::fill_n(b.begin() + offset, 512, 0);
                    std::size_t start = j * 510;
                    std::size_t size = std::min<std::size_t>(
                            510, data.size() - std::min<std::size_t>(start, data.size()));
                    if (size)
                        std::copy_n(data.begin() + start, size, b.begin() + offset);
                    if (j + 1 < count) {
                        auto [a, c] = address(next + j + 1);
                        b[offset + 510] = a;
                        b[offset + 511] = c;
                    }
                }
                next += count;
            }
        }
    }
    void put(std::string_view name, View data, const MediaAttributes &changes) override
    {
        if (cpm()) {
            putCpm(name, data, changes);
            return;
        }
        auto fs = storage.profile.fs;
        if (fs == "trdos"sv)
            checkAttrs(changes, { "type"sv, "param1"sv, "length"sv });
        else if (fs == "opus"sv)
            checkAttrs(changes, { "type"sv, "param1"sv, "param2"sv });
        else if (fs == "mgt"sv)
            checkAttrs(changes,
                       { "native-type"sv, "type"sv, "param1"sv, "param2"sv, "autostart"sv,
                         "readonly"sv, "hidden"sv });
        else
            for (auto &[k, v] : changes)
                require(k == "type"sv || k == "param1"sv || k == "param2"sv || k == "length"sv
                                || k == "readonly"sv || k == "hidden"sv || k.starts_with("tail"sv),
                        "Unsupported DEVIL attribute"sv);
        auto entries = files();
        auto it = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
        File f;
        if (it != entries.end()) {
            f = *it;
            require(!attr(f.entry.attributes, "readonly"sv)
                            || (changes.contains("readonly"sv) && !attr(changes, "readonly"sv)),
                    "File is read-only; clear readonly first"sv);
        }
        f.entry.name = std::string(name);
        f.entry.size = data.size();
        if (fs == "trdos"sv && it != entries.end() && data.size() <= f.bytes.size())
            std::ranges::copy(data, f.bytes.begin());
        else
            f.bytes.assign(data.begin(), data.end());
        if (fs == "trdos"sv || fs == "devil"sv)
            f.entry.attributes["length"s] = data.size();
        for (auto &[k, v] : changes)
            f.entry.attributes[k] = v;
        if (fs == "mgt"sv && changes.contains("type"sv)) {
            require(changes.at("type"s) <= 3, "Spectrum MGT type must be 0..3"sv);
            const auto native = changes.at("type"s) + 1;
            require(!changes.contains("native-type"sv) || changes.at("native-type"s) == native,
                    "Conflicting MGT file types"sv);
            f.entry.attributes["native-type"s] = native;
        }
        if (it == entries.end())
            entries.push_back(std::move(f));
        else
            *it = std::move(f);
        rebuild(std::move(entries));
    }
    void remove(std::string_view name) override
    {
        if (cpm()) {
            read(name);
            clearCpm(name);
            return;
        }
        auto entries = files();
        auto it = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
        require(it != entries.end(), "File not found"sv);
        require(!attr(it->entry.attributes, "readonly"sv), "File is read-only"sv);
        entries.erase(it);
        rebuild(std::move(entries));
    }
    void attributes(std::string_view name, const MediaAttributes &a) override
    {
        const auto canonical = canonicalName(name);
        name = canonical;
        // Attribute changes are allowed to clear protection without rewriting native payloads.
        auto entries = files();
        auto i = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
        require(i != entries.end(), "File not found"sv);
        if (cpm()) {
            if (storage.profile.fs != "cpm"sv && std::ranges::any_of(a, [](const auto &attribute) {
                    return attribute.first != "readonly"sv && attribute.first != "system"sv
                            && attribute.first != "archive"sv;
                })) {
                Backend::attributes(name, a);
                return;
            }
            checkAttrs(a, { "readonly"sv, "system"sv, "archive"sv });
            for (auto &[k, v] : a) {
                require(v <= 1, "Boolean attribute out of range"sv);
                auto offset = k == "readonly"sv ? 9 : k == "system"sv ? 10 : 11;
                for (auto slot : i->slots) {
                    auto &byte = storage.logical[dirPos(slot) + offset];
                    byte = uint8_t((byte & 127) | (v ? 128 : 0));
                }
            }
            return;
        }
        if (storage.profile.fs == "devil"sv || storage.profile.fs == "mgt"sv) {
            if (std::ranges::any_of(a, [](const auto &attribute) {
                    return attribute.first != "readonly"sv && attribute.first != "hidden"sv;
                })) {
                if (storage.profile.fs == "mgt"sv)
                    put(name, read(name), a);
                else
                    Backend::attributes(name, a);
                return;
            }
            checkAttrs(a, { "readonly"sv, "hidden"sv });
            for (auto &[k, v] : a) {
                require(v <= 1, "Boolean attribute out of range"sv);
                i->entry.attributes[k] = v;
            }
            rebuild(std::move(entries));
            return;
        }
        if (storage.profile.fs == "trdos"sv) {
            checkAttrs(a, { "type"sv, "param1"sv, "length"sv });
            for (auto &[k, v] : a)
                i->entry.attributes[k] = v;
            if (a.contains("type"sv)) {
                require(a.at("type"s) >= 33 && a.at("type"s) <= 126, "Invalid TR-DOS extension"sv);
                i->entry.name.back() = char(a.at("type"s));
            }
            rebuild(std::move(entries));
            return;
        }
        Backend::attributes(name, a);
    }
    void rename(std::string_view name, std::string_view to) override
    {
        const auto canonical = canonicalName(name);
        name = canonical;
        if (cpm()) {
            auto entries = cpmFiles();
            auto i = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
            require(i != entries.end(), "File not found"sv);
            auto [user, field] = cpmName(to);
            auto path = cpmPath(user, field);
            for (auto &e : entries)
                require(e.entry.name != path, "Destination exists"sv);
            require(!attr(i->entry.attributes, "readonly"sv), "File is read-only"sv);
            for (auto slot : i->slots) {
                auto pos = dirPos(slot);
                storage.logical[pos] = uint8_t(user);
                for (unsigned k = 0; k < 11; ++k)
                    storage.logical[pos + 1 + k] =
                            (storage.logical[pos + 1 + k] & 128) | uint8_t(field[k]);
            }
            return;
        }
        auto entries = files();
        auto i = std::ranges::find(entries, name, [](const File &f) { return f.entry.name; });
        require(i != entries.end(), "File not found"sv);
        require(!attr(i->entry.attributes, "readonly"sv), "File is read-only"sv);
        for (auto &e : entries)
            require(e.entry.name != to, "Destination exists"sv);
        i->entry.name = std::string(to);
        i->nativeName.clear();
        if (storage.profile.fs == "trdos"sv)
            i->entry.attributes["type"s] = uint8_t(to.back());
        rebuild(std::move(entries));
    }
    Bytes encode(std::string_view ext) override
    {
        return storage.encode(ext);
    }
    void boot(View data) override
    {
        require(cpm() && base() > 0, "This disk has no reserved CP/M boot tracks"sv);
        require(data.size() == base(),
                "Boot-track bytes must exactly fill the profile's reserved tracks"sv);
        std::ranges::copy(data, storage.logical.begin());
    }
};

std::unique_ptr<Disk> detectCpmDisk(View bytes, std::string_view extension,
                                    const MediaOptions &options)
{
    std::unique_ptr<Disk> match;
    std::vector<std::string_view> matches;
    for (const auto &profile : diskProfiles()) {
        if (profile.fs != "cpm"sv && profile.fs != "hc"sv && profile.fs != "plus3"sv)
            continue;
        // These names are aliases of cpm-720 and plus3, respectively.
        if (profile.name == "cobra-cpm"sv || profile.name == "cip04"sv)
            continue;
        auto selected = options;
        selected.filesystem = profile.name;
        try {
            auto candidate = std::make_unique<Disk>();
            candidate->storage = DiskContainer::open(bytes, extension, selected, false);
            if (!candidate->matchesCpmProfile())
                continue;
            matches.push_back(profile.name);
            match = std::move(candidate);
        } catch (const FormatException &) {
            // A candidate must pass the normal geometry and catalogue checks.
        }
    }
    if (matches.size() > 1) {
        std::string message = "Ambiguous disk filesystem; select --filesystem from:"s;
        for (const auto name : matches)
            message += " "s + std::string(name);
        throw FormatException(message);
    }
    return match;
}
} // namespace
std::unique_ptr<Backend> disk(View b, std::string_view ext, const MediaOptions &options,
                              bool create)
{
    const auto container = extension(ext);
    if (!create && options.filesystem.empty()
        && (container == ".img"sv || container == ".raw"sv || container == ".dsk"sv
            || container == ".edsk"sv || container == ".td0"sv || container == ".cqm"sv
            || container.empty())) {
        if (auto detected = detectCpmDisk(b, ext, options))
            return detected;
    }
    auto r = std::make_unique<Disk>();
    r->storage = DiskContainer::open(b, ext, options, create);
    if (create) {
        auto &p = r->storage.profile;
        auto &bytes = r->storage.logical;
        if (r->cpm()) {
            std::fill(bytes.begin(), bytes.end(), 0xE5);
            if (p.fs == "plus3"sv && r->storage.prefix.empty()) {
                std::fill_n(bytes.begin(), p.sectorSize, 0);
                bytes[0] = p.sides == 1 ? 0 : 3;
                bytes[1] = p.sides == 1 ? 0 : 1;
                bytes[2] = uint8_t(p.cylinders);
                bytes[3] = uint8_t(p.sectors);
                bytes[4] = 2;
                bytes[5] = uint8_t(p.bootTracks);
                bytes[6] = p.blockSize == 1024 ? 3 : 4;
                bytes[7] = uint8_t(p.dirEntries * 32 / p.blockSize);
                bytes[8] = 0x2A;
                bytes[9] = 0x52;
            }
        } else {
            std::fill(bytes.begin(), bytes.end(), 0);
            if (p.fs == "devil"sv)
                std::fill_n(bytes.begin(), 3 * 9216, 0xE5);
            if (p.fs == "opus"sv) {
                bytes[2] = uint8_t(p.cylinders);
                bytes[3] = 18;
                bytes[4] = uint8_t(0x40 | (p.sides == 2 ? 0x10 : 0));
                std::fill_n(bytes.begin() + 262, 10, ' ');
                validName(options.label, 10);
                textAt(bytes, 262, options.label);
            }
            r->rebuild({});
            if (p.fs == "trdos"sv) {
                validName(options.label, 8);
                std::fill_n(bytes.begin() + 8 * 256 + 0xF5, 8, ' ');
                textAt(bytes, 8 * 256 + 0xF5, options.label);
            }
        }
    }
    r->files();
    return r;
}
} // namespace zxkit::archive
