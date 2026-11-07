// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "zxkit/media/MediaArchive.h"
#include "zxkit/core/Exception.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace zxkit::archive {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
using namespace std::literals;
inline void require(bool condition, std::string_view message)
{
    if (!condition)
        throw FormatException(std::string(message));
}
inline View slice(View b, std::size_t offset, std::size_t count)
{
    require(offset <= b.size() && count <= b.size() - offset, "Truncated media"sv);
    return b.subspan(offset, count);
}
inline uint16_t word(View b, std::size_t p)
{
    auto s = slice(b, p, 2);
    return s[0] | (s[1] << 8);
}
inline uint32_t dword(View b, std::size_t p)
{
    return word(b, p) | (uint32_t(word(b, p + 2)) << 16);
}
inline void setWord(Bytes &b, std::size_t p, uint64_t n)
{
    require(n <= 65535 && p + 2 <= b.size(), "16-bit field out of range"sv);
    b[p] = uint8_t(n);
    b[p + 1] = uint8_t(n >> 8);
}
inline void setDword(Bytes &b, std::size_t p, uint64_t n)
{
    require(n <= UINT32_MAX && p + 4 <= b.size(), "32-bit field out of range"sv);
    for (unsigned i = 0; i < 4; ++i)
        b[p + i] = uint8_t(n >> (8 * i));
}
inline void append(Bytes &b, View v)
{
    b.insert(b.end(), v.begin(), v.end());
}
inline void textAt(Bytes &b, std::size_t p, std::string_view s)
{
    require(p <= b.size() && s.size() <= b.size() - p, "Text field out of range"sv);
    std::ranges::copy(s, b.begin() + p);
}
inline std::string upper(std::string_view s)
{
    std::string r(s);
    for (auto &c : r)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return r;
}
inline std::string extension(std::string_view s)
{
    std::string r(s);
    if (!r.empty() && r[0] != '.')
        r.insert(r.begin(), '.');
    for (auto &c : r)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return r;
}
inline std::string nameField(View s)
{
    std::string r;
    for (auto c : s)
        r += char(c & 127);
    while (!r.empty() && (r.back() == ' ' || r.back() == 0))
        r.pop_back();
    return r;
}
inline std::string visibleName(View bytes)
{
    while (!bytes.empty() && (bytes.back() == ' ' || bytes.back() == 0))
        bytes = bytes.first(bytes.size() - 1);
    if (bytes.empty())
        return "%20"s;
    std::string result;
    bool leadingSpace = true;
    for (auto c : bytes) {
        leadingSpace = leadingSpace && c == ' ';
        if (leadingSpace || c < 32 || c >= 127 || c == '/' || c == '\\' || c == '%')
            result += std::format("%{:02X}", c);
        else
            result += char(c);
    }
    if (result == "."sv || result == ".."sv)
        result.replace(0, 1, "%2E"sv);
    return result;
}
inline void validName(std::string_view name, std::size_t max, bool paths = false)
{
    require(!name.empty() && name.size() <= max && name != "."sv && name != ".."sv,
            "Invalid filename length"sv);
    for (unsigned char c : name)
        require(c >= 32 && (paths || c < 127) && c != '\\' && (paths || c != '/'),
                "Invalid filename character"sv);
    require(!name.starts_with('/') && name.find("../"sv) == name.npos
                    && name.find("/.."sv) == name.npos,
            "Invalid path"sv);
}
inline uint64_t attr(const MediaAttributes &a, std::string_view key, uint64_t fallback = 0)
{
    auto i = a.find(key);
    return i == a.end() ? fallback : i->second;
}
inline void checkAttrs(const MediaAttributes &a, std::initializer_list<std::string_view> keys)
{
    for (auto &[k, v] : a)
        require(std::ranges::find(keys, k) != keys.end(),
                std::format("Unsupported attribute: {}", k));
}
struct Backend
{
    virtual ~Backend() = default;
    virtual std::unique_ptr<Backend> clone() const = 0;
    virtual std::vector<MediaEntry> list() const = 0;
    virtual Bytes read(std::string_view) const = 0;
    virtual void put(std::string_view, View, const MediaAttributes &) = 0;
    virtual void remove(std::string_view) = 0;
    virtual void rename(std::string_view oldName, std::string_view newName)
    {
        for (auto &e : list())
            require(e.name != newName, "Destination exists"sv);
        auto entries = list();
        auto i = std::ranges::find(entries, oldName, &MediaEntry::name);
        require(i != entries.end(), "File not found"sv);
        auto data = read(oldName);
        auto attrs = i->attributes;
        remove(oldName);
        put(newName, data, attrs);
    }
    virtual void attributes(std::string_view name, const MediaAttributes &changes)
    {
        auto entries = list();
        auto i = std::ranges::find(entries, name, &MediaEntry::name);
        require(i != entries.end(), "File not found"sv);
        auto a = i->attributes;
        for (auto &[k, v] : changes)
            a[k] = v;
        put(name, read(name), a);
    }
    virtual void mkdir(std::string_view)
    {
        throw FormatException("This media has no directories"sv);
    }
    virtual Bytes encode(std::string_view) = 0;
    virtual void boot(View)
    {
        throw FormatException("This filesystem has no CP/M boot tracks"sv);
    }
};
std::unique_ptr<Backend> tape(View, std::string_view, bool);
std::unique_ptr<Backend> disk(View, std::string_view, const MediaOptions &, bool);
std::unique_ptr<Backend> mdos(View, std::string_view, const MediaOptions &, bool);
std::unique_ptr<Backend> nex(View, bool);
std::unique_ptr<Backend> microdrive(View, const MediaOptions &, bool);
} // namespace zxkit::archive
