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

#include "zxkit/media/SnapshotFormats.h"

#include "zxkit/core/Exception.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/media/Compression.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace zxkit {

void SnapshotFormats::loadSna(std::span<const uint8_t> data, SnapshotMachineState &state)
{
    if (data.size() != 49179 && data.size() != 131103 && data.size() != 147487)
        throw FormatException(std::format("Invalid SNA snapshot size ({} bytes)", data.size()));

    auto &regs = state.ctx.cpu.registers();
    regs.I    = data[0];
    regs.HL_  = readLe16(data, 1);
    regs.DE_  = readLe16(data, 3);
    regs.BC_  = readLe16(data, 5);
    regs.AF_  = readLe16(data, 7);
    regs.HL   = readLe16(data, 9);
    regs.DE   = readLe16(data, 11);
    regs.BC   = readLe16(data, 13);
    regs.IY   = readLe16(data, 15);
    regs.IX   = readLe16(data, 17);
    regs.IFF2 = (data[19] & 0x04) ? 1 : 0;
    regs.IFF1 = regs.IFF2;
    regs.setFullR(data[20]);
    regs.AF   = readLe16(data, 21);
    regs.SP   = readLe16(data, 23);
    regs.IM   = data[25] & 0x03;
    state.ula.setBorderColor(data[26] & 0x07);

    auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    if (data.size() == 49179) {
        for (uint32_t i = 0; i < 49152; ++i)
            state.ctx.mmu.poke(static_cast<uint16_t>(0x4000 + i), data[27 + i]);
        // 48K SNA pops PC from stack
        const uint8_t lo = state.ctx.mmu.peek(regs.SP++);
        const uint8_t hi = state.ctx.mmu.peek(regs.SP++);
        regs.PC = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        regs.WZ = regs.PC;
    } else {
        // 128K SNA
        regs.PC = readLe16(data, 49179);
        const uint8_t port7ffd = data[49181];
        state.machine.setPagingPorts(port7ffd, 0, state.ctx, state.ula);

        const uint8_t topPage = port7ffd & 0x07;
        ram.loadData(5 * 16384, data.subspan(27, 16384));
        ram.loadData(2 * 16384, data.subspan(27 + 16384, 16384));
        ram.loadData(static_cast<std::size_t>(topPage) * 16384, data.subspan(27 + 32768, 16384));

        std::size_t offset = 49183;
        for (uint8_t page = 0; page < 8; ++page) {
            if (page == 5 || page == 2 || page == topPage)
                continue;
            if (offset + 16384 <= data.size()) {
                ram.loadData(static_cast<std::size_t>(page) * 16384, data.subspan(offset, 16384));
                offset += 16384;
            }
        }
    }
}

std::vector<uint8_t> SnapshotFormats::saveSna48k(const SnapshotMachineState &state)
{
    std::vector<uint8_t> out(49179, 0);
    const auto &regs = state.ctx.cpu.registers();
    uint16_t sp = regs.SP - 2;

    out[0] = regs.I;
    writeLe16(out, 1, regs.HL_);
    writeLe16(out, 3, regs.DE_);
    writeLe16(out, 5, regs.BC_);
    writeLe16(out, 7, regs.AF_);
    writeLe16(out, 9, regs.HL);
    writeLe16(out, 11, regs.DE);
    writeLe16(out, 13, regs.BC);
    writeLe16(out, 15, regs.IY);
    writeLe16(out, 17, regs.IX);
    out[19] = regs.IFF2 ? 0x04 : 0x00;
    out[20] = regs.fullR();
    writeLe16(out, 21, regs.AF);
    writeLe16(out, 23, sp);
    out[25] = regs.IM & 0x03;
    out[26] = state.ula.borderColor() & 0x07;

    for (uint32_t i = 0; i < 49152; ++i)
        out[27 + i] = state.ctx.mmu.peek(static_cast<uint16_t>(0x4000 + i));

    if (sp >= 0x4000) {
        out[27 + (sp - 0x4000)] = static_cast<uint8_t>(regs.PC & 0xFF);
        if (static_cast<uint16_t>(sp + 1) >= 0x4000)
            out[27 + (static_cast<uint16_t>(sp + 1) - 0x4000)] = static_cast<uint8_t>(regs.PC >> 8);
    }
    return out;
}

void SnapshotFormats::loadZ80(std::span<const uint8_t> data, SnapshotMachineState &state)
{
    if (data.size() < 30)
        throw FormatException("Z80 snapshot too small (< 30 bytes)");

    auto &regs = state.ctx.cpu.registers();
    regs.setA(data[0]);
    regs.setF(data[1]);
    regs.BC = readLe16(data, 2);
    regs.HL = readLe16(data, 4);
    regs.PC = readLe16(data, 6);
    regs.SP = readLe16(data, 8);
    regs.I  = data[10];
    const uint8_t rLo = data[11] & 0x7F;
    uint8_t flags1 = data[12];
    if (flags1 == 0xFF)
        flags1 = 1;
    regs.setFullR(static_cast<uint8_t>(rLo | ((flags1 & 0x01) << 7)));
    state.ula.setBorderColor((flags1 >> 1) & 0x07);

    regs.DE  = readLe16(data, 13);
    regs.BC_ = readLe16(data, 15);
    regs.DE_ = readLe16(data, 17);
    regs.HL_ = readLe16(data, 19);
    regs.AF_ = static_cast<uint16_t>((static_cast<uint16_t>(data[21]) << 8) | data[22]);
    regs.IY  = readLe16(data, 23);
    regs.IX  = readLe16(data, 25);
    regs.IFF1 = data[27] ? 1 : 0;
    regs.IFF2 = data[28] ? 1 : 0;
    regs.IM   = data[29] & 0x03;

    if (regs.PC != 0) {
        // Version 1 Z80 snapshot (48K flat block)
        const auto payload = data.subspan(30);
        std::vector<uint8_t> unpacked;
        if (flags1 & 0x20) {
            unpacked = decompressZ80Block(payload, 49152, true);
        } else {
            if (payload.size() < 49152)
                throw FormatException("Truncated uncompressed v1 Z80 snapshot");
            unpacked.assign(payload.begin(), payload.begin() + 49152);
        }
        for (std::size_t i = 0; i < 49152 && i < unpacked.size(); ++i)
            state.ctx.mmu.poke(static_cast<uint16_t>(0x4000 + i), unpacked[i]);
        return;
    }

    // Version 2 or 3 Z80 snapshot
    if (data.size() < 34)
        throw FormatException("Truncated v2/v3 Z80 header");
    const uint16_t extLen = readLe16(data, 30);
    const std::size_t blocksStart = 32 + extLen;
    if (blocksStart > data.size())
        throw FormatException("Invalid Z80 extended header length");

    regs.PC = readLe16(data, 32);
    const uint8_t hwMode = data[34];
    const uint8_t port7ffd = data[35];
    const bool is128kMode = (extLen == 23 && hwMode >= 3) || (extLen >= 54 && hwMode >= 4);
    if (is128kMode) {
        state.machine.setPagingPorts(port7ffd, 0, state.ctx, state.ula);
    }

    auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    std::size_t pos = blocksStart;
    while (pos + 3 <= data.size()) {
        const uint16_t compLen = readLe16(data, pos);
        const uint8_t pageCode = data[pos + 2];
        pos += 3;

        std::vector<uint8_t> pageData;
        if (compLen == 0xFFFF) {
            if (pos + 16384 > data.size())
                throw FormatException("Truncated uncompressed Z80 RAM page");
            pageData.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                            data.begin() + static_cast<std::ptrdiff_t>(pos + 16384));
            pos += 16384;
        } else {
            if (pos + compLen > data.size())
                throw FormatException("Truncated compressed Z80 RAM page");
            pageData = decompressZ80Block(data.subspan(pos, compLen), 16384, false);
            pos += compLen;
        }

        if (!is128kMode || ram.count16kPages() <= 3) {
            uint16_t targetAddr = 0;
            if (pageCode == 8) targetAddr = 0x4000;
            else if (pageCode == 4) targetAddr = 0x8000;
            else if (pageCode == 5) targetAddr = 0xC000;
            else continue;
            for (std::size_t i = 0; i < 16384 && i < pageData.size(); ++i)
                state.ctx.mmu.poke(static_cast<uint16_t>(targetAddr + i), pageData[i]);
        } else if (pageCode >= 3 && pageCode <= 10) {
            const std::size_t ramPage16k = pageCode - 3;
            if (ramPage16k < ram.count16kPages())
                ram.loadData(ramPage16k * 16384, pageData);
        }
    }
}

void SnapshotFormats::loadSzx(std::span<const uint8_t> data, SnapshotMachineState &state)
{
    if (data.size() < 8 || std::memcmp(data.data(), "ZXST", 4) != 0)
        throw FormatException("Invalid SZX signature (expected 'ZXST')");

    auto &regs = state.ctx.cpu.registers();
    auto &ram  = state.ctx.mmu.bank(state.machine.ramBankId());

    std::size_t pos = 8;
    while (pos + 8 <= data.size()) {
        const char id[5] = {
            static_cast<char>(data[pos]),
            static_cast<char>(data[pos + 1]),
            static_cast<char>(data[pos + 2]),
            static_cast<char>(data[pos + 3]),
            '\0'
        };
        const uint32_t blockSize = readLe32(data, pos + 4);
        pos += 8;
        if (pos + blockSize > data.size())
            throw FormatException("Truncated SZX chunk");

        const auto chunk = data.subspan(pos, blockSize);
        pos += blockSize;

        if (std::strcmp(id, "Z80R") == 0 && chunk.size() >= 37) {
            regs.AF  = readLe16(chunk, 0);
            regs.BC  = readLe16(chunk, 2);
            regs.DE  = readLe16(chunk, 4);
            regs.HL  = readLe16(chunk, 6);
            regs.AF_ = readLe16(chunk, 8);
            regs.BC_ = readLe16(chunk, 10);
            regs.DE_ = readLe16(chunk, 12);
            regs.HL_ = readLe16(chunk, 14);
            regs.IX  = readLe16(chunk, 16);
            regs.IY  = readLe16(chunk, 18);
            regs.SP  = readLe16(chunk, 20);
            regs.PC  = readLe16(chunk, 22);
            regs.I   = chunk[24];
            regs.setFullR(chunk[25]);
            regs.IFF1 = chunk[26];
            regs.IFF2 = chunk[27];
            regs.IM   = chunk[28] & 0x03;
            regs.halted = (chunk[34] & 0x02) != 0;
            if (chunk.size() >= 37)
                regs.WZ = readLe16(chunk, 35);
        } else if (std::strcmp(id, "SPCR") == 0 && chunk.size() >= 8) {
            state.ula.setBorderColor(chunk[0] & 0x07);
            state.machine.setPagingPorts(chunk[1], chunk[2], state.ctx, state.ula);
        } else if (std::strcmp(id, "RAMP") == 0 && chunk.size() >= 3) {
            const uint16_t flags = readLe16(chunk, 0);
            const uint8_t pageNo = chunk[2];
            const auto rawPage = chunk.subspan(3);
            const std::vector<uint8_t> unpacked = (flags & 0x01)
                ? zlibDecompress(rawPage, 16384)
                : std::vector<uint8_t>(rawPage.begin(), rawPage.end());

            if (ram.count16kPages() <= 3) {
                uint16_t addr = 0;
                if (pageNo == 5) addr = 0x4000;
                else if (pageNo == 2) addr = 0x8000;
                else if (pageNo == 0) addr = 0xC000;
                else continue;
                for (std::size_t i = 0; i < 16384 && i < unpacked.size(); ++i)
                    state.ctx.mmu.poke(static_cast<uint16_t>(addr + i), unpacked[i]);
            } else if (pageNo < ram.count16kPages()) {
                ram.loadData(static_cast<std::size_t>(pageNo) * 16384, unpacked);
            }
        }
    }
}

std::vector<uint8_t> SnapshotFormats::saveSzx(const SnapshotMachineState &state)
{
    StateWriter w(65536);
    // Header: 'ZXST', major 1, minor 4, machineId (1 = 48K, 2 = 128K), flags 0
    w.writeTag("ZXST");
    w.write<uint8_t>(1);
    w.write<uint8_t>(4);
    const auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    w.write<uint8_t>(ram.count16kPages() > 3 ? 2 : 1);
    w.write<uint8_t>(0);

    // Z80R chunk (37 bytes)
    const auto &regs = state.ctx.cpu.registers();
    w.writeTag("Z80R");
    w.write<uint32_t>(37);
    w.write<uint16_t>(regs.AF);
    w.write<uint16_t>(regs.BC);
    w.write<uint16_t>(regs.DE);
    w.write<uint16_t>(regs.HL);
    w.write<uint16_t>(regs.AF_);
    w.write<uint16_t>(regs.BC_);
    w.write<uint16_t>(regs.DE_);
    w.write<uint16_t>(regs.HL_);
    w.write<uint16_t>(regs.IX);
    w.write<uint16_t>(regs.IY);
    w.write<uint16_t>(regs.SP);
    w.write<uint16_t>(regs.PC);
    w.write<uint8_t>(regs.I);
    w.write<uint8_t>(regs.fullR());
    w.write<uint8_t>(regs.IFF1);
    w.write<uint8_t>(regs.IFF2);
    w.write<uint8_t>(regs.IM);
    w.write<uint32_t>(state.ctx.clock.frameTstates());
    w.write<uint8_t>(0);
    w.write<uint8_t>(regs.halted ? 0x02 : 0x00);
    w.write<uint16_t>(regs.WZ);

    // SPCR chunk (8 bytes)
    w.writeTag("SPCR");
    w.write<uint32_t>(8);
    w.write<uint8_t>(state.ula.borderColor());
    w.write<uint8_t>(state.machine.lastPort7ffd());
    w.write<uint8_t>(state.machine.lastPort1ffd());
    w.write<uint8_t>(0);
    w.write<uint32_t>(0);

    // RAMP chunks (zlib compressed)
    const std::size_t pages = std::min<std::size_t>(ram.count16kPages(), 8);
    for (std::size_t p = 0; p < pages; ++p) {
        const uint8_t pageNo = (pages == 3)
            ? static_cast<uint8_t>((p == 0) ? 5 : (p == 1 ? 2 : 0))
            : static_cast<uint8_t>(p);
        const auto compressed = zlibCompress(ram.span16k(p));
        w.writeTag("RAMP");
        w.write<uint32_t>(static_cast<uint32_t>(3 + compressed.size()));
        w.write<uint16_t>(0x0001); // Compressed flag
        w.write<uint8_t>(pageNo);
        w.writeBytes(compressed);
    }

    return w.takeBuffer();
}

void SnapshotFormats::loadNex(std::span<const uint8_t> data, SnapshotMachineState &state)
{
    if (data.size() < 512 || std::memcmp(data.data(), "Next", 4) != 0)
        throw FormatException("Invalid NEX file signature (expected 'Next')");

    const uint8_t loadingScreenFlags = data[10];
    const uint8_t borderColor = data[11];
    const uint16_t sp = readLe16(data, 12);
    const uint16_t pc = readLe16(data, 14);

    state.ula.setBorderColor(borderColor & 0x07);
    state.ctx.cpu.registers().SP = sp;
    state.ctx.cpu.registers().PC = pc;

    std::size_t pos = 512;
    // Skip optional 512-byte palette if present (bit 7 of loadingScreenFlags == 0 when screen present)
    if (loadingScreenFlags != 0) {
        if ((loadingScreenFlags & 0x80) == 0)
            pos += 512;
        if (loadingScreenFlags & 0x01)      pos += 49152; // Layer 2 screen
        else if (loadingScreenFlags & 0x02) pos += 6912;  // ULA screen
        else if (loadingScreenFlags & 0x04) pos += 12288; // LoRes screen
        else if (loadingScreenFlags & 0x08) pos += 12288; // HiRes screen
        else if (loadingScreenFlags & 0x10) pos += 12288; // HiColour screen
    }

    // NEX bank order: 5, 2, 0, 1, 3, 4, 6, 7, 8..111
    static constexpr std::array<uint8_t, 8> kFirstEightOrder = {5, 2, 0, 1, 3, 4, 6, 7};
    auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());

    for (std::size_t i = 0; i < 112; ++i) {
        const uint8_t bank16k = (i < 8) ? kFirstEightOrder[i] : static_cast<uint8_t>(i);
        if (data[18 + bank16k] != 0) {
            if (pos + 16384 > data.size())
                throw FormatException("Truncated NEX RAM bank data");
            if (bank16k < ram.count16kPages()) {
                ram.loadData(static_cast<std::size_t>(bank16k) * 16384, data.subspan(pos, 16384));
            }
            pos += 16384;
        }
    }
}

std::vector<uint8_t> SnapshotFormats::decompressZ80Block(std::span<const uint8_t> src, std::size_t expectedSize, bool hasEndMarker)
{
    std::vector<uint8_t> out;
    out.reserve(expectedSize);
    std::size_t i = 0;
    const std::size_t limit = hasEndMarker && src.size() >= 4 ? (src.size() - 4) : src.size();

    while (i < limit && out.size() < expectedSize) {
        if (i + 3 < src.size() && src[i] == 0xED && src[i + 1] == 0xED) {
            const uint8_t count = src[i + 2];
            const uint8_t value = src[i + 3];
            out.insert(out.end(), count, value);
            i += 4;
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

} // namespace zxkit
