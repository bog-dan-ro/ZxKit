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

#include "zxkit/machines/MachineProfile.h"

#include "zxkit/media/TapeDeck.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/input/KeyboardMatrix.h"

#include <algorithm>
#include <format>
#include <utility>

namespace zxkit {

ContentionPattern MachineProfile::contentionPattern() const noexcept
{
    return ContentionPattern::Sinclair48K;
}

bool MachineProfile::hasFloatingBus() const noexcept
{
    return true;
}

bool MachineProfile::supportsZ80n() const noexcept
{
    return false;
}

void MachineProfile::onFrameStart(MachineContext &ctx)
{
    // The previous instruction can straddle the frame boundary. The ULA
    // pulse began at the boundary, before that instruction finished.
    ctx.cpu.requestInterrupt(0xFF, ctx.clock.frameTstates());
}

void MachineProfile::onScanlineStart(uint16_t /*line*/, MachineContext & /*ctx*/)
{
}

void MachineProfile::renderScanline(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula, VideoFrame &frame)
{
    ula.renderScanline(y, ctx.mmu, frame);
}

void MachineProfile::renderFullFrame(MachineContext &ctx, UlaRenderer &ula, VideoFrame &frame)
{
    ula.renderFullFrame(ctx.mmu, frame);
}

uint32_t MachineProfile::frameWidth() const noexcept
{
    return VideoFrame::kDefaultWidth;
}

uint32_t MachineProfile::frameHeight() const noexcept
{
    return VideoFrame::kDefaultHeight;
}

void MachineProfile::onFrameEnd(MachineContext & /*ctx*/, UlaRenderer &ula)
{
    ula.advanceFlashCounter();
}

BankId MachineProfile::ramBankId() const noexcept
{
    return m_ramBank;
}

BankId MachineProfile::romBankId() const noexcept
{
    return m_romBank;
}

uint8_t MachineProfile::lastPort7ffd() const noexcept
{
    return 0;
}

uint8_t MachineProfile::lastPort1ffd() const noexcept
{
    return 0;
}

void MachineProfile::setPagingPorts(uint8_t /*port7ffd*/, uint8_t /*port1ffd*/, MachineContext & /*ctx*/, UlaRenderer & /*ula*/)
{
}

bool MachineProfile::is48kMode() const noexcept
{
    return true;
}

std::size_t MachineProfile::diskDriveCount() const noexcept
{
    return 0;
}

OptionalRef<DiskImage> MachineProfile::diskDrive(std::size_t /*idx*/) noexcept
{
    return std::nullopt;
}

void MachineProfile::saveState(StateWriter & /*w*/) const
{
}

void MachineProfile::loadState(StateReader & /*r*/, MachineContext & /*ctx*/, UlaRenderer & /*ula*/)
{
}

void MachineProfile::registerStandardUlaPort(MachineContext &ctx, UlaRenderer &ula, uint16_t mask, uint16_t value)
{
    ctx.ioBus.registerHandler(
        std::string(id()) + "_ula",
        mask,
        value,
        [&ctx](uint16_t port) -> IoReadResult {
            const uint8_t keys = ctx.keyboard.readRows(static_cast<uint8_t>(port >> 8));
            // Bit 6 echoes the EAR output (Issue 3), and the tape signal flips it
            const bool tapeLevel = ctx.tape.readEar(ctx);
            const uint8_t ear = (ctx.audioMixer.beeper().earBit() != tapeLevel) ? 0x40 : 0x00;
            return IoReadResult{static_cast<uint8_t>(0xA0 | ear | (keys & 0x1F)), 0xFF};
        },
        [&ctx, &ula](uint16_t /*port*/, uint8_t val) {
            ctx.tape.update(ctx);
            ula.setBorderColor(val & 0x07);
            const bool ear = (val & 0x10) != 0;
            const bool mic = (val & 0x08) != 0;
            ctx.audioMixer.beeper().setPortFeBits(
                ctx.clock.frameBaseTstates(), ear, mic,
                ctx.audioMixer.leftBlip(), ctx.audioMixer.rightBlip());
        });
}

void MachineProfile::registerStandardAyPorts(MachineContext &ctx)
{
    ctx.audioMixer.setAyChipCount(1);
    // Select register: 0xFFFD (mask 0xC002 == 0xC000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_reg",
        0xC002,
        0xC000,
        [&ctx](uint16_t) -> uint8_t { return ctx.audioMixer.activeAy().readRegister(); },
        [&ctx](uint16_t, uint8_t val) {
            if ((val & 0xFE) == 0xFE) {
                ctx.audioMixer.setActiveAyChip(val & 0x01);
            } else {
                ctx.audioMixer.activeAy().selectRegister(val);
            }
        });
    // Data port: 0xBFFD (mask 0xC002 == 0x8000)
    ctx.ioBus.registerSimpleHandler(
        std::string(id()) + "_ay_data",
        0xC002,
        0x8000,
        nullptr,
        [&ctx](uint16_t, uint8_t val) {
            ctx.audioMixer.writeAyRegister(ctx.clock.frameBaseTstates(), val);
        });
}

MachineRegistry MachineRegistry::empty()
{
    return MachineRegistry(EmptyTag{});
}

void MachineRegistry::registerMachine(std::string id, FactoryFn factory)
{
    if (auto it = std::ranges::find(m_factories, id, &decltype(m_factories)::value_type::first); it != m_factories.end())
        it->second = std::move(factory);
    else
        m_factories.emplace_back(std::move(id), std::move(factory));
}

void MachineRegistry::unregisterMachine(std::string_view id)
{
    std::erase_if(m_factories, [id](const auto &entry) { return entry.first == id; });
}

bool MachineRegistry::contains(std::string_view id) const noexcept
{
    return std::ranges::any_of(m_factories, [id](const auto &entry) { return entry.first == id; });
}

std::unique_ptr<MachineProfile> MachineRegistry::create(std::string_view id) const
{
    for (const auto &[machineId, factory] : m_factories) {
        if (machineId == id)
            return factory();
    }
    throw InvalidArgumentException(std::format("Unknown machine profile ID '{}'", id));
}

std::vector<std::string> MachineRegistry::availableIds() const
{
    std::vector<std::string> ids;
    ids.reserve(m_factories.size());
    for (const auto &entry : m_factories)
        ids.push_back(entry.first);
    return ids;
}

} // namespace zxkit
