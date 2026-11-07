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

#include "zxkit/machines/ZxNextMachine.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/machines/SinclairMachines.h"
#include "zxkit/media/TapeDeck.h"
#include "zxkit/peripherals/storage/FatVolume.h"
#include "zxkit/peripherals/storage/SdCard.h"

#include <array>
#include <format>
#include <optional>
#include <string_view>

namespace zxkit {

using namespace std::literals;

namespace {

constexpr auto kDivMmcId = "next_divmmc"sv;

// MMU page values >= 0xE0 are ROM in slots 0/1 and unmapped in slots 2..7
constexpr uint8_t kFirstNonRamPage = 0xE0;

} // namespace

std::string_view ZxNextMachine::id() const noexcept
{
    return "next"sv;
}

std::string_view ZxNextMachine::name() const noexcept
{
    return "ZX Spectrum Next"sv;
}

FrameTimings ZxNextMachine::timings() const noexcept
{
    return kTimings128k;
}

ContentionPattern ZxNextMachine::contentionPattern() const noexcept
{
    return ContentionPattern::ZxNext;
}

bool ZxNextMachine::hasFloatingBus() const noexcept
{
    return false;
}

bool ZxNextMachine::supportsZ80n() const noexcept
{
    return true;
}

void ZxNextMachine::initialize(MachineContext &ctx, UlaRenderer &ula)
{

    // NextZXOS ROM 0..3 (64KB) + 2MB SRAM model: 224 x 8K Next RAM pages
    m_romBank = ctx.mmu.registerBank("rom", 4 * 16 * 1024, false, false);
    m_ramBank = ctx.mmu.registerBank("ram", 224 * 8 * 1024, true, true);
    m_altRomBank = ctx.mmu.registerBank("altrom", 2 * 16 * 1024, true, true);
    m_divRomBank = ctx.mmu.registerBank("divmmc_rom", 8 * 1024, false, false);
    m_divRamBank = ctx.mmu.registerBank("divmmc_ram", 16 * 8 * 1024, true, true);
    m_mfRomBank = ctx.mmu.registerBank("next_mf_rom", 8 * 1024, false, false);
    m_mfRamBank = ctx.mmu.registerBank("next_mf_ram", 8 * 1024, true, true);
    // Scoped host overrides can change between hard resets. Those load below,
    // once per reset; the traditional no-override path keeps its existing policy.
    if (!ctx.romOverrideResolver) loadRoms(ctx);

    ctx.cpu.setZ80nEnabled(true);
    ctx.audioMixer.setAyChipCount(3);
    // Keep the classic machine's hot keyboard path unchanged. Next extended
    // keys add their cached matrix only when NR68 bit 4 allows it.
    ctx.ioBus.registerHandler(
        "next_ula", 1, 0,
        [this, &ctx](uint16_t port) -> IoReadResult {
            const auto row = uint8_t(port >> 8);
            uint8_t keys = ctx.keyboard.readRows(row);
            if (!(m_nextReg.peek(0x68) & 0x10) && m_extendedKeys) {
                for (unsigned i = 0; i < 8; ++i)
                    if (!(row & (1u << i)))
                        keys &= m_extendedRows[i];
            }
            const bool tape = ctx.tape.readEar(ctx);
            const uint8_t ear = ctx.audioMixer.beeper().earBit() != tape ? 0x40 : 0;
            return {uint8_t(0xA0 | ear | (keys & 0x1F)), 0xFF};
        },
        [&ctx, &ula](uint16_t, uint8_t value) {
            ctx.tape.update(ctx);
            ula.setBorderColor(value & 7);
            ctx.audioMixer.writeBeeper(ctx.clock.frameBaseTstates(), value & 0x10, value & 8);
        });
    registerStandardAyPorts(ctx);

    wireNextRegs(ctx, ula);
    wirePorts(ctx, ula);
    wireDivMmc(ctx);
    wireMultiface(ctx, ula);
}

void ZxNextMachine::loadRoms(MachineContext &ctx)
{
    // Like the TBBlue firmware, take the ROMs from the SD card (/machines/next);
    // the ROM resolver is the fallback.
    std::optional<FatVolume> fat;
    if (ctx.sdCard.isInserted()) {
        fat.emplace([&ctx](uint64_t lba, std::span<uint8_t, 512> out) {
            return ctx.sdCard.readBlock(lba, out);
        });
    }
    const auto load = [&](std::string_view name, std::size_t size, bool optional = false) -> std::vector<uint8_t> {
        if (ctx.romOverrideResolver) {
            auto data = ctx.romOverrideResolver(name, size);
            if (!data.empty()) {
                if (data.size() != size)
                    throw RomMissingException(std::format("ROM '{}' size mismatch: expected {} bytes, got {}", name, size, data.size()));
                return data;
            }
        }
        if (fat) {
            if (auto data = fat->readFile(std::format("/machines/next/{}", name)); data && data->size() == size)
                return std::move(*data);
        }
        if (ctx.romResolver) {
            if (auto data = ctx.romResolver(name, size); data.size() == size)
                return data;
        }
        if (!optional && (fat || ctx.romResolver || ctx.romOverrideResolver))
            throw RomMissingException(std::format("The ZX Spectrum Next needs '{}': insert a NextZXOS SD card image", name));
        return {};
    };

    if (auto rom = load("enNextZX.rom", 64 * 1024); !rom.empty())
        ctx.mmu.bank(m_romBank).loadData(0, rom);
    if (auto rom = load("enNxtmmc.rom", 8 * 1024); !rom.empty())
        ctx.mmu.bank(m_divRomBank).loadData(0, rom);
    if (auto rom = load("enNextMf.rom", 8 * 1024, true); !rom.empty())
        ctx.mmu.bank(m_mfRomBank).loadData(0, rom);
    else
        ctx.mmu.bank(m_mfRomBank).fill(0x00); // removing an override must not retain its bytes

    // NextZXOS calls into the alt ROM while it boots
    if (auto rom = load("enAltZX.rom", 32 * 1024); !rom.empty())
        ctx.mmu.bank(m_altRomBank).loadData(0, rom);
}

void ZxNextMachine::wireNextRegs(MachineContext &ctx, UlaRenderer &ula)
{
    // NextREG Select port 0x243B and Data port 0x253B
    ctx.ioBus.registerSimpleHandler(
        "next_reg_select",
        0xFFFF,
        0x243B,
        [this](uint16_t) -> uint8_t { return m_nextReg.selected(); },
        [this](uint16_t, uint8_t val) { m_nextReg.select(val); });

    ctx.ioBus.registerSimpleHandler(
        "next_reg_data",
        0xFFFF,
        0x253B,
        [this](uint16_t) -> uint8_t { return m_nextReg.readSelected(); },
        [this](uint16_t, uint8_t val) { m_nextReg.writeSelected(val); });

    ctx.ioBus.setNextRegOpcodeCallback([this](uint8_t reg, uint8_t val) {
        m_nextReg.write(reg, val);
    });

    // Identification: a ZX Spectrum Next with core 3.02.03 (NextZXOS needs >= 3.01.10)
    m_nextReg.setReadHandler(0x00, [](uint8_t, uint8_t) -> uint8_t { return 0x0A; });
    m_nextReg.setReadHandler(0x01, [](uint8_t, uint8_t) -> uint8_t { return 0x32; });
    m_nextReg.setReadHandler(0x0E, [](uint8_t, uint8_t) -> uint8_t { return 0x03; });

    // NR 0x02: reset type on read, software resets on write
    m_nextReg.setReadHandler(0x02, [this](uint8_t, uint8_t) -> uint8_t { return m_resetType; });
    m_nextReg.setWriteHandler(0x02, [this, &ctx, &ula](uint8_t, uint8_t val) {
        if (val & 0x02)
            scheduleReset(ctx, ula, ResetKind::Hard);
        else if (val & 0x01)
            scheduleReset(ctx, ula, ResetKind::Soft);
        else {
            const auto old = m_resetType;
            m_resetType = uint8_t((m_resetType & 3) | (val & 0x8C));
            if ((val & 0x80) && !(old & 0x80))
                m_uart.reset(false);
            if ((val & 8) && !(old & 8))
                requestNmi(ctx, true, false);
            else if ((val & 4) && !(old & 4))
                requestNmi(ctx, false, false);
        }
    });

    m_nextReg.setReadHandler(0xB0, [this](uint8_t, uint8_t) { return uint8_t(m_extendedKeys); });
    m_nextReg.setReadHandler(0xB1, [this](uint8_t, uint8_t) { return uint8_t(m_extendedKeys >> 8); });
    for (uint8_t reg : {0x2C, 0x2D, 0x2E}) {
        // Reads are I2S from the Pi, silent when there is no I2S endpoint.
        m_nextReg.setReadHandler(reg, [](uint8_t, uint8_t) -> uint8_t { return 0; });
        m_nextReg.setWriteHandler(reg, [&ctx, reg](uint8_t, uint8_t value) {
            auto &mixer = ctx.audioMixer;
            const unsigned mask = reg == 0x2C ? 2 : reg == 0x2D ? 9 : 4;
            for (unsigned ch = 0; ch < 4; ++ch)
                if (mask & (1u << ch))
                    mixer.dac().writeChannel(ctx.clock.frameBaseTstates(), ch, value, mixer.leftBlip(),
                                             mixer.rightBlip());
        });
    }

    // NR 0x03: machine type (only changeable in config mode) and display timing
    m_nextReg.setReadHandler(0x03, [this](uint8_t, uint8_t) -> uint8_t {
        return static_cast<uint8_t>((m_timing << 4) | m_machineType);
    });
    m_nextReg.setWriteHandler(0x03, [this, &ctx, &ula](uint8_t, uint8_t val) {
        if (val & 0x80)
            m_timing = (val >> 4) & 0x07;
        if (m_machineType == 0 && (val & 0x07) != 0) {
            m_machineType = val & 0x07;
            updateMemoryMap(ctx, ula);
        }
    });

    // NR 0x07: CPU speed; reads report the actual speed in bits 5:4
    m_nextReg.setReadHandler(0x07, [&ctx](uint8_t, uint8_t val) -> uint8_t {
        return static_cast<uint8_t>(((static_cast<uint8_t>(ctx.clock.speed()) & 0x03) << 4) | (val & 0x03));
    });
    m_nextReg.setWriteHandler(0x07, [&ctx](uint8_t, uint8_t val) {
        ctx.clock.setSpeed(static_cast<CpuSpeed>(val & 0x03));
    });

    // NR 0x08: bit 7 unlocks port 0x7FFD, bit 6 disables the RAM contention
    m_nextReg.setWriteHandler(0x08, [this, &ctx, &ula](uint8_t, uint8_t val) {
        if (val & 0x80)
            m_port7ffd &= static_cast<uint8_t>(~0x20);
        m_nextReg.setRaw(0x08, val & 0x7F);
        updateMemoryMap(ctx, ula);
    });

    // NR 0x09 bit 3 clears the DivMMC MAPRAM bit
    m_nextReg.setWriteHandler(0x09, [this, &ctx](uint8_t, uint8_t val) {
        if (val & 0x08) {
            m_portE3 &= static_cast<uint8_t>(~0x40);
            m_nextReg.setRaw(0x09, val & static_cast<uint8_t>(~0x08));
            updateDivMmc(ctx);
        }
    });

    // DivMMC automap enable (NR 0x0A bit 4) and port enables (NR 0x83 bit 0)
    m_nextReg.setWriteHandler(0x0A, [this, &ctx](uint8_t, uint8_t) { updateDivMmc(ctx); });
    m_nextReg.setWriteHandler(0x83, [this, &ctx](uint8_t, uint8_t value) {
        updateDivMmc(ctx);
        if (!(value & 2)) {
            m_mfActive = m_mfMapped = m_mfAwaitingEntry = false;
            m_mfInvisible = true;
            ctx.romcs.setActive("next_multiface", false);
            ctx.cpu.removeHooks("next_multiface_entry");
        }
    });

    // Raster line
    m_nextReg.setReadHandler(0x1E, [&ctx](uint8_t, uint8_t) -> uint8_t { return (ctx.clock.rasterPos().line >> 8) & 0x01; });
    m_nextReg.setReadHandler(0x1F, [&ctx](uint8_t, uint8_t) -> uint8_t { return ctx.clock.rasterPos().line & 0xFF; });

    // Layer 2 banks
    m_nextReg.setWriteHandler(0x12, [this, &ctx](uint8_t, uint8_t) { updateLayer2MemoryMapping(ctx); });
    m_nextReg.setWriteHandler(0x13, [this, &ctx](uint8_t, uint8_t) { updateLayer2MemoryMapping(ctx); });

    // MMU slots
    for (uint8_t slot = 0; slot < 8; ++slot) {
        m_nextReg.setWriteHandler(static_cast<uint8_t>(0x50 + slot), [this, &ctx, slot](uint8_t, uint8_t) {
            mapSlot(ctx, slot);
            if (slot < 6 && (m_nextVideo.layer2WriteMapped() || m_nextVideo.layer2ReadMapped()))
                updateLayer2MemoryMapping(ctx);
            if (slot < 2)
                updateDivMmc(ctx);
        });
    }

    // Alt ROM (NR 0x8C), unified paging (NR 0x8E), paging mode (NR 0x8F)
    m_nextReg.setWriteHandler(0x8C, [this, &ctx, &ula](uint8_t, uint8_t) { updateMemoryMap(ctx, ula); });
    m_nextReg.setReadHandler(0x8E, [this](uint8_t, uint8_t) -> uint8_t { return readNr8e(); });
    m_nextReg.setWriteHandler(0x8E, [this, &ctx, &ula](uint8_t, uint8_t val) { writeNr8e(ctx, ula, val); });
    m_nextReg.setWriteHandler(0x8F, [this, &ctx, &ula](uint8_t, uint8_t) { updateLegacyPaging(ctx, ula); });

    // Palettes
    m_nextReg.setWriteHandler(0x40, [this](uint8_t, uint8_t val) { m_nextVideo.setPaletteIndex(val); });
    m_nextReg.setReadHandler(0x40, [this](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.paletteIndex(); });
    m_nextReg.setWriteHandler(0x41, [this](uint8_t, uint8_t val) { m_nextVideo.writePalette8Bit(val); });
    m_nextReg.setReadHandler(0x41, [this](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.readPalette8Bit(); });
    m_nextReg.setWriteHandler(0x43, [this](uint8_t, uint8_t val) { m_nextVideo.setPaletteControl(val); });
    m_nextReg.setWriteHandler(0x44, [this](uint8_t, uint8_t val) { m_nextVideo.writePalette9Bit(val); });
    m_nextReg.setReadHandler(0x44, [this](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.readPalette9Bit(); });

    // Clip windows (four writes cycle x1, x2, y1, y2) and their index reset
    for (uint8_t layer = 0; layer < 4; ++layer) {
        const auto clip = static_cast<NextVideo::ClipLayer>(layer);
        m_nextReg.setWriteHandler(static_cast<uint8_t>(0x18 + layer), [this, clip](uint8_t, uint8_t val) { m_nextVideo.writeClip(clip, val); });
        m_nextReg.setReadHandler(static_cast<uint8_t>(0x18 + layer), [this, clip](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.readClip(clip); });
    }
    m_nextReg.setWriteHandler(0x1C, [this](uint8_t, uint8_t val) { m_nextVideo.resetClipIndexes(val & 0x0F); });
    m_nextReg.setReadHandler(0x1C, [this](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.clipIndexes(); });

    // Sprite attributes through NextREGs: NR 0x34 selects, 0x35..0x39 write, 0x75..0x79 write and advance
    m_nextReg.setWriteHandler(0x34, [this](uint8_t, uint8_t val) {
        m_nextVideo.selectSpriteNextReg(val, (m_nextReg.peek(0x09) & 0x10) != 0);
    });
    m_nextReg.setReadHandler(0x34, [this](uint8_t, uint8_t) -> uint8_t { return m_nextVideo.spriteNextRegSelection(); });
    for (uint8_t i = 0; i < 5; ++i) {
        m_nextReg.setWriteHandler(static_cast<uint8_t>(0x35 + i), [this, i](uint8_t, uint8_t val) { m_nextVideo.writeSpriteAttributeNextReg(i, val, false); });
        m_nextReg.setWriteHandler(static_cast<uint8_t>(0x75 + i), [this, i](uint8_t, uint8_t val) { m_nextVideo.writeSpriteAttributeNextReg(i, val, true); });
    }

    // NR 0x69: Layer 2 visible (port 0x123B bit 1), ULA shadow screen (0x7FFD bit 3), Timex mode (port 0xFF)
    m_nextReg.setWriteHandler(0x69, [this, &ctx, &ula](uint8_t, uint8_t val) {
        m_nextVideo.setLayer2Visible((val & 0x80) != 0);
        m_port7ffd = static_cast<uint8_t>((m_port7ffd & ~0x08) | ((val & 0x40) ? 0x08 : 0x00));
        m_nextVideo.setTimexPort(static_cast<uint8_t>((m_nextVideo.timexPort() & 0xC0) | (val & 0x3F)));
        ula.setVramSource(m_ramBank, (m_port7ffd & 0x08) ? 7 : 5);
    });
    m_nextReg.setReadHandler(0x69, [this](uint8_t, uint8_t) -> uint8_t {
        return static_cast<uint8_t>((m_nextVideo.isLayer2Visible() ? 0x80 : 0x00) | ((m_port7ffd & 0x08) ? 0x40 : 0x00) |
                                    (m_nextVideo.timexPort() & 0x3F));
    });

    // Copper
    m_nextReg.setWriteHandler(0x60, [this](uint8_t, uint8_t val) { m_copper.writeData8(val); });
    m_nextReg.setWriteHandler(0x61, [this](uint8_t, uint8_t val) { m_copper.setControlLo(val); });
    m_nextReg.setWriteHandler(0x62, [this](uint8_t, uint8_t val) { m_copper.setControlHi(val); });
    m_nextReg.setWriteHandler(0x63, [this](uint8_t, uint8_t val) { m_copper.writeData8(val); });

    // Interrupt enables: NR 0x22 bit 2 disables the ULA interrupt, bit 1 enables
    // the line interrupt; NR 0xC4 bits 0 / 1 are the same enables, positive
    m_nextReg.setWriteHandler(0xC4, [this](uint8_t, uint8_t val) {
        const uint8_t nr22 = m_nextReg.peek(0x22) & 0x01;
        m_nextReg.setRaw(0x22, static_cast<uint8_t>(nr22 | ((val & 0x01) ? 0x00 : 0x04) | (val & 0x02)));
    });
    m_nextReg.setReadHandler(0xC4, [this](uint8_t, uint8_t val) -> uint8_t {
        const uint8_t nr22 = m_nextReg.peek(0x22);
        return static_cast<uint8_t>((val & 0x80) | (nr22 & 0x02) | ((nr22 & 0x04) ? 0x00 : 0x01));
    });

    // Hardware IM2 mode
    m_nextReg.setWriteHandler(0xC0, [&ctx](uint8_t, uint8_t val) {
        ctx.im2.setHardwareIm2Enabled((val & 0x01) != 0);
        ctx.im2.setVectorBase(val & 0xE0);
        if (!(val & 0x08))
            ctx.cpu.configureStacklessRetn(false, ctx.cpu.nmiReturnAddress());
    });
    // NMI acknowledge captures PC even in ordinary stack mode. In stackless
    // mode RETN reads these registers instead of the two bytes at SP.
    for (uint8_t reg : {0xC2, 0xC3}) {
        const unsigned shift = reg == 0xC2 ? 0 : 8;
        m_nextReg.setReadHandler(reg, [&ctx, shift](uint8_t, uint8_t) {
            return uint8_t(ctx.cpu.nmiReturnAddress() >> shift);
        });
        m_nextReg.setWriteHandler(reg, [&ctx, shift](uint8_t, uint8_t value) {
            const auto address = uint16_t((ctx.cpu.nmiReturnAddress() & ~(0xFFu << shift)) | (unsigned(value) << shift));
            ctx.cpu.configureStacklessRetn(ctx.cpu.stacklessRetnActive(), address);
        });
    }
}

void ZxNextMachine::wirePorts(MachineContext &ctx, UlaRenderer &ula)
{
    // 128K / +3 paging ports; bit 5 of 0x7FFD locks all of them
    ctx.ioBus.registerSimpleHandler(
        "next_7ffd",
        0xC002,
        0x4000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_port7ffd & 0x20)
                return;
            m_port7ffd = val;
            updateLegacyPaging(ctx, ula);
        });
    ctx.ioBus.registerSimpleHandler(
        "next_1ffd",
        0xF002,
        0x1000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_port7ffd & 0x20)
                return;
            m_port1ffd = val;
            updateLegacyPaging(ctx, ula);
        });
    ctx.ioBus.registerSimpleHandler(
        "next_dffd",
        0xF002,
        0xD000,
        nullptr,
        [this, &ctx, &ula](uint16_t, uint8_t val) {
            if (m_port7ffd & 0x20)
                return;
            m_portDffd = val;
            updateLegacyPaging(ctx, ula);
        });

    // Layer 2 control port 0x123B
    ctx.ioBus.registerSimpleHandler(
        "next_layer2_123b",
        0xFFFF,
        0x123B,
        [this](uint16_t) -> uint8_t { return m_nextVideo.layer2ControlPort(); },
        [this, &ctx](uint16_t, uint8_t val) {
            m_nextVideo.setLayer2ControlPort(val);
            updateLayer2MemoryMapping(ctx);
            updateDivMmc(ctx);
        });

    // Sprite ports 0x303B, 0x57, 0x5B
    ctx.ioBus.registerSimpleHandler(
        "next_sprite_303b",
        0xFFFF,
        0x303B,
        [this](uint16_t) -> uint8_t { return m_nextVideo.readSpriteStatus(); },
        [this](uint16_t, uint8_t val) { m_nextVideo.writeSpriteControlPort(val); });

    // Timex SCLD port 0xFF: video mode, bit 6 is the ULA interrupt disable (NR 0x22 bit 2)
    ctx.ioBus.registerSimpleHandler(
        "next_timex_ff",
        0x00FF,
        0x00FF,
        [this](uint16_t) -> uint8_t { return m_nextVideo.timexPort(); },
        [this](uint16_t, uint8_t val) {
            m_nextVideo.setTimexPort(val);
            m_nextReg.setRaw(0x22, static_cast<uint8_t>((m_nextReg.peek(0x22) & ~0x04) | ((val & 0x40) ? 0x04 : 0x00)));
        });

    ctx.ioBus.registerSimpleHandler(
        "next_sprite_attr_57",
        0x00FF,
        0x0057,
        nullptr,
        [this](uint16_t, uint8_t val) { m_nextVideo.writeSpriteAttributePort(val); });

    ctx.ioBus.registerSimpleHandler(
        "next_sprite_pat_5b",
        0x00FF,
        0x005B,
        nullptr,
        [this](uint16_t, uint8_t val) { m_nextVideo.writeSpritePatternPort(val); });

    // ZXN-DMA ports 0x6B and 0x0B
    ctx.ioBus.registerSimpleHandler(
        "next_dma_6b",
        0x00FF,
        0x006B,
        [this](uint16_t) -> uint8_t { return m_dma.readPort(false); },
        [this, &ctx](uint16_t, uint8_t val) { m_dma.writePort(val, ctx.mmu, ctx.ioBus, false); });

    ctx.ioBus.registerSimpleHandler(
        "next_dma_0b",
        0x00FF,
        0x000B,
        [this](uint16_t) -> uint8_t { return m_dma.readPort(true); },
        [this, &ctx](uint16_t, uint8_t val) { m_dma.writePort(val, ctx.mmu, ctx.ioBus, true); });

    // CTC ports 0x183B .. 0x1B3B
    for (uint8_t ch = 0; ch < 4; ++ch) {
        const uint16_t port = static_cast<uint16_t>(0x183B + (ch << 8));
        ctx.ioBus.registerSimpleHandler(
            std::format("next_ctc_{}", ch),
            0xFFFF,
            port,
            [this, ch](uint16_t) -> uint8_t { return m_ctc.readChannel(ch); },
            [this, ch](uint16_t, uint8_t val) { m_ctc.writeChannel(ch, val); });
    }

    // DAC aliases and decode precedence follow zxnext.vhd port_dac_A..D.
    ctx.ioBus.registerSimpleHandler(
        "next_dacs", 1, 1, nullptr,
        [this, &ctx](uint16_t address, uint8_t value) {
            const uint8_t port = uint8_t(address);
            const uint8_t en = m_nextReg.peek(0x84);
            unsigned mask = 0;
            if ((port == 0xDF && (en & 0x80)) || (port == 0xFB && (en & 0x20) && !(en & 4)))
                mask = 9;
            if (port == 0xB3 && (en & 0x40))
                mask = 6;
            if ((port == 0x1F && (en & 2)) || (port == 0xF1 && (en & 4)) || (port == 0x3F && (en & 8)))
                mask |= 1;
            if ((port == 0x0F && (en & 0x12)) || (port == 0xF3 && (en & 4)))
                mask |= 2;
            if ((port == 0x4F && (en & 0x12)) || (port == 0xF9 && (en & 4)))
                mask |= 4;
            if ((port == 0x5F && (en & 0x0A)) || (port == 0xFB && (en & 4)))
                mask |= 8;
            if (!mask) {
                ctx.ioBus.declineWrite();
                return;
            }
            if (m_nextReg.peek(0x08) & 8)
                for (unsigned ch = 0; ch < 4; ++ch)
                    if (mask & (1u << ch))
                        ctx.audioMixer.dac().writeChannel(ctx.clock.frameBaseTstates(), ch, value,
                                                          ctx.audioMixer.leftBlip(), ctx.audioMixer.rightBlip());
            if (port != 0xF1 && port != 0xF9)
                ctx.ioBus.declineWrite();
        },
        100, true);

    // One decoded group avoids adding six entries to every guest I/O scan.
    ctx.ioBus.registerHandler(
        "next_serial", 0xF8FF, 0x103B,
        [this, &ctx](uint16_t port) -> IoReadResult {
            if (port == 0x103B || port == 0x113B) {
                if (!(m_nextReg.peek(0x83) & 4))
                    return {0xFF, 0};
                return {uint8_t(0xFE | (port == 0x103B ? m_rtc.scl() : m_rtc.sda())), 0xFF};
            }
            if (port >= 0x133B && port <= 0x163B && (m_nextReg.peek(0x83) & 0x10))
                return {m_uart.read(port, uartTime(ctx)), 0xFF};
            return {0xFF, 0};
        },
        [this, &ctx](uint16_t port, uint8_t value) {
            if ((port == 0x103B || port == 0x113B) && (m_nextReg.peek(0x83) & 4)) {
                port == 0x103B ? m_rtc.setScl(value & 1) : m_rtc.setSda(value & 1);
            } else if (port >= 0x133B && port <= 0x163B && (m_nextReg.peek(0x83) & 0x10)) {
                m_uart.write(port, value, uartTime(ctx));
            } else
                ctx.ioBus.declineWrite();
        });

    // Built-in joystick ports (Kempston / Mega Drive 1 on 0x1F, 2 on 0x37):
    // idle, unless a joystick peripheral answers them
    for (const uint16_t port : {0x001F, 0x0037}) {
        ctx.ioBus.registerSimpleHandler(
            std::format("next_joystick_{:02x}", port),
            0x00FF,
            port,
            [](uint16_t) -> uint8_t { return 0x00; },
            nullptr,
            -1);
    }

    // SPI: port 0xE7 chip select (write only), 0xEB data. A read of 0xEB returns
    // the byte latched by the previous exchange and clocks out a new 0xFF.
    ctx.ioBus.registerSimpleHandler(
        "next_spi_cs",
        0x00FF,
        0x00E7,
        nullptr,
        [this, &ctx](uint16_t, uint8_t val) {
            m_spiCs = val;
            ctx.sdCard.setSelected(isSdCardSelected());
        });
    ctx.ioBus.registerSimpleHandler(
        "next_spi_data",
        0x00FF,
        0x00EB,
        [this, &ctx](uint16_t) -> uint8_t {
            const uint8_t val = m_spiLatch;
            m_spiLatch = isSdCardSelected() ? ctx.sdCard.transfer(0xFF) : 0xFF;
            return val;
        },
        [this, &ctx](uint16_t, uint8_t val) {
            m_spiLatch = isSdCardSelected() ? ctx.sdCard.transfer(val) : 0xFF;
        });
}

bool ZxNextMachine::isSdCardSelected() const noexcept
{
    // SD0 is selected by bits 1:0 = 10 (NR 0x0A bit 5 swaps the two card slots)
    const uint8_t sd0 = (m_nextReg.peek(0x0A) & 0x20) ? 0x01 : 0x02;
    return (m_spiCs & 0x03) == sd0;
}

uint64_t ZxNextMachine::uartTime(const MachineContext &ctx) const noexcept
{
    return ctx.clock.frameCount() * uint64_t(ctx.clock.timings().tstatesPerFrame()) + ctx.clock.frameBaseTstates();
}

void ZxNextMachine::setExtendedKey(NextKey key, bool pressed) noexcept
{
    const unsigned bit = unsigned(key);
    if (bit >= 16)
        return;
    if (pressed)
        m_extendedKeys |= uint16_t(1u << bit);
    else
        m_extendedKeys &= uint16_t(~(1u << bit));
    rebuildExtendedMatrix();
}

void ZxNextMachine::rebuildExtendedMatrix() noexcept
{
    m_extendedRows.fill(0x1F);
    using K = ZxKey;
    static constexpr std::array<std::pair<K, K>, 16> keys{{{K::CapsShift, K::Num8},
                                                           {K::CapsShift, K::Num5},
                                                           {K::CapsShift, K::Num6},
                                                           {K::CapsShift, K::Num7},
                                                           {K::SymbolShift, K::M},
                                                           {K::SymbolShift, K::N},
                                                           {K::SymbolShift, K::P},
                                                           {K::SymbolShift, K::O},
                                                           {K::CapsShift, K::SymbolShift},
                                                           {K::CapsShift, K::Num2},
                                                           {K::CapsShift, K::Num9},
                                                           {K::CapsShift, K::Num3},
                                                           {K::CapsShift, K::Num4},
                                                           {K::CapsShift, K::Space},
                                                           {K::CapsShift, K::Num1},
                                                           {K::CapsShift, K::Num0}}};
    for (unsigned bit = 0; bit < 16; ++bit)
        if (m_extendedKeys & (1u << bit)) {
            for (K key : {keys[bit].first, keys[bit].second})
                m_extendedRows[unsigned(key) >> 4] &= uint8_t(~(1u << (unsigned(key) & 7)));
        }
}

bool ZxNextMachine::canRequestNmi(bool multiface) const noexcept
{
    const auto enable = multiface ? 8 : 0x10;
    if (!(m_nextReg.peek(0x06) & enable) || m_mfActive || m_mfMapped || m_divNmiLatched || m_dma.isActive())
        return false;
    return multiface ? (m_nextReg.peek(0x83) & 2) && !(m_portE3 & 0x80) : (m_nextReg.peek(0x83) & 1);
}

bool ZxNextMachine::requestNmi(MachineContext &ctx, bool multiface, bool button)
{
    if (!canRequestNmi(multiface))
        return false;
    if (multiface) {
        m_mfActive = true;
        m_mfInvisible = false;
        armMultifaceEntry(ctx);
    } else {
        m_divNmiLatched = true;
    }
    if (!button)
        m_resetType |= multiface ? 8 : 4;
    ctx.cpu.requestNmi((m_nextReg.peek(0xC0) & 0x08) != 0);
    return true;
}

void ZxNextMachine::armMultifaceEntry(MachineContext &ctx)
{
    ctx.cpu.removeHooks("next_multiface_entry");
    m_mfAwaitingEntry = true;
    // Install the entry watcher only while an NMI is pending. Even an empty
    // hook would add a page-mask check to every instruction through DivMMC.
    ctx.cpu.addPreM1Hook(
        [this, &ctx](uint16_t pc) {
            if (pc != 0x66 || !m_mfActive)
                return;
            m_mfMapped = true;
            m_mfAwaitingEntry = false;
            ctx.romcs.setActive("next_multiface", true);
            ctx.cpu.setPreM1HookPages("next_multiface_entry", {});
            // Defer vector erasure until the CPU has finished iterating its hooks.
            ctx.scheduler.scheduleAt(ctx.clock.totalTstates() + 1, [this, &ctx](uint64_t) {
                if (!m_mfAwaitingEntry)
                    ctx.cpu.removeHooks("next_multiface_entry");
            });
        },
        "next_multiface_entry", Z80Cpu::PcPages{}.set(0));
}

void ZxNextMachine::wireMultiface(MachineContext &ctx, UlaRenderer &ula)
{
    ctx.romcs.registerProvider("next_multiface", 100, [this](Mmu &mmu) {
        mmu.mapOverlay1k(0, m_mfRomBank, 0, 8, false);
        mmu.mapOverlay1k(0x2000, m_mfRamBank, 0, 8, true);
    });
    ctx.cpu.addRetnHook(
        [this, &ctx](bool reti) {
            if (reti)
                return;
            m_mfActive = m_mfMapped = false;
            m_mfAwaitingEntry = false;
            ctx.cpu.removeHooks("next_multiface_entry");
            ctx.romcs.setActive("next_multiface", false);
        },
        "next_multiface_retn");
    ctx.ioBus.registerHandler(
        "next_multiface_io", 0x1F, 0x1F,
        [this, &ctx, &ula](uint16_t port) -> IoReadResult {
            const uint8_t low = uint8_t(port);
            const auto type = (m_nextReg.peek(0x0A) >> 6) & 3;
            const uint8_t on = type & 2 ? 0x9F : type & 1 ? 0xBF : 0x3F;
            const uint8_t off = type & 2 ? 0x1F : type & 1 ? 0x3F : 0xBF;
            if (!(m_nextReg.peek(0x83) & 2) || (low != on && low != off))
                return {0xFF, 0};
            if (low == off) {
                m_mfMapped = false;
                if (type == 0) {
                    m_mfActive = false;
                    m_mfAwaitingEntry = false;
                    ctx.cpu.removeHooks("next_multiface_entry");
                }
            } else
                m_mfMapped = type == 3 || !m_mfInvisible;
            ctx.romcs.setActive("next_multiface", m_mfMapped);
            if (low != on || (type != 3 && m_mfInvisible))
                return {0xFF, 0};
            uint8_t data = uint8_t(0x7F | (m_port7ffd & 8 ? 0x80 : 0));
            if (type == 0) {
                switch (port >> 12) {
                case 1:
                    data = m_port1ffd;
                    break;
                case 7:
                    data = m_port7ffd;
                    break;
                case 13:
                    data = m_portDffd;
                    break;
                default:
                    data = ula.borderColor();
                    break;
                }
            }
            return {data, 0xFF, true};
        },
        [this, &ctx](uint16_t port, uint8_t) {
            const uint8_t low = uint8_t(port);
            const auto type = (m_nextReg.peek(0x0A) >> 6) & 3;
            const uint8_t on = type & 2 ? 0x9F : type & 1 ? 0xBF : 0x3F;
            const uint8_t off = type & 2 ? 0x1F : type & 1 ? 0x3F : 0xBF;
            if (!(m_nextReg.peek(0x83) & 2) || (low != on && low != off)) {
                ctx.ioBus.declineWrite();
                return;
            }
            m_mfActive = false;
            m_mfAwaitingEntry = false;
            ctx.cpu.removeHooks("next_multiface_entry");
            if (low == (type == 0 ? on : off))
                m_mfInvisible = true;
        },
        200);
}

void ZxNextMachine::wireDivMmc(MachineContext &ctx)
{
    // DivMMC memory: 0x0000..0x1FFF ROM (or RAM bank 3 with MAPRAM), 0x2000..0x3FFF RAM bank
    ctx.romcs.registerProvider(
        std::string(kDivMmcId),
        90,
        [this](Mmu &mmu) {
            const bool conmem = (m_portE3 & 0x80) != 0;
            const bool mapram = (m_portE3 & 0x40) != 0;
            const std::size_t bank = m_portE3 & 0x0F;
            if (conmem || !mapram)
                mmu.mapOverlay1k(0x0000, m_divRomBank, 0, 8, false);
            else
                mmu.mapOverlay1k(0x0000, m_divRamBank, 3 * 8, 8, false);
            mmu.mapOverlay1k(0x2000, m_divRamBank, bank * 8, 8, conmem || !mapram || bank != 3);
        });

    ctx.ioBus.registerSimpleHandler(
        "next_divmmc_e3",
        0x00FF,
        0x00E3,
        [this](uint16_t) -> uint8_t { return m_portE3; },
        [this, &ctx](uint16_t, uint8_t val) {
            // MAPRAM (bit 6) is sticky; bits 5:4 don't exist
            m_portE3 = static_cast<uint8_t>((val & 0xCF) | (m_portE3 & 0x40));
            updateDivMmc(ctx);
        });

    // Instant entry points map DivMMC before the opcode fetch, delayed ones after it
    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (!m_divAutomap && divMmcEntry(pc, true)) {
            m_divAutomap = true;
            updateDivMmc(ctx);
        }
    }, std::string(kDivMmcId));
    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if (m_divAutomap) {
            if ((pc & 0xFFF8) == 0x1FF8 && (m_nextReg.peek(0xBB) & 0x40)) {
                m_divAutomap = false;
                updateDivMmc(ctx);
            }
        } else if (divMmcEntry(pc, false)) {
            m_divAutomap = true;
            updateDivMmc(ctx);
        }
    }, std::string(kDivMmcId));
    ctx.cpu.addRetnHook([this, &ctx](bool isReti) {
        if (isReti)
            return;
        m_divNmiLatched = false;
        if (m_divAutomap) {
            m_divAutomap = false;
            updateDivMmc(ctx);
        }
    }, std::string(kDivMmcId));
}

bool ZxNextMachine::divMmcEntry(uint16_t pc, bool instant) const noexcept
{
    if (!(m_nextReg.peek(0x0A) & 0x10) || !(m_nextReg.peek(0x83) & 0x01))
        return false;

    if (pc < 0x40) {
        if (pc & 0x07)
            return false;
        const uint8_t bit = static_cast<uint8_t>(1u << (pc >> 3));
        if (!(m_nextReg.peek(0xB8) & bit))
            return false;
        if (((m_nextReg.peek(0xBA) & bit) != 0) != instant)
            return false;
        return (m_nextReg.peek(0xB9) & bit) || isRom3Paged();
    }

    const uint8_t bb = m_nextReg.peek(0xBB);
    if (pc == 0x0066)
        return m_divNmiLatched && (bb & (instant ? 0x02 : 0x01));
    if (instant)
        return (pc & 0xFF00) == 0x3D00 && (bb & 0x80) && isRom3Paged();

    switch (pc) {
    case 0x04C6: return (bb & 0x04) && isRom3Paged();
    case 0x0562: return (bb & 0x08) && isRom3Paged();
    case 0x04D7: return (bb & 0x10) && isRom3Paged();
    case 0x056A: return (bb & 0x20) && isRom3Paged();
    default:     return false;
    }
}

void ZxNextMachine::updateDivMmc(MachineContext &ctx)
{
    const bool enabled = (m_nextReg.peek(0x83) & 0x01) != 0;
    const bool automap = m_divAutomap && (m_nextReg.peek(0x0A) & 0x10);
    m_divMmcMapped = enabled && ((m_portE3 & 0x80) || automap);
    ctx.romcs.setActive(kDivMmcId, m_divMmcMapped);
    if (m_divMmcMapped)
        ctx.romcs.refresh(); // the bank or MAPRAM may have changed
}

uint8_t ZxNextMachine::romNumber() const noexcept
{
    const uint8_t locks = (m_nextReg.peek(0x8C) >> 4) & 0x03;
    switch (m_machineType) {
    case 1: // 48K
        return locks;
    case 3: // +3
        if (locks)
            return locks;
        return static_cast<uint8_t>(((m_port1ffd >> 1) & 0x02) | ((m_port7ffd >> 4) & 0x01));
    default: // 128K, Pentagon
        if (locks)
            return locks & 0x01;
        return (m_port7ffd >> 4) & 0x01;
    }
}

uint8_t ZxNextMachine::altRomHalf() const noexcept
{
    const uint8_t nr8c = m_nextReg.peek(0x8C);
    if (nr8c & 0x30)
        return (nr8c >> 5) & 0x01;
    return (m_port7ffd >> 4) & 0x01;
}

bool ZxNextMachine::isRom3Paged() const noexcept
{
    if (m_nextReg.peek(0x50) < kFirstNonRamPage || (m_nextVideo.layer2ControlPort() & 0x04))
        return false;
    const uint8_t nr8c = m_nextReg.peek(0x8C);
    if ((nr8c & 0xC0) == 0x80) // alt ROM read enabled
        return altRomHalf() == 1;
    return romNumber() == 3;
}

void ZxNextMachine::mapRomSlot(MachineContext &ctx, uint8_t slot)
{
    const uint16_t addr = static_cast<uint16_t>(slot * 0x2000);
    const uint8_t nr8c = m_nextReg.peek(0x8C);
    const std::size_t romPage1k = (static_cast<std::size_t>(romNumber()) * 2 + slot) * 8;
    const std::size_t altPage1k = (static_cast<std::size_t>(altRomHalf()) * 2 + slot) * 8;
    if ((nr8c & 0xC0) == 0x80) {
        // Alt ROM replaces the ROM
        ctx.mmu.mapPages1k(addr, m_altRomBank, altPage1k, 8, true, true, false, /*forceReadOnly=*/true);
        return;
    }
    ctx.mmu.mapPages1k(addr, m_romBank, romPage1k, 8);
    if (nr8c & 0x80) // reads from the ROM, writes into the alt ROM
        ctx.mmu.mapPages1k(addr, m_altRomBank, altPage1k, 8, false, true);
}

void ZxNextMachine::mapSlot(MachineContext &ctx, uint8_t slot)
{
    const uint8_t page = m_nextReg.peek(static_cast<uint8_t>(0x50 + slot));
    const uint16_t addr = static_cast<uint16_t>(slot * 0x2000);
    if (page < kFirstNonRamPage) {
        // Banks 5 and 7 are contended at 3.5MHz, unless NR 0x08 bit 6 disables it
        const bool contended = (page == 10 || page == 11 || page == 14 || page == 15) &&
                               !(m_nextReg.peek(0x08) & 0x40);
        ctx.mmu.map8k(addr, m_ramBank, page, contended);
    } else if (slot < 2) {
        mapRomSlot(ctx, slot);
    } else {
        ctx.mmu.unmapPages1k(addr, 8);
    }
}

void ZxNextMachine::updateMemoryMap(MachineContext &ctx, UlaRenderer &ula)
{
    for (uint8_t slot = 6; slot < 8; ++slot)
        mapSlot(ctx, slot);
    updateLayer2MemoryMapping(ctx); // maps slots 0..5
    updateDivMmc(ctx);
    ula.setVramSource(m_ramBank, (m_port7ffd & 0x08) ? 7 : 5);
}

void ZxNextMachine::updateLegacyPaging(MachineContext &ctx, UlaRenderer &ula, bool rebuildTop)
{
    if (m_port1ffd & 0x01) {
        // +3 special (all RAM) paging
        static constexpr uint8_t kSpecial[4][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {4, 5, 6, 3}, {4, 7, 6, 3}};
        const auto &banks = kSpecial[(m_port1ffd >> 1) & 0x03];
        for (uint8_t i = 0; i < 4; ++i) {
            m_nextReg.setRaw(static_cast<uint8_t>(0x50 + i * 2), static_cast<uint8_t>(banks[i] * 2));
            m_nextReg.setRaw(static_cast<uint8_t>(0x51 + i * 2), static_cast<uint8_t>(banks[i] * 2 + 1));
        }
        m_specialPaging = true;
    } else {
        if (m_specialPaging) {
            static constexpr std::array<uint8_t, 4> kNormal = {0x0A, 0x0B, 0x04, 0x05};
            for (uint8_t i = 0; i < 4; ++i)
                m_nextReg.setRaw(static_cast<uint8_t>(0x52 + i), kNormal[i]);
            m_specialPaging = false;
        }
        m_nextReg.setRaw(0x50, 0xFF);
        m_nextReg.setRaw(0x51, 0xFF);
        if (rebuildTop) {
            const uint8_t bank = static_cast<uint8_t>(((m_portDffd & 0x0F) << 3) | (m_port7ffd & 0x07));
            m_nextReg.setRaw(0x56, static_cast<uint8_t>(bank * 2));
            m_nextReg.setRaw(0x57, static_cast<uint8_t>(bank * 2 + 1));
        }
    }
    updateMemoryMap(ctx, ula);
}

void ZxNextMachine::writeNr8e(MachineContext &ctx, UlaRenderer &ula, uint8_t val)
{
    const bool setBank = (val & 0x08) != 0;
    if (setBank) {
        m_port7ffd = static_cast<uint8_t>((m_port7ffd & ~0x07) | ((val >> 4) & 0x07));
        m_portDffd = (val >> 7) & 0x01;
    }
    m_port1ffd = static_cast<uint8_t>((m_port1ffd & ~0x07) | ((val >> 2) & 0x01) | ((val & 0x02) << 1) | ((val & 0x01) << 1));
    if (!(val & 0x04))
        m_port7ffd = static_cast<uint8_t>((m_port7ffd & ~0x10) | ((val & 0x01) << 4));
    updateLegacyPaging(ctx, ula, setBank);
}

uint8_t ZxNextMachine::readNr8e() const noexcept
{
    const bool special = (m_port1ffd & 0x01) != 0;
    return static_cast<uint8_t>(((m_portDffd & 0x01) << 7) | ((m_port7ffd & 0x07) << 4) | 0x08 |
                                ((m_port1ffd & 0x01) << 2) | ((m_port1ffd >> 1) & 0x02) |
                                (special ? ((m_port1ffd >> 1) & 0x01) : ((m_port7ffd >> 4) & 0x01)));
}

void ZxNextMachine::updateLayer2MemoryMapping(MachineContext &ctx)
{
    // Layer 2 maps over the normal slots: restore them first
    for (uint8_t slot = 0; slot < 6; ++slot)
        mapSlot(ctx, slot);
    const bool mapWrite = m_nextVideo.layer2WriteMapped();
    const bool mapRead = m_nextVideo.layer2ReadMapped();
    if (!mapRead && !mapWrite)
        return;
    const std::size_t bank = static_cast<std::size_t>(m_nextVideo.layer2UsesShadowBank() ? m_nextReg.peek(0x13) : m_nextReg.peek(0x12)) +
                             m_nextVideo.layer2BankOffset();
    const uint8_t segment = m_nextVideo.layer2Segment();
    // Segments 0..2 map one 16KB third at 0x0000, segment 3 all three at 0x0000..0xBFFF
    const uint8_t first = segment < 3 ? segment : 0;
    const uint8_t count = segment < 3 ? 1 : 3;
    for (uint8_t i = 0; i < count; ++i) {
        const std::size_t page16k = bank + first + i;
        if (page16k * 2 + 1 < kFirstNonRamPage)
            ctx.mmu.map16kReadWrite(static_cast<uint16_t>(i * 0x4000), m_ramBank, page16k, mapRead, mapWrite, false);
    }
}

void ZxNextMachine::scheduleReset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    // NR 0x02 is written in the middle of an instruction: reset once it is done
    ctx.scheduler.scheduleIn(ctx.clock.totalTstates(), 0, [this, &ctx, &ula, kind](uint64_t) {
        ctx.cpu.reset(kind == ResetKind::Hard);
        reset(ctx, ula, kind);
    });
}

void ZxNextMachine::handOverState(bool hard)
{
    // What the TBBlue firmware leaves behind when it starts NextZXOS (as CSpect
    // does); NextZXOS reprograms most of the rest itself.
    m_machineType = 3; // +3, config mode off
    m_timing = 3;
    if (hard) {
        m_nextReg.setRaw(0x05, 0x01);
        m_nextReg.setRaw(0x06, 0x05);
        m_nextReg.setRaw(0x08, 0x98);
        m_nextReg.setRaw(0x0A, 0x11);
        m_nextReg.setRaw(0x8C, 0x00);
        for (uint8_t reg = 0x82; reg <= 0x85; ++reg)
            m_nextReg.setRaw(reg, 0xFF);
    } else {
        // A core reset keeps the peripheral settings
        const uint8_t nr8c = m_nextReg.peek(0x8C);
        m_nextReg.setRaw(0x8C, static_cast<uint8_t>((nr8c & 0x0F) | (nr8c << 4)));
        if (m_nextReg.peek(0x85) & 0x80) {
            for (uint8_t reg = 0x82; reg <= 0x85; ++reg)
                m_nextReg.setRaw(reg, 0xFF);
        }
        m_nextReg.setRaw(0x06, m_nextReg.peek(0x06) | 0xA0);
    }
    m_nextReg.setRaw(0x07, 0x00);
    m_nextReg.setRaw(0x8E, 0x00);
    m_nextReg.setRaw(0xB8, 0x83);
    m_nextReg.setRaw(0xB9, 0x01);
    m_nextReg.setRaw(0xBA, 0x00);
    m_nextReg.setRaw(0xBB, 0xCD);
    m_nextReg.setRaw(0xC0, 0x00);
    m_nextReg.setRaw(0x22, 0x00); // ULA interrupt on, no line interrupt
    m_nextReg.setRaw(0x23, 0x00);
    static constexpr std::array<uint8_t, 8> kMmu = {0xFF, 0xFF, 0x0A, 0x0B, 0x04, 0x05, 0x00, 0x01};
    for (uint8_t slot = 0; slot < 8; ++slot)
        m_nextReg.setRaw(static_cast<uint8_t>(0x50 + slot), kMmu[slot]);
}

void ZxNextMachine::reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind)
{
    const bool hard = (kind == ResetKind::Hard);
    if (hard) {
        if (ctx.romOverrideResolver) loadRoms(ctx);
        ctx.mmu.bank(m_ramBank).fill(0x00);
        ctx.mmu.bank(m_divRamBank).fill(0x00);
    }
    // A soft reset keeps the peripheral settings (NR 0x05, 0x06, 0x08, 0x0A, ...)
    m_nextReg.reset(hard);
    if (!hard)
        m_nextReg.setRaw(0x09, m_nextReg.peek(0x09) & static_cast<uint8_t>(~0x10));
    handOverState(hard);
    m_resetType = hard ? 0x02 : 0x01;

    m_uart.reset(hard);
    m_rtc.resetBus();
    m_extendedKeys = 0;
    rebuildExtendedMatrix();
    m_mfActive = m_mfMapped = false;
    m_mfInvisible = true;
    ctx.romcs.setActive("next_multiface", false);
    m_mfAwaitingEntry = false;
    ctx.cpu.removeHooks("next_multiface_entry");
    m_copper.reset();
    m_dma.reset();
    m_ctc.reset();
    m_nextVideo.reset();
    m_port7ffd = 0;
    m_port1ffd = 0;
    m_portDffd = 0;
    m_specialPaging = false;
    m_portE3 = 0;
    m_divAutomap = false;
    m_divNmiLatched = false;
    m_spiCs = 0xFF;
    ctx.sdCard.setSelected(false);
    ctx.im2.setHardwareIm2Enabled(false);

    ctx.clock.setSpeed(CpuSpeed::Mhz3_5);
    updateMemoryMap(ctx, ula);
}

void ZxNextMachine::raiseInterrupt(MachineContext &ctx, uint8_t im2Vector)
{
    if (ctx.im2.isHardwareIm2Enabled())
        ctx.im2.raiseVector(im2Vector);
    else
        ctx.cpu.requestInterrupt(0xFF);
}

void ZxNextMachine::onFrameStart(MachineContext &ctx)
{
    if (!(m_nextReg.peek(0x22) & 0x04))
        raiseInterrupt(ctx, 11); // ULA frame interrupt
}

void ZxNextMachine::onScanlineStart(uint16_t line, MachineContext &ctx)
{
    if (m_uart.active())
        m_uart.tick(uartTime(ctx));
    if (m_dma.isActive())
        m_dma.tick(ctx.clock.frameBaseTstates(), ctx.mmu, ctx.ioBus);

    const uint8_t nr22 = m_nextReg.peek(0x22);
    if (!(nr22 & 0x02))
        return;
    // Line N fires as the raster reaches ULA pixel line N (0 = first paper line)
    const auto &timings = ctx.clock.timings();
    const uint16_t target = static_cast<uint16_t>(((nr22 & 0x01) << 8) | m_nextReg.peek(0x23));
    if (line == (timings.topBorderLines + target) % timings.linesPerFrame)
        raiseInterrupt(ctx, 0);
}

void ZxNextMachine::renderScanline(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula, VideoFrame &frame)
{
    // Full-border output moves the paper in the frame, not in Copper coordinates.
    const int32_t copperLine = int32_t(y) - ula.borderMargins().top + 32;
    if (copperLine >= 0 && copperLine < 256)
        m_copper.tickScanline(uint16_t(copperLine), m_nextReg);
    m_nextVideo.renderScanline(y, ctx.mmu, m_ramBank, ula, m_nextReg, (m_port7ffd & 0x08) != 0, frame);
}

void ZxNextMachine::onFrameEnd(MachineContext &ctx, UlaRenderer &ula)
{
    m_dma.onFrameEnd(ctx.clock.timings().tstatesPerFrame());
    m_copper.onVBlank();
    ula.advanceFlashCounter();
}

void ZxNextMachine::renderFullFrame(MachineContext &ctx, UlaRenderer &ula, VideoFrame &frame)
{
    for (uint32_t y = 0; y < ula.outputHeight(); ++y)
        m_nextVideo.renderScanline(y, ctx.mmu, m_ramBank, ula, m_nextReg, (m_port7ffd & 0x08) != 0, frame);
}

uint32_t ZxNextMachine::frameWidth() const noexcept
{
    return NextVideo::kFrameWidth;
}

uint32_t ZxNextMachine::frameHeight() const noexcept
{
    return NextVideo::kFrameHeight;
}

uint8_t ZxNextMachine::lastPort7ffd() const noexcept
{
    return m_port7ffd;
}

uint8_t ZxNextMachine::lastPort1ffd() const noexcept
{
    return m_port1ffd;
}

void ZxNextMachine::setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula)
{
    m_port7ffd = port7ffd;
    m_port1ffd = port1ffd;
    updateLegacyPaging(ctx, ula);
}

NextReg &ZxNextMachine::nextReg() noexcept
{
    return m_nextReg;
}

const NextReg &ZxNextMachine::nextReg() const noexcept
{
    return m_nextReg;
}

Copper &ZxNextMachine::copper() noexcept
{
    return m_copper;
}

const Copper &ZxNextMachine::copper() const noexcept
{
    return m_copper;
}

ZxnDma &ZxNextMachine::dma() noexcept
{
    return m_dma;
}

ZxnCtc &ZxNextMachine::ctc() noexcept
{
    return m_ctc;
}

NextVideo &ZxNextMachine::nextVideo() noexcept
{
    return m_nextVideo;
}

const NextVideo &ZxNextMachine::nextVideo() const noexcept
{
    return m_nextVideo;
}

void ZxNextMachine::saveState(StateWriter &w) const
{
    m_nextReg.saveState(w);
    m_copper.saveState(w);
    m_dma.saveState(w);
    m_ctc.saveState(w);
    m_nextVideo.saveState(w);
    w.write(m_port7ffd);
    w.write(m_port1ffd);
    w.write(m_portDffd);
    w.write(m_machineType);
    w.write(m_timing);
    w.write(m_resetType);
    w.writeBool(m_specialPaging);
    w.write(m_portE3);
    w.writeBool(m_divAutomap);
    w.writeBool(m_divNmiLatched);
    w.write(m_spiCs);
    w.write(m_spiLatch);
    m_uart.saveState(w);
    m_rtc.saveState(w);
    w.writeBool(m_mfActive);
    w.writeBool(m_mfMapped);
    w.writeBool(m_mfInvisible);
}

void ZxNextMachine::loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula)
{
    m_nextReg.loadState(r);
    m_copper.loadState(r);
    m_dma.loadState(r);
    m_ctc.loadState(r);
    m_nextVideo.loadState(r);
    m_port7ffd      = r.read<uint8_t>();
    m_port1ffd      = r.read<uint8_t>();
    m_portDffd      = r.read<uint8_t>();
    m_machineType   = r.read<uint8_t>();
    m_timing        = r.read<uint8_t>();
    m_resetType     = r.read<uint8_t>();
    m_specialPaging = r.readBool();
    m_portE3        = r.read<uint8_t>();
    m_divAutomap    = r.readBool();
    m_divNmiLatched = r.readBool();
    m_spiCs         = r.read<uint8_t>();
    m_spiLatch      = r.read<uint8_t>();
    m_uart.loadState(r);
    m_rtc.loadState(r);
    m_mfActive = r.readBool();
    m_mfMapped = r.readBool();
    m_mfInvisible = r.readBool();
    ctx.romcs.setActive("next_multiface", m_mfMapped);
    m_mfAwaitingEntry = false;
    ctx.cpu.removeHooks("next_multiface_entry");
    if (m_mfActive && !m_mfMapped)
        armMultifaceEntry(ctx);

    ctx.clock.setSpeed(static_cast<CpuSpeed>(m_nextReg.peek(0x07) & 0x03));
    ctx.im2.setHardwareIm2Enabled((m_nextReg.peek(0xC0) & 0x01) != 0);
    ctx.sdCard.setSelected(isSdCardSelected());
    updateMemoryMap(ctx, ula);
}

} // namespace zxkit
