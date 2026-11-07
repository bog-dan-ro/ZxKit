// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "../media/MediaFile.h"
#include <charconv>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <cerrno>
#include <climits>
#include <string>
#include <string_view>

using namespace zxkit;
using namespace zxkit::tool;
namespace {
struct OpenFile
{
    std::string name;
    Bytes data;
    Bytes baseline;
    unsigned references = 1;
    bool dirty = false;
    bool created = false;
    bool unlinked = false;
};
struct Mount
{
    MediaFile file;
    bool readOnly = false;
    uint64_t nextId = 1;
    std::map<uint64_t, OpenFile> openFiles;
    explicit Mount(const fs::path &path, const MediaOptions &o, bool ro)
        : file(path, o, !ro), readOnly(ro)
    {
    }
    std::vector<MediaEntry> entries() const
    {
        auto result = file.archive.list();
        for (auto &[id, f] : openFiles)
            if (!f.unlinked) {
                auto i = std::ranges::find(result, f.name, &MediaEntry::name);
                if (i == result.end())
                    result.push_back({ f.name, f.data.size(), false, {} });
                else
                    i->size = f.data.size();
            }
        return result;
    }
    void writable() const
    {
        if (readOnly)
            throw std::system_error(EROFS, std::generic_category());
    }
    uint64_t findOpen(std::string_view name) const
    {
        for (auto &[id, f] : openFiles)
            if (!f.unlinked && f.name == name)
                return id;
        return 0;
    }
    void flush(uint64_t id)
    {
        auto &f = openFiles.at(id);
        if (!f.dirty || f.unlinked)
            return;
        writable();
        if (!f.created && file.archive.read(f.name) != f.baseline)
            throw std::system_error(EBUSY, std::generic_category(),
                                    "Another media view changed this file"s);
        file.archive.put(f.name, f.data);
        file.save();
        f.baseline = f.data;
        f.dirty = false;
        f.created = false;
    }
};
Mount &mount()
{
    return *static_cast<Mount *>(fuse_get_context()->private_data);
}
std::string pathName(const char *p)
{
    return p[0] == '/' ? std::string(p + 1) : std::string(p);
}
template <class F> int call(F f) noexcept
{
    try {
        return f();
    } catch (const std::system_error &e) {
        return -e.code().value();
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::exception &e) {
        std::cerr << "zxmount: "sv << e.what() << '\n';
        return -EIO;
    }
}
MediaEntry entry(std::string_view name)
{
    auto entries = mount().entries();
    auto i = std::ranges::find(entries, name, &MediaEntry::name);
    if (i == entries.end()) {
        auto upper = [](std::string_view text) {
            std::string result(text);
            for (auto &c : result)
                if (c >= 'a' && c <= 'z')
                    c = char(c - 'a' + 'A');
            return result;
        };
        i = std::ranges::find_if(entries, [&](const MediaEntry &candidate) {
            return upper(candidate.name) == upper(name);
        });
    }
    if (i == entries.end())
        throw std::system_error(ENOENT, std::generic_category());
    return *i;
}
int statFile(const char *p, struct stat *s, struct fuse_file_info *info)
{
    return call([&] {
        std::memset(s, 0, sizeof(*s));
        s->st_uid = getuid();
        s->st_gid = getgid();
        auto name = pathName(p);
        if (name.empty()) {
            s->st_mode = S_IFDIR | 0755;
            s->st_nlink = 2;
            return 0;
        }
        auto e = entry(name);
        s->st_mode = (e.directory ? S_IFDIR | 0755 : S_IFREG | 0644);
        if (mount().readOnly
            || (e.attributes.contains("readonly"sv) && e.attributes.at("readonly"s)))
            s->st_mode &= ~0222;
        s->st_size = off_t(e.size);
        s->st_nlink = e.directory ? 2 : 1;
        if (info && info->fh && mount().openFiles.contains(info->fh))
            s->st_size = off_t(mount().openFiles.at(info->fh).data.size());
        return 0;
    });
}
int readDirectory(const char *p, void *buffer, fuse_fill_dir_t fill, off_t offset,
                  struct fuse_file_info *, enum fuse_readdir_flags)
{
    return call([&] {
        auto name = pathName(p);
        if (!name.empty() && !entry(name).directory)
            return -ENOTDIR;
        std::set<std::string> names{ "."s, ".."s };
        auto prefix = name.empty() ? ""s : name + "/"s;
        for (auto &e : mount().entries())
            if (e.name.starts_with(prefix)) {
                auto child = e.name.substr(prefix.size());
                if (!child.empty())
                    names.insert(child.substr(0, child.find('/')));
            }
        std::size_t i = 0;
        for (auto &child : names) {
            if (off_t(i++) < offset)
                continue;
            if (fill(buffer, child.c_str(), nullptr, off_t(i), fuse_fill_dir_flags(0)))
                break;
        }
        return 0;
    });
}
int openFile(const char *p, struct fuse_file_info *info)
{
    return call([&] {
        auto name = pathName(p);
        auto e = entry(name);
        name = e.name;
        if (e.directory)
            return -EISDIR;
        if ((info->flags & O_ACCMODE) != O_RDONLY) {
            mount().writable();
            if (e.attributes.contains("readonly"sv) && e.attributes.at("readonly"s))
                return -EACCES;
        }
        auto &m = mount();
        auto id = m.findOpen(name);
        if (id)
            ++m.openFiles.at(id).references;
        else {
            id = m.nextId++;
            auto data = m.file.archive.read(name);
            m.openFiles.emplace(id, OpenFile{ name, data, data });
        }
        info->fh = id;
        info->direct_io = 1;
        if (info->flags & O_TRUNC) {
            m.openFiles.at(id).data.clear();
            m.openFiles.at(id).dirty = true;
        }
        return 0;
    });
}
int createFile(const char *p, mode_t, struct fuse_file_info *info)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        auto name = pathName(p);
        for (auto &e : m.entries())
            if (e.name == name)
                return -EEXIST;
        auto slash = name.rfind('/');
        if (slash != std::string::npos && !entry(name.substr(0, slash)).directory)
            return -ENOTDIR;
        auto id = m.nextId++;
        m.openFiles.emplace(id, OpenFile{ name, {}, {}, 1, true, true });
        info->fh = id;
        info->direct_io = 1;
        return 0;
    });
}
int readData(const char *, char *buffer, std::size_t size, off_t offset,
             struct fuse_file_info *info)
{
    return call([&] {
        if (offset < 0)
            return -EINVAL;
        auto &data = mount().openFiles.at(info->fh).data;
        if (uint64_t(offset) >= data.size())
            return 0;
        size = std::min({ size, data.size() - std::size_t(offset), std::size_t(INT_MAX) });
        std::memcpy(buffer, data.data() + offset, size);
        return int(size);
    });
}
int writeData(const char *, const char *buffer, std::size_t size, off_t offset,
              struct fuse_file_info *info)
{
    return call([&] {
        mount().writable();
        if (offset < 0 || uint64_t(offset) > UINT32_MAX || size > UINT32_MAX - uint64_t(offset)
            || size > INT_MAX)
            return -EFBIG;
        auto &f = mount().openFiles.at(info->fh);
        if (f.data.size() < uint64_t(offset) + size)
            f.data.resize(std::size_t(offset) + size);
        std::memcpy(f.data.data() + offset, buffer, size);
        f.dirty = true;
        return int(size);
    });
}
int truncateFile(const char *p, off_t size, struct fuse_file_info *info)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        if (size < 0 || uint64_t(size) > UINT32_MAX)
            return -EFBIG;
        auto id = info ? info->fh : m.findOpen(pathName(p));
        if (id) {
            auto &f = m.openFiles.at(id);
            f.data.resize(std::size_t(size));
            f.dirty = true;
        } else {
            auto name = pathName(p);
            auto data = m.file.archive.read(name);
            data.resize(std::size_t(size));
            m.file.archive.put(name, data);
            m.file.save();
        }
        return 0;
    });
}
int flushFile(const char *, struct fuse_file_info *info)
{
    return call([&] {
        mount().flush(info->fh);
        return 0;
    });
}
int syncFile(const char *p, int, struct fuse_file_info *info)
{
    return flushFile(p, info);
}
int releaseFile(const char *, struct fuse_file_info *info)
{
    return call([&] {
        auto &m = mount();
        m.flush(info->fh);
        if (!--m.openFiles.at(info->fh).references)
            m.openFiles.erase(info->fh);
        return 0;
    });
}
int unlinkFile(const char *p)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        auto name = pathName(p);
        auto id = m.findOpen(name);
        if (!id || !m.openFiles.at(id).created) {
            m.file.archive.remove(name);
            m.file.save();
        }
        if (id)
            m.openFiles.at(id).unlinked = true;
        return 0;
    });
}
int mkdirFile(const char *p, mode_t)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        m.file.archive.makeDirectory(pathName(p));
        m.file.save();
        return 0;
    });
}
int renameFile(const char *p, const char *to, unsigned flags)
{
    return call([&] {
        if (flags & ~unsigned(RENAME_NOREPLACE))
            return -EINVAL;
        auto &m = mount();
        m.writable();
        auto from = pathName(p);
        auto dest = pathName(to);
        if (from == dest)
            return 0;
        for (auto &e : m.entries())
            if (e.name == dest && (flags & RENAME_NOREPLACE))
                return -EEXIST;
        auto id = m.findOpen(from);
        if (id)
            m.flush(id);
        m.file.archive.rename(from, dest, !(flags & RENAME_NOREPLACE));
        m.file.save();
        for (auto &[key, f] : m.openFiles)
            if (!f.unlinked && (f.name == from || f.name.starts_with(from + "/"s)))
                f.name = dest + f.name.substr(from.size());
        return 0;
    });
}
int listAttributes(const char *p, char *buffer, std::size_t size)
{
    return call([&] {
        std::string names;
        for (auto &[key, v] : entry(pathName(p)).attributes) {
            names += "user.zxkit."s + key;
            names += '\0';
        }
        if (!size)
            return int(names.size());
        if (size < names.size())
            return -ERANGE;
        std::memcpy(buffer, names.data(), names.size());
        return int(names.size());
    });
}
int getAttribute(const char *p, const char *key, char *buffer, std::size_t size)
{
    return call([&] {
        std::string_view k = key;
        if (!k.starts_with("user.zxkit."sv))
            return -ENODATA;
        auto e = entry(pathName(p));
        auto i = e.attributes.find(k.substr(11));
        if (i == e.attributes.end())
            return -ENODATA;
        auto value = std::to_string(i->second);
        if (!size)
            return int(value.size());
        if (size < value.size())
            return -ERANGE;
        std::memcpy(buffer, value.data(), value.size());
        return int(value.size());
    });
}
int setAttribute(const char *p, const char *key, const char *value, std::size_t size, int flags)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        std::string_view k = key;
        if (!k.starts_with("user.zxkit."sv))
            return -EOPNOTSUPP;
        auto e = entry(pathName(p));
        bool exists = e.attributes.contains(k.substr(11));
        if ((flags & 1) && exists)
            return -EEXIST;
        if ((flags & 2) && !exists)
            return -ENODATA;
        uint64_t n = 0;
        auto r = std::from_chars(value, value + size, n);
        if (r.ec != std::errc{} || r.ptr != value + size)
            return -EINVAL;
        m.file.archive.setAttributes(pathName(p), { { std::string(k.substr(11)), n } });
        m.file.save();
        return 0;
    });
}
int chmodFile(const char *p, mode_t mode, struct fuse_file_info *)
{
    return call([&] {
        auto &m = mount();
        m.writable();
        m.file.archive.setAttributes(pathName(p), { { "readonly"s, !(mode & 0222) } });
        m.file.save();
        return 0;
    });
}
} // namespace
int main(int argc, char **argv)
{
    try {
        if (argc < 3) {
            std::cout
                    << "zxmount IMAGE MOUNTPOINT [--filesystem PROFILE] [-o ro] [FUSE options]\n"sv;
            return argc == 2 && std::string_view(argv[1]) == "--help"sv ? 0 : 1;
        }
        MediaOptions options;
        std::vector<std::string> args{ argv[0], argv[2], "-s"s };
        bool readOnly = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--filesystem"sv) {
                if (++i == argc)
                    throw InvalidArgumentException("Missing filesystem profile"sv);
                options.filesystem = argv[i];
            } else {
                args.emplace_back(argv[i]);
                if (args.back() == "ro"sv || args.back() == "-oro"sv
                    || args.back().find(",ro"sv) != std::string::npos
                    || args.back().starts_with("ro,"sv))
                    readOnly = true;
            }
        }
        Mount state(argv[1], options, readOnly);
        struct fuse_operations ops{};
        ops.getattr = statFile;
        ops.readdir = readDirectory;
        ops.open = openFile;
        ops.create = createFile;
        ops.read = readData;
        ops.write = writeData;
        ops.truncate = truncateFile;
        ops.flush = flushFile;
        ops.fsync = syncFile;
        ops.release = releaseFile;
        ops.unlink = unlinkFile;
        ops.rmdir = unlinkFile;
        ops.mkdir = mkdirFile;
        ops.rename = renameFile;
        ops.listxattr = listAttributes;
        ops.getxattr = getAttribute;
        ops.setxattr = setAttribute;
        ops.chmod = chmodFile;
        std::vector<char *> pointers;
        for (auto &s : args)
            pointers.push_back(s.data());
        return fuse_main(int(pointers.size()), pointers.data(), &ops, &state);
    } catch (const std::exception &e) {
        std::cerr << "zxmount: "sv << e.what() << '\n';
        return 1;
    }
}
