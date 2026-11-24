// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"
#include "Download.h"

#include <zxkit/machines/ZxNextMachine.h>
#include <zxkit/media/Compression.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <regex>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
constexpr auto defaultDistro = "https://www.specnext.com/distro/24.11/sn-emulator-24.11.zip"sv;
constexpr auto distroPage = "https://www.specnext.com/latestdistro/"sv;
constexpr auto imageName = "cspect-next-1gb.img"sv;

Object validateCard(const std::filesystem::path &path, bool requireNextZxOs = true)
{
    SdCard card;
    card.insert(path, true);
    FatVolume volume([&card](uint64_t lba, std::span<uint8_t, 512> bytes) {
        return card.readBlock(lba, bytes);
    });
    if (!requireNextZxOs)
        return { { "path", path.string() },
                 { "bytes", card.sizeBytes() },
                 { "fatBits", volume.fatBits() } };
    Array roms;
    for (auto [name, expected] :
         std::array{ std::pair{ "enNextZX.rom"sv, 65536 }, std::pair{ "enNxtmmc.rom"sv, 8192 },
                     std::pair{ "enAltZX.rom"sv, 32768 } }) {
        auto bytes = volume.readFile("/machines/next/"s + std::string(name));
        if (!bytes || bytes->size() != std::size_t(expected))
            throw std::runtime_error("SD image is missing valid firmware: "s + std::string(name));
        roms.push_back(Object{ { "name", name }, { "bytes", bytes->size() } });
    }
    return { { "path", path.string() },
             { "bytes", card.sizeBytes() },
             { "fatBits", volume.fatBits() },
             { "roms", roms } };
}

ZxNextMachine &nextMachine(Emulator &emulator)
{
    auto *machine = dynamic_cast<ZxNextMachine *>(&emulator.machine());
    if (!machine)
        throw std::invalid_argument("Session is not a ZX Spectrum Next"s);
    return *machine;
}
} // namespace

void Tools::checkSdSharing(const std::filesystem::path &image, bool readOnly,
                           std::string_view exceptSession) const
{
    auto canonical = std::filesystem::weakly_canonical(image);
    for (auto &[id, current] : m_sessions) {
        if (id == exceptSession)
            continue;
        auto &card = current.emulator->sdCard();
        if (card.isInserted() && std::filesystem::weakly_canonical(card.imagePath()) == canonical
            && (!readOnly || !card.isReadOnly()))
            throw std::invalid_argument(
                    "Writable SD images cannot be shared between sessions; use independent copies or read-only mounts"s);
    }
}

std::string Tools::nextPlatformInfo()
{
    return result([&] {
        auto image = outputPath("next/cspect-next-1gb.img"sv);
        return Object{ { "profile", "next" },
                       { "defaultImage", image.string() },
                       { "cached", std::filesystem::is_regular_file(image) },
                       { "testedDistributionUrl", defaultDistro },
                       { "latestDistributionPage", distroPage },
                       { "imageEntry", imageName },
                       { "boot", "Firmware is read from /machines/next on the SD image." } };
    });
}

std::string Tools::downloadFile(std::string url, std::string output, std::string expectedSha256,
                                int maxMilliseconds, bool overwrite)
{
    return result([&] {
        return download(url, outputPath(output), expectedSha256, maxMilliseconds, overwrite);
    });
}

std::string Tools::discoverNextDistribution(std::string pageUrl, int maxMilliseconds)
{
    return result([&] {
        if (pageUrl.empty())
            pageUrl = distroPage;
        auto page = outputPath("next/distribution.html"sv);
        auto info = download(pageUrl, page, {}, maxMilliseconds, true, 2 * 1024 * 1024);
        auto bytes = readFile(page);
        std::string html(bytes.begin(), bytes.end());
        const std::regex link(R"(href\s*=\s*["']([^"']*sn-emulator-[^"']*\.zip)["'])",
                              std::regex::icase);
        std::smatch match;
        if (!std::regex_search(html, match, link))
            throw std::runtime_error("Distribution page has no emulator image ZIP link"s);
        auto url = resolveUrl(std::string(info.at("url").as_string()), match[1].str());
        return Object{ { "page", info.at("url") }, { "url", url }, { "imageEntry", imageName } };
    });
}

std::string Tools::prepareNextSdCard(std::string url, std::string entryName, std::string output,
                                     int maxMilliseconds, bool overwrite)
{
    return result([&] {
        checked(maxMilliseconds, 1, 300000, "maxMilliseconds"sv);
        auto image = outputPath(output.empty() ? "next/cspect-next-1gb.img"sv
                                               : std::string_view(output));
        if (std::filesystem::is_regular_file(image) && !overwrite) {
            auto info = validateCard(image);
            info["cached"] = true;
            return info;
        }
        for (auto &[id, current] : m_sessions)
            if (current.emulator->sdCard().isInserted()
                && std::filesystem::weakly_canonical(current.emulator->sdCard().imagePath())
                        == std::filesystem::weakly_canonical(image))
                throw std::invalid_argument(
                        "Eject this image from every session before replacing it"s);
        if (url.empty())
            url = defaultDistro;
        if (entryName.empty())
            entryName = imageName;
        auto zip = image;
        zip += ".zip";
        Object downloadInfo;
        auto source = zip;
        source += ".source.json";
        bool cachedZip = false;
        if (!overwrite && std::filesystem::is_regular_file(zip)
            && std::filesystem::is_regular_file(source)) {
            auto bytes = readFile(source);
            auto metadata = boost::json::parse(
                    std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            cachedZip = metadata.at("sourceUrl").as_string() == url;
            if (cachedZip)
                downloadInfo = metadata.as_object();
        }
        auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMilliseconds);
        if (!cachedZip) {
            downloadInfo = download(url, zip, {}, maxMilliseconds, overwrite);
            downloadInfo["sourceUrl"] = url;
            auto text = boost::json::serialize(downloadInfo);
            writeFile(source,
                      std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), true);
        }
        auto temporary = image;
        temporary += ".extract-part";
        if (std::filesystem::exists(temporary))
            throw std::runtime_error(
                    "An extraction partial already exists; remove it before retrying"s);
        try {
            extractZipEntryToFile(zip, entryName, temporary, [&](uint64_t, uint64_t total) {
                return total <= UINT64_C(4) * 1024 * 1024 * 1024
                        && std::chrono::steady_clock::now() < deadline;
            });
            auto info = validateCard(temporary);
            auto backup = publishFile(temporary, image, overwrite);
            info["path"] = image.string();
            info["backup"] = backup.string();
            info["cached"] = false;
            info["download"] = downloadInfo;
            return info;
        } catch (...) {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            throw;
        }
    });
}

std::string Tools::createNextSession(std::string sessionId, std::string image, bool readOnly)
{
    return result([&] {
        if (sessionId.empty() || sessionId.size() > 128 || m_sessions.contains(sessionId)
            || m_sessions.size() >= 16)
            throw std::invalid_argument(
                    "Session ID must be unique and nonempty; maximum 16 sessions"s);
        auto path = image.empty() ? outputPath("next/cspect-next-1gb.img"sv)
                                  : std::filesystem::absolute(image);
        if (!std::filesystem::is_regular_file(path))
            throw std::invalid_argument(
                    "Prepare the Next SD image with prepareNextSdCard or supply an existing image"s);
        auto configuration = config("48k"s);
        validateCard(path, false);
        checkSdSharing(path, readOnly);
        // Attach the card before the Next's first reset so it can load its ROMs.
        // The initial 48K machine uses its embedded ROM.
        auto emulator = std::make_unique<Emulator>(std::move(configuration));
        emulator->insertSdCard(path, readOnly);
        emulator->selectMachine("next"sv);
        auto [iterator, inserted] =
                m_sessions.emplace(sessionId, Session{ sessionId, std::move(emulator), {}, {} });
        if (m_current.empty())
            m_current = sessionId;
        auto info = status(iterator->second).as_object();
        info["sdImage"] = path.string();
        info["sdReadOnly"] = readOnly;
        return info;
    });
}

std::string Tools::insertSdCard(std::string sessionId, std::string image, bool readOnly)
{
    return result([&] {
        auto &current = session(sessionId);
        auto path = std::filesystem::absolute(image);
        checkSdSharing(path, readOnly, current.id);
        current.emulator->insertSdCard(path, readOnly);
        return Object{ { "path", path.string() },
                       { "readOnly", readOnly },
                       { "resetRequired", true } };
    });
}

std::string Tools::ejectSdCard(std::string sessionId)
{
    return result([&] {
        session(sessionId).emulator->ejectSdCard();
        return Object{ { "ejected", true } };
    });
}

std::string Tools::readNextRegisters(std::string sessionId)
{
    return result([&] {
        auto &machine = session(sessionId).emulator->machine();
        if (machine.id() != "next"sv)
            throw std::invalid_argument("Session is not a ZX Spectrum Next"s);
        auto &registers = dynamic_cast<ZxNextMachine &>(machine).nextReg();
        Array values;
        for (int index = 0; index < 256; ++index)
            values.push_back(registers.peek(index));
        return Object{ { "selected", registers.selected() }, { "values", values } };
    });
}

std::string Tools::writeNextRegister(std::string sessionId, int index, int value)
{
    return result([&] {
        checked(index, 0, 255, "index"sv);
        checked(value, 0, 255, "value"sv);
        auto &machine = session(sessionId).emulator->machine();
        if (machine.id() != "next"sv)
            throw std::invalid_argument("Session is not a ZX Spectrum Next"s);
        dynamic_cast<ZxNextMachine &>(machine).nextReg().write(index, value);
        return Object{ { "index", index }, { "value", value } };
    });
}

std::string Tools::listSdFiles(std::string sessionId, std::string path)
{
    return result([&] {
        auto volume = session(sessionId).emulator->sdCardVolume();
        auto entries = volume.list(path);
        if (!entries)
            throw std::invalid_argument("SD directory does not exist"s);
        Array files;
        for (auto &entry : *entries)
            files.push_back(Object{ { "name", entry.name },
                                    { "size", entry.size },
                                    { "directory", entry.isDirectory },
                                    { "attributes", entry.attributes } });
        return Object{ { "entries", files },
                       { "writable", volume.isWritable() },
                       { "fatBits", volume.fatBits() } };
    });
}

std::string Tools::readSdFile(std::string sessionId, std::string path, int offset, int length,
                              bool asText)
{
    return result([&] {
        auto volume = session(sessionId).emulator->sdCardVolume();
        auto slash = path.find_last_of("/\\");
        auto parent = slash == path.npos ? "/"s : path.substr(0, slash + 1);
        auto name = slash == path.npos ? path : path.substr(slash + 1);
        auto entries = volume.list(parent);
        if (!entries)
            throw std::invalid_argument("SD parent directory does not exist"s);
        auto found = std::ranges::find_if(
                *entries, [&](const auto &entry) { return upper(entry.name) == upper(name); });
        if (found == entries->end() || found->isDirectory)
            throw std::invalid_argument("SD file does not exist"s);
        if (found->size > 16 * 1024 * 1024)
            throw std::invalid_argument("Inline SD reads are limited to files up to 16 MiB"s);
        auto bytes = volume.readFile(path);
        if (!bytes)
            throw std::runtime_error("Cannot read SD file"s);
        checked(offset, 0, int(bytes->size()), "offset"sv);
        int remaining = int(bytes->size()) - offset;
        checked(length, 0, std::min(remaining, 65536), "length"sv);
        if (!length)
            length = std::min(remaining, 65536);
        auto part = std::span(*bytes).subspan(offset, length);
        Object info{ { "offset", offset },
                     { "length", length },
                     { "totalBytes", bytes->size() },
                     { "truncated", length < remaining } };
        if (asText)
            info["text"] = std::string(part.begin(), part.end());
        else
            info["hex"] = hex(part);
        return info;
    });
}

std::string Tools::writeSdFile(std::string sessionId, std::string path, std::string hexData)
{
    return result([&] {
        auto bytes = unhex(hexData);
        session(sessionId).emulator->sdCardVolume().writeFile(path, bytes);
        return Object{ { "written", bytes.size() }, { "resetRequired", true } };
    });
}

std::string Tools::makeSdDirectory(std::string sessionId, std::string path)
{
    return result([&] {
        session(sessionId).emulator->sdCardVolume().makeDirectory(path);
        return Object{ { "created", true }, { "resetRequired", true } };
    });
}

std::string Tools::removeSdFile(std::string sessionId, std::string path)
{
    return result([&] {
        session(sessionId).emulator->sdCardVolume().remove(path);
        return Object{ { "removed", true }, { "resetRequired", true } };
    });
}

std::string Tools::renameSdFile(std::string sessionId, std::string path, std::string destination)
{
    return result([&] {
        session(sessionId).emulator->sdCardVolume().rename(path, destination);
        return Object{ { "renamed", true }, { "resetRequired", true } };
    });
}

std::string Tools::readNextHardware(std::string sessionId, bool includeSpritePatterns)
{
    return result([&] {
        auto &machine = nextMachine(*session(sessionId).emulator);
        const auto &video = machine.nextVideo();
        const auto &copper = machine.copper();
        const auto &registers = machine.nextReg();
        Array sprites;
        for (std::size_t index = 0; index < NextVideo::kSpriteCount; ++index)
            sprites.emplace_back(hex(video.spriteAttributes(index)));

        Array palettes;
        constexpr std::array layers{ "ula", "layer2", "sprites", "tilemap" };
        for (std::size_t index = 0; index < layers.size() * 2; ++index) {
            Array colors;
            for (auto color : video.paletteColors(index))
                colors.emplace_back(color);
            palettes.emplace_back(Object{ { "layer", layers[index % layers.size()] },
                                          { "alternate", index >= layers.size() },
                                          { "rgb333", std::move(colors) },
                                          { "priorityHex", hex(video.palettePriorities(index)) } });
        }
        Object clipWindows;
        for (std::size_t layer = 0; layer < layers.size(); ++layer) {
            constexpr std::array order{ NextVideo::ClipUla, NextVideo::ClipLayer2,
                                        NextVideo::ClipSprites, NextVideo::ClipTilemap };
            Array bounds;
            for (auto coordinate : video.clipWindow(order[layer]))
                bounds.emplace_back(coordinate);
            clipWindows[layers[layer]] = std::move(bounds);
        }
        Array ctc;
        for (int channel = 0; channel < 4; ++channel)
            ctc.emplace_back(machine.ctc().readChannel(channel));
        Object output{
            { "copper",
              Object{ { "pc", copper.pc() },
                      { "mode", copper.mode() },
                      { "halted", copper.isHalted() },
                      { "instructionRamHex", hex(copper.instructionRam()) } } },
            { "dmaActive", machine.dma().isActive() },
            { "ctcCounters", std::move(ctc) },
            { "layer2",
              Object{ { "controlPort", video.layer2ControlPort() },
                      { "visible", video.isLayer2Visible() },
                      { "readMapped", video.layer2ReadMapped() },
                      { "writeMapped", video.layer2WriteMapped() },
                      { "usesShadowBank", video.layer2UsesShadowBank() },
                      { "segment", video.layer2Segment() },
                      { "bankOffset", video.layer2BankOffset() },
                      { "bank", registers.peek(0x12) },
                      { "shadowBank", registers.peek(0x13) },
                      { "xScroll",
                        unsigned(registers.peek(0x16)) | ((registers.peek(0x71) & 1u) << 8) },
                      { "yScroll", registers.peek(0x17) },
                      { "control", registers.peek(0x70) } } },
            { "tilemap",
              Object{ { "control", registers.peek(0x6b) },
                      { "mapBaseRegister", registers.peek(0x6e) },
                      { "patternBaseRegister", registers.peek(0x6f) } } },
            { "timexMode", video.timexPort() & 7 },
            { "layerControl", registers.peek(0x15) },
            { "clipWindows", std::move(clipWindows) },
            { "spriteSelected", video.spriteNextRegSelection() },
            { "spriteAttributesHex", std::move(sprites) },
            { "palettes", std::move(palettes) }
        };
        if (includeSpritePatterns)
            output["spritePatternHex"] = hex(video.spritePatternRam());
        return output;
    });
}

std::string Tools::sendNextUart(std::string sessionId, int channel, std::string hexData)
{
    return result([&] {
        checked(channel, 0, 1, "channel"sv);
        auto &emulator = *session(sessionId).emulator;
        auto &machine = nextMachine(emulator);
        const auto bytes = unhex(hexData);
        const auto accepted =
                machine.uart().receive(channel, bytes, machine.uartTime(emulator.context()));
        return Object{ { "channel", channel },
                       { "accepted", accepted },
                       { "requested", bytes.size() } };
    });
}

std::string Tools::captureNextUart(std::string sessionId, int frameCount, int maxMilliseconds,
                                   int maxBytes)
{
    return result([&] {
        checked(maxBytes, 1, 65536, "maxBytes"sv);
        auto &current = session(sessionId);
        auto &uart = nextMachine(*current.emulator).uart();
        std::array<Bytes, 2> bytes;
        std::array<std::size_t, 2> totals{};
        for (auto &channel : bytes)
            channel.reserve(maxBytes);
        struct Disconnect
        {
            NextUart &uart;
            ~Disconnect()
            {
                uart.setTransmitHandler({});
            }
        } disconnect{ uart };
        uart.setTransmitHandler([&](unsigned channel, uint8_t byte) {
            ++totals[channel];
            if (bytes[channel].size() < std::size_t(maxBytes))
                bytes[channel].push_back(byte);
        });
        auto output = execute(current, frameCount, maxMilliseconds).as_object();
        Array channels;
        for (std::size_t channel = 0; channel < bytes.size(); ++channel) {
            channels.emplace_back(
                    Object{ { "channel", channel },
                            { "hex", hex(bytes[channel]) },
                            { "totalBytes", totals[channel] },
                            { "truncated", totals[channel] > bytes[channel].size() } });
        }
        output["channels"] = std::move(channels);
        return output;
    });
}
} // namespace zxkit::mcp_tool
