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

#pragma once

#include "zxkit/bus/IoBus.h"
#include "zxkit/core/Clock.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/cpu/Im2Controller.h"
#include "zxkit/cpu/Z80Registers.h"
#include "zxkit/memory/ContentionModel.h"
#include "zxkit/memory/Mmu.h"

#include <bitset>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zxkit {

class Z80Cpu {
public:
    /// Bitmask flags gating optional CPU lifecycle hooks.
    /// When m_activeHooksMask == 0, the hot execution loop pays only a single branch.
    struct HookFlags {
        static constexpr uint32_t None          = 0x0000;
        static constexpr uint32_t PreM1         = 0x0001; // Automap before opcode fetch (DivMMC, IF1, Beta128, +D, Multiface)
        static constexpr uint32_t PostM1        = 0x0002; // Late automap / unmap after M1 fetch
        static constexpr uint32_t RfshEdge      = 0x0004; // R7 bit-7 refresh edge hook (CoBra 1 CP/M flips)
        static constexpr uint32_t Retn          = 0x0008; // RETN / RETI hook (Multiface, DivMMC, IM2 daisy-chain)
        static constexpr uint32_t TapeTrap      = 0x0010; // ROM tape load/save trap
        static constexpr uint32_t Debugger      = 0x0020; // Breakpoint / trace / step hook
        static constexpr uint32_t EvenM1        = 0x0040; // Scorpion ZS 256 even-T-state M1 alignment
        static constexpr uint32_t MagicBreak    = 0x0080; // ED FF / DD 01 magic breakpoints
    };

    using AddressHookFn   = std::function<void(uint16_t pc)>;
    /// The 256-byte pages of the address space (bit n: n * 256 to n * 256 + 255).
    using PcPages = std::bitset<256>;
    using M1ByteHookFn    = std::function<void(uint16_t pc, uint8_t opcode)>;
    using RfshHookFn      = std::function<void(uint8_t r7Bit)>;
    using RetnHookFn      = std::function<void(bool isReti)>;
    using TapeTrapFn      = std::function<bool(uint16_t pc, bool isLoad)>;
    using DebuggerHookFn  = std::function<bool(uint16_t pc)>;

    Z80Cpu(Clock &clock, Mmu &mmu, ContentionModel &contention, IoBus &ioBus, Im2Controller &im2);

    void reset(bool hard = true) noexcept;

    /// Executes a single Z80 / Z80N instruction (or services a pending NMI/INT)
    /// and returns the elapsed T-states.
    uint32_t stepInstruction();

    /// Executes instructions until `m_clock.frameBaseTstates() >= targetFrameTstates`
    /// (the frame position in base 3.5MHz T-states, so turbo speeds run more instructions)
    /// or until the debugger traps (`m_trapped == true`).
    void runUntil(uint32_t targetFrameTstates);

    // ULA /INT is a level pulse. Interrupt generators which latch individual
    // events (e.g. Next legacy interrupts) consume the event on acknowledge.
    void setInterruptRetrigger(bool enabled) noexcept { m_interruptRetrigger = enabled; }

    void requestNmi(bool stackless = false) noexcept;
    /// elapsedTstates locates an already asserted pulse (e.g. a crossed frame edge).
    void requestInterrupt(uint8_t vector = 0xFF, uint32_t elapsedTstates = 0) noexcept;

    Z80Registers &registers() noexcept { return m_regs; }
    const Z80Registers &registers() const noexcept { return m_regs; }
    void setRegisters(const Z80Registers &regs) noexcept { m_regs = regs; }

    void setZ80nEnabled(bool enabled) noexcept { m_z80nEnabled = enabled; }
    bool isZ80nEnabled() const noexcept { return m_z80nEnabled; }

    void setCmos(bool isCmos) noexcept { m_isCmos = isCmos; }
    bool isCmos() const noexcept { return m_isCmos; }

    void setEvenM1(bool enabled) noexcept
    {
        setHookFlag(HookFlags::EvenM1, enabled);
    }

    void setMagicBreakpointEnabled(bool enabled) noexcept
    {
        setHookFlag(HookFlags::MagicBreak, enabled);
    }

    /// Hooks registered with an `owner` (usually the peripheral id) are
    /// removed together by `removeHooks(owner)`, e.g. when it is detached.
    /// A pre-M1 hook runs only for the instructions fetched in its `pages`: it runs on every
    /// instruction, so a hook watching a few addresses should name their pages (and change them
    /// with `setPreM1HookPages` when what it watches changes).
    void addPreM1Hook(AddressHookFn fn, std::string owner = {}, PcPages pages = PcPages{}.set())
    {
        m_preM1Hooks.push_back({std::move(owner), std::move(fn), pages});
        updatePreM1Pages();
    }

    /// The pages the pre-M1 hooks of `owner` run for (they may change from inside the hook).
    void setPreM1HookPages(std::string_view owner, PcPages pages) noexcept
    {
        for (auto &h : m_preM1Hooks) {
            if (h.owner == owner)
                h.pages = pages;
        }
        updatePreM1Pages();
    }

    void addPostM1Hook(M1ByteHookFn fn, std::string owner = {})
    {
        m_postM1Hooks.push_back({std::move(owner), std::move(fn)});
        setHookFlag(HookFlags::PostM1, true);
    }

    void setRfshHook(RfshHookFn fn)
    {
        m_rfshHook = std::move(fn);
        setHookFlag(HookFlags::RfshEdge, static_cast<bool>(m_rfshHook));
    }

    void addRetnHook(RetnHookFn fn, std::string owner = {})
    {
        m_retnHooks.push_back({std::move(owner), std::move(fn)});
        setHookFlag(HookFlags::Retn, true);
    }

    void removeHooks(std::string_view owner)
    {
        const auto byOwner = [owner](const auto &h) { return h.owner == owner; };
        std::erase_if(m_preM1Hooks, byOwner);
        std::erase_if(m_postM1Hooks, byOwner);
        std::erase_if(m_retnHooks, byOwner);
        updatePreM1Pages();
        setHookFlag(HookFlags::PostM1, !m_postM1Hooks.empty());
        setHookFlag(HookFlags::Retn, !m_retnHooks.empty());
    }

    void setTapeTrapHook(TapeTrapFn fn)
    {
        m_tapeTrapHook = std::move(fn);
        setHookFlag(HookFlags::TapeTrap, static_cast<bool>(m_tapeTrapHook));
    }

    void setDebuggerHook(DebuggerHookFn fn)
    {
        m_debuggerHook = std::move(fn);
        setHookFlag(HookFlags::Debugger, static_cast<bool>(m_debuggerHook));
    }

    void clearPeripheralHooks()
    {
        m_preM1Hooks.clear();
        m_preM1Pages.reset();
        m_postM1Hooks.clear();
        m_rfshHook = nullptr;
        m_retnHooks.clear();
        setHookFlag(HookFlags::PreM1 | HookFlags::PostM1 | HookFlags::RfshEdge | HookFlags::Retn, false);
    }

    /// The debugger hook costs a call per instruction: it runs only while enabled
    /// (and set). `setDebuggerHook()` enables it.
    void setDebuggerHookEnabled(bool enabled) noexcept
    {
        setHookFlag(HookFlags::Debugger, enabled && static_cast<bool>(m_debuggerHook));
    }

    bool isTrapped() const noexcept { return m_trapped; }
    /// Stops `runUntil()` once the current instruction completes (e.g. a watchpoint hit).
    void trap() noexcept { m_trapped = true; }
    void clearTrap() noexcept { m_trapped = false; }

    void configureStacklessRetn(bool active, uint16_t returnAddress) noexcept
    {
        m_stacklessRetnActive = active;
        m_stacklessRetnAddress = returnAddress;
    }

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    void updatePreM1Pages() noexcept
    {
        m_preM1Pages.reset();
        for (const auto &h : m_preM1Hooks)
            m_preM1Pages |= h.pages;
        setHookFlag(HookFlags::PreM1, m_preM1Pages.any());
    }

    void setHookFlag(uint32_t flag, bool enabled) noexcept
    {
        if (enabled)
            m_activeHooksMask |= flag;
        else
            m_activeHooksMask &= ~flag;
    }

    uint8_t readByte(uint16_t addr);
    uint8_t readByteInternal(uint16_t addr);
    void writeByte(uint16_t addr, uint8_t val);
    uint8_t readPort(uint16_t port);
    void writePort(uint16_t port, uint8_t val);

    void contendRead(uint16_t addr, uint32_t time);
    void contendReadNoMreq(uint16_t addr, uint32_t time);
    void contendWriteNoMreq(uint16_t addr, uint32_t time);
    void contendPortPreIo(uint16_t port);
    void contendPortPostIo(uint16_t port);

    bool serviceNmi();
    bool serviceInterrupt();
    bool handleRetn(bool isReti);
    void executeZ80n(uint8_t extOpcode);

    Clock &m_clock;
    Mmu &m_mmu;
    ContentionModel &m_contention;
    IoBus &m_ioBus;
    Im2Controller &m_im2;

    Z80Registers m_regs{};
    uint32_t m_activeHooksMask = HookFlags::None;
    int64_t m_interruptsEnabledAt = -1;
    uint64_t m_intRequestedAt = 0;
    uint8_t m_lastR7 = 0;
    bool m_iff2Read = false;
    bool m_nmiPending = false;
    bool m_nmiStackless = false;
    bool m_interruptRetrigger = true;
    bool m_intPending = false;
    uint8_t m_intVector = 0xFF;
    bool m_stacklessRetnActive = false;
    uint16_t m_stacklessRetnAddress = 0;
    bool m_z80nEnabled = false;
    bool m_isCmos = false;
    bool m_trapped = false;

    template <typename Fn>
    struct OwnedHook {
        std::string owner;
        Fn fn;
    };

    struct PreM1Hook {
        std::string owner;
        AddressHookFn fn;
        PcPages pages;
    };

    std::vector<PreM1Hook> m_preM1Hooks;
    PcPages m_preM1Pages; // the union of the hooks' pages
    std::vector<OwnedHook<M1ByteHookFn>> m_postM1Hooks;
    RfshHookFn m_rfshHook;
    std::vector<OwnedHook<RetnHookFn>> m_retnHooks;
    TapeTrapFn m_tapeTrapHook;
    DebuggerHookFn m_debuggerHook;
};

} // namespace zxkit
