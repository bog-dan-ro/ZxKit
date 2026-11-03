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
#include "zxkit/audio/AudioMixer.h"

#include "zxkit/core/Exception.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/machines/ZxNextMachine.h"
#include "zxkit/media/Compression.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace zxkit {

namespace {

constexpr std::array<JoystickType, 9> szxJoystickTypes{
    JoystickType::Kempston, JoystickType::Fuller, JoystickType::Cursor,
    JoystickType::Sinclair1, JoystickType::Sinclair2, JoystickType::None,
    JoystickType::Timex1, JoystickType::Timex2, JoystickType::None};

// Z80 v3 programmable keys: left, right, down, up, fire. The low byte
// selects a keyboard row; the high byte selects a column within that row.
constexpr std::array<std::array<uint16_t, 5>, 3> z80JoystickKeys{{
    {0x1003, 0x0404, 0x1004, 0x0804, 0x0104}, // Cursor
    {0x1004, 0x0804, 0x0404, 0x0204, 0x0104}, // Sinclair 1
    {0x0103, 0x0203, 0x0403, 0x0803, 0x1003}  // Sinclair 2
}};

unsigned z80Version(std::span<const uint8_t> data)
{
    if (data.size() < 30) throw FormatException("Truncated Z80 header");
    if (data[6] || data[7]) return 1;
    if (data.size() < 32) throw FormatException("Truncated Z80 extended header");
    const auto size = data[30] | (unsigned(data[31]) << 8);
    if ((size != 23 && size != 54 && size != 55) || data.size() < 32 + size)
        throw FormatException("Unsupported Z80 extended header");
    return size == 23 ? 2 : 3;
}

} // namespace

SnapshotJoystickInfo SnapshotFormats::joystickInfo(std::span<const uint8_t> data, std::string_view ext)
{
    if (ext == ".z80") {
        const auto version = z80Version(data);
        switch (data[29] >> 6) {
        case 0: return {true, JoystickType::Cursor};
        case 1: return {true, JoystickType::Kempston};
        case 3: return {true, JoystickType::Sinclair1};
        case 2:
            if (version < 3) return {true, JoystickType::Sinclair2};
            for (std::size_t i = 0; i < z80JoystickKeys.size(); ++i) {
                bool match = true;
                for (std::size_t key = 0; key < 5; ++key)
                    match &= readLe16(data, 63 + key * 2) == z80JoystickKeys[i][key];
                if (match) return {true, std::array{JoystickType::Cursor, JoystickType::Sinclair1, JoystickType::Sinclair2}[i]};
            }
            return {true, std::nullopt};
        }
    }
    if (ext != ".szx") return {};
    if (data.size() < 8 || std::memcmp(data.data(), "ZXST", 4) || data[4] != 1)
        throw FormatException("Invalid SZX header");
    SnapshotJoystickInfo info;
    bool unsupported = false;
    auto add = [&](uint8_t value, bool keyboard) {
        // Spectrum+ cursor keys describe ordinary keyboard behavior, not an
        // AGF/Cursor joystick or an explicit disabled joystick declaration.
        if (keyboard && value == 5) return;
        info.present = true;
        if (value == 8) return;
        if (value >= szxJoystickTypes.size() || value == 5) {
            unsupported = true;
            return;
        }
        const auto type = szxJoystickTypes[value];
        if (info.type && info.type != type) unsupported = true;
        info.type = type;
    };
    std::size_t pos = 8;
    while (pos < data.size()) {
        if (data.size() - pos < 8) throw FormatException("Truncated SZX chunk header");
        const auto size = readLe32(data, pos + 4);
        const auto id = data.subspan(pos, 4);
        pos += 8;
        if (size > data.size() - pos) throw FormatException("Truncated SZX chunk");
        if (!std::memcmp(id.data(), "JOY\0", 4)) {
            if (size < 6) throw FormatException("Truncated SZX joystick setup");
            add(data[pos + 4], false);
            add(data[pos + 5], false);
        } else if (!std::memcmp(id.data(), "KEYB", 4)) {
            if (size < 4) throw FormatException("Truncated SZX keyboard setup");
            if (size >= 5) add(data[pos + 4], true);
        }
        pos += size;
    }
    if (unsupported) info.type.reset();
    else if (info.present && !info.type) info.type = JoystickType::None;
    return info;
}

bool SnapshotFormats::setJoystickInfo(std::vector<uint8_t> &data, std::string_view ext, JoystickType type)
{
    if (ext == ".z80") {
        const auto version = z80Version(data);
        uint8_t value;
        switch (type) {
        case JoystickType::Cursor: value = 0; break;
        case JoystickType::Kempston: case JoystickType::KempstonLoose: value = 1; break;
        case JoystickType::Sinclair1: value = 3; break;
        case JoystickType::Sinclair2: value = 2; break;
        default: return false;
        }
        data[29] = (data[29] & 0x3F) | (value << 6);
        if (version == 3 && value == 2) {
            for (std::size_t key = 0; key < 5; ++key) {
                writeLe16(data, 63 + key * 2, z80JoystickKeys[2][key]);
                writeLe16(data, 73 + key * 2, static_cast<uint16_t>('1' + key));
            }
        }
        return true;
    }
    if (ext != ".szx") return false;
    // Validate before changing any bytes, including unknown chunks and old KEYB blocks.
    joystickInfo(data, ext);
    if (type == JoystickType::KempstonLoose) type = JoystickType::Kempston;
    auto found = std::ranges::find(szxJoystickTypes, type);
    if (found == szxJoystickTypes.end()) return false;
    const auto value = type == JoystickType::None ? uint8_t(8) : static_cast<uint8_t>(found - szxJoystickTypes.begin());
    bool written = false;
    for (std::size_t pos = 8; pos < data.size();) {
        const auto size = readLe32(data, pos + 4);
        if (!std::memcmp(data.data() + pos, "JOY\0", 4)) {
            std::fill_n(data.begin() + pos + 8, 4, 0); // deprecated always-port-31 flag
            data[pos + 12] = value;
            data[pos + 13] = 8;
            written = true;
        } else if (!std::memcmp(data.data() + pos, "KEYB", 4) && size >= 5) {
            data[pos + 12] = 8; // preserve keyboard flags, remove a conflicting host mapping
        }
        pos += 8 + std::size_t(size);
    }
    if (!written) {
        const std::array<uint8_t, 14> chunk{'J','O','Y',0, 6,0,0,0, 0,0,0,0, value,8};
        data.insert(data.end(), chunk.begin(), chunk.end());
    }
    return true;
}

std::string_view SnapshotFormats::machineId(std::span<const uint8_t> data, std::string_view ext)
{
    if (ext == ".nex") {
        if (data.size() < 512 || std::memcmp(data.data(), "NextV1.", 7) || data[7] < '0' || data[7] > '2')
            throw FormatException("Invalid or unsupported NEX header");
        return "next";
    }
    if (ext == ".sna") {
        if (data.size() == 49179) return "48k";
        if (data.size() == 131103 || data.size() == 147487) return "128k";
        throw FormatException("Invalid SNA size");
    }
    if (ext == ".szx") {
        constexpr std::array<std::string_view,16> machines{"16k","48k","128k","plus2","plus2a","plus3","plus3e",
            "pentagon","tc2048","tc2068","scorpion","se","ts2068","pentagon512","pentagon1024","48k_ntsc"};
        if (data.size() < 8 || std::memcmp(data.data(), "ZXST", 4) || data[4] != 1 || data[6] >= machines.size())
            throw FormatException("Unsupported SZX header or machine");
        return machines[data[6]];
    }
    if (ext == ".z80") {
        if (data.size() < 30) throw FormatException("Truncated Z80 header");
        if (readLe16(data,6)) return "48k";
        if (data.size() < 55) throw FormatException("Truncated Z80 extended header");
        const auto length = readLe16(data,30);
        if ((length != 23 && length != 54 && length != 55) || data.size() < 32u + length)
            throw FormatException("Unsupported Z80 extended header");
        const auto mode = data[34];
        const bool modified = data[37] & 0x80;
        if (mode <= 1 || (length != 23 && mode == 3)) return modified ? "16k" : "48k";
        if ((length == 23 && (mode == 3 || mode == 4)) || (length != 23 && mode >= 4 && mode <= 6))
            return modified ? "plus2" : "128k";
        if (mode == 7 || mode == 8) return modified ? "plus2a" : "plus3";
        if (mode == 9) return "pentagon";
        if (mode == 10) return "scorpion";
        if (mode == 12) return "plus2";
        if (mode == 13) return "plus2a";
        if (mode == 14) return "tc2048";
        if (mode == 15) return "tc2068";
        if (mode == 128) return "ts2068";
        throw FormatException("Unsupported Z80 hardware mode");
    }
    throw FormatException("Unsupported snapshot format");
}

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

namespace {
std::vector<uint8_t> snaHeader(const SnapshotMachineState &state, uint16_t sp)
{
    const auto &regs = state.ctx.cpu.registers();
    std::vector<uint8_t> out(27,0);
    const auto put16 = [&](std::size_t at, uint16_t value) {
        out[at] = static_cast<uint8_t>(value); out[at+1] = static_cast<uint8_t>(value >> 8);
    };
    out[0] = regs.I;
    put16(1, regs.HL_);
    put16(3, regs.DE_);
    put16(5, regs.BC_);
    put16(7, regs.AF_);
    put16(9, regs.HL);
    put16(11, regs.DE);
    put16(13, regs.BC);
    put16(15, regs.IY);
    put16(17, regs.IX);
    out[19] = regs.IFF2 ? 0x04 : 0x00;
    out[20] = regs.fullR();
    put16(21, regs.AF);
    put16(23, sp);
    out[25] = regs.IM & 0x03;
    out[26] = state.ula.borderColor() & 0x07;

    return out;
}
} // namespace

std::vector<uint8_t> SnapshotFormats::saveSna48k(const SnapshotMachineState &state)
{
    const auto &regs = state.ctx.cpu.registers();
    const uint16_t sp = regs.SP - 2;
    if (sp < 0x4000 || static_cast<uint16_t>(sp + 1) < 0x4000)
        throw FormatException("48K SNA needs two writable stack bytes for PC; use SZX or Z80");
    auto out = snaHeader(state, sp);
    out.resize(49179);
    for (uint32_t i = 0; i < 49152; ++i)
        out[27 + i] = state.ctx.mmu.peek(static_cast<uint16_t>(0x4000 + i));

    if (sp >= 0x4000) {
        out[27 + (sp - 0x4000)] = static_cast<uint8_t>(regs.PC & 0xFF);
        if (static_cast<uint16_t>(sp + 1) >= 0x4000)
            out[27 + (static_cast<uint16_t>(sp + 1) - 0x4000)] = static_cast<uint8_t>(regs.PC >> 8);
    }
    return out;
}

std::vector<uint8_t> SnapshotFormats::saveSna128k(const SnapshotMachineState &state)
{
    const auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    if (ram.count16kPages() != 8 || (state.machine.id() != "128k" && state.machine.id() != "plus2" && state.machine.id() != "pentagon"))
        throw FormatException("128K SNA exports require a 128K/+2/Pentagon machine; use SZX for this model");
    // The common register header is identical, except that 128K SNA stores PC explicitly.
    auto out = snaHeader(state, state.ctx.cpu.registers().SP);
    const uint8_t top = state.machine.lastPort7ffd() & 7;
    const auto appendBank = [&](uint8_t bank) {
        const auto bytes = ram.span16k(bank);
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    appendBank(5); appendBank(2); appendBank(top);
    out.push_back(static_cast<uint8_t>(state.ctx.cpu.registers().PC));
    out.push_back(static_cast<uint8_t>(state.ctx.cpu.registers().PC >> 8));
    out.push_back(state.machine.lastPort7ffd());
    out.push_back(0); // TR-DOS not paged by this interchange export.
    for (uint8_t p = 0; p < 8; ++p)
        if (p != 5 && p != 2 && p != top) appendBank(p);
    return out;
}

std::vector<uint8_t> SnapshotFormats::saveZ80(const SnapshotMachineState &state)
{
    const auto id = state.machine.id();
    const auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    const auto pages = ram.count16kPages();
    uint8_t hardware;
    if (id == "48k") hardware = 0;
    else if (id == "128k") hardware = 4;
    else if (id == "plus2") hardware = 12;
    else if (id == "plus2a") hardware = 13;
    else if (id == "plus3" || id == "plus3e") hardware = 7;
    else if (id == "pentagon") hardware = 9;
    else throw FormatException("This model cannot be exported as Z80; use SZX");
    const auto &r = state.ctx.cpu.registers();
    std::vector<uint8_t> out(87, 0);
    out[0] = r.A(); out[1] = r.F();
    writeLe16(out, 2, r.BC); writeLe16(out, 4, r.HL); writeLe16(out, 8, r.SP);
    out[10] = r.I; out[11] = r.fullR() & 127;
    out[12] = (r.fullR() >> 7) | (state.ula.borderColor() << 1);
    writeLe16(out, 13, r.DE); writeLe16(out, 15, r.BC_); writeLe16(out, 17, r.DE_);
    writeLe16(out, 19, r.HL_); out[21] = r.AF_ >> 8; out[22] = static_cast<uint8_t>(r.AF_);
    writeLe16(out, 23, r.IY); writeLe16(out, 25, r.IX);
    // Host input is unknown to the core: use an unassigned v3 programmable
    // mapping until the consumer supplies a representable joystick preference.
    out[27] = r.IFF1; out[28] = r.IFF2; out[29] = r.IM | 0x80;
    writeLe16(out, 30, 55); writeLe16(out, 32, r.PC);
    out[34] = hardware; out[35] = state.machine.lastPort7ffd(); out[86] = state.machine.lastPort1ffd();
    out[61] = out[62] = 0xFF;
    if (state.ctx.audioMixer.ayChipCount()) {
        out[37] = 4;
        const auto &ay = state.ctx.audioMixer.activeAy();
        out[38] = ay.selectedRegister();
        std::ranges::copy(ay.registers(), out.begin() + 39);
    }
    const uint32_t quarter = state.ctx.clock.timings().tstatesPerFrame() / 4;
    const uint32_t t = state.ctx.clock.frameBaseTstates() % (quarter * 4);
    writeLe16(out, 55, static_cast<uint16_t>(quarter - 1 - t % quarter));
    out[57] = static_cast<uint8_t>((t / quarter + 3) % 4);
    for (std::size_t page = 0; page < pages; ++page) {
        // Uncompressed v3 pages avoid the ambiguous ED run escape entirely.
        out.push_back(0xFF); out.push_back(0xFF);
        out.push_back(pages == 3 ? std::array<uint8_t,3>{8,4,5}[page] : static_cast<uint8_t>(page + 3));
        const auto bytes = ram.span16k(page);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

std::vector<uint8_t> SnapshotFormats::saveNex(const SnapshotMachineState &state)
{
    const auto *next = dynamic_cast<const ZxNextMachine *>(&state.machine);
    if (!next) throw FormatException("NEX program export requires a ZX Spectrum Next");
    const auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    const auto &nr = next->nextReg();
    // NEX provides a fixed 128K-style entry map, not arbitrary MMU/register restoration.
    if (nr.peek(0x52) != 10 || nr.peek(0x53) != 11 || nr.peek(0x54) != 4 || nr.peek(0x55) != 5
        || (nr.peek(0x56) & 1) || nr.peek(0x57) != nr.peek(0x56) + 1)
        throw FormatException("NEX export needs banks 5/2 and one aligned entry bank mapped at 4000/8000/C000");
    std::vector<uint8_t> out(512, 0);
    std::memcpy(out.data(), "NextV1.2", 8);
    const auto count = std::min<std::size_t>(112, ram.count16kPages());
    out[8] = count > 48 ? 1 : 0; out[9] = static_cast<uint8_t>(count);
    out[11] = state.ula.borderColor();
    writeLe16(out, 12, state.ctx.cpu.registers().SP);
    writeLe16(out, 14, state.ctx.cpu.registers().PC);
    out[139] = nr.peek(0x56) / 2;
    for (std::size_t p = 0; p < count; ++p) out[18 + p] = 1;
    constexpr std::array<uint8_t,8> order{5,2,0,1,3,4,6,7};
    for (std::size_t p = 0; p < count; ++p) {
        const auto bytes = ram.span16k(p < 8 ? order[p] : p);
        out.insert(out.end(), bytes.begin(), bytes.end());
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
    if (extLen != 23 && extLen != 54 && extLen != 55)
        throw FormatException("Unsupported Z80 extended header length");
    const std::size_t blocksStart = 32 + extLen;
    if (blocksStart > data.size())
        throw FormatException("Invalid Z80 extended header length");

    if (extLen >= 54) {
        const uint32_t quarter = state.ctx.clock.timings().tstatesPerFrame() / 4;
        const uint32_t low = readLe16(data, 55);
        if (low >= quarter || data[57] > 3) throw FormatException("Invalid Z80 frame position");
        state.ctx.clock.setFrameTstates(((data[57] + 1) % 4) * quarter + quarter - 1 - low);
    }
    regs.PC = readLe16(data, 32);
    const uint8_t hwMode = data[34];
    const uint8_t port7ffd = data[35];
    const bool is128kMode = (extLen == 23 && hwMode >= 3) || (extLen >= 54 && hwMode >= 4);
    if (is128kMode) {
        state.machine.setPagingPorts(port7ffd, extLen >= 55 ? data[86] : 0, state.ctx, state.ula);
    }

    if (extLen >= 23 && blocksStart >= 55 && (is128kMode || (data[37] & 4))) {
        state.ctx.audioMixer.setAyChipCount(std::max<uint8_t>(1, state.ctx.audioMixer.ayChipCount()));
        auto &ay = state.ctx.audioMixer.activeAy();
        for (uint8_t i = 0; i < 16; ++i) { ay.selectRegister(i); ay.writeRegister(data[39 + i]); }
        ay.selectRegister(data[38]);
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
            regs.eiDelayed = (chunk[34] & 1) != 0;
            regs.Q = regs.lastQ = (chunk[34] & 4) ? regs.F() : 0;
            state.ctx.clock.setFrameTstates(readLe32(chunk, 29));
            if (chunk.size() >= 37)
                regs.WZ = readLe16(chunk, 35);
        } else if (std::strcmp(id, "AY") == 0 && chunk.size() >= 18) {
            state.ctx.audioMixer.setAyChipCount(std::max<uint8_t>(1, state.ctx.audioMixer.ayChipCount()));
            auto &ay = state.ctx.audioMixer.activeAy();
            for (uint8_t i = 0; i < 16; ++i) { ay.selectRegister(i); ay.writeRegister(chunk[2 + i]); }
            ay.selectRegister(chunk[1]);
        } else if (std::strcmp(id, "SPCR") == 0 && chunk.size() >= 8) {
            state.ula.setBorderColor(chunk[0] & 0x07);
            state.machine.setPagingPorts(chunk[1], chunk[2], state.ctx, state.ula);
        } else if (std::strcmp(id, "RAMP") == 0 && chunk.size() >= 3) {
            const uint16_t flags = readLe16(chunk, 0);
            const uint8_t pageNo = chunk[2];
            const auto rawPage = chunk.subspan(3);
            const std::vector<uint8_t> unpacked = (flags & 0x01)
                ? zlibDecompressBounded(rawPage, 16384)
                : std::vector<uint8_t>(rawPage.begin(), rawPage.end());

            if (unpacked.size() != 16384) throw FormatException("Invalid SZX RAM page size");
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
    w.write<uint8_t>(5);
    const auto &ram = state.ctx.mmu.bank(state.machine.ramBankId());
    constexpr std::array<std::string_view,16> machines{"16k","48k","128k","plus2","plus2a","plus3","plus3e",
        "pentagon","tc2048","tc2068","scorpion","se","ts2068","pentagon512","pentagon1024","48k_ntsc"};
    const auto found = std::ranges::find(machines, state.machine.id());
    const uint8_t machine = found != machines.end() ? static_cast<uint8_t>(found - machines.begin())
        : (ram.count16kPages() > 3 ? 2 : 1); // Next has its native extension in Emulator.
    w.write(machine);
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
    w.write<uint8_t>((regs.eiDelayed ? 1 : 0) | (regs.halted ? 2 : 0) | (regs.Q ? 4 : 0));
    w.write<uint16_t>(regs.WZ);

    // SPCR chunk (8 bytes)
    w.writeTag("SPCR");
    w.write<uint32_t>(8);
    w.write<uint8_t>(state.ula.borderColor());
    w.write<uint8_t>(state.machine.lastPort7ffd());
    w.write<uint8_t>(state.machine.lastPort1ffd());
    w.write<uint8_t>(0);
    w.write<uint32_t>(0);

    if (state.ctx.audioMixer.ayChipCount()) {
        w.writeTag(std::string_view("AY\0\0", 4));
        w.write<uint32_t>(18);
        w.write<uint8_t>(ram.count16kPages() <= 3 ? 2 : 0);
        const auto &ay = state.ctx.audioMixer.activeAy();
        w.write(ay.selectedRegister());
        w.writeBytes(ay.registers());
    }

    // RAMP chunks (zlib compressed)
    const std::size_t pages = std::min<std::size_t>(ram.count16kPages(), 8);
    for (std::size_t p = 0; p < pages; ++p) {
        const uint8_t pageNo = (pages <= 3)
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

    if (!dynamic_cast<ZxNextMachine *>(&state.machine))
        throw FormatException("NEX files require a ZX Spectrum Next");
    if (std::memcmp(data.data() + 4, "V1.", 3) || data[7] < '0' || data[7] > '2')
        throw FormatException("Unsupported NEX version (expected V1.0 to V1.2)");
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
        if ((loadingScreenFlags & 0x05) && (loadingScreenFlags & 0x80) == 0)
            pos += 512;
        if (loadingScreenFlags & 0x01)      pos += 49152; // Layer 2 screen
        if (loadingScreenFlags & 0x02) pos += 6912;  // ULA screen
        if (loadingScreenFlags & 0x04) pos += 12288; // LoRes screen
        if (loadingScreenFlags & 0x08) pos += 12288; // HiRes screen
        if (loadingScreenFlags & 0x10) pos += 12288; // HiColour screen
    }

    if (pos > data.size()) throw FormatException("Truncated NEX loading screen");
    auto &next = static_cast<ZxNextMachine &>(state.machine);
    if (data[139] >= 112) throw FormatException("Invalid NEX entry bank");
    next.nextReg().write(0x52, 10); next.nextReg().write(0x53, 11);
    next.nextReg().write(0x54, 4); next.nextReg().write(0x55, 5);
    next.nextReg().write(0x56, data[139] * 2); next.nextReg().write(0x57, data[139] * 2 + 1);

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
            out.insert(out.end(), std::min<std::size_t>(count, expectedSize - out.size()), value);
            i += 4;
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

} // namespace zxkit
