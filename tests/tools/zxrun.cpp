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

// zxrun: headless scripted runner, used to check that a machine boots
// (e.g. HC-2000 into CP/M, or the ZX Spectrum Next into NextZXOS).
//
//   zxrun --machine hc2000 --roms DIR [--roms DIR...] [--sdcard IMG | --sdcard-ro IMG]
//         [--io-trace MASK:VALUE] STEP...
//
// --io-trace prints every port access where (port & MASK) == VALUE (hex).
//
// Steps:
//   wait:N            run N frames
//   key:K[+K...]      tap a key chord (e.g. key:CS+SS, key:ENTER)
//   type:TEXT         tap each character of TEXT ('_' is SPACE, '~' is ENTER)
//   text:TEXT         type ASCII TEXT: capitals with CAPS SHIFT, symbols with
//                     SYMBOL SHIFT (e.g. text:.nexload games/x.nex), '\n' is ENTER
//   disk:D:FILE       insert a disk image into drive D (0 = A:)
//   load:FILE         load a snapshot / tape / any supported media file
//   tapeopt:K=V,...   tape options, 0 or 1: traps, autoplay, accel, sound (e.g. tapeopt:traps=0)
//   play | stop       press the tape deck's play / stop
//   tape              print the tape position and state
//   blocks            print the tape's blocks
//   dis:ADDR[,N]      disassemble N instructions at ADDR (hex)
//   mem:ADDR[,N]      dump N bytes of memory at ADDR (hex)
//   untilstop:N       run up to N frames, until the tape stops
//   shot:FILE.png     save a screenshot of the last frame
//   regs              print the CPU registers
//   nrtrace           from now on, print the ZX Next NextREG writes
//   trace:N           record the last N executed instructions
//   break:ADDR[,N]    stop executing at ADDR (hex), after N hits (e.g. break:0000)
//   count:ADDR        count the times ADDR (hex) runs; counts prints the counts
//   dumptrace:N       print the last N recorded instructions
//   out:PORT,VALUE    write a port (hex)
//   trdos             boot TR-DOS (Pentagon, Scorpion, Beta 128), like FUSE's disk autoload
//   reset | hardreset | nmi

#include <zxkit/zxkit.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <vector>

using namespace zxkit;

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const std::map<std::string, ZxKey> &keyNames()
{
    static const std::map<std::string, ZxKey> names = [] {
        std::map<std::string, ZxKey> m = {
            {"CS", ZxKey::CapsShift}, {"SS", ZxKey::SymbolShift},
            {"ENTER", ZxKey::Enter}, {"SPACE", ZxKey::Space},
            {"0", ZxKey::Num0}, {"1", ZxKey::Num1}, {"2", ZxKey::Num2}, {"3", ZxKey::Num3},
            {"4", ZxKey::Num4}, {"5", ZxKey::Num5}, {"6", ZxKey::Num6}, {"7", ZxKey::Num7},
            {"8", ZxKey::Num8}, {"9", ZxKey::Num9},
        };
        const std::pair<char, ZxKey> letters[] = {
            {'A', ZxKey::A}, {'B', ZxKey::B}, {'C', ZxKey::C}, {'D', ZxKey::D}, {'E', ZxKey::E},
            {'F', ZxKey::F}, {'G', ZxKey::G}, {'H', ZxKey::H}, {'I', ZxKey::I}, {'J', ZxKey::J},
            {'K', ZxKey::K}, {'L', ZxKey::L}, {'M', ZxKey::M}, {'N', ZxKey::N}, {'O', ZxKey::O},
            {'P', ZxKey::P}, {'Q', ZxKey::Q}, {'R', ZxKey::R}, {'S', ZxKey::S}, {'T', ZxKey::T},
            {'U', ZxKey::U}, {'V', ZxKey::V}, {'W', ZxKey::W}, {'X', ZxKey::X}, {'Y', ZxKey::Y},
            {'Z', ZxKey::Z},
        };
        for (auto [c, k] : letters)
            m[std::string(1, c)] = k;
        return m;
    }();
    return names;
}

std::vector<ZxKey> parseChord(std::string_view chord)
{
    std::vector<ZxKey> keys;
    std::size_t pos = 0;
    while (pos <= chord.size()) {
        const auto next = chord.find('+', pos);
        std::string name(chord.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
        for (auto &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto it = keyNames().find(name);
        if (it == keyNames().end())
            throw InvalidArgumentException(std::format("unknown key '{}'", name));
        keys.push_back(it->second);
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    return keys;
}

// The key chord producing an ASCII character on a Spectrum keyboard
std::vector<ZxKey> chordForChar(char c)
{
    static const std::map<char, char> symbolShifted = {
        {'!', '1'}, {'@', '2'}, {'#', '3'}, {'$', '4'}, {'%', '5'}, {'&', '6'}, {'\'', '7'}, {'(', '8'},
        {')', '9'}, {'_', '0'}, {'<', 'R'}, {'>', 'T'}, {';', 'O'}, {'"', 'P'}, {'^', 'H'}, {'-', 'J'},
        {'+', 'K'}, {'=', 'L'}, {':', 'Z'}, {'?', 'C'}, {'/', 'V'}, {'*', 'B'}, {',', 'N'}, {'.', 'M'},
    };
    if (c == ' ')
        return {ZxKey::Space};
    if (c == '\n')
        return {ZxKey::Enter};
    if (std::isdigit(static_cast<unsigned char>(c)) || std::islower(static_cast<unsigned char>(c)))
        return parseChord(std::string(1, c));
    if (std::isupper(static_cast<unsigned char>(c))) {
        auto keys = parseChord(std::string(1, c));
        keys.insert(keys.begin(), ZxKey::CapsShift);
        return keys;
    }
    if (const auto it = symbolShifted.find(c); it != symbolShifted.end()) {
        auto keys = parseChord(std::string(1, it->second));
        keys.insert(keys.begin(), ZxKey::SymbolShift);
        return keys;
    }
    throw InvalidArgumentException(std::format("can't type '{}'", c));
}

ZxNextMachine &nextMachine(Emulator &emulator)
{
    if (emulator.machine().id() != "next")
        throw InvalidArgumentException("this step needs the ZX Next");
    return static_cast<ZxNextMachine &>(emulator.machine());
}

class Runner {
public:
    explicit Runner(Emulator &emulator) : m_emulator(emulator) {}

    void frames(uint32_t n)
    {
        m_lastFrame = m_emulator.runFrames(n);
        if (auto pause = m_emulator.takePauseEvent())
            std::println("stopped at {:04X}", pause->pc);
    }

    void tap(const std::vector<ZxKey> &keys)
    {
        for (auto k : keys)
            m_emulator.setKeyState(k, true);
        frames(4);
        for (auto k : keys)
            m_emulator.setKeyState(k, false);
        frames(4);
    }

    void shot(const std::filesystem::path &path)
    {
        if (!m_lastFrame)
            frames(1);
        std::ofstream f(path, std::ios::binary);
        const auto png = encodePng(*m_lastFrame.readLock());
        f.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
        std::println("saved {}", path.string());
    }

private:
    Emulator &m_emulator;
    VideoFramePtr m_lastFrame;
};

} // namespace

int main(int argc, char **argv)
{
    std::string machine = "48k";
    std::vector<std::filesystem::path> romDirs;
    std::filesystem::path sdcard;
    bool sdcardReadOnly = false;
    std::vector<std::pair<uint16_t, uint16_t>> ioTraces;
    std::vector<std::string> steps;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--machine" && i + 1 < argc)
            machine = argv[++i];
        else if (arg == "--roms" && i + 1 < argc)
            romDirs.emplace_back(argv[++i]);
        else if ((arg == "--sdcard" || arg == "--sdcard-ro") && i + 1 < argc) {
            sdcardReadOnly = (arg == "--sdcard-ro");
            sdcard = argv[++i];
        }
        else if (arg == "--io-trace" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto sep = spec.find(':');
            ioTraces.emplace_back(static_cast<uint16_t>(std::stoul(spec.substr(0, sep), nullptr, 16)),
                                  static_cast<uint16_t>(std::stoul(spec.substr(sep + 1), nullptr, 16)));
        }
        else
            steps.emplace_back(arg);
    }

#ifdef ZXKIT_TEST_ROM_DIR
    romDirs.emplace_back(ZXKIT_TEST_ROM_DIR);
#endif
    EmulatorConfig cfg;
    cfg.romResolver = [romDirs](std::string_view name, std::size_t) -> std::vector<uint8_t> {
        for (const auto &dir : romDirs) {
            if (auto data = readFile(dir / name); !data.empty())
                return data;
        }
        return {};
    };

    try {
        // Headless: the emulator runs right here, no Engine thread needed
        Emulator emulator(std::move(cfg));
        auto &eng = emulator;
        if (!sdcard.empty())
            emulator.insertSdCard(sdcard, sdcardReadOnly);
        emulator.selectMachine(machine);
        if (!ioTraces.empty()) {
            {
                eng.ioBus().addObserver([ioTraces, &eng](uint16_t port, uint8_t value, bool isWrite) {
                    for (auto [mask, match] : ioTraces) {
                        if ((port & mask) == match) {
                            std::println("{:04X} {} {:04X} {:02X}", eng.cpu().registers().PC,
                                         isWrite ? "OUT" : "IN ", port, value);
                            break;
                        }
                    }
                });
            }
        }
        Runner run(emulator);
        std::vector<std::pair<uint16_t, std::shared_ptr<uint64_t>>> counters;

        for (const auto &step : steps) {
            const auto colon = step.find(':');
            const std::string cmd = step.substr(0, colon);
            const std::string arg = colon == std::string::npos ? std::string{} : step.substr(colon + 1);
            if (cmd == "wait") {
                run.frames(static_cast<uint32_t>(std::stoul(arg)));
            } else if (cmd == "key") {
                run.tap(parseChord(arg));
            } else if (cmd == "type") {
                for (char c : arg) {
                    if (c == '_')
                        run.tap({ZxKey::Space});
                    else if (c == '~')
                        run.tap({ZxKey::Enter});
                    else
                        run.tap(parseChord(std::string(1, c)));
                }
            } else if (cmd == "text") {
                for (std::size_t i = 0; i < arg.size(); ++i) {
                    if (arg[i] == '\\' && i + 1 < arg.size() && arg[i + 1] == 'n') {
                        run.tap({ZxKey::Enter});
                        ++i;
                    } else {
                        run.tap(chordForChar(arg[i]));
                    }
                }
            } else if (cmd == "disk") {
                const auto sep = arg.find(':');
                const auto drive = static_cast<std::size_t>(std::stoul(arg.substr(0, sep)));
                const std::filesystem::path file = arg.substr(sep + 1);
                emulator.insertDisk(drive, readFile(file), file.extension().string());
            } else if (cmd == "load") {
                emulator.loadMediaFile(arg);
            } else if (cmd == "tapeopt") {
                TapeOptions options = emulator.tapeDeck().options();
                std::size_t start = 0;
                while (start < arg.size()) {
                    const std::size_t end = std::min(arg.find(',', start), arg.size());
                    const std::string item = arg.substr(start, end - start);
                    const auto eq = item.find('=');
                    const std::string key = item.substr(0, eq);
                    const bool on = eq != std::string::npos && item.substr(eq + 1) != "0";
                    if (key == "traps")
                        options.romTraps = on;
                    else if (key == "autoplay")
                        options.autoPlay = on;
                    else if (key == "accel")
                        options.accelerateLoaders = on;
                    else if (key == "sound")
                        options.loadingSound = on;
                    else
                        std::println(stderr, "unknown tape option '{}'", key);
                    start = end + 1;
                }
                emulator.tapeDeck().setOptions(options);
            } else if (cmd == "play" || cmd == "stop") {
                emulator.tapeDeck().setPlaying(cmd == "play");
            } else if (cmd == "dis") {
                const auto comma = arg.find(',');
                const auto address = static_cast<uint16_t>(std::stoul(arg.substr(0, comma), nullptr, 16));
                const std::size_t count = comma == std::string::npos ? 16 : std::stoul(arg.substr(comma + 1));
                for (const auto &inst : emulator.disassemble(address, count))
                    std::println("{:04X}  {}", inst.address, inst.mnemonic);
            } else if (cmd == "mem") {
                const auto comma = arg.find(',');
                const auto address = static_cast<uint16_t>(std::stoul(arg.substr(0, comma), nullptr, 16));
                const std::size_t count = comma == std::string::npos ? 16 : std::stoul(arg.substr(comma + 1));
                const auto bytes = emulator.readMemory(address, count);
                for (std::size_t i = 0; i < bytes.size(); i += 16) {
                    std::string line = std::format("{:04X} ", static_cast<uint16_t>(address + i));
                    for (std::size_t j = i; j < std::min(i + 16, bytes.size()); ++j)
                        line += std::format(" {:02X}", bytes[j]);
                    std::println("{}", line);
                }
            } else if (cmd == "blocks") {
                const auto &blocks = emulator.tapeDeck().blocks();
                for (std::size_t i = 0; i < blocks.size(); ++i)
                    std::println("{:3} {:02X} {}", i, static_cast<int>(blocks[i].type), blocks[i].description);
            } else if (cmd == "tape") {
                const auto &deck = emulator.tapeDeck();
                std::println("tape: block {}/{} {} frame {}", deck.currentBlockIndex(), deck.blocks().size(),
                             deck.isPlaying() ? "playing" : "stopped", emulator.clock().frameCount());
            } else if (cmd == "untilstop") {
                const uint32_t limit = static_cast<uint32_t>(std::stoul(arg));
                uint32_t n = 0;
                bool seenPlaying = emulator.tapeDeck().isPlaying();
                for (; n < limit; ++n) {
                    run.frames(1);
                    if (emulator.tapeDeck().isPlaying())
                        seenPlaying = true;
                    else if (seenPlaying)
                        break;
                }
                std::println("untilstop: {} frames, block {}/{}", n, emulator.tapeDeck().currentBlockIndex(),
                             emulator.tapeDeck().blocks().size());
            } else if (cmd == "shot") {
                run.shot(arg);
            } else if (cmd == "nrtrace") {
                nextMachine(eng).nextReg().setWatchCallback([&eng](uint8_t reg, uint8_t oldVal, uint8_t newVal) {
                    std::println("{:04X} NR {:02X} = {:02X} (was {:02X})", eng.cpu().registers().PC, reg, newVal, oldVal);
                });
            } else if (cmd == "trace") {
                eng.debugger().setTraceEnabled(true, std::stoul(arg));
            } else if (cmd == "break") {
                // break:ADDR[,N]: stops at the (N+1)th time ADDR runs
                const auto comma = arg.find(',');
                Breakpoint bp;
                bp.address = static_cast<uint16_t>(std::stoul(arg.substr(0, comma), nullptr, 16));
                if (comma != std::string::npos) {
                    auto hits = std::make_shared<uint64_t>(0);
                    const uint64_t ignore = std::stoull(arg.substr(comma + 1));
                    bp.condition = [hits, ignore](const Z80Registers &) { return ++*hits > ignore; };
                }
                emulator.addBreakpoint(std::move(bp));
            } else if (cmd == "count") {
                // count:ADDR: counts the times ADDR runs, `counts` prints them
                const auto address = static_cast<uint16_t>(std::stoul(arg, nullptr, 16));
                auto hits = std::make_shared<uint64_t>(0);
                counters.emplace_back(address, hits);
                Breakpoint bp;
                bp.address = address;
                bp.condition = [hits](const Z80Registers &) { ++*hits; return false; };
                emulator.addBreakpoint(std::move(bp));
            } else if (cmd == "counts") {
                for (const auto &[address, hits] : counters)
                    std::println("count {:04X}: {}", address, *hits);
            } else if (cmd == "dumptrace") {
                {
                    const std::size_t n = std::stoul(arg);
                    const auto &log = eng.debugger().traceLog();
                    const std::size_t first = log.size() > n ? log.size() - n : 0;
                    for (std::size_t i = first; i < log.size(); ++i) {
                        const auto &t = log[i];
                        const auto inst = Disassembler::disassembleOne([&eng](uint16_t a) { return eng.mmu().peek(a); }, t.pc, true);
                        std::println("{:04X}  {:<18} AF={:04X} BC={:04X} DE={:04X} HL={:04X} SP={:04X}",
                                     t.pc, inst.mnemonic, t.af, t.bc, t.de, t.hl, t.sp);
                    }
                }
            } else if (cmd == "nextregs") {
                {
                    auto &next = nextMachine(eng);
                    std::string line;
                    for (uint8_t r : {0x07, 0x12, 0x14, 0x15, 0x22, 0x23, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x8C, 0x8E, 0xC0})
                        line += std::format("NR{:02X}={:02X} ", r, next.nextReg().read(r));
                    std::println("{}divmmc={}", line, next.isDivMmcMapped());
                }
            } else if (cmd == "regs") {
                const auto r = emulator.registers();
                std::println("PC={:04X} SP={:04X} AF={:04X} BC={:04X} DE={:04X} HL={:04X} IX={:04X} IY={:04X} I={:02X} IM={}",
                             r.PC, r.SP, r.AF, r.BC, r.DE, r.HL, r.IX, r.IY, r.I, r.IM);
            } else if (cmd == "out") {
                // out:PORT,VALUE (hex)
                const auto comma = arg.find(',');
                emulator.ioBus().write(static_cast<uint16_t>(std::stoul(arg.substr(0, comma), nullptr, 16)),
                                       static_cast<uint8_t>(std::stoul(arg.substr(comma + 1), nullptr, 16)));
            } else if (cmd == "trdos") {
                // Boot TR-DOS like FUSE's disk autoload: a boot program if the disk has none, reset,
                // 48K BASIC ROM, TR-DOS paged in, PC = 0
                if (auto disk = emulator.diskDrive(0))
                    insertTrDosBootLoader(disk->get());
                emulator.reset(ResetKind::Hard);
                emulator.ioBus().write(0x7FFD, 0x10);
                if (auto beta = emulator.findPeripheral("beta128"))
                    static_cast<Beta128Peripheral &>(beta->get()).setPaged(emulator.context(), true);
                else if (auto *pentagon = dynamic_cast<PentagonMachine *>(&emulator.machine()))
                    pentagon->beta().setPaged(emulator.context(), true);
                else if (auto *scorpion = dynamic_cast<ScorpionMachine *>(&emulator.machine()))
                    scorpion->beta().setPaged(emulator.context(), true);
                Z80Registers regs = emulator.registers();
                regs.PC = 0;
                emulator.setRegisters(regs);
            } else if (cmd == "reset") {
                emulator.reset(ResetKind::Soft);
            } else if (cmd == "hardreset") {
                emulator.reset(ResetKind::Hard);
            } else if (cmd == "nmi") {
                emulator.triggerNmi();
            } else {
                std::println(stderr, "unknown step '{}'", step);
                return 2;
            }
        }
    } catch (const std::exception &e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
