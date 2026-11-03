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

#include "zxkit/memory/Mmu.h"

#include <algorithm>
#include <utility>

namespace zxkit {

Mmu::Mmu()
{
    m_unmappedPage.fill(0xFF);
    m_sinkPage.fill(0x00);
    resetMappings();
}

BankId Mmu::registerBank(std::string name, std::size_t sizeBytes, bool writable, bool saveToSnapshot)
{
    const BankId id = static_cast<BankId>(m_banks.size() + 1);
    m_nameToId[name] = id;
    m_banks.emplace_back(id, std::move(name), sizeBytes, writable, saveToSnapshot);
    return id;
}

MemoryBank &Mmu::bank(BankId id)
{
    if (id == 0 || id > m_banks.size()) [[unlikely]]
        throw InvalidArgumentException("Invalid MemoryBank ID");
    return m_banks[id - 1];
}

const MemoryBank &Mmu::bank(BankId id) const
{
    if (id == 0 || id > m_banks.size()) [[unlikely]]
        throw InvalidArgumentException("Invalid MemoryBank ID");
    return m_banks[id - 1];
}

BankId Mmu::findBank(std::string_view name) const noexcept
{
    if (auto it = m_nameToId.find(std::string(name)); it != m_nameToId.end())
        return it->second;
    return 0;
}

void Mmu::clear() noexcept
{
    m_videoBank = 0;
    m_videoWriteCallback = nullptr;
    m_banks.clear();
    m_nameToId.clear();
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        m_customReads[i] = nullptr;
        m_customWrites[i] = nullptr;
    }
    resetMappings();
}

void Mmu::resetMappings() noexcept
{
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        m_readPages[i]  = ReadPage(m_unmappedPage);
        m_writePages[i] = WritePage(m_sinkPage);
        m_flags[i]    = SlotFlags::None;
        m_baseMap[i]  = {};
        m_overlayMap[i] = {};
        m_overlayActive[i] = false;
    }
}

void Mmu::mapPages1k(uint16_t startAddr,
                     BankId bankId,
                     std::size_t firstPage1k,
                     std::size_t count1k,
                     bool mapRead,
                     bool mapWrite,
                     bool contended,
                     bool forceReadOnly)
{
    const std::size_t startSlot = startAddr >> kPageShift;
    if (startSlot + count1k > kSlotCount)
        throw InvalidArgumentException("MMU mapping exceeds 64KB address space");

    auto &b = bank(bankId);
    const bool canWrite = b.isWritable() && !forceReadOnly;

    for (std::size_t i = 0; i < count1k; ++i) {
        const std::size_t slot = startSlot + i;
        const std::size_t p1k  = firstPage1k + i;
        if (mapRead) {
            m_baseMap[slot].bankId     = bankId;
            m_baseMap[slot].readPage1k = static_cast<uint16_t>(p1k);
            m_baseMap[slot].contended  = contended;
        }
        if (mapWrite) {
            m_baseMap[slot].writeBankId = bankId;
            m_baseMap[slot].writePage1k = static_cast<uint16_t>(p1k);
            m_baseMap[slot].writable    = canWrite;
        }
        refreshSlot(slot);
    }
}

void Mmu::map16k(uint16_t startAddr, BankId bankId, std::size_t page16k, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page16k * 16, 16, true, true, contended, forceReadOnly);
}

void Mmu::map16kReadWrite(uint16_t startAddr, BankId bankId, std::size_t page16k, bool mapRead, bool mapWrite, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page16k * 16, 16, mapRead, mapWrite, contended, forceReadOnly);
}

void Mmu::map8k(uint16_t startAddr, BankId bankId, std::size_t page8k, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page8k * 8, 8, true, true, contended, forceReadOnly);
}

void Mmu::map8kReadWrite(uint16_t startAddr, BankId bankId, std::size_t page8k, bool mapRead, bool mapWrite, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page8k * 8, 8, mapRead, mapWrite, contended, forceReadOnly);
}

void Mmu::map4k(uint16_t startAddr, BankId bankId, std::size_t page4k, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page4k * 4, 4, true, true, contended, forceReadOnly);
}

void Mmu::map2k(uint16_t startAddr, BankId bankId, std::size_t page2k, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page2k * 2, 2, true, true, contended, forceReadOnly);
}

void Mmu::map1k(uint16_t startAddr, BankId bankId, std::size_t page1k, bool contended, bool forceReadOnly)
{
    mapPages1k(startAddr, bankId, page1k, 1, true, true, contended, forceReadOnly);
}

void Mmu::unmapPages1k(uint16_t startAddr, std::size_t count1k) noexcept
{
    const std::size_t startSlot = startAddr >> kPageShift;
    const std::size_t endSlot = std::min<std::size_t>(startSlot + count1k, kSlotCount);
    for (std::size_t slot = startSlot; slot < endSlot; ++slot) {
        m_baseMap[slot] = {};
        refreshSlot(slot);
    }
}

void Mmu::mapOverlay1k(uint16_t startAddr,
                       BankId bankId,
                       std::size_t firstPage1k,
                       std::size_t count1k,
                       bool writable)
{
    const std::size_t startSlot = startAddr >> kPageShift;
    if (startSlot + count1k > kSlotCount)
        throw InvalidArgumentException("MMU overlay exceeds 64KB address space");

    auto &b = bank(bankId);
    const bool canWrite = writable && b.isWritable();
    for (std::size_t i = 0; i < count1k; ++i) {
        const std::size_t slot = startSlot + i;
        const std::size_t p1k  = firstPage1k + i;
        m_overlayMap[slot] = SlotMapping{
            .bankId      = bankId,
            .readPage1k  = static_cast<uint16_t>(p1k),
            .writeBankId = bankId,
            .writePage1k = static_cast<uint16_t>(p1k),
            .writable    = canWrite,
            .contended   = false
        };
        m_overlayActive[slot] = true;
        refreshSlot(slot);
    }
}

void Mmu::clearOverlay(uint16_t startAddr, std::size_t count1k) noexcept
{
    const std::size_t startSlot = startAddr >> kPageShift;
    const std::size_t endSlot = std::min<std::size_t>(startSlot + count1k, kSlotCount);
    for (std::size_t slot = startSlot; slot < endSlot; ++slot) {
        m_overlayActive[slot] = false;
        refreshSlot(slot);
    }
}

void Mmu::setContendedRange(uint16_t startAddr, std::size_t count1k, bool contended) noexcept
{
    const std::size_t startSlot = startAddr >> kPageShift;
    const std::size_t endSlot = std::min<std::size_t>(startSlot + count1k, kSlotCount);
    for (std::size_t slot = startSlot; slot < endSlot; ++slot) {
        m_baseMap[slot].contended = contended;
        refreshSlot(slot);
    }
}

void Mmu::poke(uint16_t addr, uint8_t val, bool allowRomWrite) noexcept
{
    const std::size_t slot = addr >> kPageShift;
    if ((m_flags[slot] & SlotFlags::Writable) != 0) {
        if (m_flags[slot] & SlotFlags::VideoWrite)
            m_videoWriteCallback();
        m_writePages[slot][addr & kPageMask] = val;
    } else if (allowRomWrite) {
        const auto &active = m_overlayActive[slot] ? m_overlayMap[slot] : m_baseMap[slot];
        if (active.bankId != 0) {
            m_banks[active.bankId - 1].page1k(active.readPage1k)[addr & kPageMask] = val;
        }
    }
}

void Mmu::setVideoWriteCallback(BankId bank, std::function<void()> callback)
{
    m_videoBank = bank;
    m_videoWriteCallback = std::move(callback);
    for (std::size_t slot = 0; slot < kSlotCount; ++slot)
        refreshSlot(slot);
}

void Mmu::setWatchpointCallback(WatchpointCallback cb)
{
    m_watchpointCb = std::move(cb);
}

void Mmu::setSlotWatchFlags(std::size_t slot, bool watchRead, bool watchWrite) noexcept
{
    if (slot >= kSlotCount)
        return;
    if (watchRead)
        m_flags[slot] |= SlotFlags::WatchRead;
    else
        m_flags[slot] &= static_cast<uint8_t>(~SlotFlags::WatchRead);

    if (watchWrite)
        m_flags[slot] |= SlotFlags::WatchWrite;
    else
        m_flags[slot] &= static_cast<uint8_t>(~SlotFlags::WatchWrite);
}

void Mmu::setCustomSlotHandlers(std::size_t slot, CustomReadHandler rd, CustomWriteHandler wr)
{
    if (slot >= kSlotCount)
        return;
    m_customReads[slot]  = std::move(rd);
    m_customWrites[slot] = std::move(wr);
    refreshSlot(slot);
}

void Mmu::saveState(StateWriter &w) const
{
    w.write<uint32_t>(static_cast<uint32_t>(m_banks.size()));
    for (const auto &b : m_banks)
        b.saveState(w);
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        w.write(m_baseMap[i].bankId);
        w.write(m_baseMap[i].readPage1k);
        w.write(m_baseMap[i].writeBankId);
        w.write(m_baseMap[i].writePage1k);
        w.writeBool(m_baseMap[i].writable);
        w.writeBool(m_baseMap[i].contended);

        w.writeBool(m_overlayActive[i]);
        w.write(m_overlayMap[i].bankId);
        w.write(m_overlayMap[i].readPage1k);
        w.write(m_overlayMap[i].writeBankId);
        w.write(m_overlayMap[i].writePage1k);
        w.writeBool(m_overlayMap[i].writable);
        w.writeBool(m_overlayMap[i].contended);
    }
}

void Mmu::loadState(StateReader &r)
{
    const auto bankCount = r.read<uint32_t>();
    if (bankCount != m_banks.size())
        throw FormatException("MMU bank count mismatch during loadState");
    for (auto &b : m_banks)
        b.loadState(r);
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        m_baseMap[i].bankId      = r.read<BankId>();
        m_baseMap[i].readPage1k  = r.read<uint16_t>();
        m_baseMap[i].writeBankId = r.read<uint16_t>();
        m_baseMap[i].writePage1k = r.read<uint16_t>();
        m_baseMap[i].writable    = r.readBool();
        m_baseMap[i].contended   = r.readBool();

        m_overlayActive[i]          = r.readBool();
        m_overlayMap[i].bankId      = r.read<BankId>();
        m_overlayMap[i].readPage1k  = r.read<uint16_t>();
        m_overlayMap[i].writeBankId = r.read<uint16_t>();
        m_overlayMap[i].writePage1k = r.read<uint16_t>();
        m_overlayMap[i].writable    = r.readBool();
        m_overlayMap[i].contended   = r.readBool();

        const auto valid = [this](const SlotMapping &map) {
            return (map.bankId == 0 || (map.bankId <= m_banks.size()
                        && map.readPage1k < m_banks[map.bankId - 1].count1kPages()))
                && (map.writeBankId == 0 || (map.writeBankId <= m_banks.size()
                        && map.writePage1k < m_banks[map.writeBankId - 1].count1kPages()));
        };
        if (!valid(m_baseMap[i]) || !valid(m_overlayMap[i]))
            throw FormatException("Invalid MMU mapping in state");
        refreshSlot(i);
    }
}

void Mmu::refreshSlot(std::size_t slot) noexcept
{
    const auto &active = m_overlayActive[slot] ? m_overlayMap[slot] : m_baseMap[slot];

    if (active.bankId != 0 && active.bankId <= m_banks.size()) {
        m_readPages[slot] = m_banks[active.bankId - 1].page1k(active.readPage1k);
    } else {
        m_readPages[slot] = ReadPage(m_unmappedPage);
    }

    if (active.writable && active.writeBankId != 0 && active.writeBankId <= m_banks.size()) {
        m_writePages[slot] = m_banks[active.writeBankId - 1].page1k(active.writePage1k);
    } else {
        m_writePages[slot] = WritePage(m_sinkPage);
    }

    uint8_t f = m_flags[slot] & (SlotFlags::WatchRead | SlotFlags::WatchWrite);
    if (active.contended)
        f |= SlotFlags::Contended;
    if (active.writable)
        f |= SlotFlags::Writable;
    if (m_customReads[slot])
        f |= SlotFlags::CustomRead;
    if (m_customWrites[slot])
        f |= SlotFlags::CustomWrite;
    if (m_videoWriteCallback && active.writable && active.writeBankId == m_videoBank
        && active.writePage1k % 16 < 7)
        f |= SlotFlags::VideoWrite;
    m_flags[slot] = f;
}

uint8_t Mmu::handleSlowRead(std::size_t slot, uint16_t addr, uint8_t val) const
{
    if (m_customReads[slot])
        val = m_customReads[slot](addr, val);
    if ((m_flags[slot] & SlotFlags::WatchRead) && m_watchpointCb)
        m_watchpointCb(addr, val, false);
    return val;
}

void Mmu::handleSlowWrite(std::size_t slot, uint16_t addr, uint8_t val)
{
    if (m_flags[slot] & SlotFlags::VideoWrite)
        m_videoWriteCallback();
    if (m_customWrites[slot]) {
        m_customWrites[slot](addr, val);
    } else {
        m_writePages[slot][addr & kPageMask] = val;
    }
    if ((m_flags[slot] & SlotFlags::WatchWrite) && m_watchpointCb)
        m_watchpointCb(addr, val, true);
}

} // namespace zxkit
