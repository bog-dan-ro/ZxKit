// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include "../MdrFormat.h"
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
uint8_t sum255(View b)
{
    unsigned n = 0;
    for (auto c : b)
        n += c;
    return uint8_t(n % 255);
}
bool validSectorHeader(View sector)
{
    return (sector[0] & 1) && sector[1] > 0 && sector[1] <= mdr::kMaxSectors
            && sum255(sector.first(14)) == sector[14];
}

struct Microdrive final : Backend
{
    Bytes image;

    std::size_t sectorCount() const
    {
        return image.size() / mdr::kSectorSize;
    }

    bool writeProtected() const
    {
        return image.size() % mdr::kSectorSize == 1 && image.back() != 0;
    }

    struct File
    {
        MediaEntry entry;
        Bytes bytes;
        std::vector<std::size_t> records;
    };
    std::vector<File> files() const
    {
        std::map<std::string, std::multimap<unsigned, std::size_t>> records;
        for (std::size_t i = 0; i < sectorCount(); ++i) {
            auto record = slice(image, i * mdr::kSectorSize, mdr::kSectorSize);
            const auto flags = record[15];
            const auto size = word(record, 17);
            // Zero-length EOF marks a bad sector, and erased records have no data checksum.
            if ((flags & 1) || size == 0 || size > 512 || (!(flags & 2) && size != 512))
                continue;
            if (!validSectorHeader(record) || sum255(record.subspan(15, 14)) != record[29]
                || sum255(record.subspan(30, 512)) != record[542])
                continue;
            auto name = nameField(record.subspan(19, 10));
            records[name].emplace(record[16], i);
        }
        std::vector<File> out;
        for (auto &[name, map] : records) {
            File f;
            f.entry.name = name;
            unsigned next = 0;
            bool eof = false;
            for (auto &[number, index] : map) {
                if (number != next++ || eof) {
                    eof = false;
                    break;
                }
                f.records.push_back(index);
                auto rec = slice(image, index * mdr::kSectorSize, mdr::kSectorSize);
                append(f.bytes, rec.subspan(30, word(rec, 17)));
                eof = rec[15] & 2;
            }
            if (!eof)
                continue;
            if (image[f.records[0] * mdr::kSectorSize + 15] & 4) {
                if (f.bytes.size() < 9)
                    continue;
                auto size = word(f.bytes, 1);
                if (size > f.bytes.size() - 9)
                    continue;
                f.entry.attributes = { { "type"s, f.bytes[0] },
                                       { "param1"s,
                                         f.bytes[0] == 0 ? word(f.bytes, 7) : word(f.bytes, 3) },
                                       { "param2"s, word(f.bytes, 5) } };
                f.bytes = Bytes(f.bytes.begin() + 9, f.bytes.begin() + 9 + size);
            }
            f.entry.size = f.bytes.size();
            out.push_back(std::move(f));
        }
        return out;
    }
    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Microdrive>(*this);
    }
    std::string filesystem() const override
    {
        return "microdrive"s;
    }
    std::vector<MediaEntry> list() const override
    {
        std::vector<MediaEntry> out;
        for (auto &f : files())
            out.push_back(f.entry);
        return out;
    }
    Bytes read(std::string_view name) const override
    {
        for (auto &f : files())
            if (f.entry.name == name)
                return f.bytes;
        throw FormatException("Microdrive file not found"sv);
    }
    void remove(std::string_view name) override
    {
        require(!writeProtected(), "Microdrive cartridge is write-protected"sv);
        for (auto &f : files())
            if (f.entry.name == name) {
                for (auto n : f.records) {
                    std::size_t p = n * mdr::kSectorSize;
                    std::fill_n(image.begin() + p + mdr::kHeaderSize, mdr::kRecordSize, 0);
                }
                return;
            }
        throw FormatException("Microdrive file not found"sv);
    }
    void put(std::string_view name, View data, const MediaAttributes &changes) override
    {
        validName(name, 10);
        checkAttrs(changes, { "type"sv, "param1"sv, "param2"sv });
        require(!writeProtected(), "Microdrive cartridge is write-protected"sv);
        MediaAttributes a;
        for (auto &f : files())
            if (f.entry.name == name) {
                a = f.entry.attributes;
                remove(name);
                break;
            }
        for (auto &[k, v] : changes)
            a[k] = v;
        Bytes payload;
        if (a.contains("type"sv)) {
            require(attr(a, "type"sv) <= 3 && data.size() <= 65535,
                    "Microdrive Spectrum file fields out of range"sv);
            payload.resize(9);
            payload[0] = uint8_t(a["type"s]);
            setWord(payload, 1, data.size());
            setWord(payload, 3, attr(a, "param1"sv, 32768));
            setWord(payload, 5, attr(a, "param2"sv, data.size()));
            setWord(payload, 7, attr(a, "param1"sv, 32768));
        }
        append(payload, data);
        if (payload.empty()) {
            payload.resize(9);
            payload[0] = 3;
            a["type"s] = 3;
        }
        std::size_t count = (payload.size() + 511) / 512;
        std::vector<std::size_t> free;
        for (std::size_t i = 0; i < sectorCount(); ++i) {
            auto r = slice(image, i * mdr::kSectorSize, mdr::kSectorSize);
            if (!validSectorHeader(r))
                continue;
            if (!(r[15] & 3) && word(r, 17) == 0)
                free.push_back(i);
            else if (!(r[15] & 1) && word(r, 17) != 0 && nameField(r.subspan(19, 10)) == name)
                throw FormatException("An unreadable Microdrive file already uses that name"sv);
        }
        require(count <= free.size(), "Microdrive full"sv);
        for (unsigned n = 0; n < count; ++n) {
            auto pos = free[n] * mdr::kSectorSize;
            std::size_t offset = n * 512,
                        size = std::min<std::size_t>(512, payload.size() - offset);
            std::fill_n(image.begin() + pos + mdr::kHeaderSize, mdr::kRecordSize, 0);
            image[pos + 15] = uint8_t((a.contains("type"sv) ? 4 : 0) | (n + 1 == count ? 2 : 0));
            image[pos + 16] = uint8_t(n);
            setWord(image, pos + 17, size);
            std::fill_n(image.begin() + pos + 19, 10, ' ');
            textAt(image, pos + 19, name);
            image[pos + 29] = sum255(slice(image, pos + 15, 14));
            std::copy_n(payload.begin() + offset, size, image.begin() + pos + 30);
            image[pos + 542] = sum255(slice(image, pos + 30, 512));
        }
    }
    Bytes encode(std::string_view ext) override
    {
        require(ext.empty() || extension(ext) == ".mdr"sv,
                "Microdrive conversion requires file copying"sv);
        return image;
    }
};
} // namespace
std::unique_ptr<Backend> microdrive(View b, const MediaOptions &opts, bool create)
{
    auto r = std::make_unique<Microdrive>();
    if (create) {
        validName(opts.label, 10);
        r->image.resize(mdr::kMaxSectors * mdr::kSectorSize + 1);
        for (std::size_t i = 0; i < mdr::kMaxSectors; ++i) {
            auto p = i * mdr::kSectorSize;
            r->image[p] = 1;
            r->image[p + 1] = uint8_t(mdr::kMaxSectors - i);
            std::fill_n(r->image.begin() + p + 4, 10, ' ');
            textAt(r->image, p + 4, opts.label);
            r->image[p + 14] = sum255(slice(r->image, p, 14));
        }
    } else {
        mdr::sectors(b);
        r->image.assign(b.begin(), b.end());
    }
    return r;
}
} // namespace zxkit::archive
