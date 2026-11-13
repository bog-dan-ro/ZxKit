// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MediaFile.h"
#include "zxkit/media/FatImage.h"

#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zxkit::tool {

/// An image zxmedia works on: a host file, as an archive saved atomically or a FAT card
/// edited in place, or an image inside another one, saved back into it
class Image
{
public:
    virtual ~Image() = default;
    virtual std::string filesystem() const = 0;
    virtual std::vector<MediaEntry> list() const = 0;
    virtual Bytes read(std::string_view name) const = 0;
    virtual void put(std::string_view name, std::span<const uint8_t> data,
                     const MediaAttributes &attributes) = 0;
    virtual void remove(std::string_view name) = 0;
    virtual void rename(std::string_view name, std::string_view newName, bool replace) = 0;
    virtual void setAttributes(std::string_view name, const MediaAttributes &attributes) = 0;
    virtual void makeDirectory(std::string_view name) = 0;
    virtual void setBootTracks(std::span<const uint8_t>)
    {
        throw FormatException("This image has no CP/M boot tracks"sv);
    }
    virtual Bytes encode(std::string_view extension) const = 0;
    /// Writes the changes where the image came from
    virtual void save() = 0;
    /// The entry called `name`, matched as the filesystem does
    std::optional<MediaEntry> find(std::string_view name) const
    {
        const bool fat = filesystem() == "fat"sv;
        const auto same = [&](std::string_view a) {
            return fat ? std::ranges::equal(a, name,
                                            [](char x, char y) {
                                                return std::toupper(uint8_t(x))
                                                        == std::toupper(uint8_t(y));
                                            })
                       : a == name;
        };
        for (auto &entry : list())
            if (same(entry.name))
                return entry;
        return std::nullopt;
    }
};

namespace detail {
inline bool tapeExtension(std::string_view extension)
{
    return extension == ".tap"sv || extension == ".tzx"sv || extension == ".pzx"sv
            || extension == ".csw"sv || extension == ".nex"sv || extension == ".mdr"sv;
}

inline std::string lowerExtension(std::string_view name)
{
    auto extension = fs::path(std::string(name)).extension().string();
    for (auto &c : extension)
        c = char(std::tolower(uint8_t(c)));
    return extension;
}

// Detection is conservative: when it can't settle a disk, the one profile that reads it
inline MediaArchive openArchive(std::span<const uint8_t> bytes, const std::string &extension,
                                MediaOptions &options)
{
    try {
        return MediaArchive::open(bytes, extension, options);
    } catch (const std::exception &) {
        if (!options.filesystem.empty() || tapeExtension(extension))
            throw;
        const auto matches = MediaArchive::probe(bytes, extension);
        if (matches.size() == 1) {
            options.filesystem = matches.front().profile;
            return MediaArchive::open(bytes, extension, options);
        }
        if (matches.empty())
            throw;
        std::string message = "Several filesystems read this image; choose one with "
                              "--filesystem:"s;
        for (const auto &match : matches)
            message += std::format(" {} ({} files)", match.profile, match.files);
        throw FormatException(message);
    }
}

inline bool fatSignature(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 512> sector{};
    if (!file.read(reinterpret_cast<char *>(sector.data()), sector.size()))
        return false;
    return sector[510] == 0x55 && sector[511] == 0xAA
            && (sector[0] == 0xEB || sector[0] == 0xE9 || sector[446 + 4] != 0);
}
} // namespace detail

/// An archive in memory: a host file saved atomically, or an image inside another one
class ArchiveImage final : public Image
{
public:
    // A host file
    ArchiveImage(const fs::path &file, MediaOptions options, bool writable)
        : m_path(imagePath(file)),
          m_lock(writable ? std::make_unique<ImageLock>(m_path) : nullptr),
          m_original(readFile(m_path)),
          m_archive(
                  detail::openArchive(m_original, detail::lowerExtension(m_path.string()), options))
    {
    }
    // An entry of `parent`
    ArchiveImage(std::unique_ptr<Image> parent, std::string entry, MediaOptions options)
        : m_parent(std::move(parent)),
          m_entry(std::move(entry)),
          m_original(m_parent->read(m_entry)),
          m_archive(detail::openArchive(m_original, detail::lowerExtension(m_entry), options))
    {
    }
    // A new image: `create`
    ArchiveImage(std::unique_ptr<Image> parent, std::string entry, MediaArchive archive)
        : m_parent(std::move(parent)), m_entry(std::move(entry)), m_archive(std::move(archive))
    {
    }

    std::string filesystem() const override
    {
        return m_archive.filesystem();
    }
    std::vector<MediaEntry> list() const override
    {
        return m_archive.list();
    }
    Bytes read(std::string_view name) const override
    {
        return m_archive.read(name);
    }
    void put(std::string_view name, std::span<const uint8_t> data,
             const MediaAttributes &attributes) override
    {
        m_archive.put(name, data, attributes);
    }
    void remove(std::string_view name) override
    {
        m_archive.remove(name);
    }
    void rename(std::string_view name, std::string_view newName, bool replace) override
    {
        m_archive.rename(name, newName, replace);
    }
    void setAttributes(std::string_view name, const MediaAttributes &attributes) override
    {
        m_archive.setAttributes(name, attributes);
    }
    void makeDirectory(std::string_view name) override
    {
        m_archive.makeDirectory(name);
    }
    void setBootTracks(std::span<const uint8_t> data) override
    {
        m_archive.setBootTracks(data);
    }
    Bytes encode(std::string_view extension) const override
    {
        return m_archive.encode(extension);
    }
    void save() override
    {
        auto encoded = m_archive.encode();
        if (m_parent) {
            m_parent->put(m_entry, encoded, {});
            m_parent->save();
        } else {
            if (readFile(m_path) != m_original)
                throw IoException("Image changed externally; refusing to overwrite it"sv);
            atomicWrite(m_path, encoded, true);
        }
        m_original = std::move(encoded);
    }

private:
    std::unique_ptr<Image> m_parent;
    std::string m_entry;
    fs::path m_path;
    std::unique_ptr<ImageLock> m_lock;
    Bytes m_original;
    MediaArchive m_archive;
};

/// A FAT card edited in place, a block at a time: large cards are not copied for a change
class FatCardImage final : public Image
{
public:
    FatCardImage(const fs::path &file, bool writable)
        : m_path(imagePath(file)),
          m_lock(writable ? std::make_unique<ImageLock>(m_path) : nullptr),
          m_file(m_path,
                 std::ios::binary | std::ios::in | (writable ? std::ios::out : std::ios::in))
    {
        if (!m_file)
            throw IoException("Cannot open: "s + m_path.string());
        FatVolume::BlockWriter writer;
        if (writable)
            writer = [this](uint64_t lba, std::span<const uint8_t, 512> in) {
                m_file.clear();
                m_file.seekp(std::streamoff(lba * 512));
                m_file.write(reinterpret_cast<const char *>(in.data()), 512);
                return bool(m_file);
            };
        m_fat.emplace(
                [this](uint64_t lba, std::span<uint8_t, 512> out) {
                    m_file.clear();
                    m_file.seekg(std::streamoff(lba * 512));
                    m_file.read(reinterpret_cast<char *>(out.data()), 512);
                    return m_file.gcount() == 512;
                },
                std::move(writer));
    }

    std::string filesystem() const override
    {
        return "fat"s;
    }
    std::vector<MediaEntry> list() const override
    {
        return m_fat->list({}, true);
    }
    Bytes read(std::string_view name) const override
    {
        return m_fat->read(name);
    }
    void put(std::string_view name, std::span<const uint8_t> data,
             const MediaAttributes &attributes) override
    {
        m_fat->put(name, data, attributes);
    }
    void remove(std::string_view name) override
    {
        m_fat->remove(name);
    }
    void rename(std::string_view name, std::string_view newName, bool replace) override
    {
        m_fat->rename(name, newName, replace);
    }
    void setAttributes(std::string_view name, const MediaAttributes &attributes) override
    {
        m_fat->setAttributes(name, attributes);
    }
    void makeDirectory(std::string_view name) override
    {
        m_fat->makeDirectory(name);
    }
    Bytes encode(std::string_view extension) const override
    {
        auto ext = detail::lowerExtension("x."s + std::string(extension));
        if (extension.starts_with('.'))
            ext = detail::lowerExtension("x"s + std::string(extension));
        if (!extension.empty() && ext != ".img"sv && ext != ".raw"sv)
            throw FormatException("FAT SD cards use IMG or RAW containers"sv);
        m_file.flush();
        return readFile(m_path);
    }
    void save() override
    {
        m_file.flush();
        if (!m_file)
            throw IoException("Cannot write: "s + m_path.string());
    }

private:
    fs::path m_path;
    std::unique_ptr<ImageLock> m_lock;
    mutable std::fstream m_file;
    std::optional<FatImage> m_fat;
};

/// Opens a host image: a FAT card in place, anything else as an archive
inline std::unique_ptr<Image> openHostImage(const fs::path &path, MediaOptions options,
                                            bool writable)
{
    if ((options.filesystem.empty() || options.filesystem == "fat"sv)
        && detail::fatSignature(path)) {
        try {
            return std::make_unique<FatCardImage>(path, writable);
        } catch (const FormatException &) {
            // A boot sector without a FAT: read it as a disk
        }
    }
    return std::make_unique<ArchiveImage>(path, std::move(options), writable);
}

/// Where a path leads: an image, and the entry the path names inside it when `entry` was
/// asked for (empty when the path names the image itself)
struct ResolvedPath
{
    std::unique_ptr<Image> image;
    std::string entry;
    fs::path host; ///< a host file and no image, for a plain file
};

/// What a path names: an image, an entry of one, or a new entry of the innermost image
enum class PathTarget { Image, Entry, New };

/// Resolves IMAGE/ENTRY/... paths the way MC's do: the longest prefix that is a host file is
/// the outer image, and the rest goes through the images inside it. `options` (an explicit
/// filesystem) apply to the innermost image. For an Entry, a last component that is an entry
/// is returned rather than opened; for a New one, whatever follows the innermost image.
inline ResolvedPath resolvePath(std::string_view text, const MediaOptions &options, bool writable,
                                PathTarget target = PathTarget::Image)
{
    const bool entry = target != PathTarget::Image;
    const fs::path whole{ std::string(text) };
    if (fs::is_regular_file(whole)) {
        if (entry)
            return { nullptr, {}, whole };
        return { openHostImage(whole, options, writable), {}, {} };
    }
    std::vector<std::string> parts;
    for (const auto &part : whole)
        if (!part.empty() && part != "/"sv)
            parts.push_back(part.string());
    fs::path host = whole.is_absolute() ? whole.root_path() : fs::path{};
    std::size_t used = 0;
    while (used < parts.size() && !fs::is_regular_file(host)) {
        host /= parts[used];
        ++used;
    }
    if (!fs::is_regular_file(host))
        throw IoException("Cannot open: "s + std::string(text));
    std::vector<std::string> rest(parts.begin() + std::ptrdiff_t(used), parts.end());
    const auto join = [&](std::size_t from, std::size_t to) {
        std::string name;
        for (auto i = from; i < to; ++i)
            name += (name.empty() ? ""s : "/"s) + rest[i];
        return name;
    };
    auto current = openHostImage(host, rest.empty() ? options : MediaOptions{}, writable);
    for (std::size_t at = 0; at < rest.size();) {
        // The longest name inside this image that is a file
        std::size_t end = rest.size();
        std::optional<MediaEntry> found;
        for (; end > at; --end)
            if ((found = current->find(join(at, end))) && !found->directory)
                break;
        if (end == at) {
            if (target == PathTarget::New)
                return { std::move(current), join(at, rest.size()), {} };
            throw IoException(
                    std::format("No such image or file inside: {}", join(at, rest.size())));
        }
        if (entry && end == rest.size())
            return { std::move(current), found->name, {} };
        const bool innermost = end == rest.size();
        current = std::make_unique<ArchiveImage>(std::move(current), found->name,
                                                 innermost ? options : MediaOptions{});
        at = end;
    }
    return { std::move(current), {}, {} };
}

} // namespace zxkit::tool
