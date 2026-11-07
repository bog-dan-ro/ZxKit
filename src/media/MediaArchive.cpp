// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/MediaArchive.h"
#include "archive/ArchiveInternal.h"
#include "archive/DiskContainer.h"
#include <string>
#include <string_view>
namespace zxkit {
using namespace std::literals;
namespace archive {
std::unique_ptr<Backend> fat(View, const MediaOptions &, bool);
}
struct MediaArchive::Impl
{
    std::unique_ptr<archive::Backend> backend;
    explicit Impl(std::unique_ptr<archive::Backend> b) : backend(std::move(b)) { }
    template <class F> void mutate(F f)
    {
        auto next = backend->clone();
        f(*next);
        // Validate the native serialization before accepting a transaction.
        next->encode({});
        backend = std::move(next);
    }
};
namespace {
std::unique_ptr<archive::Backend> make(archive::View b, std::string_view ext,
                                       const MediaOptions &options, bool create)
{
    using namespace archive;
    using namespace std::literals;
    auto e = extension(ext);
    if (!create) {
        auto magic = [&](std::string_view signature) {
            return b.size() >= signature.size()
                    && std::ranges::equal(b.first(signature.size()), signature,
                                          [](uint8_t a, char c) { return a == uint8_t(c); });
        };
        if (magic("ZXTape!\x1A"sv))
            e = ".tzx"s;
        else if (magic("PZXT"sv))
            e = ".pzx"s;
        else if (magic("Compressed Square Wave\x1A"sv))
            e = ".csw"s;
        else if (magic("NextV1."sv))
            e = ".nex"s;
        else if (magic("P3D\x1A"sv))
            e = ".p3d"s;
        else if (magic("SINCLAIR"sv))
            e = ".scl"s;
        else if (magic("EXTENDED CPC DSK File"sv) || magic("MV - CPC"sv))
            e = ".dsk"s;
        else if (magic("CQ\x14"sv))
            e = ".cqm"s;
        else if (magic("TD"sv) || magic("td"sv))
            e = ".td0"s;
        else if (b.size() == 137923)
            e = ".mdr"s;
        else if (b.size() >= 9 * 256 && b[8 * 256 + 0xE7] == 0x10
                 && (b.size() == 655360 || b.size() == 327680 || b.size() == 163840))
            e = ".trd"s;
        else if (e != ".tap"sv && options.filesystem.empty()) {
            std::size_t p = 0;
            bool tape = !b.empty();
            while (p < b.size() && tape) {
                if (b.size() - p < 2) {
                    tape = false;
                    break;
                }
                std::size_t n = word(b, p);
                p += 2;
                if (n < 2 || n > b.size() - p || (b[p] != 0 && b[p] != 255)) {
                    tape = false;
                    break;
                }
                p += n;
            }
            if (tape)
                e = ".tap"s;
        }
    }
    const bool mdosSignature = !create
            && ((b.size() >= 208 && std::ranges::equal(b.subspan(204, 4), "SDOS"sv))
                || (e == ".dsk"sv && b.size() >= 720
                    && std::ranges::equal(b.subspan(716, 4), "SDOS"sv)));
    if (e == ".d40"sv || e == ".d80"sv || mdosSignature || options.filesystem == "mdos40"sv
        || options.filesystem == "mdos80"sv) {
        auto selected = options;
        if (selected.filesystem.empty())
            selected.filesystem = e == ".d40"sv || b.size() == 368640 ? "mdos40"s : "mdos80"s;
        return mdos(b, e, selected, create);
    }
    if (e == ".tap"sv || e == ".tzx"sv || e == ".pzx"sv || e == ".csw"sv)
        return tape(b, e, create);
    if (e == ".nex"sv)
        return nex(b, create);
    if (e == ".mdr"sv)
        return microdrive(b, options, create);
    if (e == ".p3d"sv)
        return disk(b, e, options, create);
    if (options.filesystem == "fat"sv
        || (!create && b.size() >= 512 && b[510] == 85 && b[511] == 170
            && (b[0] == 0xEB || b[0] == 0xE9 || b[446 + 4] != 0)))
        return fat(b, options, create);
    return disk(b, e, options, create);
}
} // namespace
MediaArchive::MediaArchive(std::unique_ptr<Impl> p) : m_impl(std::move(p)) { }
MediaArchive::MediaArchive(MediaArchive &&) noexcept = default;
MediaArchive &MediaArchive::operator=(MediaArchive &&) noexcept = default;
MediaArchive::~MediaArchive() = default;
MediaArchive MediaArchive::open(std::span<const uint8_t> b, std::string_view ext,
                                const MediaOptions &o)
{
    return MediaArchive(std::make_unique<Impl>(make(b, ext, o, false)));
}
MediaArchive MediaArchive::create(std::string_view ext, const MediaOptions &o)
{
    return MediaArchive(std::make_unique<Impl>(make({}, ext, o, true)));
}
std::vector<std::string> MediaArchive::profiles()
{
    std::vector<std::string> out;
    for (auto &p : archive::diskProfiles())
        out.emplace_back(p.name);
    out.emplace_back("fat"s);
    out.emplace_back("next-cpm"s);
    return out;
}
std::vector<MediaEntry> MediaArchive::list() const
{
    return m_impl->backend->list();
}
std::vector<uint8_t> MediaArchive::read(std::string_view n) const
{
    return m_impl->backend->read(n);
}
void MediaArchive::put(std::string_view n, std::span<const uint8_t> d, const MediaAttributes &a)
{
    m_impl->mutate([&](auto &b) { b.put(n, d, a); });
}
void MediaArchive::remove(std::string_view n)
{
    m_impl->mutate([&](auto &b) { b.remove(n); });
}
void MediaArchive::rename(std::string_view n, std::string_view to, bool replace)
{
    if (n == to)
        return;
    m_impl->mutate([&](auto &b) {
        if (replace)
            for (auto &e : b.list())
                if (e.name == to) {
                    archive::require(!e.directory, "Cannot replace a directory"sv);
                    b.remove(to);
                    break;
                }
        b.rename(n, to);
    });
}
void MediaArchive::setAttributes(std::string_view n, const MediaAttributes &a)
{
    m_impl->mutate([&](auto &b) { b.attributes(n, a); });
}
void MediaArchive::makeDirectory(std::string_view n)
{
    m_impl->mutate([&](auto &b) { b.mkdir(n); });
}
std::vector<uint8_t> MediaArchive::encode(std::string_view e) const
{
    return m_impl->backend->encode(e);
}
void MediaArchive::setBootTracks(std::span<const uint8_t> d)
{
    m_impl->mutate([&](auto &b) { b.boot(d); });
}
} // namespace zxkit
