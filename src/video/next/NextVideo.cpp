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

#include "zxkit/video/next/NextVideo.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace zxkit {

namespace {

constexpr uint32_t kBorderCells = 64; // 32 pixels, two cells each
constexpr uint32_t kPaperCells = 512;

enum Palette : uint8_t { PalUla = 0, PalLayer2 = 1, PalSprites = 2, PalTilemap = 3, PalSecond = 4 };

// RRRGGGBB -> RGB333: the 9th (blue LSB) bit is B1 | B0
constexpr uint16_t rgb8to9(uint8_t v) noexcept
{
    return static_cast<uint16_t>((v << 1) | ((v | (v >> 1)) & 0x01));
}

// The standard Spectrum colours in RRRGGGBB, bright magenta E7 so it isn't the default transparent E3
constexpr std::array<uint8_t, 16> kUlaColours = {
    0x00, 0x02, 0xA0, 0xA2, 0x14, 0x16, 0xB4, 0xB6,
    0x00, 0x03, 0xE0, 0xE7, 0x1C, 0x1F, 0xFC, 0xFF,
};

const std::array<uint32_t, 512> &rgbaTable()
{
    static const std::array<uint32_t, 512> table = [] {
        std::array<uint32_t, 512> t{};
        for (uint16_t i = 0; i < 512; ++i)
            t[i] = rgb333ToRgba(i);
        return t;
    }();
    return table;
}

// Offset of ULA bitmap row `row` (0..191), column `col` (0..31) in the display file
constexpr uint16_t ulaBitmapOffset(uint32_t row, uint32_t col) noexcept
{
    return static_cast<uint16_t>(((row & 0xC0) << 5) | ((row & 0x07) << 8) | ((row & 0x38) << 2) | col);
}

} // namespace

NextVideo::NextVideo()
{
    reset();
}

void NextVideo::reset() noexcept
{
    m_layer2Port = 0;
    m_layer2BankOffset = 0;
    m_layer2Visible = false;

    m_sprites = {};
    m_spriteSlot = 0;
    m_spriteAttrByte = 0;
    m_patternAddr = 0;
    m_nrSprite = 0;
    m_spriteStatus = 0;

    // The palettes as the firmware leaves them
    for (uint16_t i = 0; i < 256; ++i) {
        const uint16_t ula = rgb8to9(kUlaColours[i & 15]);
        m_palettes[PalUla][i] = m_palettes[PalUla + PalSecond][i] = ula;
        m_palettes[PalLayer2][i] = m_palettes[PalLayer2 + PalSecond][i] = rgb8to9(static_cast<uint8_t>(i));
        m_palettes[PalSprites][i] = m_palettes[PalSprites + PalSecond][i] = rgb8to9(static_cast<uint8_t>(i));
        m_palettes[PalTilemap][i] = m_palettes[PalTilemap + PalSecond][i] = 0;
    }
    for (auto &p : m_palettePriority)
        p.fill(0);
    m_palIndex = 0;
    m_palControl = 0;
    m_pal9BitFirstByte = 0;
    m_pal9BitAwaitingSecond = false;

    m_clip[ClipLayer2] = {0, 255, 0, 191};
    m_clip[ClipSprites] = {0, 255, 0, 191};
    m_clip[ClipUla] = {0, 255, 0, 191};
    m_clip[ClipTilemap] = {0, 159, 0, 255};
    m_clipIndex.fill(0);

    m_timexPort = 0;
}

// ── Layer 2 port ─────────────────────────────────────────────────────────

void NextVideo::setLayer2ControlPort(uint8_t val) noexcept
{
    if (val & 0x10) {
        m_layer2BankOffset = val & 0x07;
        return;
    }
    m_layer2Port = val & 0xED;
    m_layer2Visible = (val & 0x02) != 0;
}

uint8_t NextVideo::layer2ControlPort() const noexcept
{
    return static_cast<uint8_t>(m_layer2Port | (m_layer2Visible ? 0x02 : 0x00));
}

// ── Sprites ──────────────────────────────────────────────────────────────

void NextVideo::writeSpriteControlPort(uint8_t val) noexcept
{
    m_spriteSlot = val & 0x7F;
    m_spriteAttrByte = 0;
    m_patternAddr = static_cast<uint16_t>(((val & 0x3F) << 8) | (val & 0x80));
}

uint8_t NextVideo::readSpriteStatus() noexcept
{
    return std::exchange(m_spriteStatus, 0);
}

void NextVideo::writeSpriteAttributePort(uint8_t val) noexcept
{
    auto &spr = m_sprites[m_spriteSlot];
    spr.b[m_spriteAttrByte] = val;
    ++m_spriteAttrByte;
    if (m_spriteAttrByte == 4 && !(spr.b[3] & 0x40)) {
        spr.b[4] = 0; // a 4 byte sprite
        m_spriteAttrByte = 5;
    }
    if (m_spriteAttrByte >= 5) {
        m_spriteAttrByte = 0;
        m_spriteSlot = (m_spriteSlot + 1) & 0x7F;
    }
}

void NextVideo::writeSpritePatternPort(uint8_t val) noexcept
{
    m_patternRam[m_patternAddr] = val;
    m_patternAddr = (m_patternAddr + 1) & (kPatternRamSize - 1);
}

void NextVideo::selectSpriteNextReg(uint8_t val, bool tieToPorts) noexcept
{
    m_nrSprite = val & 0x7F;
    if (tieToPorts) {
        m_spriteSlot = val & 0x7F;
        m_spriteAttrByte = 0;
        m_patternAddr = static_cast<uint16_t>(((val & 0x3F) << 8) | (val & 0x80));
    }
}

void NextVideo::writeSpriteAttributeNextReg(uint8_t byte, uint8_t val, bool increment) noexcept
{
    m_sprites[m_nrSprite].b[byte % 5] = val;
    if (increment)
        m_nrSprite = (m_nrSprite + 1) & 0x7F;
}

// ── Palettes ─────────────────────────────────────────────────────────────

void NextVideo::setPaletteIndex(uint8_t idx) noexcept
{
    m_palIndex = idx;
    m_pal9BitAwaitingSecond = false;
}

void NextVideo::setPaletteControl(uint8_t ctrl) noexcept
{
    m_palControl = ctrl;
    m_pal9BitAwaitingSecond = false;
}

void NextVideo::writePalette8Bit(uint8_t val) noexcept
{
    const uint8_t pal = (m_palControl >> 4) & 0x07;
    m_palettes[pal][m_palIndex] = rgb8to9(val);
    m_palettePriority[pal][m_palIndex] = 0;
    m_pal9BitAwaitingSecond = false;
    if (!(m_palControl & 0x80))
        ++m_palIndex;
}

void NextVideo::writePalette9Bit(uint8_t val) noexcept
{
    if (!m_pal9BitAwaitingSecond) {
        m_pal9BitFirstByte = val;
        m_pal9BitAwaitingSecond = true;
        return;
    }
    const uint8_t pal = (m_palControl >> 4) & 0x07;
    m_palettes[pal][m_palIndex] = static_cast<uint16_t>((m_pal9BitFirstByte << 1) | (val & 0x01));
    m_palettePriority[pal][m_palIndex] = (val >> 6) & 0x03;
    m_pal9BitAwaitingSecond = false;
    if (!(m_palControl & 0x80))
        ++m_palIndex;
}

uint8_t NextVideo::readPalette8Bit() const noexcept
{
    return static_cast<uint8_t>(m_palettes[(m_palControl >> 4) & 0x07][m_palIndex] >> 1);
}

uint8_t NextVideo::readPalette9Bit() const noexcept
{
    const uint8_t pal = (m_palControl >> 4) & 0x07;
    return static_cast<uint8_t>((m_palettePriority[pal][m_palIndex] << 6) | (m_palettes[pal][m_palIndex] & 0x01));
}

// ── Clip windows ─────────────────────────────────────────────────────────

void NextVideo::writeClip(ClipLayer layer, uint8_t val) noexcept
{
    m_clip[layer][m_clipIndex[layer]] = val;
    m_clipIndex[layer] = (m_clipIndex[layer] + 1) & 0x03;
}

uint8_t NextVideo::readClip(ClipLayer layer) const noexcept
{
    return m_clip[layer][m_clipIndex[layer]];
}

void NextVideo::resetClipIndexes(uint8_t mask) noexcept
{
    for (uint8_t layer = 0; layer < 4; ++layer) {
        if (mask & (1u << layer))
            m_clipIndex[layer] = 0;
    }
}

uint8_t NextVideo::clipIndexes() const noexcept
{
    return static_cast<uint8_t>((m_clipIndex[ClipTilemap] << 6) | (m_clipIndex[ClipUla] << 4) |
                                (m_clipIndex[ClipSprites] << 2) | m_clipIndex[ClipLayer2]);
}

// ── Rendering ────────────────────────────────────────────────────────────

bool NextVideo::isGloballyTransparent(uint16_t rgb, const NextReg &nr) noexcept
{
    return (rgb >> 1) == nr.peek(0x14);
}

void NextVideo::renderUla(uint32_t y, std::span<const uint8_t> ram, const UlaRenderer &ula, const NextReg &nr,
                          bool shadowScreen, Line &out) const
{
    const uint8_t nr68 = nr.peek(0x68);
    if (nr68 & 0x80)
        return; // ULA disabled

    const uint8_t palette = (m_palControl & 0x02) ? PalUla + PalSecond : PalUla;
    const bool ulaNext = (m_palControl & 0x01) != 0;
    const bool ulaPlus = (nr68 & 0x08) != 0;
    const uint8_t timexMode = shadowScreen ? 0 : (m_timexPort & 0x07);
    const bool hires = timexMode == 0x06;
    const bool hicolour = timexMode == 0x02;
    const uint8_t fmt = nr.peek(0x42);
    const bool fmtValid = fmt == 0x01 || fmt == 0x03 || fmt == 0x07 || fmt == 0x0F ||
                          fmt == 0x1F || fmt == 0x3F || fmt == 0x7F;
    const uint16_t fallback = rgb8to9(nr.peek(0x4A));
    const uint8_t hiresInk = (m_timexPort >> 3) & 0x07;

    const auto cell = [&](uint16_t rgb, bool border) {
        Cell c;
        c.rgb = rgb;
        c.opaque = !isGloballyTransparent(rgb, nr);
        c.border = border;
        return c;
    };

    uint16_t borderRgb;
    if (hires)
        borderRgb = m_palettes[palette][0x18 | (~hiresInk & 0x07)];
    else if (ulaPlus)
        borderRgb = m_palettes[palette][0xC8 | ula.borderColor()];
    else if (ulaNext)
        borderRgb = (fmt == 0xFF) ? fallback : m_palettes[palette][0x80 | ula.borderColor()];
    else
        borderRgb = m_palettes[palette][0x10 | ula.borderColor()];

    const auto &clip = m_clip[ClipUla];
    const uint8_t clipY2 = std::min<uint8_t>(clip[3], 191);
    const Cell borderCell = cell(borderRgb, true);

    if (y < 32 || y >= 224) {
        const bool hidden = (y < 32) ? (clip[2] > 0) : (clipY2 < 191 || clip[2] > clipY2);
        if (!hidden)
            out.fill(borderCell);
        return;
    }

    const uint32_t py = y - 32;
    if (py < clip[2] || py > clipY2)
        return; // the whole row is clipped, border included

    const bool leftHidden = clip[0] > 0;
    const bool rightHidden = clip[1] < 255 || clip[0] > clip[1];
    for (uint32_t c = 0; c < kBorderCells; ++c) {
        if (!leftHidden)
            out[c] = borderCell;
        if (!rightHidden)
            out[kBorderCells + kPaperCells + c] = borderCell;
    }

    // LoRes: 128x96, 8bpp, replaces the ULA pixels
    if (nr.peek(0x15) & 0x80) {
        const uint8_t loResOffset = nr.peek(0x6A) & 0x0F;
        const uint32_t ly = (py + nr.peek(0x33)) % 192;
        for (uint32_t px = clip[0]; px <= std::min<uint32_t>(clip[1], 255); ++px) {
            const uint32_t lx = (px + nr.peek(0x32)) & 0xFF;
            const uint32_t addr = 5u * 16384 + (ly >> 1) * 128 + (lx >> 1) + (ly >= 96 ? 0x800 : 0);
            const uint8_t d = ram[addr];
            const uint8_t index = static_cast<uint8_t>(((((d >> 4) + loResOffset) & 0x0F) << 4) | (d & 0x0F));
            const Cell pc = cell(m_palettes[palette][index], false);
            out[kBorderCells + px * 2] = pc;
            out[kBorderCells + px * 2 + 1] = pc;
        }
        return;
    }

    const uint32_t screenBase = (shadowScreen ? 7u : 5u) * 16384 + (timexMode == 0x01 ? 0x2000 : 0);
    const uint32_t row = (py + nr.peek(0x27)) % 192;
    const uint32_t scrollCells = 2u * nr.peek(0x26) + ((nr68 & 0x04) ? 1 : 0);
    const bool flashInverted = ula.flashInverted();

    for (uint32_t pc = 0; pc < kPaperCells; ++pc) {
        const uint32_t px = pc >> 1; // clipping is on the unscrolled position
        if (px < clip[0] || px > clip[1])
            continue;
        const uint32_t src = (pc + scrollCells) & 511;
        bool ink;
        uint8_t attr = 0;
        if (hires) {
            const uint32_t col = src >> 4;
            const uint32_t within = src & 15;
            const uint8_t byte = ram[screenBase + (within < 8 ? 0 : 0x2000) + ulaBitmapOffset(row, col)];
            ink = (byte >> (7 - (within & 7))) & 1;
        } else {
            const uint32_t sx = src >> 1;
            const uint32_t col = sx >> 3;
            const uint8_t byte = ram[screenBase + ulaBitmapOffset(row, col)];
            ink = (byte >> (7 - (sx & 7))) & 1;
            attr = hicolour ? ram[screenBase + 0x2000 + ulaBitmapOffset(row, col)]
                            : ram[screenBase + 0x1800 + (row >> 3) * 32 + col];
        }

        uint16_t rgb;
        if (hires) {
            const uint8_t ink3 = hiresInk;
            rgb = m_palettes[palette][ink ? (0x08 | ink3) : (0x18 | (~ink3 & 0x07))];
        } else if (ulaPlus) {
            const uint8_t group = static_cast<uint8_t>(0xC0 | ((attr >> 6) << 4));
            rgb = m_palettes[palette][ink ? (group | (attr & 0x07)) : (group | 0x08 | ((attr >> 3) & 0x07))];
        } else if (ulaNext) {
            if (ink)
                rgb = m_palettes[palette][attr & fmt];
            else
                rgb = fmtValid ? m_palettes[palette][0x80 | (attr >> std::popcount(fmt))] : fallback;
        } else {
            const uint8_t bright = (attr >> 3) & 0x08;
            if ((attr & 0x80) && flashInverted)
                ink = !ink;
            rgb = m_palettes[palette][ink ? (bright | (attr & 0x07)) : (0x10 | bright | ((attr >> 3) & 0x07))];
        }
        out[kBorderCells + pc] = cell(rgb, false);
    }
}

void NextVideo::renderLayer2(uint32_t y, std::span<const uint8_t> ram, const NextReg &nr, Line &out) const
{
    const uint8_t nr70 = nr.peek(0x70);
    const uint8_t resolution = (nr70 >> 4) & 0x03;
    const uint8_t offset = nr70 & 0x0F;
    const uint8_t palette = (m_palControl & 0x04) ? PalLayer2 + PalSecond : PalLayer2;
    const std::size_t base = static_cast<std::size_t>(nr.peek(0x12)) * 16384;
    const auto &clip = m_clip[ClipLayer2];

    const auto put = [&](uint32_t c, uint8_t index) {
        const uint16_t rgb = m_palettes[palette][index];
        if (isGloballyTransparent(rgb, nr))
            return;
        out[c] = Cell{rgb, true, (m_palettePriority[palette][index] & 0x02) != 0, false, false};
    };
    const auto shifted = [offset](uint8_t p) {
        return static_cast<uint8_t>(((((p >> 4) + offset) & 0x0F) << 4) | (p & 0x0F));
    };

    if (resolution == 0) {
        // 256x192, row major, over the paper only
        if (y < 32 || y >= 224)
            return;
        const uint32_t py = y - 32;
        if (py < clip[2] || py > clip[3])
            return;
        const uint32_t srcY = (py + nr.peek(0x17)) % 192;
        const uint8_t scrollX = nr.peek(0x16);
        for (uint32_t px = clip[0]; px <= std::min<uint32_t>(clip[1], 255); ++px) {
            const std::size_t addr = base + srcY * 256 + ((px + scrollX) & 0xFF);
            if (addr >= ram.size())
                continue;
            const uint8_t index = shifted(ram[addr]);
            put(kBorderCells + px * 2, index);
            put(kBorderCells + px * 2 + 1, index);
        }
        return;
    }

    // 320x256 and 640x256: column major, over the whole frame
    if (y < clip[2] || y > clip[3])
        return;
    const uint32_t scrollX = ((nr.peek(0x71) & 0x01) << 8) | nr.peek(0x16);
    const uint32_t srcY = (y + nr.peek(0x17)) & 0xFF;
    const uint32_t x1 = clip[0] * 2u;
    const uint32_t x2 = std::min<uint32_t>(clip[1] * 2u + 1, 319);
    for (uint32_t x = x1; x <= x2; ++x) {
        uint32_t srcX = x + scrollX;
        if (srcX >= 320)
            srcX -= 320;
        const std::size_t addr = base + srcX * 256 + srcY;
        if (addr >= ram.size())
            continue;
        const uint8_t p = ram[addr];
        if (resolution == 1) {
            const uint8_t index = shifted(p);
            put(x * 2, index);
            put(x * 2 + 1, index);
        } else {
            put(x * 2, static_cast<uint8_t>((offset << 4) | (p >> 4)));
            put(x * 2 + 1, static_cast<uint8_t>((offset << 4) | (p & 0x0F)));
        }
    }
}

void NextVideo::renderSprites(uint32_t y, const NextReg &nr, Line &out) const
{
    const uint8_t nr15 = nr.peek(0x15);
    const auto &clip = m_clip[ClipSprites];
    uint32_t cx1, cx2, cy1, cy2;
    if (!(nr15 & 0x02)) {
        cx1 = clip[0] + 32u;
        cx2 = clip[1] + 32u;
        cy1 = clip[2] + 32u;
        cy2 = std::min<uint32_t>(clip[3] + 32u, 223);
    } else if (!(nr15 & 0x20)) {
        cx1 = 0;
        cx2 = 319;
        cy1 = 0;
        cy2 = 255;
    } else {
        cx1 = clip[0] * 2u;
        cx2 = clip[1] * 2u + 1;
        cy1 = clip[2];
        cy2 = clip[3];
    }
    cx2 = std::min<uint32_t>(cx2, 319);
    if (y < cy1 || y > cy2)
        return;

    struct Resolved {
        uint16_t x = 0, y = 0;
        uint8_t pattern = 0; // N5..N0 N6
        uint8_t palette = 0;
        bool visible = false, fourBit = false, xm = false, ym = false, rot = false;
        uint8_t xs = 0, ys = 0;
    };
    std::array<Resolved, kSpriteCount> resolved;
    Resolved anchor;
    bool anchorUnified = false;
    for (std::size_t i = 0; i < kSpriteCount; ++i) {
        const auto &b = m_sprites[i].b;
        const bool extended = (b[3] & 0x40) != 0;
        const uint8_t b4 = extended ? b[4] : 0;
        Resolved r;
        if (extended && (b4 & 0xC0) == 0x40) {
            // Relative sprite
            int ox = static_cast<int8_t>(b[0]);
            int oy = static_cast<int8_t>(b[1]);
            if (anchorUnified) {
                if (anchor.rot)
                    std::swap(ox, oy);
                if (anchor.rot != anchor.xm)
                    ox = -ox;
                if (anchor.ym)
                    oy = -oy;
                ox *= 1 << anchor.xs;
                oy *= 1 << anchor.ys;
            }
            r.x = static_cast<uint16_t>((anchor.x + ox) & 0x1FF);
            r.y = static_cast<uint16_t>((anchor.y + oy) & 0x1FF);
            r.visible = anchor.visible && (b[3] & 0x80);
            r.fourBit = anchor.fourBit;
            r.pattern = static_cast<uint8_t>(((b[3] & 0x3F) << 1) | ((b4 >> 5) & 0x01));
            if (b4 & 0x01)
                r.pattern = static_cast<uint8_t>((r.pattern + anchor.pattern) & 0x7F);
            r.palette = b[2] >> 4;
            if (b[2] & 0x01)
                r.palette = static_cast<uint8_t>((r.palette + anchor.palette) & 0x0F);
            const bool xm = (b[2] & 0x08) != 0;
            const bool ym = (b[2] & 0x04) != 0;
            const bool rot = (b[2] & 0x02) != 0;
            if (anchorUnified) {
                const bool rxm = anchor.rot ? (ym != rot) : xm;
                const bool rym = anchor.rot ? (xm != rot) : ym;
                r.xm = anchor.xm != rxm;
                r.ym = anchor.ym != rym;
                r.rot = anchor.rot != rot;
                r.xs = anchor.xs;
                r.ys = anchor.ys;
            } else {
                r.xm = xm;
                r.ym = ym;
                r.rot = rot;
                r.xs = (b4 >> 3) & 0x03;
                r.ys = (b4 >> 1) & 0x03;
            }
        } else {
            // Anchor
            r.x = static_cast<uint16_t>(b[0] | ((b[2] & 0x01) << 8));
            r.y = static_cast<uint16_t>(b[1] | ((b4 & 0x01) << 8));
            r.visible = (b[3] & 0x80) != 0;
            r.fourBit = (b4 & 0x80) != 0;
            r.pattern = static_cast<uint8_t>(((b[3] & 0x3F) << 1) | (r.fourBit ? ((b4 >> 6) & 0x01) : 0));
            r.palette = b[2] >> 4;
            r.xm = (b[2] & 0x08) != 0;
            r.ym = (b[2] & 0x04) != 0;
            r.rot = (b[2] & 0x02) != 0;
            r.xs = (b4 >> 3) & 0x03;
            r.ys = (b4 >> 1) & 0x03;
            anchor = r;
            anchorUnified = (b4 & 0x20) != 0;
        }
        resolved[i] = r;
    }

    std::array<int16_t, 320> line;
    line.fill(-1);
    const uint8_t transparentIndex = nr.peek(0x4B);
    const bool zeroOnTop = (nr15 & 0x40) != 0;
    for (std::size_t n = 0; n < kSpriteCount; ++n) {
        const auto &r = resolved[zeroOnTop ? kSpriteCount - 1 - n : n];
        if (!r.visible)
            continue;
        const uint32_t rowInSprite = (y - r.y) & 0x1FF;
        if (rowInSprite >= (16u << r.ys))
            continue;
        const uint32_t prow = rowInSprite >> r.ys;
        const uint32_t width = 16u << r.xs;
        const uint32_t patternBase = static_cast<uint32_t>(r.pattern >> 1) * 256 +
                                     (r.fourBit ? (r.pattern & 0x01) * 128u : 0);
        for (uint32_t col = 0; col < width; ++col) {
            const uint32_t sx = (r.x + col) & 0x1FF;
            if (sx < cx1 || sx > cx2)
                continue;
            const uint32_t pcol = col >> r.xs;
            uint32_t py = r.ym ? 15 - prow : prow;
            uint32_t px = (r.xm != r.rot) ? 15 - pcol : pcol;
            if (r.rot)
                std::swap(px, py);
            uint8_t index;
            if (r.fourBit) {
                const uint8_t byte = m_patternRam[(patternBase + py * 8 + (px >> 1)) & (kPatternRamSize - 1)];
                const uint8_t nibble = (px & 1) ? (byte & 0x0F) : (byte >> 4);
                if (nibble == (transparentIndex & 0x0F))
                    continue;
                index = static_cast<uint8_t>((r.palette << 4) | nibble);
            } else {
                const uint8_t v = m_patternRam[(patternBase + py * 16 + px) & (kPatternRamSize - 1)];
                if (v == transparentIndex)
                    continue;
                index = static_cast<uint8_t>(((((v >> 4) + r.palette) & 0x0F) << 4) | (v & 0x0F));
            }
            line[sx] = index;
        }
    }

    const uint8_t palette = (m_palControl & 0x08) ? PalSprites + PalSecond : PalSprites;
    for (uint32_t x = 0; x < 320; ++x) {
        if (line[x] < 0)
            continue;
        const Cell c{m_palettes[palette][static_cast<uint8_t>(line[x])], true, false, false, false};
        out[x * 2] = c;
        out[x * 2 + 1] = c;
    }
}

void NextVideo::renderTilemap(uint32_t y, std::span<const uint8_t> ram, const NextReg &nr, Line &out) const
{
    const uint8_t nr6b = nr.peek(0x6B);
    const bool cols80 = (nr6b & 0x40) != 0;
    const bool noAttributes = (nr6b & 0x20) != 0;
    const bool textMode = (nr6b & 0x08) != 0;
    const bool tiles512 = (nr6b & 0x02) != 0;
    const bool overUla = (nr6b & 0x01) != 0;
    const uint8_t palette = (nr6b & 0x10) ? PalTilemap + PalSecond : PalTilemap;
    const auto &clip = m_clip[ClipTilemap];
    if (y < clip[2] || y > clip[3])
        return;

    const auto bankBase = [](uint8_t v) -> uint32_t {
        return ((v & 0x80) ? 7u : 5u) * 16384 + ((v & 0x3Fu) << 8);
    };
    const uint32_t mapBase = bankBase(nr.peek(0x6E));
    const uint32_t defBase = bankBase(nr.peek(0x6F));
    const uint32_t cols = cols80 ? 80 : 40;
    const uint32_t entrySize = noAttributes ? 1 : 2;
    const uint32_t scrollX = ((nr.peek(0x2F) & 0x03) << 8) | nr.peek(0x30);
    const uint32_t ty = (y + nr.peek(0x31)) & 0xFF;
    const uint32_t row = ty >> 3;
    const uint32_t py = ty & 7;
    const uint8_t transparentIndex = nr.peek(0x4C) & 0x0F;
    const uint8_t defaultAttr = nr.peek(0x6C);
    // Tilemap data lives in banks 5 and 7: keep every access inside them
    const auto at = [ram](uint32_t addr) { return ram[(addr - 5u * 16384) % (3u * 16384) + 5u * 16384]; };

    const uint32_t x1 = clip[0] * 2u;
    const uint32_t x2 = std::min<uint32_t>(clip[1] * 2u + 1, 319);
    for (uint32_t c = x1 * 2; c <= x2 * 2 + 1; ++c) {
        // 40 columns: both cells of a pixel are the same
        if (!cols80 && (c & 1)) {
            out[c] = out[c - 1];
            continue;
        }
        const uint32_t tx = cols80 ? (c + scrollX) % 640 : ((c >> 1) + scrollX) % 320;
        const uint32_t col = tx >> 3;
        const uint32_t px = tx & 7;
        const uint32_t entry = mapBase + (row * cols + col) * entrySize;
        const uint8_t attr = noAttributes ? defaultAttr : at(entry + 1);
        uint32_t tile = at(entry);
        if (tiles512)
            tile |= (attr & 0x01u) << 8;

        Cell cell;
        if (textMode) {
            const uint8_t bits = at(defBase + tile * 8 + py);
            const uint8_t index = static_cast<uint8_t>((attr & 0xFE) | ((bits >> (7 - px)) & 0x01));
            cell.rgb = m_palettes[palette][index];
            cell.opaque = !isGloballyTransparent(cell.rgb, nr);
        } else {
            const bool xm = (attr & 0x08) != 0;
            const bool ym = (attr & 0x04) != 0;
            const bool rot = (attr & 0x02) != 0;
            uint32_t tpx = (xm != rot) ? 7 - px : px;
            uint32_t tpy = ym ? 7 - py : py;
            if (rot)
                std::swap(tpx, tpy);
            const uint8_t byte = at(defBase + tile * 32 + tpy * 4 + (tpx >> 1));
            const uint8_t nibble = (tpx & 1) ? (byte & 0x0F) : (byte >> 4);
            if (nibble == transparentIndex)
                continue;
            cell.rgb = m_palettes[palette][(attr & 0xF0) | nibble];
            cell.opaque = true;
        }
        cell.belowUla = ((attr & 0x01) || tiles512) && !overUla;
        out[c] = cell;
    }
}

void NextVideo::renderScanline(uint32_t y, const Mmu &mmu, BankId ramBankId, const UlaRenderer &ula,
                               const NextReg &nr, bool shadowScreen, VideoFrame &frame) const
{
    if (y >= UlaRenderer::kTotalHeightLines || ramBankId == 0 || frame.width() < kFrameWidth ||
        frame.height() < kFrameHeight)
        return;

    const auto ramSpan = mmu.bank(ramBankId).bytes();
    const uint8_t nr15 = nr.peek(0x15);

    Line ulaLine{};
    Line layer2{};
    Line sprites{};
    Line tilemap{};
    renderUla(y, ramSpan, ula, nr, shadowScreen, ulaLine);
    if (m_layer2Visible)
        renderLayer2(y, ramSpan, nr, layer2);
    if (nr15 & 0x01)
        renderSprites(y, nr, sprites);
    const bool tilemapOn = (nr.peek(0x6B) & 0x80) != 0;
    if (tilemapOn)
        renderTilemap(y, ramSpan, nr, tilemap);

    const uint16_t fallback = rgb8to9(nr.peek(0x4A));
    const uint8_t order = (nr15 >> 2) & 0x07;
    const uint8_t blendSource = (nr.peek(0x68) >> 5) & 0x03;
    const auto &rgba = rgbaTable();
    auto row0 = frame.scanline(y * 2);
    auto row1 = frame.scanline(y * 2 + 1);

    for (uint32_t c = 0; c < kFrameWidth; ++c) {
        const Cell &u = ulaLine[c];
        const Cell &t = tilemap[c];
        const Cell &l = layer2[c];
        const Cell &s = sprites[c];
        // ULA and tilemap make one layer
        const Cell &merged = (t.opaque && (!t.belowUla || !u.opaque)) ? t : u;

        uint16_t rgb = fallback;
        if (l.opaque && l.priority && order < 6) {
            rgb = l.rgb;
        } else if (order < 6) {
            // Sprites still show over the ULA border in the orders that put U above them
            const bool skipU = (order >= 3) && u.border && !t.opaque && s.opaque;
            static constexpr std::array<std::array<uint8_t, 3>, 6> kOrders = {{
                {0, 1, 2}, {1, 0, 2}, {0, 2, 1}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}, // S=0 L=1 U=2
            }};
            for (uint8_t layer : kOrders[order]) {
                const Cell &candidate = layer == 0 ? s : (layer == 1 ? l : merged);
                if (layer == 2 && skipU)
                    continue;
                if (candidate.opaque) {
                    rgb = candidate.rgb;
                    break;
                }
            }
        } else {
            // Blend modes: Layer 2 mixed with the ULA and/or the tilemap
            const Cell empty;
            const Cell *blended = &empty;
            const Cell *above = &empty;
            const Cell *below = &empty;
            switch (blendSource) {
            case 0:
                blended = &u;
                (t.belowUla ? below : above) = &t;
                break;
            case 2:
                blended = &merged;
                break;
            case 3:
                blended = &t;
                (t.belowUla ? above : below) = &u;
                break;
            default:
                above = t.belowUla ? &u : &t;
                below = t.belowUla ? &t : &u;
                break;
            }
            const auto mix = [&]() -> uint16_t {
                uint16_t result = 0;
                for (int shift : {6, 3, 0}) {
                    const int a = (l.rgb >> shift) & 7;
                    const int b = blended->opaque ? (blended->rgb >> shift) & 7 : 0;
                    int sum = a + b;
                    if (order == 6)
                        sum = std::min(sum, 7);
                    else if (!blended->opaque)
                        sum = a;
                    else
                        sum = sum <= 4 ? 0 : (sum >= 12 ? 7 : sum - 5);
                    result = static_cast<uint16_t>(result | (sum << shift));
                }
                return result;
            };
            if (l.opaque && l.priority)
                rgb = mix();
            else if (above->opaque)
                rgb = above->rgb;
            else if (s.opaque)
                rgb = s.rgb;
            else if (below->opaque)
                rgb = below->rgb;
            else if (l.opaque)
                rgb = mix();
        }
        const uint32_t pixel = rgba[rgb & 0x1FF];
        row0[c] = pixel;
        row1[c] = pixel;
    }
}

// ── State ────────────────────────────────────────────────────────────────

void NextVideo::saveState(StateWriter &w) const
{
    w.write(m_layer2Port);
    w.write(m_layer2BankOffset);
    w.writeBool(m_layer2Visible);
    for (const auto &s : m_sprites)
        w.writeBytes(s.b);
    w.writeBytes(m_patternRam);
    w.write(m_spriteSlot);
    w.write(m_spriteAttrByte);
    w.write(m_patternAddr);
    w.write(m_nrSprite);
    for (const auto &p : m_palettes)
        for (uint16_t v : p)
            w.write(v);
    for (const auto &p : m_palettePriority)
        w.writeBytes(p);
    w.write(m_palIndex);
    w.write(m_palControl);
    w.write(m_pal9BitFirstByte);
    w.writeBool(m_pal9BitAwaitingSecond);
    for (const auto &c : m_clip)
        w.writeBytes(c);
    w.writeBytes(m_clipIndex);
    w.write(m_timexPort);
}

void NextVideo::loadState(StateReader &r)
{
    m_layer2Port = r.read<uint8_t>();
    m_layer2BankOffset = r.read<uint8_t>();
    m_layer2Visible = r.readBool();
    for (auto &s : m_sprites)
        r.readBytes(s.b);
    r.readBytes(m_patternRam);
    m_spriteSlot = r.read<uint8_t>();
    m_spriteAttrByte = r.read<uint8_t>();
    m_patternAddr = r.read<uint16_t>();
    m_nrSprite = r.read<uint8_t>();
    for (auto &p : m_palettes)
        for (auto &v : p)
            v = r.read<uint16_t>();
    for (auto &p : m_palettePriority)
        r.readBytes(p);
    m_palIndex = r.read<uint8_t>();
    m_palControl = r.read<uint8_t>();
    m_pal9BitFirstByte = r.read<uint8_t>();
    m_pal9BitAwaitingSecond = r.readBool();
    for (auto &c : m_clip)
        r.readBytes(c);
    r.readBytes(m_clipIndex);
    m_timexPort = r.read<uint8_t>();
}

} // namespace zxkit
