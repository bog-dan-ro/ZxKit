// Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tools.h"
#include "../media/MediaFile.h"

#include <zxkit/peripherals/expansion/ZxPrinter.h>
#include <zxkit/peripherals/storage/Interface1.h>
#include <algorithm>
#include <stdexcept>

namespace zxkit::mcp_tool {
using namespace std::literals;
namespace {
void checkMediaChange(Emulator &emulator)
{
    if (emulator.isRzxSeeking() || emulator.rzx().isPlaying() || emulator.rzx().isRecording()
        || emulator.rzx().isReviewing())
        throw std::invalid_argument(
                "Finish RZX playback or recording before changing mounted media"s);
}

DiskImage &floppy(Emulator &emulator, int drive)
{
    checked(drive, 0, int(emulator.diskDriveCount()) - 1, "drive"sv);
    return emulator.diskDrive(drive)->get();
}

Interface1Peripheral &interface1(Emulator &emulator)
{
    if (auto device = emulator.interface1())
        return device->get();
    throw std::invalid_argument("Attach ZX Interface 1 before using Microdrive cartridges"s);
}

ZxPrinterPeripheral &printer(Emulator &emulator)
{
    auto device = emulator.findPeripheral(ZxPrinterPeripheral::kId);
    if (!device || !device->get().supportsMachine(emulator.machine()))
        throw std::invalid_argument("Attach the emulated printer on a supported machine first"s);
    return static_cast<ZxPrinterPeripheral &>(device->get());
}

void checkDiscard(bool modified, bool discardChanges)
{
    if (modified && !discardChanges)
        throw std::invalid_argument(
                "The mounted medium has changes. Set discardChanges=true to replace or eject it"s);
}
} // namespace

std::string Tools::listMountedMedia(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        const auto activity = emulator.mediaActivity();
        Array floppies;
        for (std::size_t drive = 0; drive < emulator.diskDriveCount(); ++drive) {
            const auto &disk = emulator.diskDrive(drive)->get();
            floppies.emplace_back(Object{ { "drive", drive },
                                          { "inserted", !disk.empty() },
                                          { "tracks", disk.trackCount() },
                                          { "sides", disk.sideCount() },
                                          { "modified", disk.isModified() },
                                          { "writeProtected", disk.isWriteProtected() },
                                          { "busy", (activity.busyDisks & (1u << drive)) != 0 },
                                          { "accessCount", disk.accessCount() } });
        }
        Array cartridges;
        const auto device = emulator.interface1();
        if (device) {
            for (std::size_t drive = 0; drive < Interface1Peripheral::kMaxCartridges; ++drive) {
                const auto &interface = device->get();
                cartridges.emplace_back(
                        Object{ { "drive", drive },
                                { "inserted", interface.hasCartridge(drive) },
                                { "modified", interface.cartridgeModified(drive) },
                                { "writeProtected", interface.cartridgeWriteProtected(drive) } });
            }
        }
        return Object{ { "floppies", std::move(floppies) },
                       { "interface1Attached", device.has_value() },
                       { "microdrives", std::move(cartridges) },
                       { "sdCardBusy", activity.sdCardBusy } };
    });
}

std::string Tools::insertFloppy(std::string sessionId, int drive, std::string path,
                                bool writeProtected, bool discardChanges)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        checkMediaChange(emulator);
        auto &disk = floppy(emulator, drive);
        checkDiscard(disk.isModified(), discardChanges);
        const auto source = MediaFile::fromBytes(readFile(path),
                                                 std::filesystem::path(path).extension().string());
        DiskImage replacement;
        replacement.loadFromBytes(source.data, source.extension);
        replacement.setWriteProtected(writeProtected);
        disk = std::move(replacement);
        return Object{ { "drive", drive },
                       { "tracks", disk.trackCount() },
                       { "sides", disk.sideCount() },
                       { "writeProtected", writeProtected } };
    });
}

std::string Tools::ejectFloppy(std::string sessionId, int drive, bool discardChanges)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        checkMediaChange(emulator);
        auto &disk = floppy(emulator, drive);
        checkDiscard(disk.isModified(), discardChanges);
        emulator.ejectDisk(drive);
        return Object{ { "drive", drive }, { "inserted", false } };
    });
}

std::string Tools::autoBootDisk(std::string sessionId)
{
    return result([&] {
        auto &current = session(sessionId);
        checkMediaChange(*current.emulator);
        if (!current.emulator->autoBootDisk())
            throw std::invalid_argument(
                    "Autoboot requires a disk in drive A on a supported machine"s);
        current.lastPause.reset();
        return status(current);
    });
}

std::string Tools::readFloppyTrack(std::string sessionId, int drive, int track, int side,
                                   bool includeData)
{
    return result([&] {
        const auto &disk = floppy(*session(sessionId).emulator, drive);
        checked(track, 0, int(disk.trackCount()) - 1, "track"sv);
        checked(side, 0, int(disk.sideCount()) - 1, "side"sv);
        Array sectors;
        std::size_t remaining = 64 * 1024;
        bool truncated = false;
        for (const DiskImage::Sector &sector : disk.trackSectors(track, side)) {
            Object entry{ { "c", sector.track },          { "h", sector.side },
                          { "r", sector.sectorId },       { "n", sector.sizeCode },
                          { "st1", sector.st1 },          { "st2", sector.st2 },
                          { "mfm", sector.mfm },          { "reads", sector.reads },
                          { "bytes", sector.data.size() } };
            if (includeData) {
                const auto size = std::min(remaining, sector.data.size());
                entry["hex"] = hex(std::span(sector.data).first(size));
                entry["truncated"] = size != sector.data.size();
                truncated |= size != sector.data.size();
                remaining -= size;
            }
            sectors.emplace_back(std::move(entry));
        }
        return Object{ { "drive", drive },
                       { "track", track },
                       { "side", side },
                       { "sectors", std::move(sectors) },
                       { "truncated", truncated } };
    });
}

std::string Tools::attachInterface1(std::string sessionId, std::string romPath)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        checkMediaChange(emulator);
        if (!emulator.interface1()) {
            auto rom = romPath.empty() ? Bytes{} : readFile(romPath);
            if (!romPath.empty() && rom.empty())
                throw std::invalid_argument("Interface 1 ROM is empty"s);
            emulator.attachPeripheral(std::make_unique<Interface1Peripheral>(std::move(rom)));
        }
        return Object{ { "attached", true } };
    });
}

std::string Tools::insertMicrodrive(std::string sessionId, int drive, std::string path,
                                    bool discardChanges)
{
    return result([&] {
        checked(drive, 0, Interface1Peripheral::kMaxCartridges - 1, "drive"sv);
        auto &emulator = *session(sessionId).emulator;
        checkMediaChange(emulator);
        auto &interface = interface1(emulator);
        checkDiscard(interface.cartridgeModified(drive), discardChanges);
        const auto source = MediaFile::fromBytes(readFile(path),
                                                 std::filesystem::path(path).extension().string());
        if (source.extension != ".mdr"sv)
            throw std::invalid_argument("Microdrive cartridges require an MDR image"s);
        interface.insertCartridge(drive, source.data);
        return Object{ { "drive", drive }, { "inserted", true } };
    });
}

std::string Tools::ejectMicrodrive(std::string sessionId, int drive, bool discardChanges)
{
    return result([&] {
        checked(drive, 0, Interface1Peripheral::kMaxCartridges - 1, "drive"sv);
        auto &emulator = *session(sessionId).emulator;
        checkMediaChange(emulator);
        auto &interface = interface1(emulator);
        checkDiscard(interface.cartridgeModified(drive), discardChanges);
        interface.ejectCartridge(drive);
        return Object{ { "drive", drive }, { "inserted", false } };
    });
}

std::string Tools::saveMicrodrive(std::string sessionId, int drive, std::string path,
                                  bool overwrite)
{
    return result([&] {
        checked(drive, 0, Interface1Peripheral::kMaxCartridges - 1, "drive"sv);
        auto &interface = interface1(*session(sessionId).emulator);
        const auto output = outputPath(path);
        if (upper(output.extension().string()) != ".MDR"sv)
            throw std::invalid_argument("Microdrive output must use the .mdr extension"s);
        const auto bytes = interface.saveCartridge(drive);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        const zxkit::tool::ImageLock lock(zxkit::tool::imagePath(output));
        zxkit::tool::atomicWrite(output, bytes, overwrite);
        interface.markCartridgeSaved(drive, bytes);
        return Object{ { "path", output.string() }, { "bytes", bytes.size() } };
    });
}

std::string Tools::attachPrinter(std::string sessionId)
{
    return result([&] {
        auto &emulator = *session(sessionId).emulator;
        if (!ZxPrinterPeripheral::availableOn(emulator.machine()))
            throw std::invalid_argument("This machine has no supported printer interface"s);
        if (!emulator.findPeripheral(ZxPrinterPeripheral::kId)) {
            checkMediaChange(emulator);
            emulator.attachPeripheral(std::make_unique<ZxPrinterPeripheral>());
        }
        return Object{ { "attached", true } };
    });
}

std::string Tools::capturePrinter(std::string sessionId, std::string path, bool overwrite)
{
    return result([&] {
        const auto &paper = printer(*session(sessionId).emulator).paper();
        Array lines;
        for (const auto &line : paper.textLines()) {
            Array unknown;
            for (std::size_t column = 0; column < line.characters.size(); ++column) {
                if (!line.characters[column])
                    unknown.emplace_back(column);
            }
            lines.emplace_back(
                    Object{ { "row", line.row }, { "unknownColumns", std::move(unknown) } });
        }
        Object output{ { "text", paper.text() },
                       { "rows", paper.rows().size() },
                       { "textLines", std::move(lines) },
                       { "truncated", paper.truncated() } };
        if (!path.empty()) {
            const auto destination = outputPath(path);
            if (upper(destination.extension().string()) != ".PNG"sv)
                throw std::invalid_argument("Printer image output must use the .png extension"s);
            const auto bytes = paper.toPng();
            if (bytes.empty())
                throw std::invalid_argument("Printer paper is empty"s);
            writeFile(destination, bytes, overwrite);
            output["path"] = destination.string();
            output["bytes"] = bytes.size();
        }
        return output;
    });
}

std::string Tools::clearPrinter(std::string sessionId)
{
    return result([&] {
        printer(*session(sessionId).emulator).clearPaper();
        return Object{ { "cleared", true } };
    });
}
} // namespace zxkit::mcp_tool
