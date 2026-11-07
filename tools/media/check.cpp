// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "zxkit/media/MediaArchive.h"
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
using namespace zxkit;
using namespace std::literals;
int main(int argc, char **argv)
{
    try {
        if (argc != 4)
            return 2;
        std::ifstream input(argv[2], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        MediaOptions options{ .filesystem = argv[1] };
        if (options.filesystem == "auto"sv)
            options.filesystem.clear();
        auto source = MediaArchive::open(bytes, argv[3], options);
        std::map<std::string, std::vector<uint8_t>> originals;
        MediaEntry first;
        bool tape = false;
        for (const auto &entry : source.list()) {
            if (entry.directory && entry.name == "blocks"sv)
                tape = true;
            if (entry.directory || entry.name.starts_with("blocks/"sv)
                || entry.name.starts_with("basic/"sv))
                continue;
            originals[entry.name] = source.read(entry.name);
            if (first.name.empty())
                first = entry;
        }
        auto encoded = source.encode();
        auto edited = MediaArchive::open(encoded, argv[3], options);
        for (auto &[name, data] : originals)
            if (edited.read(name) != data)
                throw std::runtime_error("Native round trip changed an item"s);
        std::cout << "read="sv << originals.size() << " roundtrip=ok "sv;
        if (first.name.empty()) {
            const bool mdos = options.filesystem.starts_with("mdos"sv)
                    || (bytes.size() >= 720
                        && std::string_view(reinterpret_cast<const char *>(bytes.data() + 716), 4)
                                == "SDOS"sv);
            const std::string name = tape                       ? "files/ZXTEST"s
                    : mdos                                      ? "ZXTEST.B"s
                    : options.filesystem.starts_with("trdos"sv) ? "ZXTEST.C"s
                                                                : "ZXTEST"s;
            const std::vector<uint8_t> payload{ 1, 2, 3, 4, 0, 255 };
            edited.put(name, payload);
            auto fresh = MediaArchive::open(edited.encode(), argv[3], options);
            const auto read = fresh.read(name);
            if (read.size() < payload.size()
                || !std::equal(payload.begin(), payload.end(), read.begin()))
                throw std::runtime_error("Empty image add failed"s);
            fresh.remove(name);
            std::cout << "mutation=ok-empty\n"sv;
            return 0;
        }
        if (first.attributes.contains("readonly"sv))
            edited.setAttributes(first.name, { { "readonly"s, 0 } });
        auto changed = originals.at(first.name);
        if (!changed.empty())
            changed.back() ^= 0x5A;
        auto attrs = first.attributes;
        if (attrs.contains("readonly"sv))
            attrs["readonly"s] = 0;
        edited.put(first.name, changed, attrs);
        auto actual = edited.read(first.name);
        if (actual != changed)
            throw std::runtime_error("Update did not preserve the requested bytes"s);
        edited.put(first.name, originals.at(first.name), attrs);
        std::string testName = tape                         ? "files/ZXTEST"s
                : options.filesystem.starts_with("trdos"sv) ? "ZXTEST.C"s
                : options.filesystem.starts_with("mdos"sv)  ? "ZXTEST.B"s
                                                            : "ZXTEST"s;
        edited.rename(first.name, testName);
        if (edited.read(testName) != originals.at(first.name))
            throw std::runtime_error("Rename changed bytes"s);
        edited.remove(testName);
        edited.put(testName, std::vector<uint8_t>{ 1, 2, 3, 4, 0, 255 });
        auto added = edited.read(testName);
        const std::vector<uint8_t> expected{ 1, 2, 3, 4, 0, 255 };
        if (added.size() < expected.size()
            || !std::equal(expected.begin(), expected.end(), added.begin()))
            throw std::runtime_error("Add changed bytes"s);
        edited.remove(testName);
        for (auto &[name, data] : originals)
            if (name != first.name && edited.read(name) != data)
                throw std::runtime_error("Mutation changed a different item"s);
        auto final = MediaArchive::open(edited.encode(), argv[3], options);
        for (auto &[name, data] : originals)
            if (name != first.name && final.read(name) != data)
                throw std::runtime_error("Saved mutation changed a different item"s);
        std::cout << "mutation=ok\n"sv;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
