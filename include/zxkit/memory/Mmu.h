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

#include "zxkit/core/StateStream.h"
#include "zxkit/memory/MemoryBank.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zxkit {

/// 64 x 1KB Page-Table MMU supporting:
/// - Fast page-table Read/Write dispatch (zero branching on unguarded writes via sink page)
/// - 1KB, 2KB, 4KB, 8KB, and 16KB banking with independent Read & Write mapping
/// - Contended page flags per 1KB slot
/// - Prioritized /ROMCS & peripheral memory overlays (Interface 1, Multiface, DivMMC, Beta128, etc.)
/// - Custom per-slot read/write traps and debugger memory watchpoints
class Mmu
{
public:
    static constexpr std::size_t kPageShift = 10; // 1 KB = 2^10
    static constexpr std::size_t kPageSize = 1u << kPageShift; // 1024 bytes
    static constexpr uint16_t kPageMask = kPageSize - 1; // 0x03FF
    static constexpr std::size_t kSlotCount = 65536 / kPageSize; // 64 slots

    struct SlotFlags
    {
        static constexpr uint8_t None = 0x00;
        static constexpr uint8_t Contended = 0x01;
        static constexpr uint8_t Writable = 0x02;
        static constexpr uint8_t WatchRead = 0x04;
        static constexpr uint8_t WatchWrite = 0x08;
        static constexpr uint8_t CustomRead = 0x10;
        static constexpr uint8_t CustomWrite = 0x20;
        static constexpr uint8_t VideoWrite = 0x40;
        static constexpr uint8_t SlowRead = WatchRead | CustomRead;
        static constexpr uint8_t SlowWrite = WatchWrite | CustomWrite | VideoWrite;
    };

    struct SlotMapping
    {
        BankId bankId = 0;
        uint16_t readPage1k = 0;
        uint16_t writeBankId = 0;
        uint16_t writePage1k = 0;
        bool writable = false;
        bool contended = false;
    };

    using CustomReadHandler = std::function<uint8_t(uint16_t addr, uint8_t underlyingByte)>;
    using CustomWriteHandler = std::function<void(uint16_t addr, uint8_t val)>;
    using WatchpointCallback = std::function<void(uint16_t addr, uint8_t val, bool isWrite)>;

    Mmu();
    // The page tables point into the MMU itself
    Mmu(const Mmu &) = delete;
    Mmu &operator=(const Mmu &) = delete;

    BankId registerBank(std::string name, std::size_t sizeBytes, bool writable,
                        bool saveToSnapshot = true);
    MemoryBank &bank(BankId id);
    const MemoryBank &bank(BankId id) const;
    BankId findBank(std::string_view name) const noexcept;
    std::span<const MemoryBank> banks() const noexcept
    {
        return m_banks;
    }

    void clear() noexcept;
    void resetMappings() noexcept;

    /// Maps `count1k` contiguous 1KB pages starting at `startAddr` for reading and/or writing.
    void mapPages1k(uint16_t startAddr, BankId bankId, std::size_t firstPage1k, std::size_t count1k,
                    bool mapRead = true, bool mapWrite = true, bool contended = false,
                    bool forceReadOnly = false);

    void map16k(uint16_t startAddr, BankId bankId, std::size_t page16k, bool contended = false,
                bool forceReadOnly = false);
    void map16kReadWrite(uint16_t startAddr, BankId bankId, std::size_t page16k, bool mapRead,
                         bool mapWrite, bool contended = false, bool forceReadOnly = false);
    void map8k(uint16_t startAddr, BankId bankId, std::size_t page8k, bool contended = false,
               bool forceReadOnly = false);
    void map8kReadWrite(uint16_t startAddr, BankId bankId, std::size_t page8k, bool mapRead,
                        bool mapWrite, bool contended = false, bool forceReadOnly = false);
    void map4k(uint16_t startAddr, BankId bankId, std::size_t page4k, bool contended = false,
               bool forceReadOnly = false);
    void map2k(uint16_t startAddr, BankId bankId, std::size_t page2k, bool contended = false,
               bool forceReadOnly = false);
    void map1k(uint16_t startAddr, BankId bankId, std::size_t page1k, bool contended = false,
               bool forceReadOnly = false);

    /// Unmaps `count1k` pages: reads return 0xFF (floating bus), writes are discarded.
    void unmapPages1k(uint16_t startAddr, std::size_t count1k) noexcept;

    /// Peripheral overlay mapping (e.g. /ROMCS for Interface 1, DivMMC, Multiface, Beta128, +D).
    void mapOverlay1k(uint16_t startAddr, BankId bankId, std::size_t firstPage1k,
                      std::size_t count1k, bool writable);

    void clearOverlay(uint16_t startAddr, std::size_t count1k) noexcept;
    void setContendedRange(uint16_t startAddr, std::size_t count1k, bool contended) noexcept;

    bool isContended(uint16_t addr) const noexcept
    {
        return (m_flags[addr >> kPageShift] & SlotFlags::Contended) != 0;
    }

    bool isOverlayActiveAt(uint16_t addr) const noexcept
    {
        return m_overlayActive[addr >> kPageShift];
    }

    /// Fast CPU memory read.
    uint8_t read(uint16_t addr) const
    {
        const std::size_t slot = addr >> kPageShift;
        const uint8_t val = m_readPages[slot][addr & kPageMask];
        if (m_flags[slot] & SlotFlags::SlowRead) [[unlikely]] {
            return handleSlowRead(slot, addr, val);
        }
        return val;
    }

    /// Fast CPU memory write.
    void write(uint16_t addr, uint8_t val)
    {
        const std::size_t slot = addr >> kPageShift;
        if (m_flags[slot] & SlotFlags::SlowWrite) [[unlikely]] {
            handleSlowWrite(slot, addr, val);
            return;
        }
        m_writePages[slot][addr & kPageMask] = val;
    }

    /// Side-effect-free peek for debugger / disassembler / snapshot saver.
    uint8_t peek(uint16_t addr) const noexcept
    {
        return m_readPages[addr >> kPageShift][addr & kPageMask];
    }

    /// Direct poke bypassing ROM protection if targeting a valid bank (for debugger / loaders).
    void poke(uint16_t addr, uint8_t val, bool allowRomWrite = false) noexcept;

    const SlotMapping &effectiveMapping(std::size_t slot) const noexcept
    {
        return m_overlayActive[slot] ? m_overlayMap[slot] : m_baseMap[slot];
    }

    /// Called before writes to the first 7KB of each 16KB page of a video RAM bank.
    void setVideoWriteCallback(BankId bank, std::function<void()> callback);

    void setWatchpointCallback(WatchpointCallback cb);
    void setSlotWatchFlags(std::size_t slot, bool watchRead, bool watchWrite) noexcept;
    void setCustomSlotHandlers(std::size_t slot, CustomReadHandler rd, CustomWriteHandler wr);

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    void refreshSlot(std::size_t slot) noexcept;
    uint8_t handleSlowRead(std::size_t slot, uint16_t addr, uint8_t val) const;
    void handleSlowWrite(std::size_t slot, uint16_t addr, uint8_t val);

    std::vector<MemoryBank> m_banks;
    std::unordered_map<std::string, BankId> m_nameToId;

    using ReadPage = std::span<const uint8_t, kPageSize>;
    using WritePage = std::span<uint8_t, kPageSize>;

    // Every slot always maps a page: the unmapped page reads 0xFF, the sink page swallows writes
    std::array<uint8_t, kPageSize> m_unmappedPage{};
    std::array<uint8_t, kPageSize> m_sinkPage{};

    template <typename Page> static std::array<Page, kSlotCount> allSlots(Page page) noexcept
    {
        return [page]<std::size_t... I>(std::index_sequence<I...>) {
            return std::array<Page, kSlotCount>{ ((void)I, page)... };
        }(std::make_index_sequence<kSlotCount>{});
    }

    std::array<ReadPage, kSlotCount> m_readPages = allSlots(ReadPage(m_unmappedPage));
    std::array<WritePage, kSlotCount> m_writePages = allSlots(WritePage(m_sinkPage));
    std::array<uint8_t, kSlotCount> m_flags{};

    std::array<SlotMapping, kSlotCount> m_baseMap{};
    std::array<SlotMapping, kSlotCount> m_overlayMap{};
    std::array<bool, kSlotCount> m_overlayActive{};

    std::array<CustomReadHandler, kSlotCount> m_customReads{};
    std::array<CustomWriteHandler, kSlotCount> m_customWrites{};
    WatchpointCallback m_watchpointCb;
    BankId m_videoBank = 0;
    std::function<void()> m_videoWriteCallback;
};

} // namespace zxkit
