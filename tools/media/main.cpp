// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Fat32Copy.h"
#include "MediaImage.h"
#include "zxkit/media/BasicCodec.h"
#include "zxkit/media/MediaView.h"
#include "zxkit/media/ScreenshotFormat.h"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
using namespace zxkit;
using namespace zxkit::tool;
namespace {
uint64_t integer(std::string_view text)
{
    uint64_t n = 0;
    int base = 10;
    if (text.starts_with("0x"sv)) {
        text.remove_prefix(2);
        base = 16;
    }
    auto r = std::from_chars(text.data(), text.data() + text.size(), n, base);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
        throw InvalidArgumentException("Invalid integer"sv);
    return n;
}
// An address: decimal (23803) or hex ($5CFB, #5CFB, 0x5CFB, 5CFBh, or with A to F)
uint16_t address(std::string_view text)
{
    int base = 10;
    std::string digits(text);
    if (digits.starts_with('$') || digits.starts_with('#')) {
        digits.erase(0, 1);
        base = 16;
    } else if (digits.starts_with("0x"sv) || digits.starts_with("0X"sv)) {
        digits.erase(0, 2);
        base = 16;
    } else if (digits.ends_with('h') || digits.ends_with('H')) {
        digits.pop_back();
        base = 16;
    } else if (digits.find_first_of("abcdefABCDEF"sv) != digits.npos) {
        base = 16;
    }
    unsigned value = 0;
    auto r = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (digits.empty() || r.ec != std::errc{} || r.ptr != digits.data() + digits.size()
        || value > 65535)
        throw InvalidArgumentException("Invalid address: "s + std::string(text));
    return uint16_t(value);
}
MediaAttributes attributes(const std::vector<std::string> &args, std::size_t start)
{
    MediaAttributes a;
    for (std::size_t i = start; i < args.size(); ++i) {
        auto pos = args[i].find('=');
        if (pos == std::string::npos)
            throw InvalidArgumentException("Attributes use key=value"sv);
        a[args[i].substr(0, pos)] = integer(std::string_view(args[i]).substr(pos + 1));
    }
    return a;
}
void output(const fs::path &path, std::span<const uint8_t> data, bool force)
{
    if (path == "-"sv) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
        if (!std::cout)
            throw IoException("Cannot write standard output"sv);
        return;
    }
    ImageLock lock(imagePath(path));
    atomicWrite(path, data, force);
}
void listing(const Image &media, bool mc)
{
    for (auto &e : media.list()) {
        auto permissions = e.directory
                ? "drwxr-xr-x"sv
                : (e.attributes.contains("readonly"sv) && e.attributes.at("readonly"s)
                           ? "-r--r--r--"sv
                           : "-rw-r--r--"sv);
        if (mc)
            std::cout << permissions << " 1 0 0 "sv << e.size << " Jan 01 2000 "sv << e.name
                      << '\n';
        else {
            std::cout << (e.directory ? "d "sv : "f "sv) << e.size << '\t' << e.name;
            for (auto &[key, value] : e.attributes)
                std::cout << '\t' << key << '=' << value;
            std::cout << '\n';
        }
    }
}

// The viewer's pages, as text: highlighted with ANSI colours on a terminal
namespace ansi {
constexpr auto reset = "\x1b[0m"sv;
constexpr auto dim = "\x1b[2m"sv;
constexpr auto keyword = "\x1b[36m"sv;
constexpr auto number = "\x1b[94m"sv;
constexpr auto string = "\x1b[33m"sv;
constexpr auto comment = "\x1b[2;3m"sv;
constexpr auto control = "\x1b[93m"sv;
constexpr auto decoy = "\x1b[2;9m"sv;
constexpr auto used = "\x1b[1;91m"sv;
} // namespace ansi
std::string paint(std::string_view style, std::string_view text)
{
    return text.empty() ? std::string()
                        : std::string(style) + std::string(text) + std::string(ansi::reset);
}
std::string paintBasic(const BasicCodec::ListingLine &line)
{
    using Kind = BasicCodec::ListingPart::Kind;
    std::string out;
    for (const auto &part : line) {
        if (part.listed) {
            if (!part.listed->empty())
                out += paint(ansi::decoy, *part.listed) + " "s;
            out += paint(ansi::used, part.text);
            continue;
        }
        switch (part.kind) {
        case Kind::LineNumber:
            out += paint(ansi::dim, part.text);
            break;
        case Kind::Keyword:
            out += paint(ansi::keyword, part.text);
            break;
        case Kind::Number:
            out += paint(ansi::number, part.text);
            break;
        case Kind::String:
            out += paint(ansi::string, part.text);
            break;
        case Kind::Comment:
        case Kind::Code:
            out += paint(ansi::comment, part.text);
            break;
        case Kind::Control:
            out += paint(ansi::control, part.text);
            break;
        case Kind::Text:
            out += part.text;
            break;
        }
    }
    return out;
}
std::string paintLine(MediaViewPage::Kind kind, std::string_view line)
{
    using Kind = MediaViewPage::Kind;
    if (kind == Kind::Hex || kind == Kind::Raw) {
        // ADDR  HH HH … (16 bytes)  text
        const auto space = line.find(' ');
        if (space == line.npos || line.size() < space + 50)
            return paint(ansi::dim, line);
        return paint(ansi::dim, line.substr(0, space)) + std::string(line.substr(space, 50))
                + paint(ansi::dim, line.substr(space + 50));
    }
    if (kind == Kind::Disassembly || kind == Kind::Usr) {
        // ADDR  bytes        MNEMONIC operands
        if (line.size() < 18)
            return std::string(line);
        const auto instruction = line.substr(18);
        const auto space = instruction.find(' ');
        return paint(ansi::dim, line.substr(0, 18))
                + paint(ansi::keyword, instruction.substr(0, space))
                + std::string(space == instruction.npos ? ""sv : instruction.substr(space));
    }
    if (kind == Kind::Unfiltered) {
        // \{KEYWORD} tokens, \xHH bytes, strings and !raw data
        if (line.starts_with("!raw"sv))
            return paint(ansi::dim, line);
        std::string out;
        for (std::size_t i = 0; i < line.size();) {
            if (line.substr(i).starts_with("\\{"sv)) {
                const auto end = line.find('}', i);
                const auto length = end == line.npos ? line.size() - i : end + 1 - i;
                out += paint(ansi::keyword, line.substr(i, length));
                i += length;
            } else if (line.substr(i).starts_with("\\x"sv) && line.size() - i >= 4) {
                out += paint(ansi::dim, line.substr(i, 4));
                i += 4;
            } else if (line[i] == '"') {
                const auto end = line.find('"', i + 1);
                const auto length = end == line.npos ? line.size() - i : end + 1 - i;
                out += paint(ansi::string, line.substr(i, length));
                i += length;
            } else {
                out += line[i++];
            }
        }
        return out;
    }
    return std::string(line);
}
std::string pageTitle(const MediaViewPage &page)
{
    using Kind = MediaViewPage::Kind;
    switch (page.kind) {
    case Kind::Basic:
        return "Unhacked"s;
    case Kind::Unfiltered:
        return "Unfiltered"s;
    case Kind::Usr:
        return std::format("USR {}", page.address);
    case Kind::Screen:
        return "Screen"s;
    case Kind::AsScreen:
        return "As screen"s;
    case Kind::Hex:
        return "Hex"s;
    case Kind::Disassembly:
        return "Disassembly"s;
    case Kind::Text:
        return "Text"s;
    case Kind::Array:
        return "Array"s;
    case Kind::Block:
        return "Block"s;
    case Kind::Raw:
        break;
    }
    return "Raw"s;
}
bool pageNamed(const MediaViewPage &page, std::string_view name)
{
    using Kind = MediaViewPage::Kind;
    if (name == "basic"sv || name == "unhacked"sv)
        return page.kind == Kind::Basic;
    if (name == "screen"sv)
        return page.kind == Kind::Screen || page.kind == Kind::AsScreen;
    if (name == "disassembly"sv || name == "disasm"sv)
        return page.kind == Kind::Disassembly;
    static constexpr std::array names = { std::pair{ "unfiltered"sv, Kind::Unfiltered },
                                          std::pair{ "usr"sv, Kind::Usr },
                                          std::pair{ "hex"sv, Kind::Hex },
                                          std::pair{ "text"sv, Kind::Text },
                                          std::pair{ "array"sv, Kind::Array },
                                          std::pair{ "block"sv, Kind::Block },
                                          std::pair{ "raw"sv, Kind::Raw } };
    for (const auto &[known, kind] : names)
        if (name == known)
            return page.kind == kind;
    throw InvalidArgumentException("Unknown view: "s + std::string(name));
}
uint32_t lineAddress(std::string_view line)
{
    uint32_t value = 0;
    const auto end = line.find(' ');
    auto r = std::from_chars(line.data(), line.data() + std::min(end, line.size()), value, 16);
    return r.ec == std::errc{} ? value : UINT32_MAX;
}
struct ViewOptions
{
    std::string as;
    std::optional<uint16_t> from;
    std::optional<uint16_t> origin;
    std::string png;
    bool colour = false;
    bool force = false;
};
void view(std::span<const uint8_t> data, MediaContent content, uint16_t loadAddress,
          std::string_view name, const ViewOptions &options)
{
    using Kind = MediaViewPage::Kind;
    auto pages = viewMedia(data, content, options.origin.value_or(loadAddress), name);
    std::vector<MediaViewPage *> chosen;
    const auto choose = [&](auto &&wanted) {
        for (auto &page : pages)
            if (wanted(page))
                chosen.push_back(&page);
    };
    if (!options.as.empty())
        choose([&](const MediaViewPage &page) { return pageNamed(page, options.as); });
    else if (!options.png.empty())
        choose([](const MediaViewPage &page) { return page.screen.has_value(); });
    else if (options.from) {
        // Going to an address: in the disassembly, else in the hex dump
        for (auto kind : { Kind::Disassembly, Kind::Usr, Kind::Hex })
            if (chosen.empty())
                choose([&](const MediaViewPage &page) { return page.kind == kind; });
    } else
        choose([](const MediaViewPage &) { return true; });
    if (chosen.empty()) {
        std::string message = "This file has no such view; it has:"s;
        for (const auto &page : pages)
            message += " "s + pageTitle(page);
        throw InvalidArgumentException(message);
    }
    if (!options.png.empty()) {
        const auto screen =
                std::ranges::find_if(chosen, [](auto *page) { return page->screen.has_value(); });
        if (screen == chosen.end())
            throw InvalidArgumentException("This view has no screen to save"sv);
        output(options.png, encodePng(*(*screen)->screen), options.force);
        return;
    }
    for (auto *page : chosen) {
        if (chosen.size() > 1)
            std::cout << (options.colour ? paint(ansi::dim, "== "s + pageTitle(*page) + " =="s)
                                         : "== "s + pageTitle(*page) + " =="s)
                      << '\n';
        if (page->screen) {
            std::cout << "(a 256x192 screen: save it with --png FILE)\n"sv;
            continue;
        }
        auto lines = page->lines;
        std::size_t first = 0;
        if (options.from && (page->kind == Kind::Disassembly || page->kind == Kind::Usr)) {
            // An address inside an instruction restarts the disassembly there
            lines = disassemblyLines(page->bytes, uint16_t(page->address), *options.from);
            const auto at = std::ranges::find(lines, uint32_t(*options.from), lineAddress);
            if (at == lines.end())
                throw InvalidArgumentException(
                        std::format("{} is outside this disassembly", *options.from));
            first = std::size_t(at - lines.begin());
        } else if (options.from && page->kind == Kind::Hex) {
            const auto at = std::ranges::find_if(lines, [&](const std::string &line) {
                const auto start = lineAddress(line);
                return start <= *options.from && *options.from < start + 16;
            });
            if (at == lines.end())
                throw InvalidArgumentException(
                        std::format("{} is outside this hex dump", *options.from));
            first = std::size_t(at - lines.begin());
        }
        for (auto i = first; i < lines.size(); ++i) {
            if (!options.colour)
                std::cout << lines[i] << '\n';
            else if (page->kind == Kind::Basic && i < page->listing.size())
                std::cout << paintBasic(page->listing[i]) << '\n';
            else
                std::cout << paintLine(page->kind, lines[i]) << '\n';
        }
    }
}
void help()
{
    std::cout << R"(zxmedia [--filesystem PROFILE] [--dialect 48|128|next] [--force] COMMAND ...
  profiles                         List disk filesystem/geometry profiles
  create IMAGE [--boot-tracks FILE] Create a blank image (or install boot tracks)
  list IMAGE                       List files, native attributes and components
  get IMAGE ENTRY OUTPUT           Extract a file ('-' means standard output)
  view PATH [ENTRY]                Show a file the way the file manager does: unhacked and
                                   unfiltered BASIC, USR code, hex, disassembly, text, screens
  put IMAGE INPUT ENTRY [key=value ...]  Add or replace a file
  rm IMAGE ENTRY                   Remove a file
  rename IMAGE ENTRY NEWNAME        Rename a file
  attrs IMAGE ENTRY key=value ...   Change native attributes
  mkdir IMAGE DIRECTORY            Create a FAT directory
  boot IMAGE BOOT_TRACKS           Install exactly the reserved CP/M tracks
  convert INPUT OUTPUT             Convert compatible containers/tapes; TAP -> WAV
  copy INPUT ENTRY OUTPUT NEWNAME [key=value ...]  Transfer bytes between images
  fat32-copy INPUT OUTPUT           Copy a FAT card's files into a standard FAT32 layout
  basic compile INPUT OUTPUT       Tokenize numbered BASIC text
  basic decompile INPUT OUTPUT     Lossless editable BASIC listing and stored data
  basic list INPUT OUTPUT          Readable, unhacked listing (stored numbers, code in REMs)
  basic array-compile INPUT OUTPUT  Array text -> data (use --array-type numeric|string)
  basic array-decompile INPUT OUTPUT  Array data -> editable text
  mc COMMAND IMAGE [ENTRY [HOST]]   Midnight Commander extfs protocol
View options: --as unhacked|unfiltered|usr|screen|hex|disassembly|text|array|block|raw,
  --from ADDRESS (go to it; inside an instruction the disassembly restarts there),
  --org ADDRESS (load address), --png FILE (save the screen), --color auto|always|never.
  Addresses are decimal (23803) or hex ($5CFB, 0x5CFB, 5CFBh).
An IMAGE or PATH may go through images, as in MC: card.img/nextzxos/cpm-a.p3d.
FAT cards are edited in place. Disks detection can't settle are tried with every profile.
Use --filesystem for ambiguous raw/TD0/CQM images. Existing outputs require --force.
Tape files live under files/NAME; editable listings under basic/NAME.bas;
raw tape blocks under blocks/000000.tzx, or .tap on TAP tapes (rename one to another
index to move it). NEX exposes header.bin and banks/NNN.bin.
)"sv;
}
} // namespace
int main(int argc, char **argv)
{
    try {
        MediaOptions opts;
        BasicDialect dialect = BasicDialect::Spectrum48;
        bool force = false;
        bool characterArray = false;
        ViewOptions viewOptions;
        std::string colour = "auto"s;
        std::vector<std::string> args;
        if (const char *p = std::getenv("ZXMEDIA_FILESYSTEM"))
            opts.filesystem = p;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a == "--filesystem"sv || a == "--dialect"sv || a == "--label"sv
                || a == "--array-type"sv || a == "--as"sv || a == "--from"sv || a == "--org"sv
                || a == "--png"sv || a == "--color"sv) {
                if (++i == argc)
                    throw InvalidArgumentException("Option requires a value"sv);
                std::string_view value = argv[i];
                if (a == "--filesystem"sv)
                    opts.filesystem = value;
                else if (a == "--label"sv)
                    opts.label = value;
                else if (a == "--as"sv)
                    viewOptions.as = value;
                else if (a == "--from"sv)
                    viewOptions.from = address(value);
                else if (a == "--org"sv)
                    viewOptions.origin = address(value);
                else if (a == "--png"sv)
                    viewOptions.png = value;
                else if (a == "--color"sv) {
                    if (value != "auto"sv && value != "always"sv && value != "never"sv)
                        throw InvalidArgumentException("Use --color auto, always or never"sv);
                    colour = value;
                } else if (a == "--array-type"sv) {
                    if (value != "numeric"sv && value != "string"sv)
                        throw InvalidArgumentException("Unknown array type"sv);
                    characterArray = value == "string"sv;
                } else {
                    if (value == "48"sv)
                        dialect = BasicDialect::Spectrum48;
                    else if (value == "128"sv)
                        dialect = BasicDialect::Spectrum128;
                    else if (value == "next"sv)
                        dialect = BasicDialect::Next;
                    else
                        throw InvalidArgumentException("Unknown BASIC dialect"sv);
                }
            } else if (a == "--force"sv)
                force = true;
            else if (a == "--help"sv || a == "-h"sv) {
                help();
                return 0;
            } else
                args.emplace_back(a);
        }
        if (args.empty()) {
            help();
            return 0;
        }
        auto command = args[0];
        auto count = [&](std::size_t n) {
            if (args.size() != n)
                throw InvalidArgumentException("Wrong argument count; see --help"sv);
        };
        // Images to read, and images to change (locked, saved where they came from)
        auto open = [&](const std::string &p) {
            return std::move(resolvePath(p, opts, false).image);
        };
        auto edit = [&](const std::string &p) {
            return std::move(resolvePath(p, opts, true).image);
        };
        if (command == "profiles"sv) {
            count(1);
            for (auto &p : MediaArchive::profiles())
                std::cout << p << '\n';
        } else if (command == "create"sv) {
            if (args.size() != 2 && args.size() != 4)
                throw InvalidArgumentException("create IMAGE [--boot-tracks FILE]"sv);
            fs::path path = args[1];
            auto media = MediaArchive::create(path.extension().string(), opts);
            if (args.size() == 4) {
                if (args[2] != "--boot-tracks"sv)
                    throw InvalidArgumentException("Expected --boot-tracks"sv);
                media.setBootTracks(readFile(args[3]));
            }
            if (path.parent_path().empty() || fs::is_directory(path.parent_path())) {
                output(path, media.encode(), force);
            } else {
                // A new image inside an image
                auto parent = resolvePath(args[1], {}, true, PathTarget::New);
                if (parent.entry.empty() || (parent.image->find(parent.entry) && !force))
                    throw IoException("Output exists: "s + path.string());
                parent.image->put(parent.entry, media.encode(), {});
                parent.image->save();
            }
        } else if (command == "list"sv) {
            count(2);
            listing(*open(args[1]), false);
        } else if (command == "get"sv) {
            count(4);
            output(args[3], open(args[1])->read(args[2]), force);
        } else if (command == "view"sv) {
            if (args.size() != 2 && args.size() != 3)
                throw InvalidArgumentException("view PATH [ENTRY]"sv);
            viewOptions.force = force;
#ifdef _WIN32
            viewOptions.colour =
                    colour == "always"sv || (colour == "auto"sv && _isatty(_fileno(stdout)));
#else
            viewOptions.colour =
                    colour == "always"sv || (colour == "auto"sv && isatty(STDOUT_FILENO));
#endif
            auto resolved = resolvePath(args[1], opts, false,
                                        args.size() == 2 ? PathTarget::Entry : PathTarget::Image);
            if (args.size() == 3)
                resolved.entry = args[2];
            if (resolved.image && !resolved.entry.empty()) {
                const auto entry = resolved.image->find(resolved.entry);
                if (!entry || entry->directory)
                    throw IoException("No such file: "s + resolved.entry);
                const auto data = resolved.image->read(entry->name);
                view(data,
                     mediaContent(resolved.image->filesystem(), entry->name, entry->attributes,
                                  entry->size),
                     mediaLoadAddress(entry->name, entry->attributes), entry->name, viewOptions);
            } else {
                const auto file = resolved.host.empty() ? fs::path(args[1]) : resolved.host;
                const auto data = readFile(file);
                view(data, fileContent(file.filename().string(), data.size()), 0,
                     file.filename().string(), viewOptions);
            }
        } else if (command == "convert"sv) {
            count(3);
            fs::path out = args[2];
            output(out, open(args[1])->encode(out.extension().string()), force);
        } else if (command == "basic"sv) {
            count(4);
            auto bytes = readFile(args[2]);
            Bytes out;
            const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            if (args[1] == "compile"sv)
                out = BasicCodec::compile(text, dialect);
            else if (args[1] == "array-compile"sv)
                out = BasicCodec::compileArray(text, characterArray);
            else if (args[1] == "array-decompile"sv) {
                auto s = BasicCodec::decompileArray(bytes, characterArray);
                out.assign(s.begin(), s.end());
            } else if (args[1] == "decompile"sv) {
                auto s = BasicCodec::decompile(bytes, dialect);
                out.assign(s.begin(), s.end());
            } else if (args[1] == "list"sv) {
                auto s = BasicCodec::listing(bytes, dialect);
                out.assign(s.begin(), s.end());
            } else
                throw InvalidArgumentException("Use basic compile, decompile or list"sv);
            output(args[3], out, force);
        } else if (command == "fat32-copy"sv) {
            count(3);
            copyFat32(args[1], args[2], opts.label, force);
        } else if (command == "copy"sv) {
            if (args.size() < 5)
                throw InvalidArgumentException("copy INPUT ENTRY OUTPUT NEWNAME [attributes]"sv);
            auto data = open(args[1])->read(args[2]);
            auto target = edit(args[3]);
            target->put(args[4], data, attributes(args, 5));
            target->save();
        } else if (command == "mc"sv) {
            if (args.size() < 3)
                throw InvalidArgumentException("Invalid extfs command"sv);
            auto op = args[1];
            if (op == "list"sv) {
                count(3);
                listing(*open(args[2]), true);
            } else if (op == "copyout"sv) {
                count(5);
                output(args[4], open(args[2])->read(args[3]), true);
            } else {
                auto file = edit(args[2]);
                if (op == "copyin"sv) {
                    count(5);
                    file->put(args[3], readFile(args[4]), {});
                } else if (op == "rm"sv || op == "rmdir"sv) {
                    count(4);
                    file->remove(args[3]);
                } else if (op == "mkdir"sv) {
                    count(4);
                    file->makeDirectory(args[3]);
                } else
                    throw InvalidArgumentException("Unsupported extfs operation"sv);
                file->save();
            }
        } else {
            if (args.size() < 2)
                throw InvalidArgumentException("Image path required"sv);
            auto file = edit(args[1]);
            if (command == "put"sv) {
                if (args.size() < 4)
                    throw InvalidArgumentException("put IMAGE INPUT ENTRY [attributes]"sv);
                file->put(args[3], readFile(args[2]), attributes(args, 4));
            } else if (command == "rm"sv) {
                count(3);
                file->remove(args[2]);
            } else if (command == "rename"sv) {
                count(4);
                file->rename(args[2], args[3], force);
            } else if (command == "attrs"sv) {
                if (args.size() < 4)
                    throw InvalidArgumentException("attrs IMAGE ENTRY key=value"sv);
                file->setAttributes(args[2], attributes(args, 3));
            } else if (command == "mkdir"sv) {
                count(3);
                file->makeDirectory(args[2]);
            } else if (command == "boot"sv) {
                count(3);
                file->setBootTracks(readFile(args[2]));
            } else
                throw InvalidArgumentException("Unknown command; see --help"sv);
            file->save();
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "zxmedia: "sv << e.what() << '\n';
        return 1;
    }
}
