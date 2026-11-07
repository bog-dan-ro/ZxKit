// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "MediaFile.h"
#include "zxkit/media/BasicCodec.h"
#include <iostream>
#include <charconv>
#include <cstdlib>
#ifdef _WIN32
#include <io.h>
#include <string>
#include <string_view>
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
void listing(const MediaArchive &media, bool mc)
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
void help()
{
    std::cout << R"(zxmedia [--filesystem PROFILE] [--dialect 48|128|next] [--force] COMMAND ...
  profiles                         List disk filesystem/geometry profiles
  create IMAGE [--boot-tracks FILE] Create a blank image (or install boot tracks)
  list IMAGE                       List files, native attributes and components
  get IMAGE ENTRY OUTPUT           Extract a file ('-' means standard output)
  put IMAGE INPUT ENTRY [key=value ...]  Add or replace a file
  rm IMAGE ENTRY                   Remove a file
  rename IMAGE ENTRY NEWNAME        Rename a file
  attrs IMAGE ENTRY key=value ...   Change native attributes
  mkdir IMAGE DIRECTORY            Create a FAT directory
  boot IMAGE BOOT_TRACKS           Install exactly the reserved CP/M tracks
  convert INPUT OUTPUT             Convert compatible containers/tapes; TAP -> WAV
  copy INPUT ENTRY OUTPUT NEWNAME [key=value ...]  Transfer bytes between images
  basic compile INPUT OUTPUT       Tokenize numbered BASIC text
  basic decompile INPUT OUTPUT     Lossless editable BASIC listing and stored data
  basic array-compile INPUT OUTPUT  Array text -> data (use --array-type numeric|string)
  basic array-decompile INPUT OUTPUT  Array data -> editable text
  mc COMMAND IMAGE [ENTRY [HOST]]   Midnight Commander extfs protocol
Use --filesystem for ambiguous raw/TD0/CQM images. Existing outputs require --force.
Tape files live under files/NAME; editable listings under basic/NAME.bas;
raw tape blocks under blocks/000000.tzx. NEX exposes header.bin and banks/NNN.bin.
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
        std::vector<std::string> args;
        if (const char *p = std::getenv("ZXMEDIA_FILESYSTEM"))
            opts.filesystem = p;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a == "--filesystem"sv || a == "--dialect"sv || a == "--label"sv
                || a == "--array-type"sv) {
                if (++i == argc)
                    throw InvalidArgumentException("Option requires a value"sv);
                if (a == "--filesystem"sv)
                    opts.filesystem = argv[i];
                else if (a == "--label"sv)
                    opts.label = argv[i];
                else if (a == "--array-type"sv) {
                    std::string_view t = argv[i];
                    if (t != "numeric"sv && t != "string"sv)
                        throw InvalidArgumentException("Unknown array type"sv);
                    characterArray = t == "string"sv;
                } else {
                    std::string_view d = argv[i];
                    if (d == "48"sv)
                        dialect = BasicDialect::Spectrum48;
                    else if (d == "128"sv)
                        dialect = BasicDialect::Spectrum128;
                    else if (d == "next"sv)
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
        auto open = [&](const fs::path &p) {
            auto b = readFile(p);
            return MediaArchive::open(b, p.extension().string(), opts);
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
            output(path, media.encode(), force);
        } else if (command == "list"sv) {
            count(2);
            listing(open(args[1]), false);
        } else if (command == "get"sv) {
            count(4);
            output(args[3], open(args[1]).read(args[2]), force);
        } else if (command == "convert"sv) {
            count(3);
            fs::path out = args[2];
            output(out, open(args[1]).encode(out.extension().string()), force);
        } else if (command == "basic"sv) {
            count(4);
            auto bytes = readFile(args[2]);
            Bytes out;
            if (args[1] == "compile"sv)
                out = BasicCodec::compile(
                        std::string_view(reinterpret_cast<const char *>(bytes.data()),
                                         bytes.size()),
                        dialect);
            else if (args[1] == "array-compile"sv)
                out = BasicCodec::compileArray(
                        std::string_view(reinterpret_cast<const char *>(bytes.data()),
                                         bytes.size()),
                        characterArray);
            else if (args[1] == "array-decompile"sv) {
                auto s = BasicCodec::decompileArray(bytes, characterArray);
                out.assign(s.begin(), s.end());
            } else if (args[1] == "decompile"sv) {
                auto s = BasicCodec::decompile(bytes, dialect);
                out.assign(s.begin(), s.end());
            } else
                throw InvalidArgumentException("Use basic compile or basic decompile"sv);
            output(args[3], out, force);
        } else if (command == "copy"sv) {
            if (args.size() < 5)
                throw InvalidArgumentException("copy INPUT ENTRY OUTPUT NEWNAME [attributes]"sv);
            auto data = open(args[1]).read(args[2]);
            MediaFile file(args[3], opts);
            file.archive.put(args[4], data, attributes(args, 5));
            file.save();
        } else if (command == "mc"sv) {
            if (args.size() < 3)
                throw InvalidArgumentException("Invalid extfs command"sv);
            auto op = args[1];
            if (op == "list"sv) {
                count(3);
                listing(open(args[2]), true);
            } else if (op == "copyout"sv) {
                count(5);
                output(args[4], open(args[2]).read(args[3]), true);
            } else {
                MediaFile file(args[2], opts);
                if (op == "copyin"sv) {
                    count(5);
                    file.archive.put(args[3], readFile(args[4]));
                } else if (op == "rm"sv || op == "rmdir"sv) {
                    count(4);
                    file.archive.remove(args[3]);
                } else if (op == "mkdir"sv) {
                    count(4);
                    file.archive.makeDirectory(args[3]);
                } else
                    throw InvalidArgumentException("Unsupported extfs operation"sv);
                file.save();
            }
        } else {
            if (args.size() < 2)
                throw InvalidArgumentException("Image path required"sv);
            MediaFile file(args[1], opts);
            if (command == "put"sv) {
                if (args.size() < 4)
                    throw InvalidArgumentException("put IMAGE INPUT ENTRY [attributes]"sv);
                file.archive.put(args[3], readFile(args[2]), attributes(args, 4));
            } else if (command == "rm"sv) {
                count(3);
                file.archive.remove(args[2]);
            } else if (command == "rename"sv) {
                count(4);
                file.archive.rename(args[2], args[3], force);
            } else if (command == "attrs"sv) {
                if (args.size() < 4)
                    throw InvalidArgumentException("attrs IMAGE ENTRY key=value"sv);
                file.archive.setAttributes(args[2], attributes(args, 3));
            } else if (command == "mkdir"sv) {
                count(3);
                file.archive.makeDirectory(args[2]);
            } else if (command == "boot"sv) {
                count(3);
                file.archive.setBootTracks(readFile(args[2]));
            } else
                throw InvalidArgumentException("Unknown command; see --help"sv);
            file.save();
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "zxmedia: "sv << e.what() << '\n';
        return 1;
    }
}
