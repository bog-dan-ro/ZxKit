// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveInternal.h"
#include "zxkit/media/TapeConversion.h"
#include "zxkit/media/BasicCodec.h"
#include <charconv>
#include <string>
#include <string_view>

namespace zxkit::archive {
using namespace std::literals;
namespace {
Bytes parseFile(View b, std::string_view ext, std::vector<TapeBlock> &blocks)
{
    if (ext == ".tap"sv)
        blocks = TapeFormats::parseTap(b);
    else if (ext == ".tzx"sv)
        blocks = TapeFormats::parseTzx(b);
    else if (ext == ".pzx"sv)
        blocks = TapeFormats::parsePzx(b);
    else if (ext == ".csw"sv)
        blocks = TapeFormats::parseCsw(b);
    else
        throw FormatException("Unsupported tape format"sv);
    if (ext == ".pzx"sv) {
        for (std::size_t i = 0; i + 1 < blocks.size(); ++i) {
            auto &pilot = blocks[i];
            auto &data = blocks[i + 1];
            if (pilot.type != TapeBlockType::PzxPulses || data.type != TapeBlockType::PzxData
                || data.data.empty())
                continue;
            unsigned expected = data.data[0] < 128 ? 8063 : 3223;
            unsigned count = 0;
            bool valid = true;
            std::size_t n = 0;
            while (n + 1 < pilot.pulses.size() && pilot.pulses[n] == 2168) {
                count += pilot.pulses[n + 1];
                n += 2;
            }
            valid = n + 4 == pilot.pulses.size() && pilot.pulses[n] == 667
                    && pilot.pulses[n + 1] == 1 && pilot.pulses[n + 2] == 735
                    && pilot.pulses[n + 3] == 1;
            if (!valid || count != expected || data.dataSymbolCount != data.data.size() * 8
                || data.value != 1 || data.pilotPulse != 0 || data.dataSymbols.size() != 2
                || data.dataSymbols[0].pulses != std::vector<uint16_t>{ 855, 855 }
                || data.dataSymbols[1].pulses != std::vector<uint16_t>{ 1710, 1710 })
                continue;
            uint16_t pause = 0;
            std::size_t remove = 2;
            if (i + 2 < blocks.size() && blocks[i + 2].type == TapeBlockType::PzxPause) {
                auto &p = blocks[i + 2];
                if (p.value != 1 || p.pulses.size() != 1 || p.pulses[0] % 3500
                    || p.pulses[0] / 3500 > 65535)
                    continue;
                pause = uint16_t(p.pulses[0] / 3500);
                ++remove;
            }
            auto standard = TapeBlock::standard(data.data, pause);
            blocks.erase(blocks.begin() + i, blocks.begin() + i + remove);
            blocks.insert(blocks.begin() + i, std::move(standard));
        }
    }
    return {};
}
uint8_t checksum(View b)
{
    uint8_t v = 0;
    for (auto c : b)
        v ^= c;
    return v;
}
struct Tape final : Backend
{
    std::string ext;
    std::vector<TapeBlock> blocks;
    Bytes original;
    bool modified = false;
    struct File
    {
        std::string name;
        std::size_t index;
    };
    std::vector<File> files() const
    {
        std::vector<File> out;
        for (std::size_t i = 0; i + 1 < blocks.size(); ++i) {
            auto &h = blocks[i];
            auto &d = blocks[i + 1];
            if (h.type != TapeBlockType::StandardData || d.type != TapeBlockType::StandardData
                || h.data.size() != 19 || h.data[0] || h.data[1] > 3 || d.data.size() < 2
                || d.data[0] != 255)
                continue;
            auto native = nameField(slice(h.data, 2, 10));
            for (auto &c : native)
                if (c == '/' || c == '\\' || uint8_t(c) < 32)
                    c = '_';
            if (native.empty() || native == "."sv || native == ".."sv)
                native = "unnamed"sv;
            std::string name = native;
            unsigned suffix = 1;
            while (std::ranges::find(out, name, &File::name) != out.end())
                name = native + "~"s + std::to_string(++suffix);
            out.push_back({ name, i });
            ++i;
        }
        return out;
    }
    std::size_t fileIndex(std::string_view name) const
    {
        auto entries = files();
        auto i = std::ranges::find(entries, name, &File::name);
        require(i != entries.end(), "Tape file not found"sv);
        return i->index;
    }
    std::size_t blockIndex(std::string_view name, bool append = false) const
    {
        require(name.starts_with("blocks/"sv) && name.ends_with(".tzx"sv), "Invalid block path"sv);
        auto s = name.substr(7, name.size() - 11);
        std::size_t n = 0;
        auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        require(r.ec == std::errc{} && r.ptr == s.data() + s.size()
                        && (n < blocks.size() || (append && n == blocks.size())),
                "Tape block not found"sv);
        return n;
    }
    Bytes rawBlock(std::size_t index) const
    {
        auto b = blocks[index];
        auto original = b;
        if (b.type == TapeBlockType::JumpTo)
            b.value = 0;
        if (b.type == TapeBlockType::CallSequence || b.type == TapeBlockType::SelectBlock)
            std::ranges::fill(b.offsets, 0);
        auto bytes = TapeFormats::writeTzx(std::span(&b, 1));
        if (b.type == TapeBlockType::JumpTo)
            setWord(bytes, 11, uint16_t(original.value));
        if (b.type == TapeBlockType::CallSequence)
            for (std::size_t i = 0; i < original.offsets.size(); ++i)
                setWord(bytes, 13 + i * 2, uint16_t(original.offsets[i]));
        if (b.type == TapeBlockType::SelectBlock) {
            std::size_t p = 14;
            for (std::size_t i = 0; i < original.offsets.size(); ++i) {
                setWord(bytes, p, uint16_t(original.offsets[i]));
                p += 3 + original.labels[i].size();
            }
        }
        return bytes;
    }
    MediaAttributes attrs(std::size_t i) const
    {
        auto &h = blocks[i].data;
        return { { "type"s, h[1] },
                 { "param1"s, word(h, 14) },
                 { "param2"s, word(h, 16) },
                 { "pause"s, blocks[i + 1].pauseAfterMs } };
    }
    void erase(std::size_t first, std::size_t count)
    {
        auto map = [&](int64_t old) -> int64_t {
            require(old >= 0 && old <= int64_t(blocks.size()), "Invalid tape control target"sv);
            require(old < int64_t(first) || old >= int64_t(first + count),
                    "A tape control block targets the deleted item"sv);
            return old >= int64_t(first + count) ? old - count : old;
        };
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (i >= first && i < first + count)
                continue;
            auto &b = blocks[i];
            auto origin = map(i);
            auto target = [&](int64_t n) {
                auto delta = map(int64_t(i) + n) - origin;
                require(delta >= INT16_MIN && delta <= INT16_MAX, "Tape control offset overflow"sv);
                return int16_t(delta);
            };
            if (b.type == TapeBlockType::JumpTo)
                b.value = target(b.value);
            if (b.type == TapeBlockType::CallSequence || b.type == TapeBlockType::SelectBlock)
                for (auto &n : b.offsets)
                    n = target(n);
        }
        blocks.erase(blocks.begin() + first, blocks.begin() + first + count);
    }
    std::unique_ptr<Backend> clone() const override
    {
        return std::make_unique<Tape>(*this);
    }
    std::vector<MediaEntry> list() const override
    {
        std::vector<MediaEntry> out{ { "files"s, 0, true, {} },
                                     { "blocks"s, 0, true, {} },
                                     { "basic"s, 0, true, {} } };
        for (auto &f : files()) {
            auto &data = blocks[f.index + 1].data;
            auto a = attrs(f.index);
            out.push_back({ "files/"s + f.name, data.size() - 2, false, a });
            if (a.at("type"s) == 0) {
                auto text =
                        BasicCodec::decompile(slice(data, 1, data.size() - 2), BasicDialect::Next);
                out.push_back({ "basic/"s + f.name + ".bas"s, text.size(), false, a });
            }
        }
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            auto data = rawBlock(i);
            out.push_back({ std::format("blocks/{:06}.tzx", i), data.size(), false, {} });
        }
        return out;
    }
    Bytes read(std::string_view name) const override
    {
        if (name.starts_with("blocks/"sv)) {
            auto i = blockIndex(name);
            return rawBlock(i);
        }
        bool basic = name.starts_with("basic/"sv);
        require(name.starts_with("files/"sv) || basic, "Invalid tape file path"sv);
        if (basic)
            require(name.ends_with(".bas"sv), "Invalid BASIC path"sv);
        auto n = basic ? name.substr(6, name.size() - 10) : name.substr(6);
        auto i = fileIndex(n);
        auto &data = blocks[i + 1].data;
        auto bytes = slice(data, 1, data.size() - 2);
        if (basic) {
            auto text = BasicCodec::decompile(bytes, BasicDialect::Next);
            return Bytes(text.begin(), text.end());
        }
        return Bytes(bytes.begin(), bytes.end());
    }
    void put(std::string_view name, View bytes, const MediaAttributes &changes) override
    {
        modified = true;
        if (name.starts_with("blocks/"sv)) {
            checkAttrs(changes, {});
            auto i = blockIndex(name, true);
            if (i < blocks.size() && std::ranges::equal(rawBlock(i), bytes))
                return;
            auto parsed = TapeFormats::parseTzx(bytes);
            require(parsed.size() == 1, "Raw block edit must contain exactly one TZX block"sv);
            if (i == blocks.size())
                blocks.push_back(std::move(parsed[0]));
            else
                blocks[i] = std::move(parsed[0]);
            return;
        }
        bool basic = name.starts_with("basic/"sv);
        require(basic || name.starts_with("files/"sv), "Use files/NAME or basic/NAME.bas"sv);
        if (basic)
            require(name.ends_with(".bas"sv), "Invalid BASIC path"sv);
        std::string native(basic ? name.substr(6, name.size() - 10) : name.substr(6));
        auto entries = files();
        auto existing = std::ranges::find(entries, native, &File::name);
        std::size_t index = existing == entries.end() ? blocks.size() : existing->index;
        MediaAttributes a = index < blocks.size()
                ? attrs(index)
                : MediaAttributes{
                      { "type"s, 3 }, { "param1"s, 32768 }, { "param2"s, 32768 }, { "pause"s, 1000 }
                  };
        checkAttrs(changes, { "type"sv, "param1"sv, "param2"sv, "pause"sv });
        for (auto &[k, v] : changes)
            a[k] = v;
        Bytes compiled;
        if (basic) {
            compiled = BasicCodec::compile(
                    std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
                    BasicDialect::Next);
            bytes = compiled;
            a["type"s] = 0;
            a["param1"s] = 32768;
            std::size_t program = 0;
            while (compiled.size() - program >= 4 && compiled[program] < 40) {
                std::size_t n = word(compiled, program + 2) + 4;
                if (n > compiled.size() - program)
                    break;
                program += n;
            }
            a["param2"s] = program;
        }
        require(bytes.size() <= 65533 && attr(a, "type"sv) <= 3 && attr(a, "param1"sv) <= 65535
                        && attr(a, "param2"sv) <= 65535 && attr(a, "pause"sv) <= 65535,
                "Tape file fields out of range"sv);
        Bytes header;
        if (index < blocks.size())
            header = blocks[index].data;
        else {
            validName(native, 10);
            header.resize(19, ' ');
            header[0] = 0;
            textAt(header, 2, native);
        }
        header[1] = uint8_t(a["type"s]);
        setWord(header, 12, bytes.size());
        setWord(header, 14, a["param1"s]);
        setWord(header, 16, a["param2"s]);
        header[18] = checksum(slice(header, 0, 18));
        Bytes data{ 255 };
        append(data, bytes);
        data.push_back(checksum(data));
        auto h = TapeBlock::standard(std::move(header));
        auto d = TapeBlock::standard(std::move(data), uint16_t(a["pause"s]));
        if (index == blocks.size()) {
            blocks.push_back(std::move(h));
            blocks.push_back(std::move(d));
        } else {
            blocks[index] = std::move(h);
            blocks[index + 1] = std::move(d);
        }
    }
    void remove(std::string_view name) override
    {
        modified = true;
        if (name.starts_with("blocks/"sv))
            erase(blockIndex(name), 1);
        else if (name.starts_with("files/"sv))
            erase(fileIndex(name.substr(6)), 2);
        else if (name.starts_with("basic/"sv) && name.ends_with(".bas"sv))
            erase(fileIndex(name.substr(6, name.size() - 10)), 2);
        else
            throw FormatException("Invalid tape path"sv);
    }
    void rename(std::string_view name, std::string_view newName) override
    {
        modified = true;
        require(name.starts_with("files/"sv) && newName.starts_with("files/"sv),
                "Rename tape logical files through files/"sv);
        for (auto &e : list())
            require(e.name != newName, "Destination exists"sv);
        auto native = newName.substr(6);
        validName(native, 10);
        auto &h = blocks[fileIndex(name.substr(6))].data;
        std::fill(h.begin() + 2, h.begin() + 12, ' ');
        textAt(h, 2, native);
        h[18] = checksum(slice(h, 0, 18));
    }
    Bytes encode(std::string_view target) override
    {
        auto format = target.empty() ? ext : extension(target);
        if (!modified && format == ext && !original.empty())
            return original;
        for (auto &b : blocks)
            require(!b.description.starts_with("Unsupported block"sv),
                    "Cannot rewrite unsupported TZX blocks without losing data"sv);
        if (modified && ext == ".tzx"sv && !original.empty()) {
            auto parsed = TapeFormats::parseTzx(original);
            require(TapeFormats::writeTzx(parsed).size() >= original.size(),
                    "Cannot edit a truncated or noncanonical TZX without losing its tail"sv);
        }
        if (format == ".tap"sv)
            return TapeFormats::writeTap(blocks);
        if (format == ".tzx"sv)
            return TapeFormats::writeTzx(blocks);
        if (format == ".pzx"sv) {
            if (modified && ext == ".pzx"sv && !original.empty())
                for (std::size_t p = 0; p < original.size();) {
                    auto tag = nameField(slice(original, p, 4));
                    require(tag == "PZXT"sv || tag == "PULS"sv || tag == "DATA"sv || tag == "PAUS"sv
                                    || tag == "STOP"sv || tag == "BRWS"sv,
                            "Cannot rewrite unknown PZX chunks"sv);
                    p += 8 + dword(original, p + 4);
                    require(p <= original.size(), "Truncated PZX chunk"sv);
                }
            auto out = TapeConversion::writePzx(blocks);
            if (ext == ".pzx"sv && !original.empty()) {
                std::size_t size = 8 + dword(original, 4);
                auto header = slice(original, 0, size);
                out.erase(out.begin(), out.begin() + 10);
                out.insert(out.begin(), header.begin(), header.end());
            }
            return out;
        }
        if (format == ".csw"sv)
            return TapeConversion::writeCsw(blocks);
        if (format == ".wav"sv) {
            require(ext == ".tap"sv, "WAV conversion accepts TAP input only"sv);
            return TapeConversion::tapToWav(blocks);
        }
        throw FormatException("Unsuitable tape conversion"sv);
    }
};
} // namespace
std::unique_ptr<Backend> tape(View b, std::string_view ext, bool create)
{
    auto result = std::make_unique<Tape>();
    result->ext = extension(ext);
    if (!create) {
        parseFile(b, result->ext, result->blocks);
        result->original.assign(b.begin(), b.end());
    }
    return result;
}
} // namespace zxkit::archive
