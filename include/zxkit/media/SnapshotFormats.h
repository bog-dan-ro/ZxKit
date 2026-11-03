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

#include "zxkit/machines/MachineProfile.h"
#include "zxkit/input/Joystick.h"
#include "zxkit/video/UlaRenderer.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace zxkit {

struct SnapshotMachineState {
    MachineContext &ctx;
    MachineProfile &machine;
    UlaRenderer &ula;
};

struct SnapshotJoystickInfo {
    bool present = false;
    // A single declared interface, including explicit None. Empty for missing,
    // unsupported or conflicting declarations; never guess a programmable mapping.
    std::optional<JoystickType> type;
};

/// Loads and saves .sna, .z80 (v1/v2/v3), .szx (ZX-State), and .nex (ZX Spectrum Next) snapshots.
class SnapshotFormats {
public:
    /// Base machine declared by the interchange header (throws for unsupported hardware).
    static std::string_view machineId(std::span<const uint8_t> data, std::string_view extension);
    static SnapshotJoystickInfo joystickInfo(std::span<const uint8_t> data, std::string_view extension);
    /// Write host input preferences without changing machine state. False if the
    /// format cannot represent this interface (the existing bytes stay unchanged).
    static bool setJoystickInfo(std::vector<uint8_t> &data, std::string_view extension, JoystickType type);

    // ── SNA Format (48K & 128K) ───────────────────────────────────────────
    static void loadSna(std::span<const uint8_t> data, SnapshotMachineState &state);
    static std::vector<uint8_t> saveSna48k(const SnapshotMachineState &state);

    // ── Z80 Format (v1, v2, v3) ───────────────────────────────────────────
    static std::vector<uint8_t> saveSna128k(const SnapshotMachineState &state);
    static std::vector<uint8_t> saveZ80(const SnapshotMachineState &state);
    /// Program export: NEX stores RAM, PC/SP and a fixed entry map, not full CPU/device state.
    static std::vector<uint8_t> saveNex(const SnapshotMachineState &state);
    static void loadZ80(std::span<const uint8_t> data, SnapshotMachineState &state);

    // ── SZX Format (ZX-State) ─────────────────────────────────────────────
    static void loadSzx(std::span<const uint8_t> data, SnapshotMachineState &state);
    static std::vector<uint8_t> saveSzx(const SnapshotMachineState &state);

    // ── NEX Format (ZX Spectrum Next) ─────────────────────────────────────
    static void loadNex(std::span<const uint8_t> data, SnapshotMachineState &state);

    static std::vector<uint8_t> decompressZ80Block(std::span<const uint8_t> src, std::size_t expectedSize, bool hasEndMarker);

private:
    static constexpr uint16_t readLe16(std::span<const uint8_t> b, std::size_t offset) noexcept
    {
        return static_cast<uint16_t>(b[offset] | (static_cast<uint16_t>(b[offset + 1]) << 8));
    }

    static constexpr uint32_t readLe32(std::span<const uint8_t> b, std::size_t offset) noexcept
    {
        return static_cast<uint32_t>(b[offset])
             | (static_cast<uint32_t>(b[offset + 1]) << 8)
             | (static_cast<uint32_t>(b[offset + 2]) << 16)
             | (static_cast<uint32_t>(b[offset + 3]) << 24);
    }

    static void writeLe16(std::vector<uint8_t> &b, std::size_t offset, uint16_t val) noexcept
    {
        b[offset]     = static_cast<uint8_t>(val & 0xFF);
        b[offset + 1] = static_cast<uint8_t>(val >> 8);
    }
};

} // namespace zxkit
