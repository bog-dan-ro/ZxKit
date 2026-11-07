// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include "zxkit/peripherals/storage/FatVolume.h"
#include <set>
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
struct Fat final : Backend
{
    Bytes image;
    FatVolume volume() const
    {
        return FatVolume([this](uint64_t lba, std::span<uint8_t, 512> out) {
            if (lba >= image.size() / 512)
                return false;
            std::copy_n(image.begin() + lba * 512, 512, out.begin());
            return true;
        });
    }
    FatVolume writable()
    {
        return FatVolume(
                [this](uint64_t lba, std::span<uint8_t, 512> out) {
                    if (lba >= image.size() / 512)
                        return false;
                    std::copy_n(image.begin() + lba * 512, 512, out.begin());
                    return true;
                },
                [this](uint64_t lba, std::span<const uint8_t, 512> in) {
                    if (lba >= image.size() / 512)
                        return false;
                    std::ranges::copy(in, image.begin() + lba * 512);
                    return true;
                });
    }
    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Fat>(*this);
    }
    std::vector<MediaEntry> list() const override
    {
        auto v = volume();
        std::vector<std::string> paths{ ""s };
        std::vector<MediaEntry> out;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            require(paths.size() < 65536, "FAT directory tree exceeds limit (possibly cyclic)"sv);
            auto entries = v.list(paths[i]);
            require(entries.has_value(), "FAT directory disappeared"sv);
            for (auto &e : *entries) {
                auto path = paths[i].empty() ? e.name : paths[i] + "/"s + e.name;
                out.push_back({ path,
                                e.size,
                                e.isDirectory,
                                { { "readonly"s, bool(e.attributes & 1) },
                                  { "hidden"s, bool(e.attributes & 2) },
                                  { "system"s, bool(e.attributes & 4) },
                                  { "archive"s, bool(e.attributes & 32) } } });
                if (e.isDirectory)
                    paths.push_back(path);
            }
        }
        return out;
    }
    Bytes read(std::string_view name) const override
    {
        auto bytes = volume().readFile(name);
        require(bytes.has_value(), "FAT file not found"sv);
        return std::move(*bytes);
    }
    void attributes(std::string_view name, const MediaAttributes &a) override
    {
        checkAttrs(a, { "readonly"sv, "hidden"sv, "system"sv, "archive"sv });
        auto entries = list();
        auto i = std::ranges::find_if(
                entries, [&](const MediaEntry &entry) { return upper(entry.name) == upper(name); });
        require(i != entries.end(), "FAT entry not found"sv);
        auto attrs = i->attributes;
        for (auto &[k, v] : a) {
            require(v <= 1, "Boolean attribute out of range"sv);
            attrs[k] = v;
        }
        uint8_t bits = uint8_t(attr(attrs, "readonly"sv) | (attr(attrs, "hidden"sv) << 1)
                               | (attr(attrs, "system"sv) << 2) | (attr(attrs, "archive"sv) << 5));
        writable().setFileAttributes(name, bits);
    }
    void put(std::string_view name, View bytes, const MediaAttributes &a) override
    {
        validName(name, 4096, true);
        auto entries = list();
        auto i = std::ranges::find_if(
                entries, [&](const MediaEntry &entry) { return upper(entry.name) == upper(name); });
        require(i == entries.end() || !attr(i->attributes, "readonly"sv)
                        || (a.contains("readonly"sv) && !attr(a, "readonly"sv)),
                "FAT file is read-only"sv);
        writable().writeFile(name, bytes);
        if (!a.empty())
            attributes(name, a);
    }
    void remove(std::string_view name) override
    {
        auto entries = list();
        auto i = std::ranges::find_if(
                entries, [&](const MediaEntry &entry) { return upper(entry.name) == upper(name); });
        require(i != entries.end() && !attr(i->attributes, "readonly"sv),
                "FAT file missing or read-only"sv);
        writable().remove(name);
    }
    void rename(std::string_view name, std::string_view to) override
    {
        validName(to, 4096, true);
        writable().rename(name, to);
    }
    void mkdir(std::string_view name) override
    {
        validName(name, 4096, true);
        writable().makeDirectory(name);
    }
    Bytes encode(std::string_view ext) override
    {
        require(ext.empty() || extension(ext) == ".img"sv || extension(ext) == ".raw"sv,
                "FAT SD cards use IMG or RAW containers"sv);
        return image;
    }
};
} // namespace
std::unique_ptr<Backend> fat(View b, const MediaOptions &options, bool create)
{
    auto r = std::make_unique<Fat>();
    if (create) {
        // 32 MiB FAT16 superfloppy; 512-byte clusters, two FAT copies and 512 root entries.
        auto &i = r->image;
        i.resize(32 * 1024 * 1024);
        i[0] = 0xEB;
        i[1] = 0x3C;
        i[2] = 0x90;
        textAt(i, 3, "ZXKIT   "sv);
        setWord(i, 11, 512);
        i[13] = 1;
        setWord(i, 14, 1);
        i[16] = 2;
        setWord(i, 17, 512);
        i[21] = 0xF8;
        setWord(i, 22, 254);
        setWord(i, 24, 32);
        setWord(i, 26, 2);
        setDword(i, 32, 65536);
        i[36] = 128;
        i[38] = 0x29;
        textAt(i, 54, "FAT16   "sv);
        i[510] = 85;
        i[511] = 170;
        validName(options.label, 11);
        std::fill_n(i.begin() + 43, 11, ' ');
        textAt(i, 43, upper(options.label));
        for (auto p : { std::size_t(512), std::size_t(255 * 512) }) {
            i[p] = 0xF8;
            i[p + 1] = 255;
            i[p + 2] = 255;
            i[p + 3] = 255;
        }
    } else
        r->image.assign(b.begin(), b.end());
    r->volume();
    return r;
}
} // namespace zxkit::archive
