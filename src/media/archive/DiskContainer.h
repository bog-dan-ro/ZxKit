// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ArchiveInternal.h"
#include "zxkit/peripherals/storage/FloppyDisk.h"
namespace zxkit::archive {
struct Profile
{
    std::string_view name;
    std::string_view fs;
    unsigned cylinders;
    unsigned sides;
    unsigned sectors;
    unsigned sectorSize;
    unsigned blockSize;
    unsigned dirEntries;
    unsigned bootTracks;
    unsigned skew = 1;
    unsigned firstSector = 1;
};
std::span<const Profile> diskProfiles();
struct DiskContainer
{
    DiskImage image;
    Profile profile;
    Bytes logical;
    Bytes prefix;
    std::string ext;
    static DiskContainer open(View, std::string_view, const MediaOptions &, bool);
    Bytes encode(std::string_view) const;
};
} // namespace zxkit::archive
