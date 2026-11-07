// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <map>
#include <vector>

namespace zxkit {

using MediaAttributes = std::map<std::string, uint64_t, std::less<>>;
struct MediaEntry
{
    std::string name;
    uint64_t size = 0;
    bool directory = false;
    MediaAttributes attributes;
};
struct MediaOptions
{
    /// Required when a raw disk's filesystem/geometry is ambiguous. See profiles().
    std::string filesystem;
    std::string label = "ZXKIT";
};

/// Editable media independent of emulation. Mutations have a strong exception guarantee.
/// Names are relative '/' paths. Format-specific attributes are validated, never ignored.
/// Generic CP/M lengths are rounded to 128-byte records by that filesystem.
class MediaArchive
{
public:
    static MediaArchive open(std::span<const uint8_t> bytes, std::string_view extension,
                             const MediaOptions &options = {});
    static MediaArchive create(std::string_view extension, const MediaOptions &options = {});
    static std::vector<std::string> profiles();
    MediaArchive(MediaArchive &&) noexcept;
    MediaArchive &operator=(MediaArchive &&) noexcept;
    ~MediaArchive();

    std::vector<MediaEntry> list() const;
    std::vector<uint8_t> read(std::string_view name) const;
    void put(std::string_view name, std::span<const uint8_t> data,
             const MediaAttributes &attributes = {});
    void remove(std::string_view name);
    void rename(std::string_view name, std::string_view newName, bool replace = false);
    void setAttributes(std::string_view name, const MediaAttributes &attributes);
    void makeDirectory(std::string_view name);
    /// Serialize in the original container, or convert to a suitable container.
    std::vector<uint8_t> encode(std::string_view extension = {}) const;
    /// Installs caller-provided boot-track bytes. Boot files must also be added with put().
    void setBootTracks(std::span<const uint8_t> data);

private:
    struct Impl;
    explicit MediaArchive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace zxkit
