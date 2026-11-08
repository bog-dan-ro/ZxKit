// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "RomTestSupport.h"
#include "zxkit/media/MediaArchive.h"
#include "zxkit/media/BasicCodec.h"
#include "zxkit/media/TapeConversion.h"
#include "zxkit/peripherals/storage/FloppyDisk.h"
#include "zxkit/core/Exception.h"
#include "media/NumberParsing.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
using namespace zxkit;
using namespace std::literals;
using Bytes = std::vector<uint8_t>;
namespace {
template <class F> void fails(F fn)
{
    bool thrown = false;
    try {
        fn();
    } catch (const std::exception &) {
        thrown = true;
    }
    assert(thrown);
}
void basic()
{
    assert((BasicCodec::encodeNumber(1) == std::array<uint8_t, 5>{ 0, 0, 1, 0, 0 }));
    assert((BasicCodec::encodeNumber(0.5) == std::array<uint8_t, 5>{ 128, 0, 0, 0, 0 }));
    assert((BasicCodec::encodeNumber(-1) == std::array<uint8_t, 5>{ 0, 255, 255, 255, 0 }));
    for (double n : { 0., 1., -1., 65535., -65536., 65536., 0.5, 3.14159265, 1e-20, 1e30 })
        assert(std::abs((BasicCodec::decodeNumber(BasicCodec::encodeNumber(n)) - n) / (n ? n : 1))
               < 1e-9);
    auto data = BasicCodec::compile(
            "10 PRINT \"hello 12\": LET a=1.5\n20 REM PRINT 123\n30 GO TO 10\n!raw 610000010000\n"sv);
    assert(data[0] == 0 && data[1] == 10 && data[4] == 0xF5);
    auto text = BasicCodec::decompile(data);
    assert(BasicCodec::compile(text) == data);
    auto next = BasicCodec::compile("10 BANK 1: LAYER 2: REG 18,9\n"sv, BasicDialect::Next);
    assert(next[4] == 0x9A);
    assert(BasicCodec::compile(BasicCodec::decompile(next, BasicDialect::Next), BasicDialect::Next)
           == next);
    auto quote = BasicCodec::compile("10 PRINT \"\\xF5\\\\\"\n"sv);
    assert(BasicCodec::compile(BasicCodec::decompile(quote)) == quote);
    fails([] { BasicCodec::compile("20 PRINT 1\n10 PRINT 2\n"sv); });
    fails([] { BasicCodec::encodeNumber(1e100); });
    for (bool chars : { false, true }) {
        auto a = BasicCodec::compileArray(
                chars ? "2,5\n\"hello\"\n\"ZX\\x80\"\n"sv : "2,3\n1 -1 0.5 1e10 3.14 0\n"sv, chars);
        assert(BasicCodec::compileArray(BasicCodec::decompileArray(a, chars), chars) == a);
    }
    fails([] { BasicCodec::compileArray("3\n1 2\n"sv); });
}
std::vector<std::pair<bool, uint64_t>> waveform(const std::vector<TapeBlock> &blocks)
{
    std::vector<std::pair<bool, uint64_t>> out;
    TapeCursor cursor;
    for (std::size_t n = 0; n < 1000000 && !cursor.atEnd(blocks); ++n) {
        auto e = cursor.next(blocks);
        if (!e.length)
            continue;
        if (!out.empty() && out.back().first == e.level)
            out.back().second += e.length;
        else
            out.emplace_back(e.level, e.length);
    }
    return out;
}
void tape()
{
    auto a = MediaArchive::create("tap"sv);
    Bytes payload{ 1, 2, 3, 0, 4 };
    a.put("files/CODE"sv, payload, { { "type"s, 3 }, { "param1"s, 40000 } });
    assert(a.read("files/CODE"sv) == payload);
    auto raw = a.encode();
    auto blocks = TapeFormats::parseTap(raw);
    assert(blocks.size() == 2 && blocks[0].data.size() == 19);
    assert(blocks[0].data[14] == 0x40 && blocks[0].data[15] == 0x9C);
    auto tzx = a.encode("tzx"sv);
    auto b = MediaArchive::open(tzx, "tzx"sv);
    assert(b.read("files/CODE"sv) == payload);
    auto pzx = a.encode("pzx"sv);
    auto pblocks = TapeFormats::parsePzx(pzx);
    assert(MediaArchive::open(pzx, "pzx"sv).encode("tap"sv) == raw);
    assert(waveform(blocks) == waveform(pblocks));
    auto wave = a.encode("wav"sv);
    assert(std::string(wave.begin(), wave.begin() + 4) == "RIFF"sv && wave[20] == 1
           && wave[34] == 8);
    auto csw = a.encode("csw"sv);
    assert(waveform(blocks) == waveform(TapeFormats::parseCsw(csw)));
    auto before = b.encode();
    fails([&] { b.put("files/TOOLONGNAME"sv, payload); });
    assert(b.encode() == before);
    b.rename("files/CODE"sv, "files/OTHER"sv);
    assert(b.read("files/OTHER"sv) == payload);
    b.remove("files/OTHER"sv);
    assert(b.list().size() == 3);
    a.put("basic/HELLO.bas"sv, Bytes{ '1', '0', ' ', 'P', 'R', 'I', 'N', 'T', ' ', '1', '\n' });
    auto listing = a.read("basic/HELLO.bas"sv);
    a.put("basic/HELLO.bas"sv, listing);
    assert(!a.read("files/HELLO"sv).empty());
}
void disks()
{
    Bytes large(41003);
    for (std::size_t i = 0; i < large.size(); ++i)
        large[i] = uint8_t(i * 73 + 19);
    for (auto &profile : MediaArchive::profiles()) {
        if (profile == "fat"sv || profile == "next-cpm"sv)
            continue;
        std::cerr << "profile "sv << profile << '\n';
        MediaOptions opts{ .filesystem = profile };
        auto a = MediaArchive::create("img"sv, opts);
        std::string name = profile.starts_with("trdos"sv) ? "FILE.C"s
                : profile.starts_with("hc-basic"sv)       ? "FILE.B"s
                                                          : "FILE.BIN"s;
        if (profile == "devil"sv || profile == "mgt"sv || profile.starts_with("opus"sv))
            name = "FILE"s;
        if (profile.starts_with("mdos"sv))
            name = "FILE.B"s;
        a.put(name, large);
        auto got = a.read(name);
        assert(got.size() >= large.size());
        assert(std::equal(large.begin(), large.end(), got.begin()));
        auto encoded = a.encode();
        auto b = MediaArchive::open(encoded, "img"sv, opts);
        assert(b.read(name) == got);
        for (auto ext : { "dsk"sv, "td0"sv, "cqm"sv }) {
            auto converted = a.encode(ext);
            auto c = MediaArchive::open(converted, ext, opts);
            assert(c.read(name) == got);
        }
        if (profile.starts_with("hc-basic"sv) || profile.starts_with("mdos"sv) || profile == "mgt"sv
            || profile == "devil"sv || profile.starts_with("opus"sv)) {
            a.setAttributes(name, { { "param1"s, 40000 } });
            const auto changedEntries = a.list();
            const auto changedFile = std::ranges::find(changedEntries, name, &MediaEntry::name);
            assert(changedFile != changedEntries.end()
                   && changedFile->attributes.at("param1"s) == 40000);
        }
        if (profile == "mgt"sv) {
            a.setAttributes(name, { { "type"s, 0 } });
            auto entries = a.list();
            auto entry = std::ranges::find(entries, name, &MediaEntry::name);
            assert(entry->attributes.at("type"s) == 0 && entry->attributes.at("native-type"s) == 1);
            assert(a.read(name) == got);
            a.setAttributes(name, { { "native-type"s, 4 } });
            entries = a.list();
            entry = std::ranges::find(entries, name, &MediaEntry::name);
            assert(entry->attributes.at("type"s) == 3);
        }
        auto before = a.encode();
        fails([&] { a.put(name, Bytes(2 * 1024 * 1024)); });
        assert(before == a.encode());
        auto newName = profile.starts_with("trdos"sv) ? "NEW.C"sv
                : profile.starts_with("mdos"sv)       ? "NEW.B"sv
                                                      : "NEW"sv;
        a.rename(name, newName);
        assert(a.read(newName).size() >= large.size());
        a.put(newName, Bytes{ 5, 6 });
        auto updated = a.read(newName);
        assert(updated[0] == 5 && updated[1] == 6);
        a.remove(newName);
        for (auto &e : a.list())
            assert(e.directory);
    }
    auto plus = MediaArchive::create("dsk"sv);
    plus.put("HELLO.BAS"sv, Bytes{ 1, 2, 3 },
             { { "type"s, 0 }, { "param1"s, 10 }, { "param2"s, 3 } });
    assert(plus.read("HELLO.BAS"sv) == Bytes({ 1, 2, 3 }));
    auto disk = plus.encode();
    DiskImage native;
    native.loadFromBytes(disk, "dsk"sv);
    auto dir = native.findSector(1, 0, 1);
    assert(dir);
    assert(dir->get().data[0] == 0);
    assert(dir->get().data[1] == 'H');
    plus.setAttributes("HELLO.BAS"sv, { { "readonly"s, 1 } });
    fails([&] { plus.put("HELLO.BAS"sv, Bytes{ 4 }); });
    plus.setAttributes("HELLO.BAS"sv, { { "readonly"s, 0 } });
    plus.put("user01/HELLO.BAS"sv, Bytes{ 5 });
    assert(plus.read("user01/HELLO.BAS"sv)[0] == 5);
    plus.rename("user01/HELLO.BAS"sv, "user02/MOVED.BAS"sv);
    assert(plus.read("user02/MOVED.BAS"sv)[0] == 5);
    plus.setBootTracks(Bytes(4608, 0xA7));
    auto boot = plus.encode();
    native.loadFromBytes(boot, "dsk"sv);
    assert(native.findSector(0, 0, 1)->get().data[0] == 0xA7);
    auto trd = MediaArchive::create("trd"sv);
    trd.put("TEST.C"sv, Bytes{ 4, 5, 6 });
    auto scl = trd.encode("scl"sv);
    assert(std::string(scl.begin(), scl.begin() + 8) == "SINCLAIR"sv);
    assert(MediaArchive::open(scl, "scl"sv).read("TEST.C"sv) == Bytes({ 4, 5, 6 }));
}
void historicalLayouts()
{
    auto trdos = MediaArchive::create("trd"sv);
    trdos.put("TEST.C"sv, Bytes(1000, 0xA5));
    auto trimmed = trdos.encode();
    trimmed.resize(20 * 256);
    auto reopened = MediaArchive::open(trimmed, "trd"sv);
    assert(reopened.read("TEST.C"sv) == Bytes(1000, 0xA5));
    trimmed.resize(19 * 256);
    fails([&] { MediaArchive::open(trimmed, "trd"sv); });
    auto zeroLength = trdos.encode();
    zeroLength[11] = 0;
    zeroLength[12] = 0;
    auto opaque = MediaArchive::open(zeroLength, "trd"sv);
    assert(opaque.read("TEST.C"sv).size() == 1024);

    auto named = trdos.encode();
    const auto slashName = "A/B     "sv;
    std::ranges::copy(slashName, named.begin());
    auto escaped = MediaArchive::open(named, "trd"sv);
    assert(escaped.read("A%2FB.C"sv) == Bytes(1000, 0xA5));
    escaped.setAttributes("A%2FB.C"sv, { { "param1"s, 40000 } });
    assert(escaped.read("A%2FB.C"sv) == Bytes(1000, 0xA5));

    auto hc = MediaArchive::create("img"sv, { .filesystem = "hc-basic-40"s });
    hc.put("run"sv, Bytes(1200, 0x5A));
    hc.put("RUN"sv, Bytes(1500, 0xA5));
    const auto raw = hc.encode();
    assert(raw.size() == 327680);
    assert(raw[16] == 2 && raw[17] == 0 && raw[48] == 3 && raw[49] == 0);
    // The second file's allocation block starts at logical sector 24, physical sector 2.
    assert(raw[(16 + 1) * 256] == 3 && raw[(16 + 1) * 256 + 9] == 0xA5);
    assert(hc.read("run"sv) != hc.read("RUN"sv));
    auto blankName = raw;
    for (std::size_t index = 1; index <= 11; ++index)
        blankName.at(index) = ' ';
    auto unnamed = MediaArchive::open(blankName, "img"sv, { .filesystem = "hc-basic-40"s });
    assert(unnamed.read("%20"sv) == Bytes(1200, 0x5A));
    unnamed.put("%20"sv, Bytes(1100, 0x5B));
    assert(unnamed.read("%20"sv) == Bytes(1100, 0x5B));
}
void diskDetection()
{
    const Bytes payload(41003, 0xA5);
    for (const auto profile :
         { "hc-basic-40"sv, "hc-basic-80"sv, "cpm-360"sv, "cpm-720"sv, "plus3"sv, "plus3-720"sv }) {
        auto source = MediaArchive::create("img"sv, { .filesystem = std::string(profile) });
        const auto name = profile.starts_with("hc-basic"sv) ? "demo.bas"sv : "DEMO.BIN"sv;
        source.put(name, payload);
        const auto expected = source.read(name);
        for (const auto container : { "img"sv, "dsk"sv, "td0"sv, "cqm"sv }) {
            auto detected = MediaArchive::open(source.encode(container), container);
            assert(detected.read(name) == expected);
            assert(detected.encode("img"sv).size() == source.encode("img"sv).size());
            detected.rename(name, "NEW"sv);
            detected.put("NEW"sv, Bytes{ 1, 2, 3 });
            auto reopened = MediaArchive::open(detected.encode(container), container);
            const auto bytes = reopened.read("NEW"sv);
            assert(bytes.size() >= 3 && bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3);
        }
    }

    auto hc = MediaArchive::create("img"sv, { .filesystem = "hc-basic-80"s });
    fails([&] { MediaArchive::open(hc.encode(), "img"sv); });
    hc.put("raw"sv, payload, { { "raw"s, 1 } });
    fails([&] { MediaArchive::open(hc.encode(), "img"sv); });
    hc.put("typed"sv, payload);
    const auto encoded = hc.encode();
    assert(MediaArchive::open(encoded, ""sv).read("typed"sv) == payload);
    auto crossLinked = encoded;
    std::copy_n(crossLinked.begin() + 16, 16, crossLinked.begin() + 48);
    fails([&] { MediaArchive::open(crossLinked, "img"sv); });

    auto cpm = MediaArchive::create("img"sv, { .filesystem = "cpm-720"s });
    cpm.put("CPM.BIN"sv, Bytes(128, 0xA5));
    auto plus3 = MediaArchive::create("img"sv, { .filesystem = "plus3-720"s });
    plus3.put("PLUS3.BIN"sv, Bytes(128, 0x5A));
    auto ambiguous = cpm.encode();
    const auto other = plus3.encode();
    // Both complete catalogues and their files fit in distinct areas of this image.
    std::copy_n(other.begin(), 512, ambiguous.begin());
    std::copy_n(other.begin() + 4608, 10240, ambiguous.begin() + 4608);
    fails([&] { MediaArchive::open(ambiguous, "img"sv); });
    const auto selected = MediaArchive::open(ambiguous, "img"sv, { .filesystem = "cpm-720"s });
    assert(selected.read("CPM.BIN"sv) == Bytes(128, 0xA5));
}
void next()
{
    auto n = MediaArchive::create("nex"sv);
    Bytes bank(16384, 0x5A);
    n.put("banks/005.bin"sv, bank);
    auto b = n.encode();
    assert(b[9] == 1 && b[23] == 1 && b[512] == 0x5A);
    auto r = MediaArchive::open(b, "nex"sv);
    assert(r.read("banks/005.bin"sv) == bank);
    r.remove("banks/005.bin"sv);
    assert(r.encode().size() == 512);
    auto disk = MediaArchive::create("p3d"sv);
    disk.put("HELLO.COM"sv, Bytes{ 1, 2, 3 });
    auto p = disk.encode();
    assert(p.size() == 16 * 1024 * 1024 + 512 && p[0] == 'P' && p[3] == 26 && p[6] == 6
           && p[32] == 3);
    auto opened = MediaArchive::open(p, "p3d"sv);
    assert(opened.read("HELLO.COM"sv)[1] == 2);
    auto f = MediaArchive::create("img"sv, { .filesystem = "fat"s });
    f.makeDirectory("one/two"sv);
    f.put("one/two/hello world.txt"sv, Bytes{ 5, 6, 7 });
    f.setAttributes("one/two/hello world.txt"sv, { { "hidden"s, 1 } });
    f.rename("one/two/hello world.txt"sv, "one/new.txt"sv);
    assert(f.read("one/new.txt"sv) == Bytes({ 5, 6, 7 }));
    f.rename("one/two"sv, "two"sv);
    fails([&] { f.rename("one"sv, "one/inside"sv); });
    auto raw = f.encode();
    auto openedFat = MediaArchive::open(raw, "img"sv);
    assert(openedFat.read("one/new.txt"sv) == Bytes({ 5, 6, 7 }));
    openedFat.remove("one/new.txt"sv);
    openedFat.remove("two"sv);
    openedFat.remove("one"sv);
}
void historicalMedia()
{
    const std::filesystem::path fixtures = ZXKIT_MEDIA_FIXTURES;
    auto original = test::readFile(fixtures / "cobra"sv / "KRYS80_CL_SYSDISK.img"sv);
    auto source = MediaArchive::open(original, "img"sv, { .filesystem = "cobra-cpm"s });
    auto bootable = MediaArchive::create("dsk"sv, { .filesystem = "cobra-cpm"s });
    bootable.setBootTracks(std::span(original).first(18432));
    bootable.put("CBOT.SYS"sv, source.read("CBOT.SYS"sv));
    Emulator emulator(test::romConfig(fixtures / "roms"sv, "cobra1_80k"s));
    emulator.insertDisk(0, bootable.encode(), ".dsk"sv);
    emulator.runFrames(100);
    test::tap(emulator, { ZxKey::Enter });
    emulator.runFrames(200);
    test::tap(emulator, { ZxKey::D });
    emulator.runFrames(1200);
    assert(emulator.mmu().read(0) == 0xC3 && emulator.cpu().registers().IM == 2);
    auto td0 = test::readFile(fixtures / "cobra"sv / "UTILS1.TD0"sv);
    auto devil = MediaArchive::open(td0, "td0"sv, { .filesystem = "devil"s });
    auto stat = devil.read("STAT"sv);
    assert(stat.size() == 10293 && stat[0] == 0 && stat[1] == 10);
    auto entries = devil.list();
    devil.setAttributes("STAT"sv, { { "readonly"s, 1 } });
    auto edited = MediaArchive::open(devil.encode(), "td0"sv, { .filesystem = "devil"s });
    for (auto &e : entries)
        assert(edited.read(e.name) == devil.read(e.name));
    assert(edited.read("STAT"sv) == stat);
}
void microdrive()
{
    auto a = MediaArchive::create("mdr"sv);
    Bytes data(2701, 0xA5);
    a.put("TEST"sv, data, { { "type"s, 3 }, { "param1"s, 50000 } });
    auto bytes = a.encode();
    assert(bytes.size() == 137923 && bytes[0] == 1 && bytes[1] == 254);
    auto b = MediaArchive::open(bytes, "mdr"sv);
    assert(b.read("TEST"sv) == data);
    b.rename("TEST"sv, "NEW"sv);
    b.remove("NEW"sv);
    assert(b.list().empty());
    bytes.back() = 1;
    auto protectedMedia = MediaArchive::open(bytes, "mdr"sv);
    fails([&] { protectedMedia.remove("TEST"sv); });
}
void numberParsing()
{
    struct Case
    {
        std::string_view text;
        std::errc error;
        std::size_t length;
        double value;
    };
    constexpr std::array cases = {
        Case{ "0"sv, std::errc{}, 1, 0 },
        Case{ "10"sv, std::errc{}, 2, 10 },
        Case{ "1.5"sv, std::errc{}, 3, 1.5 },
        Case{ ".5"sv, std::errc{}, 2, 0.5 },
        Case{ "1."sv, std::errc{}, 2, 1 },
        Case{ "-2.25"sv, std::errc{}, 5, -2.25 },
        Case{ "1E3"sv, std::errc{}, 3, 1000 },
        Case{ "1e-2"sv, std::errc{}, 4, 0.01 },
        Case{ "2.5E+2X"sv, std::errc{}, 6, 250 },
        Case{ "1E"sv, std::errc{}, 1, 1 },
        Case{ "1E+"sv, std::errc{}, 1, 1 },
        Case{ "1AND"sv, std::errc{}, 1, 1 },
        Case{ "3.14159265358979"sv, std::errc{}, 16, 3.14159265358979 },
        Case{ "."sv, std::errc::invalid_argument, 0, -7 },
        Case{ "-"sv, std::errc::invalid_argument, 0, -7 },
        Case{ "X"sv, std::errc::invalid_argument, 0, -7 },
        Case{ "1E999"sv, std::errc::result_out_of_range, 5, -7 },
    };
    for (const auto &c : cases) {
        const char *first = c.text.data();
        const char *last = first + c.text.size();
        double classic = -7;
        auto classicResult = parseClassicNumber(first, last, classic);
        assert(classicResult.ec == c.error);
        assert(std::size_t(classicResult.ptr - first) == c.length);
        assert(classic == c.value);
        // Uses std::from_chars wherever the C++ library has it.
        double standard = -7;
        auto standardResult = parseNumber(first, last, standard);
        assert(standardResult.ec == classicResult.ec && standardResult.ptr == classicResult.ptr);
        assert(standard == classic);
    }
}
} // namespace
int main()
{
    numberParsing();
    basic();
    tape();
    disks();
    historicalLayouts();
    diskDetection();
    next();
    historicalMedia();
    microdrive();
    std::cout << "Media archive tests passed\n"sv;
}
