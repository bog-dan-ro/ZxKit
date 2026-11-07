// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include <numeric>
#include <string>
#include <string_view>
namespace zxkit::archive {
using namespace std::literals;
namespace {
struct Nex final : Backend
{
    Bytes header;
    std::map<std::string, Bytes, std::less<>> parts;
    std::vector<std::pair<std::string, std::size_t>> screens() const
    {
        std::vector<std::pair<std::string, std::size_t>> out;
        auto flags = header[10];
        bool extended = header[7] == '3' && (flags & 64);
        bool palette = !(flags & 128) && ((flags & 5) || (extended && header[152]));
        if (palette)
            out.emplace_back("screens/palette.bin"s, 512);
        if (extended) {
            require(header[152] >= 1 && header[152] <= 3, "Unknown NEX screen mode"sv);
            if (header[152] != 3)
                out.emplace_back("screens/layer2-wide.bin"s, 81920);
        } else {
            if (flags & 1)
                out.emplace_back("screens/layer2.bin"s, 49152);
            if (flags & 2)
                out.emplace_back("screens/ula.scr"s, 6912);
            if (flags & 4)
                out.emplace_back("screens/lores.bin"s, 12288);
            if (flags & 8)
                out.emplace_back("screens/hires.bin"s, 12288);
            if (flags & 16)
                out.emplace_back("screens/hicolour.bin"s, 12288);
        }
        if (header[7] == '3' && header[153])
            out.emplace_back("screens/copper.bin"s, 2048);
        return out;
    }
    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Nex>(*this);
    }
    std::vector<MediaEntry> list() const override
    {
        std::vector<MediaEntry> out{ { "header.bin"s, 512, false, {} },
                                     { "banks"s, 0, true, {} },
                                     { "screens"s, 0, true, {} } };
        for (auto &[k, b] : parts)
            out.push_back({ k, b.size(), false, {} });
        return out;
    }
    Bytes read(std::string_view name) const override
    {
        if (name == "header.bin"sv) {
            auto copy = *this;
            auto bytes = copy.encode({});
            bytes.resize(512);
            return bytes;
        }
        auto i = parts.find(name);
        require(i != parts.end(), "NEX component not found"sv);
        return i->second;
    }
    void put(std::string_view name, View data, const MediaAttributes &a) override
    {
        checkAttrs(a, {});
        if (name == "header.bin"sv) {
            require(data.size() == 512, "NEX header must be 512 bytes"sv);
            header.assign(data.begin(), data.end());
            return;
        }
        if (name.starts_with("banks/"sv)) {
            bool valid = false;
            for (unsigned i = 0; i < 112; ++i)
                if (name == std::format("banks/{:03}.bin", i))
                    valid = true;
            require(valid && data.size() == 16384,
                    "NEX banks are banks/000.bin..111.bin, exactly 16384 bytes"sv);
        } else if (name == "extra.bin"sv) {
        } else {
            const std::map<std::string, std::pair<std::size_t, unsigned>, std::less<>> supported{
                { "screens/palette.bin"s, { 512, 0 } },
                { "screens/layer2.bin"s, { 49152, 1 } },
                { "screens/ula.scr"s, { 6912, 2 } },
                { "screens/lores.bin"s, { 12288, 4 } },
                { "screens/hires.bin"s, { 12288, 8 } },
                { "screens/hicolour.bin"s, { 12288, 16 } },
                { "screens/layer2-wide.bin"s, { 81920, 64 } },
                { "screens/copper.bin"s, { 2048, 256 } }
            };
            auto i = supported.find(name);
            require(i != supported.end() && i->second.first == data.size(),
                    "Unknown NEX screen component or wrong size"sv);
            auto flag = i->second.second;
            if (!flag)
                header[10] &= 127;
            else if (flag == 256) {
                require(header[7] == '3', "Copper screens require NEX V1.3"sv);
                header[153] = 1;
            } else {
                if (flag == 64)
                    require(header[7] == '3' && (header[152] == 1 || header[152] == 2),
                            "Select a NEX V1.3 wide screen mode in the header first"sv);
                header[10] |= uint8_t(flag);
            }
        }
        parts[std::string(name)] = Bytes(data.begin(), data.end());
    }
    void remove(std::string_view name) override
    {
        require(parts.erase(std::string(name)) == 1, "NEX component not found"sv);
        if (name == "screens/palette.bin"sv)
            header[10] |= 128;
        else if (name == "screens/copper.bin"sv)
            header[153] = 0;
        else {
            const std::map<std::string, unsigned, std::less<>> flags{
                { "screens/layer2.bin"s, 1 },    { "screens/ula.scr"s, 2 },
                { "screens/lores.bin"s, 4 },     { "screens/hires.bin"s, 8 },
                { "screens/hicolour.bin"s, 16 }, { "screens/layer2-wide.bin"s, 64 }
            };
            if (auto i = flags.find(name); i != flags.end())
                header[10] &= uint8_t(~i->second);
        }
    }
    Bytes encode(std::string_view ext) override
    {
        require(ext.empty() || extension(ext) == ".nex"sv,
                "NEX cannot convert to a disk or tape"sv);
        require(header.size() == 512
                        && std::string_view(reinterpret_cast<const char *>(header.data()), 7)
                                == "NextV1."sv
                        && header[7] >= '0' && header[7] <= '3',
                "Invalid NEX header/version"sv);
        require(header[8] <= 1 && header[11] <= 7 && header[139] < 112,
                "Invalid NEX entry settings"sv);
        Bytes out = header;
        out[9] = 0;
        for (unsigned i = 0; i < 112; ++i) {
            out[18 + i] = parts.contains(std::format("banks/{:03}.bin", i));
            out[9] += out[18 + i];
        }
        auto expected = screens();
        for (auto &[name, n] : expected) {
            auto i = parts.find(name);
            require(i != parts.end() && i->second.size() == n, "NEX screen component missing"sv);
            append(out, i->second);
        }
        for (auto &[name, data] : parts)
            if (name.starts_with("screens/"sv))
                require(std::ranges::find(expected, name,
                                          &std::pair<std::string, std::size_t>::first)
                                != expected.end(),
                        "NEX screen flags would discard a component"sv);
        if (header[7] == '3')
            setDword(out, 144, out.size());
        constexpr std::array<unsigned, 8> first{ 5, 2, 0, 1, 3, 4, 6, 7 };
        auto bank = [&](unsigned n) {
            auto i = parts.find(std::format("banks/{:03}.bin", n));
            if (i != parts.end())
                append(out, i->second);
        };
        for (auto n : first)
            bank(n);
        for (unsigned n = 8; n < 112; ++n)
            bank(n);
        if (auto i = parts.find("extra.bin"sv); i != parts.end())
            append(out, i->second);
        if (header[7] == '3' && header[143]) {
            // V1.3 CRC32C covers bytes following the 512-byte header.
            uint32_t crc = ~uint32_t{ 0 };
            auto consume = [&](View bytes) {
                for (auto c : bytes) {
                    crc ^= c;
                    for (unsigned i = 0; i < 8; ++i)
                        crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78 : 0);
                }
            };
            consume(View(out).subspan(512));
            consume(View(out).first(508));
            setDword(out, 508, ~crc);
        }
        return out;
    }
};
} // namespace
std::unique_ptr<Backend> nex(View b, bool create)
{
    auto r = std::make_unique<Nex>();
    if (create) {
        r->header.resize(512);
        textAt(r->header, 0, "NextV1.2"sv);
        r->header[10] = 128;
        return r;
    }
    r->header = Bytes(slice(b, 0, 512).begin(), slice(b, 0, 512).end());
    require(std::string_view(reinterpret_cast<const char *>(b.data()), 7) == "NextV1."sv
                    && b[7] >= '0' && b[7] <= '3',
            "Unsupported NEX version"sv);
    std::size_t pos = 512;
    for (auto &[name, n] : r->screens()) {
        auto s = slice(b, pos, n);
        r->parts[name] = Bytes(s.begin(), s.end());
        pos += n;
    }
    std::vector<unsigned> order{ 5, 2, 0, 1, 3, 4, 6, 7 };
    for (unsigned n = 8; n < 112; ++n)
        order.push_back(n);
    unsigned count = 0;
    for (auto n : order) {
        require(b[18 + n] <= 1, "Invalid NEX bank map"sv);
        if (!b[18 + n])
            continue;
        auto s = slice(b, pos, 16384);
        r->parts[std::format("banks/{:03}.bin", n)] = Bytes(s.begin(), s.end());
        pos += 16384;
        ++count;
    }
    require(count == b[9], "NEX bank count disagrees with map"sv);
    if (pos < b.size())
        r->parts["extra.bin"s] = Bytes(b.begin() + pos, b.end());
    if (b[7] == '3' && b[143]) {
        auto encoded = r->encode({});
        require(dword(encoded, 508) == dword(b, 508), "NEX CRC32C mismatch"sv);
    }
    return r;
}
} // namespace zxkit::archive
