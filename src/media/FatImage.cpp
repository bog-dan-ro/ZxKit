// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/FatImage.h"
#include "archive/ArchiveInternal.h"

#include <string>
#include <string_view>

namespace zxkit {
using namespace std::literals;
using namespace archive;

namespace {
MediaAttributes attributesOf(uint8_t bits)
{
    return { { "readonly"s, bool(bits & 1) },
             { "hidden"s, bool(bits & 2) },
             { "system"s, bool(bits & 4) },
             { "archive"s, bool(bits & 32) } };
}
} // namespace

FatImage::FatImage(FatVolume::BlockReader reader, FatVolume::BlockWriter writer)
    : m_volume(std::move(reader), std::move(writer))
{
}

std::vector<MediaEntry> FatImage::list(std::string_view folder, bool recursive) const
{
    std::vector<std::string> folders{ std::string(folder) };
    std::vector<MediaEntry> out;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        require(folders.size() < 65536, "FAT directory tree exceeds limit (possibly cyclic)"sv);
        const auto entries = m_volume.list(folders[i]);
        if (!entries) {
            require(i == 0, "FAT directory disappeared"sv);
            break;
        }
        for (const auto &e : *entries) {
            auto path = folders[i].empty() ? e.name : folders[i] + "/"s + e.name;
            out.push_back({ path, e.size, e.isDirectory, attributesOf(e.attributes) });
            if (recursive && e.isDirectory)
                folders.push_back(std::move(path));
        }
    }
    return out;
}

std::optional<MediaEntry> FatImage::find(std::string_view name) const
{
    const auto slash = name.rfind('/');
    const auto folder = slash == name.npos ? std::string_view{} : name.substr(0, slash);
    for (auto &entry : list(folder))
        if (upper(entry.name) == upper(name))
            return entry;
    return std::nullopt;
}

std::vector<uint8_t> FatImage::read(std::string_view name) const
{
    auto bytes = m_volume.readFile(name);
    require(bytes.has_value(), "FAT file not found"sv);
    return std::move(*bytes);
}

void FatImage::put(std::string_view name, std::span<const uint8_t> data,
                   const MediaAttributes &attributes)
{
    validName(name, 4096, true);
    checkAttrs(attributes, { "readonly"sv, "hidden"sv, "system"sv, "archive"sv });
    const auto entry = find(name);
    require(!entry || !attr(entry->attributes, "readonly"sv)
                    || (attributes.contains("readonly"sv) && !attr(attributes, "readonly"sv)),
            "FAT file is read-only"sv);
    m_volume.writeFile(name, data);
    if (!attributes.empty())
        setAttributes(name, attributes);
}

void FatImage::remove(std::string_view name)
{
    const auto entry = find(name);
    require(entry && !attr(entry->attributes, "readonly"sv), "FAT file missing or read-only"sv);
    m_volume.remove(name);
}

void FatImage::rename(std::string_view name, std::string_view newName, bool replace)
{
    validName(newName, 4096, true);
    if (replace && upper(name) != upper(newName))
        if (const auto existing = find(newName)) {
            require(!existing->directory, "Cannot replace a directory"sv);
            remove(newName);
        }
    m_volume.rename(name, newName);
}

void FatImage::setAttributes(std::string_view name, const MediaAttributes &attributes)
{
    checkAttrs(attributes, { "readonly"sv, "hidden"sv, "system"sv, "archive"sv });
    const auto entry = find(name);
    require(entry.has_value(), "FAT entry not found"sv);
    auto a = entry->attributes;
    for (auto &[k, v] : attributes) {
        require(v <= 1, "Boolean attribute out of range"sv);
        a[k] = v;
    }
    m_volume.setFileAttributes(name,
                               uint8_t(attr(a, "readonly"sv) | (attr(a, "hidden"sv) << 1)
                                       | (attr(a, "system"sv) << 2) | (attr(a, "archive"sv) << 5)));
}

void FatImage::makeDirectory(std::string_view name)
{
    validName(name, 4096, true);
    m_volume.makeDirectory(name);
}

} // namespace zxkit
