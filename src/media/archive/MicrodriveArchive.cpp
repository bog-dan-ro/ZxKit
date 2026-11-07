// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include <set>
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
struct Microdrive final : Backend
{
    Bytes image;
    struct File
    {
        MediaEntry entry;
        Bytes bytes;
        std::vector<unsigned> records;
    };
    std::vector<File> files() const
    {
        std::map<std::string, std::map<unsigned, unsigned>> records;
        for (unsigned i = 0; i < 254; ++i) {
            auto record = slice(image, i * 543, 543);
            auto flags = record[15];
            auto size = word(record, 17);
            if (!(flags & 2) && size != 512)
                continue;
            if ((flags & 2) && !size)
                continue;
            require(size <= 512, "Invalid Microdrive record length"sv);
            require(sum255(record.first(14)) == record[14]
                            && sum255(record.subspan(15, 14)) == record[29]
                            && sum255(record.subspan(30, 512)) == record[542],
                    "Microdrive checksum mismatch"sv);
            auto name = nameField(record.subspan(19, 10));
            require(records[name].emplace(record[16], i).second,
                    "Duplicate Microdrive file record"sv);
        }
        std::vector<File> out;
        for (auto &[name, map] : records) {
            File f;
            f.entry.name = name;
            unsigned next = 0;
            bool eof = false;
            for (auto &[number, index] : map) {
                require(number == next++ && !eof, "Incomplete Microdrive file"sv);
                f.records.push_back(index);
                auto rec = slice(image, index * 543, 543);
                append(f.bytes, rec.subspan(30, word(rec, 17)));
                eof = rec[15] & 2;
            }
            require(eof, "Microdrive EOF record missing"sv);
            if (image[f.records[0] * 543 + 15] & 4) {
                require(f.bytes.size() >= 9, "Microdrive Spectrum header missing"sv);
                auto size = word(f.bytes, 1);
                require(size <= f.bytes.size() - 9, "Microdrive Spectrum length exceeds file"sv);
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
        require(!image.back(), "Microdrive cartridge is write-protected"sv);
        for (auto &f : files())
            if (f.entry.name == name) {
                for (auto n : f.records) {
                    std::size_t p = n * 543;
                    std::fill_n(image.begin() + p + 15, 528, 0);
                }
                return;
            }
        throw FormatException("Microdrive file not found"sv);
    }
    void put(std::string_view name, View data, const MediaAttributes &changes) override
    {
        validName(name, 10);
        checkAttrs(changes, { "type"sv, "param1"sv, "param2"sv });
        require(!image.back(), "Microdrive cartridge is write-protected"sv);
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
        std::vector<unsigned> free;
        for (unsigned i = 0; i < 254; ++i) {
            auto r = slice(image, i * 543, 543);
            if (!(r[15] & 2) && word(r, 17) == 0)
                free.push_back(i);
        }
        require(count <= free.size(), "Microdrive full"sv);
        for (unsigned n = 0; n < count; ++n) {
            auto pos = free[n] * 543;
            std::size_t offset = n * 512,
                        size = std::min<std::size_t>(512, payload.size() - offset);
            std::fill_n(image.begin() + pos + 15, 528, 0);
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
        r->image.resize(254 * 543 + 1);
        for (unsigned i = 0; i < 254; ++i) {
            auto p = i * 543;
            r->image[p] = 1;
            r->image[p + 1] = uint8_t(254 - i);
            std::fill_n(r->image.begin() + p + 4, 10, ' ');
            textAt(r->image, p + 4, opts.label);
            r->image[p + 14] = sum255(slice(r->image, p, 14));
        }
    } else {
        require(b.size() == 254 * 543 + 1,
                "MDR image must have 254 sectors and a protection byte"sv);
        r->image.assign(b.begin(), b.end());
        r->files();
    }
    return r;
}
} // namespace zxkit::archive
