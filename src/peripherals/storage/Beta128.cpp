/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "zxkit/peripherals/storage/Beta128.h"

#include "zxkit/cpu/Z80Cpu.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

bool insertTrDosBootLoader(DiskImage &disk)
{
    const auto spec = disk.findSector(0, 0, 9);
    if (!spec || spec->get().data.size() < 256)
        return false;
    auto &info = spec->get().data;
    if (info[0] != 0 || info[0xE7] != 0x10 || info[0xE4] >= 128)
        return false;

    // The catalog: 16 entries of 16 bytes in sectors 1-8; 0 ends it, 1 is a deleted file
    std::array<uint8_t, 8> firstBasic{};
    bool hasBasic = false;
    for (uint8_t i = 0; i < info[0xE4]; ++i) {
        const auto sector = disk.findSector(0, 0, static_cast<uint8_t>(1 + i / 16));
        if (!sector)
            return false;
        const auto entry = std::span(sector->get().data).subspan((i % 16) * 16, 16);
        if (entry[0] <= 0x01 || entry[8] != 'B')
            continue;
        if (std::ranges::equal(entry.first(8), std::string_view("boot    ")))
            return false; // it has one
        if (!hasBasic) {
            std::ranges::copy(entry.first(8), firstBasic.begin());
            hasBasic = true;
        }
    }
    const unsigned freeSectors = info[0xE5] | (info[0xE6] << 8);
    if (!hasBasic || freeSectors == 0)
        return false;

    // 1 RANDOMIZE USR 15619: REM : RUN "name", then TR-DOS's end of program and autostart line 1
    std::vector<uint8_t> program{0x00, 0x01, 0x1C, 0x00, 0xF9, 0xC0, 0x31, 0x35, 0x36, 0x31, 0x39, 0x0E,
                                 0x00, 0x00, 0x03, 0x3D, 0x00, 0x3A, 0xEA, 0x3A, 0xF7, 0x22};
    program.insert(program.end(), firstBasic.begin(), firstBasic.end());
    program.insert(program.end(), {0x22, 0x0D});
    const auto length = static_cast<uint16_t>(program.size());
    program.insert(program.end(), {0x80, 0xAA, 0x01, 0x00});

    const uint8_t startSector = info[0xE1];
    const uint8_t startTrack = info[0xE2];
    const bool doubleSided = disk.sideCount() > 1;
    const auto data = disk.findSector(static_cast<uint8_t>(doubleSided ? startTrack / 2 : startTrack),
                                      static_cast<uint8_t>(doubleSided ? startTrack % 2 : 0),
                                      static_cast<uint8_t>(startSector + 1));
    const auto catalog = disk.findSector(0, 0, static_cast<uint8_t>(1 + info[0xE4] / 16));
    if (!data || !catalog)
        return false;
    std::ranges::fill(data->get().data, 0);
    std::ranges::copy(program, data->get().data.begin());

    const auto entry = std::span(catalog->get().data).subspan((info[0xE4] % 16) * 16, 16);
    std::ranges::copy(std::string_view("boot    B"), entry.begin());
    entry[9] = entry[11] = static_cast<uint8_t>(length);
    entry[10] = entry[12] = static_cast<uint8_t>(length >> 8);
    entry[13] = 1; // sectors
    entry[14] = startSector;
    entry[15] = startTrack;

    info[0xE4] = static_cast<uint8_t>(info[0xE4] + 1);
    const unsigned left = freeSectors - 1;
    info[0xE5] = static_cast<uint8_t>(left);
    info[0xE6] = static_cast<uint8_t>(left >> 8);
    info[0xE1] = static_cast<uint8_t>((startSector + 1) % 16);
    info[0xE2] = static_cast<uint8_t>(startSector == 15 ? startTrack + 1 : startTrack);
    return true;
}

void Beta128Peripheral::onAttach(MachineContext &ctx)
{
    if (m_options.machineRomBank) {
        m_romBank = *m_options.machineRomBank;
        m_romPage = m_options.machineRomPage;
    } else {
        m_romBank = ctx.mmu.registerBank("beta128_rom", 16 * 1024, false, false);
        m_romPage = 0;
        if (ctx.romResolver || ctx.romOverrideResolver)
            ctx.mmu.bank(m_romBank).loadData(0, ctx.loadRom("trdos.rom", 16 * 1024));
    }

    ctx.romcs.registerProvider(std::string(id()), 30, [this](Mmu &mmu) {
        mmu.mapOverlay1k(0x0000, m_romBank, m_romPage * 16, 16, false);
    });

    // FUSE's rule: TR-DOS pages in when an instruction is fetched at 0x3Dxx, and out when one
    // is fetched above the ROM, only while the 48K BASIC ROM is selected
    // (the hook watches only the pages that can change the paging: see setPaged())
    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (m_paged) {
            if (pc >= 0x4000 && basicRomSelected())
                setPaged(ctx, false);
        } else if ((pc & 0xFF00) == 0x3D00 && basicRomSelected()) {
            setPaged(ctx, true);
        }
    }, std::string(id()), hookPages(false));

    // The WD1793 registers (0x1F, 0x3F, 0x5F, 0x7F) and the system port 0xFF. While TR-DOS is
    // paged in they own these ports (as on the Pentagon, and in Unreal Speccy): the Kempston
    // joystick's 0x1F doesn't answer
    ctx.ioBus.registerHandler(
        std::string(id()),
        0x001F,
        0x001F,
        [this, &ctx](uint16_t port) -> IoReadResult {
            if (!m_paged)
                return {0xFF, 0x00};
            m_fdc.update(ctx.clock.totalTstates());
            switch (port & 0xFF) {
            case 0x1F: return {m_fdc.readStatus(ctx.clock.totalTstates()), 0xFF, true};
            case 0x3F: return {m_fdc.readTrack(), 0xFF, true};
            case 0x5F: return {m_fdc.readSector(), 0xFF, true};
            case 0x7F: return {m_fdc.readData(), 0xFF, true};
            case 0xFF:
                return {static_cast<uint8_t>((m_fdc.intrq() ? 0x80 : 0x00) | (m_fdc.drq() ? 0x40 : 0x00)), 0xFF, true};
            default: return {0xFF, 0x00};
            }
        },
        [this, &ctx](uint16_t port, uint8_t value) {
            if (!m_paged)
                return;
            m_fdc.update(ctx.clock.totalTstates());
            switch (port & 0xFF) {
            case 0x1F: m_fdc.writeCommand(value, ctx.clock.totalTstates()); break;
            case 0x3F: m_fdc.writeTrack(value); break;
            case 0x5F: m_fdc.writeSector(value); break;
            case 0x7F: m_fdc.writeData(value); break;
            case 0xFF: writeSystem(value); break;
            default: break;
            }
        },
        10); // before the Kempston joystick: TR-DOS's ports hide it
}

void Beta128Peripheral::onDetach(MachineContext &ctx)
{
    m_paged = false;
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(id());
}

void Beta128Peripheral::reset(MachineContext &ctx, ResetKind /*kind*/)
{
    m_fdc.reset();
    m_fdc.setClockHz(ctx.clock.timings().baseClockHz * ctx.clock.speedMultiplier());
    writeSystem(0x3C);
    setPaged(ctx, m_options.pagedAtReset);
}

OptionalRef<DiskImage> Beta128Peripheral::diskDrive(std::size_t idx) noexcept
{
    if (idx >= Wd1793::kDriveCount)
        return std::nullopt;
    return m_fdc.drive(idx);
}

Z80Cpu::PcPages Beta128Peripheral::hookPages(bool paged) noexcept
{
    // Paged out: an instruction fetched at 0x3Dxx pages TR-DOS in. Paged in: one fetched at
    // 0x4000 or above pages it out
    Z80Cpu::PcPages pages;
    if (paged) {
        for (std::size_t page = 0x40; page < 0x100; ++page)
            pages.set(page);
    } else {
        pages.set(0x3D);
    }
    return pages;
}

void Beta128Peripheral::setPaged(MachineContext &ctx, bool paged)
{
    if (m_paged != paged)
        ctx.cpu.setPreM1HookPages(id(), hookPages(paged));
    m_paged = paged;
    ctx.romcs.setActive(id(), paged);
}

void Beta128Peripheral::writeSystem(uint8_t value) noexcept
{
    // Bits 0-1 the drive, bit 2 low resets the controller, bit 4 the side (0: the second one)
    if (!(value & 0x04) && (m_system & 0x04))
        m_fdc.reset();
    m_fdc.selectDrive(value & 0x03);
    m_fdc.selectSide((value & 0x10) ? 0 : 1);
    m_system = value;
}

} // namespace zxkit
