// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/BasicCodec.h"
#include "zxkit/media/FatImage.h"
#include "zxkit/media/MediaArchive.h"
#include "zxkit/media/MediaPreview.h"
#include "zxkit/media/MediaView.h"
#include "zxkit/media/TapeFormats.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace zxkit;
using namespace std::literals;
using Bytes = std::vector<uint8_t>;
using Kind = MediaViewPage::Kind;

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

Bytes bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

std::vector<Kind> kinds(const std::vector<MediaViewPage> &pages)
{
    std::vector<Kind> out;
    for (const auto &page : pages)
        out.push_back(page.kind);
    return out;
}

void contents()
{
    assert(mediaContent("tape"sv, "blocks/000001.tap"sv, {}, 21) == MediaContent::TapeBlock);
    assert(mediaContent("tape"sv, "basic/GAME.bas"sv, {}, 100) == MediaContent::Text);
    assert(mediaContent("tape"sv, "files/GAME"sv, { { "type"s, 0 } }, 100) == MediaContent::Basic);
    assert(mediaContent("plus3"sv, "SCREEN"sv, { { "type"s, 3 } }, 6912) == MediaContent::Screen);
    assert(mediaContent("plus3"sv, "CODE"sv, { { "type"s, 3 } }, 6913) == MediaContent::Code);
    assert(mediaContent("trdos"sv, "boot.B"sv, { { "type"s, 'B' } }, 9) == MediaContent::Basic);
    assert(mediaContent("trdos"sv, "mman$.C"sv, { { "type"s, 'C' } }, 6912)
           == MediaContent::Screen);
    assert(mediaContent("cpm-720"sv, "CA2.TXT"sv, { { "user"s, 0 } }, 9) == MediaContent::Text);
    assert(mediaContent("cpm-720"sv, "D.COM"sv, { { "user"s, 0 } }, 9) == MediaContent::Unknown);
    assert(mediaLoadAddress("DDT.COM"sv, { { "user"s, 0 } }) == 0x100);
    assert(mediaLoadAddress("CODE"sv, { { "param1"s, 32768 } }) == 32768);
    assert(mediaLoadAddress("DATA"sv, {}) == 0);
    assert(fileContent("title.SCR"sv, 6912) == MediaContent::Screen);
    assert(fileContent("game.z80"sv, 6912) == MediaContent::Snapshot);
    assert(fileContent("notes.txt"sv, 3) == MediaContent::Text);
    assert(fileContent("data.bin"sv, 3) == MediaContent::Unknown);
}

void texts()
{
    // CP/M: CR LF lines up to the end-of-file mark of the last record
    auto cpm = bytesOf("Hello\r\nNext\r\n"sv);
    cpm.resize(128, 0x1A);
    auto text = mediaText(cpm);
    assert(text && text->text == "Hello\nNext\n"sv && text->lineEnding == "\r\n"sv && text->utf8);
    // WordStar sets bit 7 on word ends: read as 7-bit ASCII
    text = mediaText(bytesOf("Hello wor\xEC"
                             "d\r\n"sv));
    assert(text && text->text == "Hello world\n"sv && !text->utf8 && text->bit7);
    assert(mediaText(bytesOf("caf\xC3\xA9\n"sv))->utf8);
    // A program with an early end-of-file byte, or mostly 8-bit bytes, is no text
    Bytes program(300, 0x3E);
    for (std::size_t i = 0; i < program.size(); i += 7)
        program[i] = 0;
    program[5] = 0x1A;
    assert(!mediaText(program));
    assert(!mediaText(Bytes(100, 0xC9)));
    assert(mediaText(bytesOf("one\rtwo\r"sv))->lineEnding == "\r"sv);
}

void lines()
{
    const Bytes code{ 0x21, 0x2F, 0x63, 0xC9 };
    const auto hex = hexLines(code, 0x8000);
    assert(hex.size() == 1 && hex[0].starts_with("8000  21 2F 63 C9"sv)
           && hex[0].ends_with("!/c."sv));
    assert(hexLines(Bytes(40, 0), 0, 32).back() == "\xE2\x80\xA6 8 more bytes"sv);
    auto listing = disassemblyLines(code, 0x8000);
    assert(listing.size() == 2 && listing[0].starts_with("8000  21 2F 63"sv)
           && listing[0].ends_with("LD HL,$632F"sv));
    // Going to 8001, inside LD HL,nn, restarts there
    listing = disassemblyLines(code, 0x8000, 0x8001);
    assert(listing.size() == 4 && listing[1].starts_with("8001  2F"sv)
           && listing[3].starts_with("8003  C9"sv));
}

void views()
{
    // A protected loader: a REM holding code, called from a second line 0
    const Bytes loader{ 0, 0,  9, 0,    0xEA, 0x10, 6,    'H', 'I', 0xF3, 0xC9, 0, 0x0D, 0,
                        0, 10, 0, 0xF5, 0xC0, '0',  0x0E, 0,   0,   0xD4, 0x5C, 0, 0x0D };
    auto pages = viewMedia(loader, MediaContent::Basic);
    assert((kinds(pages) == std::vector{ Kind::Basic, Kind::Unfiltered, Kind::Usr, Kind::Hex }));
    assert(pages[0].lines[1] == "   0 PRINT USR 23764"sv && pages[0].listing.size() == 2);
    assert(pages[2].address == 23764 && pages[2].lines[0].starts_with("5CD4  F3"sv));
    assert(pages[3].address == basicProgramStart);
    // The same with code after a credit in line 0, the program after a disk's channels
    Bytes credit{ 0,   0,    20,   0,    0xF4, 0xC6, 0xFF, 'B',  'Y',  ' ',  'M',  'E',
                  ' ', 0x11, 0x3E, 0xEF, 0x21, 0x97, 0xF3, 0x1A, 0xFE, 0x00, 0xC9, 0x0D };
    const auto run = BasicCodec::compile("1 RANDOMIZE USR VAL\"23826\"\n"sv);
    credit.insert(credit.end(), run.begin(), run.end());
    pages = viewMedia(credit, MediaContent::Basic);
    assert((kinds(pages) == std::vector{ Kind::Basic, Kind::Unfiltered, Kind::Usr, Kind::Hex }));
    assert(pages[0].address == 23813 && pages[3].address == 23813);
    assert(pages[2].address == 23826 && pages[2].lines[0].starts_with("5D12  11 3E EF"sv));
    // The code to the end of its line, not into the next one
    assert(pages[2].bytes.size() == 10 && pages[2].lines.back().ends_with("RET"sv));

    // Code, a screen, and code larger than a screen
    pages = viewMedia(Bytes{ 0xC9 }, MediaContent::Code, 0x8000);
    assert((kinds(pages) == std::vector{ Kind::Hex, Kind::Disassembly }));
    assert(pages[1].bytes == Bytes{ 0xC9 } && pages[1].address == 0x8000);
    pages = viewMedia(Bytes(6912, 0), MediaContent::Screen);
    assert((kinds(pages) == std::vector{ Kind::Screen, Kind::Hex, Kind::Disassembly }));
    assert(pages[0].screen && pages[0].screen->width() == 256 && pages[1].address == 16384);
    pages = viewMedia(Bytes(7000, 0), MediaContent::Code, 0x4000);
    assert(kinds(pages).back() == Kind::AsScreen);

    // A tape's screen data block: flag, 6912 bytes, checksum
    Bytes data{ 0xFF };
    data.resize(6913, 0);
    data.push_back(0xFF);
    const auto block = TapeBlock::standard(data);
    const auto tap = TapeFormats::writeTap(std::span(&block, 1));
    pages = viewMedia(tap, MediaContent::TapeBlock);
    assert((kinds(pages) == std::vector{ Kind::Screen, Kind::Block, Kind::Hex, Kind::Raw }));

    // Text, or bytes that are none
    pages = viewMedia(bytesOf("10 PRINT\n"sv), MediaContent::Unknown);
    assert((kinds(pages) == std::vector{ Kind::Text, Kind::Hex })
           && pages[0].lines[0] == "10 PRINT"sv);
    pages = viewMedia(Bytes(20, 0), MediaContent::Unknown);
    assert((kinds(pages) == std::vector{ Kind::Hex, Kind::Disassembly }));
    pages = viewMedia(BasicCodec::compileArray("2\n1 2\n"sv), MediaContent::NumberArray);
    assert(pages[0].kind == Kind::Array);
}

void fatInPlace()
{
    auto card = MediaArchive::create(".img"sv, { .filesystem = "fat"s }).encode();
    const auto size = card.size();
    FatImage image(
            [&](uint64_t lba, std::span<uint8_t, 512> out) {
                if (lba >= card.size() / 512)
                    return false;
                std::copy_n(card.begin() + lba * 512, 512, out.begin());
                return true;
            },
            [&](uint64_t lba, std::span<const uint8_t, 512> in) {
                if (lba >= card.size() / 512)
                    return false;
                std::ranges::copy(in, card.begin() + lba * 512);
                return true;
            });
    assert(image.isWritable());
    image.makeDirectory("nextzxos"sv);
    image.put("nextzxos/README.TXT"sv, bytesOf("hello"sv), { { "archive"s, 1 } });
    assert(image.read("NEXTZXOS/readme.txt"sv) == bytesOf("hello"sv));
    assert(image.find("nextzxos/readme.txt"sv)->attributes.at("archive"s) == 1);
    assert(image.list().size() == 1 && image.list({}, true).size() == 2);
    assert(image.list("missing"sv).empty());
    image.setAttributes("nextzxos/README.TXT"sv, { { "readonly"s, 1 } });
    fails([&] { image.remove("nextzxos/README.TXT"sv); });
    fails([&] { image.put("nextzxos/README.TXT"sv, bytesOf("x"sv)); });
    fails([&] { image.setAttributes("nextzxos/README.TXT"sv, { { "user"s, 1 } }); });
    image.setAttributes("nextzxos/README.TXT"sv, { { "readonly"s, 0 } });
    image.put("nextzxos/OTHER.TXT"sv, bytesOf("other"sv));
    image.rename("nextzxos/OTHER.TXT"sv, "nextzxos/README.TXT"sv, true);
    assert(image.read("nextzxos/README.TXT"sv) == bytesOf("other"sv));
    // The changes went to the image's blocks, which MediaArchive reads back
    assert(card.size() == size);
    assert(MediaArchive::open(card, ".img"sv).read("nextzxos/README.TXT"sv) == bytesOf("other"sv));
    image.remove("nextzxos/README.TXT"sv);
    image.remove("nextzxos"sv);
    assert(image.list({}, true).empty());
    fails([&] {
        FatImage readOnly([&](uint64_t lba, std::span<uint8_t, 512> out) {
            std::copy_n(card.begin() + lba * 512, 512, out.begin());
            return true;
        });
        readOnly.makeDirectory("x"sv);
    });
}

void probing()
{
    // An HC-2000 BASIC disk in a DSK container: its geometry suggests +3
    auto hc = MediaArchive::create(".dsk"sv, { .filesystem = "hc-basic-80"s });
    hc.put("PROG"sv, BasicCodec::compile("10 PRINT 1\n"sv), { { "type"s, 0 } });
    const auto matches = MediaArchive::probe(hc.encode(), ".dsk"sv);
    assert(matches.size() == 1 && matches[0].profile == "hc-basic-80"sv && matches[0].files == 1);
    assert(MediaArchive::probe(Bytes(1000, 0x55), ".img"sv).empty());
}
} // namespace

void screens()
{
    Bytes scr(6912);
    for (std::size_t i = 0; i < scr.size(); ++i)
        scr[i] = uint8_t(i * 7);
    assert(extractMediaScreen(scr, "title.scr"sv) == scr);

    Bytes sna(27, 0);
    sna.insert(sna.end(), scr.begin(), scr.end());
    sna.resize(27 + 49152);
    assert(extractMediaScreen(sna, "game.sna"sv) == scr);

    // A tape's SCREEN$ block: its flag, the screen and the checksum
    const auto block = [](Bytes data) {
        Bytes out{ uint8_t(data.size()), uint8_t(data.size() >> 8) };
        out.insert(out.end(), data.begin(), data.end());
        return out;
    };
    Bytes header{ 0,   3,   'S',  'C',  'R',  'E',  'E',  'N',  ' ', ' ',
                  ' ', ' ', 0x00, 0x1B, 0x00, 0x40, 0x00, 0x80, 0 };
    Bytes data{ 0xFF };
    data.insert(data.end(), scr.begin(), scr.end());
    data.push_back(0);
    Bytes tap = block(header);
    const auto screenBlock = block(data);
    tap.insert(tap.end(), screenBlock.begin(), screenBlock.end());
    assert(extractMediaScreen(tap, "game.tap"sv) == scr);
    assert(extractMediaPreview(tap, "game.tap"sv).width() == 256);

    // Files without a screen: nothing to keep, and no preview
    assert(extractMediaScreen(block(header), "basic.tap"sv).empty());
    assert(extractMediaScreen(bytesOf("notes"sv), "notes.txt"sv).empty());
    fails([] { extractMediaPreview(bytesOf("notes"sv), "notes.txt"sv); });
}

int main()
{
    contents();
    texts();
    lines();
    views();
    fatInPlace();
    probing();
    screens();
    std::cout << "Media view tests passed\n"sv;
}
