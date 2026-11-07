// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "DiskContainer.h"

#include <set>
#include <string>
#include <string_view>

namespace zxkit::archive {
using namespace std::literals;
namespace {

class Mdos final : public Backend
{
public:
    DiskContainer storage;

    unsigned fat(unsigned sector) const
    {
        require(sector < 1705, "MDOS FAT index out of range"sv);
        const unsigned index = sector % 341;
        const unsigned position = (sector / 341 + 1) * 512 + index * 3 / 2;
        const auto &bytes = storage.logical;
        return index & 1 ? bytes[position + 1] | ((bytes[position] & 15) << 8)
                         : bytes[position] | ((bytes[position + 1] >> 4) << 8);
    }

    void setFat(unsigned sector, unsigned value)
    {
        require(sector < 1705 && value <= 4095, "MDOS FAT value out of range"sv);
        const unsigned index = sector % 341;
        const unsigned position = (sector / 341 + 1) * 512 + index * 3 / 2;
        auto &bytes = storage.logical;
        if (index & 1) {
            bytes[position + 1] = uint8_t(value);
            bytes[position] = uint8_t((bytes[position] & 0xF0) | (value >> 8));
        } else {
            bytes[position] = uint8_t(value);
            bytes[position + 1] = uint8_t((bytes[position + 1] & 15) | ((value >> 8) << 4));
        }
    }

    static unsigned directoryPosition(unsigned slot)
    {
        // MDOS visits even directory sectors before odd directory sectors.
        const unsigned group = slot / 16;
        return (6 + (group % 4) * 2 + group / 4) * 512 + (slot % 16) * 32;
    }

    struct File
    {
        MediaEntry entry;
        unsigned slot;
        std::vector<unsigned> sectors;
        Bytes data;
    };

    std::vector<File> files() const
    {
        std::vector<File> result;
        const auto &bytes = storage.logical;
        require(nameField(slice(bytes, 204, 4)) == "SDOS"sv, "Missing MDOS signature"sv);
        std::set<unsigned> allocated;
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto entry = slice(bytes, directoryPosition(slot), 32);
            if (entry[0] == 0xE5 || !entry[0])
                continue;
            require("PBCNQS"sv.find(char(entry[0])) != std::string_view::npos,
                    "Unknown MDOS file type"sv);
            File file;
            file.slot = slot;
            file.entry.name = visibleName(entry.subspan(1, 10)) + "."s + char(entry[0]);
            file.entry.size = word(entry, 11) | (uint32_t(entry[21]) << 16);
            const auto flags = entry[20];
            file.entry.attributes = { { "type"s, entry[0] },
                                      { "param1"s, word(entry, 13) },
                                      { "param2"s, word(entry, 15) },
                                      { "hidden"s, bool(flags & 128) },
                                      { "system"s, bool(flags & 64) },
                                      { "protected"s, bool(flags & 32) },
                                      { "archive"s, bool(flags & 16) },
                                      { "readable"s, bool(flags & 8) },
                                      { "readonly"s, !(flags & 4) },
                                      { "executable"s, bool(flags & 2) },
                                      { "deletable"s, bool(flags & 1) } };
            unsigned sector = word(entry, 17);
            std::size_t remaining = file.entry.size;
            do {
                require(sector >= 14 && sector < bytes.size() / 512,
                        "MDOS file sector outside disk"sv);
                require(allocated.insert(sector).second, "Cross-linked or cyclic MDOS file"sv);
                file.sectors.push_back(sector);
                const unsigned next = fat(sector);
                const std::size_t count = std::min<std::size_t>(remaining, 512);
                append(file.data, slice(bytes, sector * 512, count));
                remaining -= count;
                if (next >= 0xC00) {
                    require(!remaining && (next >= 0xE00 || (!file.entry.size && next == 0xC00)),
                            "Invalid MDOS file termination"sv);
                    break;
                }
                require(remaining > 0, "MDOS chain exceeds file length"sv);
                sector = next;
            } while (true);
            result.push_back(std::move(file));
        }
        return result;
    }

    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Mdos>(*this);
    }
    std::vector<MediaEntry> list() const override
    {
        std::vector<MediaEntry> result;
        for (const auto &file : files())
            result.push_back(file.entry);
        return result;
    }
    Bytes read(std::string_view name) const override
    {
        for (const auto &file : files())
            if (file.entry.name == name)
                return file.data;
        throw FormatException("MDOS file not found"sv);
    }
    void remove(std::string_view name) override
    {
        for (const auto &file : files()) {
            if (file.entry.name != name)
                continue;
            require(!attr(file.entry.attributes, "readonly"sv)
                            && attr(file.entry.attributes, "deletable"sv),
                    "MDOS file is protected"sv);
            for (const auto sector : file.sectors)
                setFat(sector, 0);
            storage.logical[directoryPosition(file.slot)] = 0xE5;
            return;
        }
        throw FormatException("MDOS file not found"sv);
    }
    void attributes(std::string_view name, const MediaAttributes &changes) override
    {
        checkAttrs(changes,
                   { "type"sv, "param1"sv, "param2"sv, "hidden"sv, "system"sv, "protected"sv,
                     "archive"sv, "readable"sv, "readonly"sv, "executable"sv, "deletable"sv });
        for (auto file : files()) {
            if (file.entry.name != name)
                continue;
            for (const auto &[key, value] : changes) {
                if (key == "type"sv) {
                    require(value <= 255 && "PBCNQS"sv.find(char(value)) != std::string_view::npos,
                            "Invalid MDOS type"sv);
                    auto target = file.entry.name;
                    target.back() = char(value);
                    for (const auto &other : files())
                        require(other.entry.name == name || other.entry.name != target,
                                "Destination exists"sv);
                } else if (key == "param1"sv || key == "param2"sv)
                    require(value <= 65535, "MDOS parameter out of range"sv);
                else
                    require(value <= 1, "Boolean attribute out of range"sv);
                file.entry.attributes[key] = value;
            }
            const auto &a = file.entry.attributes;
            const auto position = directoryPosition(file.slot);
            storage.logical[position] = uint8_t(a.at("type"s));
            setWord(storage.logical, position + 13, a.at("param1"s));
            setWord(storage.logical, position + 15, a.at("param2"s));
            storage.logical[position + 20] =
                    uint8_t((attr(a, "hidden"sv) << 7) | (attr(a, "system"sv) << 6)
                            | (attr(a, "protected"sv) << 5) | (attr(a, "archive"sv) << 4)
                            | (attr(a, "readable"sv) << 3) | (!attr(a, "readonly"sv) << 2)
                            | (attr(a, "executable"sv) << 1) | attr(a, "deletable"sv));
            return;
        }
        throw FormatException("MDOS file not found"sv);
    }
    void put(std::string_view name, View data, const MediaAttributes &changes) override
    {
        checkAttrs(changes,
                   { "type"sv, "param1"sv, "param2"sv, "hidden"sv, "system"sv, "protected"sv,
                     "archive"sv, "readable"sv, "readonly"sv, "executable"sv, "deletable"sv });
        const auto dot = name.rfind('.');
        require(dot != name.npos && dot <= 10 && dot + 2 == name.size()
                        && "PBCNQS"sv.find(name.back()) != std::string_view::npos,
                "Use MDOS NAME.P/B/C/N/Q/S filenames"sv);
        validName(name.substr(0, dot), 10);
        require(data.size() <= 0xFFFFFF, "MDOS file too large"sv);
        MediaAttributes a{ { "readable"s, 1 }, { "deletable"s, 1 }, { "executable"s, 1 } };
        for (const auto &file : files()) {
            if (file.entry.name == name) {
                a = file.entry.attributes;
                if (attr(changes, "readonly"sv, 1) == 0)
                    attributes(name, { { "readonly"s, 0 } });
                remove(name);
                break;
            }
        }
        for (const auto &[key, value] : changes)
            a[key] = value;
        require(!a.contains("type"sv) || a.at("type"s) == uint8_t(name.back()),
                "MDOS type must match the filename extension"sv);
        auto &bytes = storage.logical;
        unsigned slot = 0;
        while (slot < 128 && bytes[directoryPosition(slot)] != 0xE5
               && bytes[directoryPosition(slot)] != 0)
            ++slot;
        require(slot < 128, "MDOS directory full"sv);
        const std::size_t needed = std::max<std::size_t>(1, (data.size() + 511) / 512);
        std::vector<unsigned> free;
        for (unsigned sector = 14; sector < bytes.size() / 512 && free.size() < needed; ++sector)
            if (fat(sector) == 0)
                free.push_back(sector);
        require(free.size() == needed, "MDOS disk full"sv);
        for (std::size_t index = 0; index < free.size(); ++index) {
            const std::size_t offset = std::min<std::size_t>(index * 512, data.size());
            const std::size_t count = std::min<std::size_t>(512, data.size() - offset);
            std::fill_n(bytes.begin() + free[index] * 512, 512, 0);
            std::copy_n(data.begin() + offset, count, bytes.begin() + free[index] * 512);
            setFat(free[index],
                   index + 1 < free.size() ? free[index + 1]
                           : data.empty()  ? 0xC00
                                           : 0xE00 | (count % 512));
        }
        const auto position = directoryPosition(slot);
        std::fill_n(bytes.begin() + position, 32, 0xE5);
        bytes[position] = uint8_t(name.back());
        std::fill_n(bytes.begin() + position + 1, 10, 0);
        textAt(bytes, position + 1, name.substr(0, dot));
        setWord(bytes, position + 11, data.size() & 65535);
        bytes[position + 21] = uint8_t(data.size() >> 16);
        setWord(bytes, position + 13, attr(a, "param1"sv, 32768));
        setWord(bytes, position + 15, attr(a, "param2"sv, 32768));
        setWord(bytes, position + 17, free.front());
        bytes[position + 19] = 0;
        MediaAttributes flags = a;
        flags.erase("type"s);
        flags.erase("param1"s);
        flags.erase("param2"s);
        attributes(name, flags);
    }
    Bytes encode(std::string_view extension) override
    {
        return storage.encode(extension);
    }
};
} // namespace

std::unique_ptr<Backend> mdos(View bytes, std::string_view extension, const MediaOptions &options,
                              bool create)
{
    auto result = std::make_unique<Mdos>();
    result->storage = DiskContainer::open(bytes, extension, options, create);
    if (create) {
        auto &data = result->storage.logical;
        std::fill(data.begin(), data.end(), 0xE5);
        std::fill_n(data.begin(), 6 * 512, 0);
        for (unsigned sector = 0; sector < 1705; ++sector)
            result->setFat(sector, sector < 14 || sector >= data.size() / 512 ? 0xDDD : 0);
        for (unsigned sector = 1; sector <= 5; ++sector)
            data[sector * 512 + 511] = uint8_t((data[sector * 512 + 511] & 0xF0) | 13);
        data[177] = result->storage.profile.cylinders == 40 ? 0x18 : 0x10;
        data[178] = uint8_t(result->storage.profile.cylinders);
        data[179] = 9;
        std::copy_n(data.begin() + 177, 4, data.begin() + 181);
        std::copy_n(data.begin() + 177, 4, data.begin() + 128);
        validName(options.label, 10);
        std::fill_n(data.begin() + 192, 10, 0);
        textAt(data, 192, options.label);
        textAt(data, 204, "SDOS"sv);
    }
    result->files();
    return result;
}
} // namespace zxkit::archive
