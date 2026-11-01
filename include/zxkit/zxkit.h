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

#include "zxkit/core/Clock.h"
#include "zxkit/core/Emulator.h"
#include "zxkit/core/Engine.h"
#include "zxkit/core/Events.h"
#include "zxkit/core/Exception.h"
#include "zxkit/core/OptionalRef.h"
#include "zxkit/core/Scheduler.h"
#include "zxkit/core/Shared.h"
#include "zxkit/core/SpinSharedMutex.h"
#include "zxkit/core/StateStream.h"

#include "zxkit/cpu/Im2Controller.h"
#include "zxkit/cpu/Z80Ctc.h"
#include "zxkit/cpu/Z80Cpu.h"
#include "zxkit/cpu/Z80Registers.h"

#include "zxkit/memory/ContentionModel.h"
#include "zxkit/memory/MemoryBank.h"
#include "zxkit/memory/Mmu.h"

#include "zxkit/bus/IoBus.h"
#include "zxkit/bus/Peripheral.h"
#include "zxkit/bus/RomcsStack.h"

#include "zxkit/video/UlaRenderer.h"
#include "zxkit/video/VideoFrame.h"
#include "zxkit/video/VideoFilters.h"
#include "zxkit/video/next/NextVideo.h"

#include "zxkit/audio/AudioMixer.h"
#include "zxkit/audio/Ay8912.h"
#include "zxkit/audio/Beeper.h"
#include "zxkit/audio/BlipBuffer.h"
#include "zxkit/audio/Dac.h"

#include "zxkit/input/Joystick.h"
#include "zxkit/input/KeyboardMatrix.h"
#include "zxkit/input/Mouse.h"

#include "zxkit/peripherals/expansion/ExpansionPeripherals.h"
#include "zxkit/peripherals/net/NetworkPeripherals.h"
#include "zxkit/peripherals/next/Copper.h"
#include "zxkit/peripherals/next/NextReg.h"
#include "zxkit/peripherals/next/ZxnCtc.h"
#include "zxkit/peripherals/next/ZxnDma.h"
#include "zxkit/peripherals/storage/Beta128.h"
#include "zxkit/peripherals/storage/FatVolume.h"
#include "zxkit/peripherals/storage/FloppyDisk.h"
#include "zxkit/peripherals/storage/Interface1.h"
#include "zxkit/peripherals/storage/MassStorage.h"
#include "zxkit/peripherals/storage/SdCard.h"
#include "zxkit/peripherals/storage/Wd1793.h"

#include "zxkit/machines/CloneMachines.h"
#include "zxkit/machines/MachineProfile.h"
#include "zxkit/machines/RomanianMachines.h"
#include "zxkit/machines/SinclairMachines.h"
#include "zxkit/machines/ZxNextMachine.h"

#include "zxkit/media/Compression.h"
#include "zxkit/media/MediaPreview.h"
#include "zxkit/media/MediaRegistry.h"
#include "zxkit/media/RzxFormat.h"
#include "zxkit/media/ScreenshotFormat.h"
#include "zxkit/media/SnapshotFormats.h"
#include "zxkit/media/TapeDeck.h"
#include "zxkit/media/TapeFormats.h"

#include "zxkit/debug/Assembler.h"
#include "zxkit/debug/Breakpoints.h"
#include "zxkit/debug/Debugger.h"
#include "zxkit/debug/Disassembler.h"
#include "zxkit/debug/PokeFinder.h"
#include "zxkit/debug/RewindBuffer.h"
#include "zxkit/debug/SymbolTable.h"
