// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include "zxkit/media/FatImage.h"
#include <set>
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
struct Fat final : Backend
{
    Bytes image;
    // Over this copy of the image: a transaction edits a clone
    FatVolume::BlockReader reader() const
    {
        return [this](uint64_t lba, std::span<uint8_t, 512> out) {
            if (lba >= image.size() / 512)
                return false;
            std::copy_n(image.begin() + lba * 512, 512, out.begin());
            return true;
        };
    }
    FatImage files() const
    {
        return FatImage(reader());
    }
    FatImage writableFiles()
    {
        return FatImage(reader(), [this](uint64_t lba, std::span<const uint8_t, 512> in) {
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
    std::string filesystem() const override
    {
        return "fat"s;
    }
    std::vector<MediaEntry> list() const override
    {
        return files().list({}, true);
    }
    Bytes read(std::string_view name) const override
    {
        return files().read(name);
    }
    void attributes(std::string_view name, const MediaAttributes &a) override
    {
        writableFiles().setAttributes(name, a);
    }
    void put(std::string_view name, View bytes, const MediaAttributes &a) override
    {
        writableFiles().put(name, bytes, a);
    }
    void remove(std::string_view name) override
    {
        writableFiles().remove(name);
    }
    void rename(std::string_view name, std::string_view to) override
    {
        writableFiles().rename(name, to);
    }
    void mkdir(std::string_view name) override
    {
        writableFiles().makeDirectory(name);
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
    r->files();
    return r;
}
} // namespace zxkit::archive
