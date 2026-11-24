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

#include "zxkit/peripherals/storage/Interface1.h"

#include "../../media/MdrFormat.h"
#include "zxkit/core/Exception.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/machines/MachineProfile.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace zxkit {

using namespace std::literals;

namespace {
constexpr uint8_t kClock = 0x02;
constexpr uint8_t kReadMode = 0x04;
constexpr std::size_t kPreambleSize = 12;
} // namespace

Interface1Peripheral::Interface1Peripheral(std::vector<uint8_t> rom) : m_snapshotRom(std::move(rom))
{
    if (!m_snapshotRom.empty() && m_snapshotRom.size() != 8192 && m_snapshotRom.size() != 16384)
        throw InvalidArgumentException("Interface 1 ROM must be 8 KiB or 16 KiB"sv);
}

std::string_view Interface1Peripheral::id() const noexcept
{
    return "interface1"sv;
}

std::string_view Interface1Peripheral::name() const noexcept
{
    return "ZX Interface 1 + Microdrive"sv;
}

bool Interface1Peripheral::supportsMachine(const MachineProfile &machine) const noexcept
{
    return machine.supportsInterface1();
}

void Interface1Peripheral::onAttach(MachineContext &ctx)
{
    if (m_snapshotRom.empty())
        m_configuredRom = ctx.loadRom("if1-2.rom"sv, 8 * 1024);
    m_romBank = ctx.mmu.findBank("if1_rom"sv);
    if (!m_romBank)
        m_romBank = ctx.mmu.registerBank("if1_rom"s, 16 * 1024, false, false);
    installRom(ctx);

    ctx.romcs.registerProvider(std::string(id()), 20, [this](Mmu &mmu) {
        const auto upperPage = m_snapshotRom.size() == 16384 ? 8 : 0;
        mmu.mapOverlay1k(0x0000, m_romBank, 0, 8, false);
        mmu.mapOverlay1k(0x2000, m_romBank, upperPage, 8, false);
    });

    ctx.cpu.addPreM1Hook([this, &ctx](uint16_t pc) {
        if (pc == 0x0008 || pc == 0x1708)
            ctx.romcs.setActive(id(), true);
    }, std::string(id()));
    ctx.cpu.addPostM1Hook([this, &ctx](uint16_t pc, uint8_t) {
        if (pc == 0x0700)
            ctx.romcs.setActive(id(), false);
    }, std::string(id()));

    ctx.ioBus.registerSimpleHandler(
            std::string(id()), 0x0018, 0x0000, [this](uint16_t) { return readMdrByte(); },
            [this](uint16_t, uint8_t value) { writeMdrByte(value); });
    ctx.ioBus.registerSimpleHandler(
            std::string(id()), 0x0018, 0x0008, [this](uint16_t) { return readMdrStatus(); },
            [this](uint16_t, uint8_t value) { writeMdrControl(value); });
    // RS-232 and ZX Net lines with no external device connected.
    ctx.ioBus.registerSimpleHandler(
            std::string(id()), 0x0018, 0x0010, [](uint16_t) -> uint8_t { return 0xFF; },
            [this](uint16_t, uint8_t) { restartTransfers(); });
}

void Interface1Peripheral::installRom(MachineContext &ctx)
{
    auto &bank = ctx.mmu.bank(m_romBank);
    const auto &rom = m_snapshotRom.empty() ? m_configuredRom : m_snapshotRom;
    bank.loadData(0, rom);
}

void Interface1Peripheral::setSnapshotRom(MachineContext &ctx, std::vector<uint8_t> rom,
                                          uint8_t driveCount, bool paged)
{
    if (rom.empty() && m_configuredRom.empty())
        m_configuredRom = ctx.loadRom("if1-2.rom"sv, 8 * 1024);
    reset(ctx, ResetKind::Soft);
    m_snapshotRom = std::move(rom);
    m_driveCount = driveCount;
    installRom(ctx);
    ctx.romcs.setActive(id(), paged);
}

void Interface1Peripheral::onDetach(MachineContext &ctx)
{
    ctx.cpu.removeHooks(id());
    ctx.romcs.unregisterProvider(id());
    ctx.ioBus.unregisterOwner(id());
}

void Interface1Peripheral::reset(MachineContext &ctx, ResetKind)
{
    ctx.romcs.setActive(id(), false);
    m_motors = 0;
    m_control = kReadMode;
    for (auto &cartridge : m_cartridges)
        cartridge.position = 0;
    restartTransfers();
}

void Interface1Peripheral::insertCartridge(std::size_t driveIdx, std::span<const uint8_t> mdrBytes)
{
    if (driveIdx >= kMaxCartridges)
        throw InvalidArgumentException("Microdrive index out of range (0..7)"sv);
    const auto sectors = mdr::sectors(mdrBytes);
    Cartridge cartridge;
    cartridge.data.assign(sectors.begin(), sectors.end());
    cartridge.blockSize = mdr::kHeaderSize;
    if (mdrBytes.size() != sectors.size())
        cartridge.protection = mdrBytes.back();
    m_cartridges[driveIdx] = std::move(cartridge);
}

std::size_t Interface1Peripheral::insertCartridge(std::span<const uint8_t> mdrBytes)
{
    for (std::size_t drive = 0; drive < kMaxCartridges; ++drive) {
        if (!hasCartridge(drive)) {
            insertCartridge(drive, mdrBytes);
            return drive;
        }
    }
    throw PeripheralException("All eight Microdrives have cartridges inserted"sv);
}

void Interface1Peripheral::ejectCartridge(std::size_t driveIdx)
{
    if (driveIdx >= kMaxCartridges)
        throw InvalidArgumentException("Microdrive index out of range (0..7)"sv);
    m_cartridges[driveIdx] = {};
}

bool Interface1Peripheral::hasCartridge(std::size_t driveIdx) const noexcept
{
    return driveIdx < kMaxCartridges && !m_cartridges[driveIdx].data.empty();
}

bool Interface1Peripheral::cartridgeModified(std::size_t driveIdx) const noexcept
{
    return hasCartridge(driveIdx) && m_cartridges[driveIdx].modified;
}

bool Interface1Peripheral::cartridgeWriteProtected(std::size_t driveIdx) const noexcept
{
    return hasCartridge(driveIdx) && m_cartridges[driveIdx].protection.value_or(0) != 0;
}

std::vector<uint8_t> Interface1Peripheral::saveCartridge(std::size_t driveIdx) const
{
    if (!hasCartridge(driveIdx))
        throw PeripheralException("No cartridge in that Microdrive"sv);
    const auto &cartridge = m_cartridges[driveIdx];
    auto bytes = cartridge.data;
    if (cartridge.protection)
        bytes.push_back(*cartridge.protection);
    return bytes;
}

void Interface1Peripheral::markCartridgeSaved(std::size_t driveIdx,
                                              std::span<const uint8_t> savedBytes) noexcept
{
    if (!hasCartridge(driveIdx))
        return;
    auto &cartridge = m_cartridges[driveIdx];
    const auto size = cartridge.data.size();
    if (savedBytes.size() == size + cartridge.protection.has_value()
        && (!cartridge.protection || *cartridge.protection == savedBytes.back())
        && std::ranges::equal(cartridge.data, savedBytes.first(size)))
        cartridge.modified = false;
}

BankId Interface1Peripheral::romBankId() const noexcept
{
    return m_romBank;
}

uint8_t Interface1Peripheral::readMdrByte() noexcept
{
    uint8_t value = 0xFF;
    for (std::size_t drive = 0; drive < kMaxCartridges; ++drive) {
        if (!(m_motors & (1u << drive)) || !hasCartridge(drive))
            continue;
        auto &cartridge = m_cartridges[drive];
        if (cartridge.transferred < cartridge.blockSize) {
            ++cartridge.transferred;
            cartridge.lastByte = cartridge.data[cartridge.position];
            cartridge.position = (cartridge.position + 1) % cartridge.data.size();
        }
        value &= cartridge.lastByte;
    }
    return value;
}

void Interface1Peripheral::writeMdrByte(uint8_t value) noexcept
{
    if (m_control & kReadMode)
        return;
    for (std::size_t drive = 0; drive < kMaxCartridges; ++drive) {
        if (!(m_motors & (1u << drive)) || !hasCartridge(drive))
            continue;
        auto &cartridge = m_cartridges[drive];
        if (cartridge.protection.value_or(0)
            || cartridge.transferred >= kPreambleSize + cartridge.blockSize)
            continue;
        // The physical block starts with ten zero bytes and two sync bytes, omitted from MDR.
        if (cartridge.transferred++ < kPreambleSize)
            continue;
        cartridge.data[cartridge.position] = value;
        cartridge.position = (cartridge.position + 1) % cartridge.data.size();
        cartridge.modified = true;
    }
}

uint8_t Interface1Peripheral::readMdrStatus() noexcept
{
    uint8_t status = 0xFF;
    for (std::size_t drive = 0; drive < kMaxCartridges; ++drive) {
        if (!(m_motors & (1u << drive)) || !hasCartridge(drive))
            continue;
        auto &cartridge = m_cartridges[drive];
        // Supply a run of gap polls followed by sync so the ROM can find each block.
        if (cartridge.statusPolls >= 15)
            status &= 0xF9;
        cartridge.statusPolls = (cartridge.statusPolls + 1) % 31;
        if (cartridge.protection.value_or(0))
            status &= 0xFE;
    }
    return status;
}

void Interface1Peripheral::writeMdrControl(uint8_t value) noexcept
{
    if (!(value & kClock) && (m_control & kClock)) {
        const auto driveMask = (1u << m_driveCount) - 1;
        m_motors = static_cast<uint8_t>(((m_motors << 1) | ((value & 1) == 0)) & driveMask);
    }
    m_control = value;
    restartTransfers();
}

void Interface1Peripheral::restartTransfers() noexcept
{
    for (auto &cartridge : m_cartridges) {
        if (cartridge.data.empty())
            continue;
        const auto offset = cartridge.position % mdr::kSectorSize;
        if (offset != 0 && offset != mdr::kHeaderSize) {
            const auto next = offset < mdr::kHeaderSize ? mdr::kHeaderSize : mdr::kSectorSize;
            cartridge.position = (cartridge.position + next - offset) % cartridge.data.size();
        }
        cartridge.transferred = 0;
        cartridge.blockSize =
                cartridge.position % mdr::kSectorSize == 0 ? mdr::kHeaderSize : mdr::kRecordSize;
    }
}

} // namespace zxkit
