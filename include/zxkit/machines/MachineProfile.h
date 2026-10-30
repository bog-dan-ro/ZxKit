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

#include "zxkit/bus/Peripheral.h"
#include "zxkit/core/Clock.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/memory/ContentionModel.h"
#include "zxkit/video/UlaRenderer.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zxkit {

/// Base class for all emulated Z80 / ZX Spectrum machines.
/// Users can define custom Z80 computers (with arbitrary memory banking,
/// CPU hooks, and I/O decoding) by subclassing `MachineProfile` and registering
/// it with `MachineRegistry`.
class MachineProfile {
public:
    virtual ~MachineProfile() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual FrameTimings timings() const noexcept = 0;
    virtual ContentionPattern contentionPattern() const noexcept;
    virtual bool hasFloatingBus() const noexcept;
    virtual bool supportsZ80n() const noexcept;
    /// Software sees a 48K Spectrum: a 16K/48K machine (or clone), or a 128K one with its paging
    /// locked (48K BASIC). Tapes use it for their "stop the tape in 48K mode" blocks.
    virtual bool is48kMode() const noexcept;

    virtual void initialize(MachineContext &ctx, UlaRenderer &ula) = 0;
    virtual void reset(MachineContext &ctx, UlaRenderer &ula, ResetKind kind) = 0;

    /// Called at the start of every frame; requests the ULA frame interrupt by default.
    virtual void onFrameStart(MachineContext &ctx);
    /// Called before the CPU runs machine scanline `line` (0 = first line of the frame),
    /// e.g. for raster (line) interrupts.
    virtual void onScanlineStart(uint16_t line, MachineContext &ctx);
    virtual void renderScanline(uint32_t y, const MachineContext &ctx, const UlaRenderer &ula, VideoFrame &frame);
    /// Redraws the whole frame from the current state (e.g. after loading a snapshot).
    virtual void renderFullFrame(MachineContext &ctx, UlaRenderer &ula, VideoFrame &frame);
    /// Size of the frames this machine renders (320x256 with the usual 32 pixel border).
    virtual uint32_t frameWidth() const noexcept;
    virtual uint32_t frameHeight() const noexcept;
    virtual void onFrameEnd(MachineContext &ctx, UlaRenderer &ula);

    virtual BankId ramBankId() const noexcept;
    virtual BankId romBankId() const noexcept;
    virtual uint8_t lastPort7ffd() const noexcept;
    virtual uint8_t lastPort1ffd() const noexcept;
    virtual void setPagingPorts(uint8_t port7ffd, uint8_t port1ffd, MachineContext &ctx, UlaRenderer &ula);

    /// Built-in floppy drives (e.g. HC-2000), used by `Emulator::insertDisk`.
    virtual std::size_t diskDriveCount() const noexcept;
    virtual OptionalRef<DiskImage> diskDrive(std::size_t idx) noexcept;

    virtual void saveState(StateWriter &w) const;
    virtual void loadState(StateReader &r, MachineContext &ctx, UlaRenderer &ula);

protected:
    void registerStandardUlaPort(MachineContext &ctx, UlaRenderer &ula, uint16_t mask = 0x0001, uint16_t value = 0x0000);
    void registerStandardAyPorts(MachineContext &ctx);

    BankId m_ramBank = 0;
    BankId m_romBank = 0;
};

/// The machines an `Emulator` can create by id. A plain value: every emulator owns its
/// own registry, so custom machines registered for one never leak into another.
class MachineRegistry {
public:
    using FactoryFn = std::function<std::unique_ptr<MachineProfile>()>;

    /// A registry with all the built-in machines.
    MachineRegistry();
    /// A registry without any machine.
    static MachineRegistry empty();

    /// Adds a machine, or replaces the factory of an existing id.
    void registerMachine(std::string id, FactoryFn factory);
    void unregisterMachine(std::string_view id);
    bool contains(std::string_view id) const noexcept;
    std::unique_ptr<MachineProfile> create(std::string_view id) const;
    /// The machine ids, in registration order.
    std::vector<std::string> availableIds() const;

private:
    struct EmptyTag {};
    explicit MachineRegistry(EmptyTag) {}

    std::vector<std::pair<std::string, FactoryFn>> m_factories;
};

} // namespace zxkit
