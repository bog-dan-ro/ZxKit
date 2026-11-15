// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "zxkit/media/BasicCodec.h"
#include "zxkit/media/MediaArchive.h"
#include "zxkit/video/VideoFrame.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

/// What a file holds, to view it
enum class MediaContent : uint8_t {
    Unknown, ///< bytes: hex, disassembly, text when they read as text
    Basic, ///< a tokenized BASIC program
    NumberArray,
    CharacterArray,
    Code, ///< bytes loaded at an address
    Screen, ///< a 6912-byte screen
    Text,
    TapeBlock, ///< a one-block TAP or TZX file, as a tape's blocks/ hold
    Snapshot
};

/// The content of an entry of an image opened with `filesystem` (MediaArchive::filesystem()):
/// from its tape or +3DOS type, or TR-DOS letter, else from its name.
MediaContent mediaContent(std::string_view filesystem, std::string_view name,
                          const MediaAttributes &attributes, uint64_t size);
/// Where an entry's code loads: its header's address (param1), 256 for a CP/M .COM program,
/// else 0
uint16_t mediaLoadAddress(std::string_view name, const MediaAttributes &attributes);
/// The content of a host file, from its name and size
MediaContent fileContent(std::string_view name, uint64_t size);
/// Names that are text by their extension: .txt, .asm, .doc, .bas and the like
bool textFileName(std::string_view name);

/// A file's text: UTF-8 with LF lines, or 7-bit ASCII as CP/M keeps it
struct MediaText
{
    std::string text;
    bool utf8 = true;
    bool bit7 = false; ///< bit-7 characters (WordStar's formatting) were dropped
    std::string lineEnding = "\n"; ///< the file's own: "\r\n" (CP/M), "\r" or "\n"
};
/// The text in `data` when it reads as text. CP/M's end-of-file mark (0x1A) ends it when it
/// falls in the last 128-byte record.
std::optional<MediaText> mediaText(std::span<const uint8_t> data);

/// "ADDRESS  16 hex bytes  ASCII" lines, at most `limit` bytes, then a line saying how many
/// more there are
std::vector<std::string> hexLines(std::span<const uint8_t> data, uint32_t address,
                                  std::size_t limit = 256 * 1024);
/// "ADDRESS  bytes  instruction" lines, from `address`, at most `limit` bytes. When `restart`
/// falls inside an instruction, the next instruction starts there.
std::vector<std::string> disassemblyLines(std::span<const uint8_t> data, uint16_t address,
                                          int restart = -1, std::size_t limit = 64 * 1024);

/// Where a 48K or 128K BASIC program starts without Interface 1's channels. BASIC views take
/// the program to load where BasicCodec::programStart() says: here, or after the channels.
inline constexpr uint16_t basicProgramStart = 23755;

/// One way to look at a file
struct MediaViewPage
{
    enum class Kind : uint8_t {
        Basic, ///< the unhacked listing: `listing`, its text in `lines`
        Unfiltered, ///< every byte, as BasicCodec::decompile() writes them
        Usr, ///< the code a USR call in the program runs, from `address`
        Screen, ///< `screen`: exactly a screen
        AsScreen, ///< `screen`: the first 6912 bytes of larger data
        Hex, ///< from `address`
        Disassembly, ///< from `address`; `bytes` disassembles again, see disassemblyLines()
        Text,
        Array,
        Block, ///< a tape block's type, length and timings
        Raw ///< a tape block's one-block TAP or TZX file, in hex
    };
    Kind kind = Kind::Hex;
    uint32_t address = 0;
    std::vector<std::string> lines;
    std::vector<BasicCodec::ListingLine> listing;
    std::vector<uint8_t> bytes;
    std::optional<VideoFrame> screen;
};

/// The pages that fit `content`, the most telling first. `address` is where code loads (a
/// tape or +3DOS header's first parameter); `name` names a snapshot's format.
std::vector<MediaViewPage> viewMedia(std::span<const uint8_t> data, MediaContent content,
                                     uint16_t address = 0, std::string_view name = {});

} // namespace zxkit
