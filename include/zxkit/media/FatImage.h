// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "zxkit/media/MediaArchive.h"
#include "zxkit/peripherals/storage/FatVolume.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace zxkit {

/// A FAT image (the ZX Next's SD card) edited in place, a block at a time, with MediaArchive's
/// entry names and attributes (readonly, hidden, system, archive). For images too large to
/// copy for each change: unlike MediaArchive, a change is not a transaction, its blocks are
/// written as it goes. Names are relative '/' paths, matched without regard to ASCII case.
class FatImage
{
public:
    /// Mounts the first FAT partition, read-only without a `writer`. Throws FormatException
    /// if there is none.
    explicit FatImage(FatVolume::BlockReader reader, FatVolume::BlockWriter writer = {});

    bool isWritable() const noexcept
    {
        return m_volume.isWritable();
    }
    /// The entries directly in `folder` ("" is the root), or with `recursive` all those below
    /// it, folders before their contents. Empty for a missing folder.
    std::vector<MediaEntry> list(std::string_view folder = {}, bool recursive = false) const;
    std::optional<MediaEntry> find(std::string_view name) const;
    std::vector<uint8_t> read(std::string_view name) const;
    /// Creates or replaces a file; its folder must exist. A read-only file is kept unless the
    /// attributes clear readonly.
    void put(std::string_view name, std::span<const uint8_t> data,
             const MediaAttributes &attributes = {});
    /// Removes a file or an empty folder that is not read-only
    void remove(std::string_view name);
    void rename(std::string_view name, std::string_view newName, bool replace = false);
    void setAttributes(std::string_view name, const MediaAttributes &attributes);
    void makeDirectory(std::string_view name);

private:
    FatVolume m_volume;
};

} // namespace zxkit
