// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "zxkit/media/MediaArchive.h"
#include "zxkit/core/Exception.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <limits>
#include <memory>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <string>
#include <string_view>
#endif
namespace zxkit::tool {
using Bytes = std::vector<uint8_t>;
using namespace std::literals;
namespace fs = std::filesystem;
inline Bytes readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw IoException("Cannot open: "s + path.string());
    auto size = file.tellg();
    if (size < 0 || uint64_t(size) > uint64_t(std::numeric_limits<std::streamsize>::max()))
        throw IoException("Invalid input file size"sv);
    Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty()
        && !file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())))
        throw IoException("Cannot read: "s + path.string());
    return bytes;
}
class ImageLock
{
public:
    explicit ImageLock(const fs::path &path)
    {
        auto lock = path;
        lock += ".zxmedia.lock"sv;
#ifdef _WIN32
        m_handle = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
            throw IoException("Image is locked or lock cannot be opened"sv);
#else
        m_fd = ::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (m_fd < 0)
            throw IoException("Cannot open image lock"sv);
        if (flock(m_fd, LOCK_EX | LOCK_NB) < 0) {
            ::close(m_fd);
            m_fd = -1;
            throw IoException("Image is already being edited"sv);
        }
#endif
    }
    ~ImageLock()
    {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
#else
        if (m_fd >= 0)
            ::close(m_fd);
#endif
    }
    ImageLock(const ImageLock &) = delete;
    ImageLock &operator=(const ImageLock &) = delete;

private:
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};
inline fs::path imagePath(const fs::path &path)
{
    if (fs::exists(path))
        return fs::canonical(path);
    return fs::weakly_canonical(fs::absolute(path));
}
inline void atomicWrite(const fs::path &input, std::span<const uint8_t> bytes, bool replace)
{
    auto path = imagePath(input);
    if (!replace && fs::exists(path))
        throw IoException("Output exists: "s + path.string());
    fs::path stage;
    std::random_device random;
    for (unsigned i = 0; i < 10; ++i) {
        stage = path.parent_path() / (".zxmedia-"s + std::to_string(random()));
        if (fs::create_directory(stage))
            break;
        stage.clear();
    }
    if (stage.empty())
        throw IoException("Cannot create transaction directory"sv);
    struct Cleanup
    {
        fs::path p;
        ~Cleanup()
        {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{ stage };
    auto temporary = stage / "image"sv;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out
            || !out.write(reinterpret_cast<const char *>(bytes.data()),
                          std::streamsize(bytes.size())))
            throw IoException("Cannot write image transaction"sv);
        out.flush();
        if (!out)
            throw IoException("Cannot flush image transaction"sv);
    }
    if (fs::exists(path))
        fs::permissions(temporary, fs::status(path).permissions());
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        throw IoException("Cannot commit image transaction"sv);
#else
    int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw IoException("Cannot open image transaction for sync"sv);
    int status = ::fsync(fd);
    ::close(fd);
    if (status < 0)
        throw IoException("Cannot sync image transaction"sv);
    if (!replace) {
        // A hard link gives no-replace semantics even if another process creates the output.
        if (::link(temporary.c_str(), path.c_str()) < 0)
            throw IoException("Cannot create output (possibly already exists)"sv);
    } else
        fs::rename(temporary, path);
    int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        ::fsync(directory);
        ::close(directory);
    }
#endif
}
class MediaFile
{
public:
    fs::path path;
    std::unique_ptr<ImageLock> lock;
    Bytes original;
    MediaArchive archive;
    explicit MediaFile(const fs::path &name, const MediaOptions &options = {}, bool writable = true)
        : path(imagePath(name)),
          lock(writable ? std::make_unique<ImageLock>(path) : nullptr),
          original(readFile(path)),
          archive(MediaArchive::open(original, path.extension().string(), options))
    {
    }
    void save()
    {
        auto encoded = archive.encode();
        if (readFile(path) != original)
            throw IoException("Image changed externally; refusing to overwrite it"sv);
        atomicWrite(path, encoded, true);
        original = std::move(encoded);
    }
};
} // namespace zxkit::tool
