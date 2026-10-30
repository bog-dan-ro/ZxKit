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
#include "zxkit/bus/RomcsStack.h"
#include "zxkit/core/Clock.h"
#include "zxkit/core/OptionalRef.h"
#include "zxkit/core/Scheduler.h"
#include "zxkit/core/StateStream.h"
#include "zxkit/cpu/Im2Controller.h"
#include "zxkit/memory/ContentionModel.h"
#include "zxkit/memory/Mmu.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace zxkit {

class Z80Cpu;
class AudioMixer;
class KeyboardMatrix;
class DiskImage;
class SdCard;
class TapeDeck;

enum class ResetKind : uint8_t {
    Soft = 0,
    Hard = 1
};

using RomResolverFn = std::function<std::vector<uint8_t>(std::string_view romName, std::size_t expectedSize)>;

/// Context passed to every MachineProfile and Peripheral so they can wire
/// ports, memory banks, overlays, CPU M1/RFSH/RETN hooks, timers, and audio.
struct MachineContext {
    Clock &clock;
    Scheduler &scheduler;
    Mmu &mmu;
    ContentionModel &contention;
    IoBus &ioBus;
    RomcsStack &romcs;
    Z80Cpu &cpu;
    Im2Controller &im2;
    AudioMixer &audioMixer;
    KeyboardMatrix &keyboard;
    SdCard &sdCard;
    TapeDeck &tape;
    RomResolverFn romResolver;

    std::vector<uint8_t> loadRom(std::string_view romName, std::size_t expectedSize = 0) const;
};

/// Base class for all internal and external ZX Spectrum / Z80 peripherals.
/// Users can create custom virtual peripherals by subclassing `Peripheral`
/// and attaching them to an `Emulator` without modifying any framework internals.
class Peripheral {
public:
    virtual ~Peripheral() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;

    virtual void onAttach(MachineContext &ctx) = 0;
    virtual void onDetach(MachineContext & /*ctx*/) {}

    virtual void reset(MachineContext &ctx, ResetKind kind) = 0;

    virtual void onFrameStart() {}
    virtual void onFrameEnd() {}

    /// Floppy drives this peripheral provides (used by `Emulator::insertDisk`).
    virtual std::size_t diskDriveCount() const noexcept { return 0; }
    virtual OptionalRef<DiskImage> diskDrive(std::size_t /*idx*/) noexcept { return std::nullopt; }

    virtual void saveState(StateWriter & /*w*/) const {}
    virtual void loadState(StateReader & /*r*/) {}
};

} // namespace zxkit
