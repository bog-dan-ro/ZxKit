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

#include "zxkit/debug/Disassembler.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

namespace zxkit {

namespace {

constexpr std::array<std::string_view, 8> kReg8 = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
constexpr std::array<std::string_view, 4> kReg16 = {"BC", "DE", "HL", "SP"};
constexpr std::array<std::string_view, 4> kReg16Af = {"BC", "DE", "HL", "AF"};
constexpr std::array<std::string_view, 8> kCond = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
constexpr std::array<std::string_view, 8> kAlu = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
constexpr std::array<std::string_view, 8> kRot = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
constexpr std::array<uint8_t, 8> kImModes = {0, 0, 1, 2, 0, 0, 1, 2};

// Decodes one instruction. Operands are fetched in their byte order, never inside
// a std::format call: the order its arguments are evaluated in is unspecified.
class Decoder {
public:
    Decoder(const Disassembler::ReadByteFn &readByte, uint16_t pc, bool z80n)
        : m_readByte(readByte)
        , m_pc(pc)
        , m_z80n(z80n)
    {
        m_out.address = pc;
        m_out.length = 0;
    }

    DisasmInstruction decode()
    {
        const uint8_t b0 = fetch8();
        switch (b0) {
        case 0xCB:
            decodeCb();
            break;
        case 0xED:
            decodeEd();
            break;
        case 0xDD:
        case 0xFD:
            decodeIndexed(b0 == 0xDD ? "IX" : "IY");
            break;
        default:
            decodeMain(b0, {});
            break;
        }
        return std::move(m_out);
    }

private:
    uint8_t fetch8()
    {
        const uint8_t v = m_readByte(static_cast<uint16_t>(m_pc + m_out.length));
        if (m_out.length < m_out.bytes.size())
            m_out.bytes[m_out.length] = v;
        ++m_out.length;
        return v;
    }

    uint16_t fetch16()
    {
        const uint8_t lo = fetch8();
        const uint8_t hi = fetch8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }

    void set(std::string mnemonic, bool stepOver = false)
    {
        m_out.mnemonic = std::move(mnemonic);
        m_out.isStepOverCandidate = stepOver;
    }

    static std::string displaced(std::string_view idx, int8_t d)
    {
        return std::format("({}{}${:02X})", idx, d < 0 ? '-' : '+', d < 0 ? -d : d);
    }

    void decodeCb()
    {
        const uint8_t op = fetch8();
        const uint8_t x = op >> 6;
        const uint8_t y = (op >> 3) & 7;
        const std::string_view r = kReg8[op & 7];
        if (x == 0)
            set(std::format("{} {}", kRot[y], r));
        else
            set(std::format("{} {},{}", x == 1 ? "BIT" : x == 2 ? "RES" : "SET", y, r));
    }

    // DD CB d op / FD CB d op: the displacement comes before the opcode. Apart from BIT,
    // the result is also copied into a register when the low bits don't select (HL).
    void decodeIndexedCb(std::string_view idx)
    {
        fetch8(); // CB
        const auto d = static_cast<int8_t>(fetch8());
        const uint8_t op = fetch8();
        const uint8_t x = op >> 6;
        const uint8_t y = (op >> 3) & 7;
        const uint8_t z = op & 7;
        const std::string mem = displaced(idx, d);
        const std::string copy = (x != 1 && z != 6) ? std::format(",{}", kReg8[z]) : std::string{};
        if (x == 0)
            set(std::format("{} {}{}", kRot[y], mem, copy));
        else
            set(std::format("{} {},{}{}", x == 1 ? "BIT" : x == 2 ? "RES" : "SET", y, mem, copy));
    }

    void decodeIndexed(std::string_view idx)
    {
        const uint8_t next = m_readByte(static_cast<uint16_t>(m_pc + 1));
        if (next == 0xCB) {
            decodeIndexedCb(idx);
            return;
        }
        // A prefix followed by another prefix, or by an instruction that doesn't use
        // HL, H, L or (HL), has no effect: show it on its own
        if (next != 0xDD && next != 0xED && next != 0xFD) {
            fetch8();
            if (decodeMain(next, idx))
                return;
        }
        m_out = {};
        m_out.address = m_pc;
        m_out.length = 1;
        m_out.bytes[0] = idx == "IX" ? 0xDD : 0xFD;
        set(std::format("DEFB ${:02X}", m_out.bytes[0]));
    }

    void decodeEd()
    {
        const uint8_t op = fetch8();
        if (m_z80n && decodeZ80n(op))
            return;

        const uint8_t y = (op >> 3) & 7;
        const uint8_t p = y >> 1;
        switch (op) {
        case 0x47: set("LD I,A"); return;
        case 0x4F: set("LD R,A"); return;
        case 0x57: set("LD A,I"); return;
        case 0x5F: set("LD A,R"); return;
        case 0x67: set("RRD"); return;
        case 0x6F: set("RLD"); return;
        case 0x4D: set("RETI"); return;
        case 0xA0: set("LDI"); return;
        case 0xA1: set("CPI"); return;
        case 0xA2: set("INI"); return;
        case 0xA3: set("OUTI"); return;
        case 0xA8: set("LDD"); return;
        case 0xA9: set("CPD"); return;
        case 0xAA: set("IND"); return;
        case 0xAB: set("OUTD"); return;
        case 0xB0: set("LDIR", true); return;
        case 0xB1: set("CPIR", true); return;
        case 0xB2: set("INIR", true); return;
        case 0xB3: set("OTIR", true); return;
        case 0xB8: set("LDDR", true); return;
        case 0xB9: set("CPDR", true); return;
        case 0xBA: set("INDR", true); return;
        case 0xBB: set("OTDR", true); return;
        default: break;
        }

        if ((op & 0xC0) == 0x40) {
            switch (op & 7) {
            case 0: // IN r,(C); ED 70 only sets the flags
                set(y == 6 ? std::string("IN (C)") : std::format("IN {},(C)", kReg8[y]));
                return;
            case 1: // OUT (C),r; ED 71 outputs 0
                set(y == 6 ? std::string("OUT (C),0") : std::format("OUT (C),{}", kReg8[y]));
                return;
            case 2:
                set(std::format("{} HL,{}", (y & 1) ? "ADC" : "SBC", kReg16[p]));
                return;
            case 3: {
                const uint16_t nn = fetch16();
                set((y & 1) ? std::format("LD {},(${:04X})", kReg16[p], nn)
                            : std::format("LD (${:04X}),{}", nn, kReg16[p]));
                return;
            }
            case 4: set("NEG"); return;  // and its undocumented mirrors
            case 5: set("RETN"); return; // ditto (ED 4D, RETI, is handled above)
            case 6: set(std::format("IM {}", kImModes[y])); return;
            default: break;
            }
        }
        set(std::format("DEFB $ED,${:02X}", op));
    }

    bool decodeZ80n(uint8_t op)
    {
        switch (op) {
        case 0x23: set("SWAPNIB"); return true;
        case 0x24: set("MIRROR A"); return true;
        case 0x27: set(std::format("TEST ${:02X}", fetch8())); return true;
        case 0x28: set("BSLA DE,B"); return true;
        case 0x29: set("BSRA DE,B"); return true;
        case 0x2A: set("BSRL DE,B"); return true;
        case 0x2B: set("BSRF DE,B"); return true;
        case 0x2C: set("BRLC DE,B"); return true;
        case 0x30: set("MUL D,E"); return true;
        case 0x31: set("ADD HL,A"); return true;
        case 0x32: set("ADD DE,A"); return true;
        case 0x33: set("ADD BC,A"); return true;
        case 0x34: set(std::format("ADD HL,${:04X}", fetch16())); return true;
        case 0x35: set(std::format("ADD DE,${:04X}", fetch16())); return true;
        case 0x36: set(std::format("ADD BC,${:04X}", fetch16())); return true;
        case 0x8A: { // big endian
            const uint8_t hi = fetch8();
            const uint8_t lo = fetch8();
            set(std::format("PUSH ${:04X}", (hi << 8) | lo));
            return true;
        }
        case 0x90: set("OUTINB"); return true;
        case 0x91: {
            const uint8_t reg = fetch8();
            const uint8_t value = fetch8();
            set(std::format("NEXTREG ${:02X},${:02X}", reg, value));
            return true;
        }
        case 0x92: set(std::format("NEXTREG ${:02X},A", fetch8())); return true;
        case 0x93: set("PIXELDN"); return true;
        case 0x94: set("PIXELAD"); return true;
        case 0x95: set("SETAE"); return true;
        case 0x98: set("JP (C)"); return true;
        case 0xA4: set("LDIX"); return true;
        case 0xA5: set("LDWS"); return true;
        case 0xAC: set("LDDX"); return true;
        case 0xB4: set("LDIRX", true); return true;
        case 0xB6: set("LDIRSCALE", true); return true;
        case 0xB7: set("LDPIRX", true); return true;
        case 0xBC: set("LDDRX", true); return true;
        default: return false;
        }
    }

    // The unprefixed opcodes. With `idx` (IX or IY, after a DD/FD prefix) HL becomes the
    // index register, (HL) becomes (IX+d) and H/L become IXH/IXL. Returns whether the
    // index register was used, i.e. whether the prefix has an effect.
    bool decodeMain(uint8_t op, std::string_view idx)
    {
        bool usedIndex = false;
        const auto hl = [&]() -> std::string_view {
            if (idx.empty())
                return "HL";
            usedIndex = true;
            return idx;
        };
        const auto memHl = [&]() -> std::string {
            if (idx.empty())
                return "(HL)";
            usedIndex = true;
            return displaced(idx, static_cast<int8_t>(fetch8()));
        };
        // H and L are not replaced in an instruction that also uses (IX+d)
        const auto reg8 = [&](uint8_t r, bool withMemory = false) -> std::string {
            if (r == 6)
                return memHl();
            if (!idx.empty() && !withMemory && (r == 4 || r == 5)) {
                usedIndex = true;
                return std::format("{}{}", idx, r == 4 ? 'H' : 'L');
            }
            return std::string(kReg8[r]);
        };
        const auto rp = [&](uint8_t p) -> std::string_view { return p == 2 ? hl() : kReg16[p]; };
        const auto rp2 = [&](uint8_t p) -> std::string_view { return p == 2 ? hl() : kReg16Af[p]; };
        const auto relative = [&]() {
            const auto rel = static_cast<int8_t>(fetch8());
            return static_cast<uint16_t>(m_pc + m_out.length + rel);
        };

        switch (op) {
        case 0x00: set("NOP"); return usedIndex;
        case 0x02: set("LD (BC),A"); return usedIndex;
        case 0x07: set("RLCA"); return usedIndex;
        case 0x08: set("EX AF,AF'"); return usedIndex;
        case 0x0A: set("LD A,(BC)"); return usedIndex;
        case 0x0F: set("RRCA"); return usedIndex;
        case 0x10: set(std::format("DJNZ ${:04X}", relative()), true); return usedIndex;
        case 0x12: set("LD (DE),A"); return usedIndex;
        case 0x17: set("RLA"); return usedIndex;
        case 0x18: set(std::format("JR ${:04X}", relative())); return usedIndex;
        case 0x1A: set("LD A,(DE)"); return usedIndex;
        case 0x1F: set("RRA"); return usedIndex;
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38: {
            const uint16_t target = relative();
            set(std::format("JR {},${:04X}", kCond[(op >> 3) & 3], target));
            return usedIndex;
        }
        case 0x22: {
            const uint16_t nn = fetch16();
            set(std::format("LD (${:04X}),{}", nn, hl()));
            return usedIndex;
        }
        case 0x27: set("DAA"); return usedIndex;
        case 0x2A: {
            const uint16_t nn = fetch16();
            set(std::format("LD {},(${:04X})", hl(), nn));
            return usedIndex;
        }
        case 0x2F: set("CPL"); return usedIndex;
        case 0x32: set(std::format("LD (${:04X}),A", fetch16())); return usedIndex;
        case 0x37: set("SCF"); return usedIndex;
        case 0x3A: set(std::format("LD A,(${:04X})", fetch16())); return usedIndex;
        case 0x3F: set("CCF"); return usedIndex;
        case 0x76: set("HALT"); return usedIndex;
        case 0xC3: set(std::format("JP ${:04X}", fetch16())); return usedIndex;
        case 0xC9: set("RET"); return usedIndex;
        case 0xCD: set(std::format("CALL ${:04X}", fetch16()), true); return usedIndex;
        case 0xD3: set(std::format("OUT (${:02X}),A", fetch8())); return usedIndex;
        case 0xD9: set("EXX"); return usedIndex;
        case 0xDB: set(std::format("IN A,(${:02X})", fetch8())); return usedIndex;
        case 0xE3: set(std::format("EX (SP),{}", hl())); return usedIndex;
        case 0xE9: set(std::format("JP ({})", hl())); return usedIndex;
        case 0xEB: set("EX DE,HL"); return usedIndex; // never IX
        case 0xF3: set("DI"); return usedIndex;
        case 0xF9: set(std::format("LD SP,{}", hl())); return usedIndex;
        case 0xFB: set("EI"); return usedIndex;
        default: break;
        }

        const uint8_t x = op >> 6;
        const uint8_t y = (op >> 3) & 7;
        const uint8_t z = op & 7;
        const uint8_t p = y >> 1;
        const uint8_t q = y & 1;

        if (x == 0) {
            if (z == 1) {
                if (q == 0) {
                    const std::string_view r = rp(p);
                    set(std::format("LD {},${:04X}", r, fetch16()));
                } else {
                    const std::string_view dst = hl();
                    set(std::format("ADD {},{}", dst, rp(p)));
                }
            } else if (z == 3) {
                set(std::format("{} {}", q ? "DEC" : "INC", rp(p)));
            } else if (z == 4 || z == 5) {
                set(std::format("{} {}", z == 4 ? "INC" : "DEC", reg8(y)));
            } else if (z == 6) {
                const std::string r = reg8(y); // (IX+d): the displacement comes before n
                set(std::format("LD {},${:02X}", r, fetch8()));
            }
        } else if (x == 1) {
            const bool withMemory = y == 6 || z == 6;
            const std::string dst = reg8(y, withMemory);
            const std::string src = reg8(z, withMemory);
            set(std::format("LD {},{}", dst, src));
        } else if (x == 2) {
            set(std::format("{}{}", kAlu[y], reg8(z)));
        } else {
            switch (z) {
            case 0: set(std::format("RET {}", kCond[y])); break;
            case 1: set(std::format("POP {}", rp2(p))); break;
            case 2: set(std::format("JP {},${:04X}", kCond[y], fetch16())); break;
            case 4: set(std::format("CALL {},${:04X}", kCond[y], fetch16()), true); break;
            case 5: set(std::format("PUSH {}", rp2(p))); break;
            case 6: set(std::format("{}${:02X}", kAlu[y], fetch8())); break;
            case 7: set(std::format("RST ${:02X}", y * 8), true); break;
            default: set(std::format("DEFB ${:02X}", op)); break;
            }
        }
        return usedIndex;
    }

    const Disassembler::ReadByteFn &m_readByte;
    uint16_t m_pc;
    bool m_z80n;
    DisasmInstruction m_out;
};

} // namespace

DisasmInstruction Disassembler::disassembleOne(const ReadByteFn &readByte, uint16_t pc, bool z80n)
{
    return Decoder(readByte, pc, z80n).decode();
}

std::vector<DisasmInstruction> Disassembler::disassembleCount(const ReadByteFn &readByte,
                                                              uint16_t startPc,
                                                              std::size_t count,
                                                              bool z80n)
{
    std::vector<DisasmInstruction> result;
    result.reserve(count);
    uint16_t pc = startPc;
    for (std::size_t i = 0; i < count; ++i) {
        auto inst = disassembleOne(readByte, pc, z80n);
        pc = static_cast<uint16_t>(pc + inst.length);
        result.push_back(std::move(inst));
    }
    return result;
}

} // namespace zxkit
