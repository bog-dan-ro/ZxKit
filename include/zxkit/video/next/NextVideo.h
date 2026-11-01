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
#include "zxkit/memory/Mmu.h"
#include "zxkit/peripherals/next/NextReg.h"
#include "zxkit/video/UlaRenderer.h"
#include "zxkit/video/VideoFrame.h"

#include <array>
#include <cstdint>
#include <span>

namespace zxkit {

/// ZX Spectrum Next video: ULA (standard / ULANext / ULA+ / Timex hi-colour &
/// hi-res / LoRes), Layer 2 (256x192, 320x256, 640x256), 128 hardware sprites
/// (4/8-bit patterns, relative sprites, scaling, rotation), the 40/80 column
/// tilemap and their composition (NR 0x15 orders, blend modes, clip windows).
///
/// The frame is composited on the Next's 640 cell wide grid (two cells per
/// 7MHz pixel, so hi-res modes keep their detail) and every line is output
/// twice: a 640x512 frame with the usual 32 pixel (64 cell) border.
class NextVideo {
public:
    static constexpr uint32_t kFrameWidth = 640;
    static constexpr uint32_t kFrameHeight = 512;
    static constexpr std::size_t kSpriteCount = 128;
    static constexpr std::size_t kPatternRamSize = 16 * 1024;

    NextVideo();
    void reset() noexcept;

    // Layer 2 port 0x123B
    void setLayer2ControlPort(uint8_t val) noexcept;
    uint8_t layer2ControlPort() const noexcept;
    bool isLayer2Visible() const noexcept { return m_layer2Visible; }
    void setLayer2Visible(bool visible) noexcept { m_layer2Visible = visible; }
    bool layer2WriteMapped() const noexcept { return (m_layer2Port & 0x01) != 0; }
    bool layer2ReadMapped() const noexcept { return (m_layer2Port & 0x04) != 0; }
    bool layer2UsesShadowBank() const noexcept { return (m_layer2Port & 0x08) != 0; }
    uint8_t layer2Segment() const noexcept { return (m_layer2Port >> 6) & 0x03; }
    uint8_t layer2BankOffset() const noexcept { return m_layer2BankOffset; }

    // Sprites: ports 0x303B, 0x57, 0x5B and NR 0x34..0x39 / 0x75..0x79
    void writeSpriteControlPort(uint8_t val) noexcept;
    uint8_t readSpriteStatus() noexcept;
    void writeSpriteAttributePort(uint8_t val) noexcept;
    void writeSpritePatternPort(uint8_t val) noexcept;
    void selectSpriteNextReg(uint8_t val, bool tieToPorts) noexcept;
    /// NR 0x35..0x39 (`increment` = false) and NR 0x75..0x79 (true): attribute `byte` of the NR 0x34 sprite.
    void writeSpriteAttributeNextReg(uint8_t byte, uint8_t val, bool increment) noexcept;
    uint8_t spriteNextRegSelection() const noexcept { return m_nrSprite; }

    // Palettes: NR 0x40, 0x41, 0x43, 0x44
    void setPaletteIndex(uint8_t idx) noexcept;
    uint8_t paletteIndex() const noexcept { return m_palIndex; }
    void setPaletteControl(uint8_t ctrl) noexcept;
    void writePalette8Bit(uint8_t val) noexcept;
    void writePalette9Bit(uint8_t val) noexcept;
    uint8_t readPalette8Bit() const noexcept;
    uint8_t readPalette9Bit() const noexcept;
    void resetPaletteSequence() noexcept { m_pal9BitAwaitingSecond = false; }

    // Clip windows NR 0x18 (Layer 2), 0x19 (sprites), 0x1A (ULA), 0x1B (tilemap), index reset NR 0x1C
    enum ClipLayer : uint8_t { ClipLayer2 = 0, ClipSprites = 1, ClipUla = 2, ClipTilemap = 3 };
    void writeClip(ClipLayer layer, uint8_t val) noexcept;
    uint8_t readClip(ClipLayer layer) const noexcept;
    void resetClipIndexes(uint8_t mask) noexcept;
    uint8_t clipIndexes() const noexcept;

    // Timex SCLD port 0xFF (mirrored in NR 0x69 bits 5:0)
    void setTimexPort(uint8_t val) noexcept { m_timexPort = val; }
    uint8_t timexPort() const noexcept { return m_timexPort; }

    /// Renders frame row `y` (0 .. 255, rows 2y and 2y+1 of the 640x512 frame).
    /// `ramBankId` is the Next RAM (16KB bank N at N * 16KB), `shadowScreen`
    /// is port 0x7FFD bit 3 (ULA reading bank 7).
    void renderScanline(uint32_t y, const Mmu &mmu, BankId ramBankId, const UlaRenderer &ula,
                        const NextReg &nextReg, bool shadowScreen, VideoFrame &frame) const;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    struct SpriteAttr {
        std::array<uint8_t, 5> b{};
    };
    struct Cell {
        uint16_t rgb = 0;       // 9-bit RGB333
        bool opaque = false;
        bool priority = false;  // Layer 2 palette priority bit
        bool belowUla = false;  // tilemap: below the ULA
        bool border = false;    // ULA: border pixel
    };
    using Line = std::array<Cell, kFrameWidth>;

    void renderUla(int32_t y, std::span<const uint8_t> ram, const UlaRenderer &ula, const NextReg &nr,
                   bool shadowScreen, Line &out) const;
    void renderLayer2(uint32_t y, std::span<const uint8_t> ram, const NextReg &nr, Line &out) const;
    void renderSprites(uint32_t y, const NextReg &nr, Line &out) const;
    void renderTilemap(uint32_t y, std::span<const uint8_t> ram, const NextReg &nr, Line &out) const;

    uint16_t paletteRgb(uint8_t palette, uint8_t index) const noexcept { return m_palettes[palette][index]; }
    static bool isGloballyTransparent(uint16_t rgb, const NextReg &nr) noexcept;

    // Layer 2
    uint8_t m_layer2Port = 0;
    uint8_t m_layer2BankOffset = 0;
    bool m_layer2Visible = false;

    // Sprites
    std::array<SpriteAttr, kSpriteCount> m_sprites{};
    std::array<uint8_t, kPatternRamSize> m_patternRam{};
    uint8_t m_spriteSlot = 0;
    uint8_t m_spriteAttrByte = 0;
    uint16_t m_patternAddr = 0;
    uint8_t m_nrSprite = 0;
    uint8_t m_spriteStatus = 0;

    // Palettes: 0 ULA, 1 Layer 2, 2 sprites, 3 tilemap; +4 for the second ones
    std::array<std::array<uint16_t, 256>, 8> m_palettes{};
    std::array<std::array<uint8_t, 256>, 8> m_palettePriority{};
    uint8_t m_palIndex = 0;
    uint8_t m_palControl = 0;
    uint8_t m_pal9BitFirstByte = 0;
    bool m_pal9BitAwaitingSecond = false;

    std::array<std::array<uint8_t, 4>, 4> m_clip{};
    std::array<uint8_t, 4> m_clipIndex{};

    uint8_t m_timexPort = 0;
};

} // namespace zxkit
