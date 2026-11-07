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
#include "zxkit/core/StateStream.h"
#include "zxkit/memory/Mmu.h"
#include "zxkit/video/VideoFrame.h"

#include <array>
#include <cstdint>

namespace zxkit {

enum class VideoDisplayMode : uint8_t {
    StandardUla = 0, // 256x192 bitmap + 32x24 attributes at base + 0x1800
    SecondScreen, // Timex SCLD mode 1: 256x192 at base + 0x2000
    TimexHiColor, // Timex SCLD mode 2: 256x192 bitmap at +0x0000, 8x1 attributes at +0x2000
    TimexHiRes, // Timex SCLD mode 6: 512x192 monochrome using +0x0000 and +0x2000
    Pentagon16Color // Pentagon 16-colour pixel mode
};

/// ULA / Timex SCLD / Pentagon renderer producing contiguous RGBA8888 VideoFrames.
/// Sinclair machines use the incremental raster; static redraws and other machines
/// can use the scanline renderer.
/// Supports dynamic VRAM bank switching (e.g. 128K shadow screen page 5/7, HC-2000/CoBra
/// VRAM at 0x4000 vs 0xC000) and mid-frame border/mode changes.
class UlaRenderer
{
public:
    static constexpr uint32_t kBorderLeftPx = 32;
    static constexpr uint32_t kScreenWidthPx = 256;
    static constexpr uint32_t kBorderRightPx = 32;
    static constexpr uint32_t kTotalWidthPx =
            kBorderLeftPx + kScreenWidthPx + kBorderRightPx; // 320

    static constexpr uint32_t kBorderTopLines = 32;
    static constexpr uint32_t kScreenLines = 192;
    static constexpr uint32_t kBorderBottomLines = 32;
    static constexpr uint32_t kTotalHeightLines =
            kBorderTopLines + kScreenLines + kBorderBottomLines; // 256

    UlaRenderer();

    /// Bounded to 128 per side. Emulator applies machine-specific limits.
    void setBorderMargins(BorderMargins margins) noexcept;
    BorderMargins borderMargins() const noexcept
    {
        return m_margins;
    }
    uint32_t outputWidth() const noexcept
    {
        return (m_margins.left + kScreenWidthPx + m_margins.right)
                * (m_mode == VideoDisplayMode::TimexHiRes ? 2 : 1);
    }
    uint32_t outputHeight() const noexcept
    {
        return m_margins.top + kScreenLines + m_margins.bottom;
    }

    void reset() noexcept;

    void setVramSource(BankId bankId, uint16_t page16k, uint16_t offsetBytes = 0) noexcept;

    BankId vramBankId() const noexcept
    {
        return m_vramBankId;
    }
    uint16_t vramPage16k() const noexcept
    {
        return m_vramPage16k;
    }

    void setBorderColor(uint8_t color) noexcept;

    uint8_t borderColor() const noexcept
    {
        return m_borderColor;
    }

    void setScldRegister(uint8_t val) noexcept;

    uint8_t scldRegister() const noexcept
    {
        return m_scldReg;
    }

    void setDisplayMode(VideoDisplayMode mode) noexcept
    {
        m_mode = mode;
    }
    VideoDisplayMode displayMode() const noexcept
    {
        return m_mode;
    }

    /// Renders a single visible output row `y` into `frame`.
    void renderScanline(uint32_t y, const Mmu &mmu, VideoFrame &frame) const;

    void renderFullFrame(const Mmu &mmu, VideoFrame &frame);

    /// Draw the ULA timeline before mutating video RAM or port state. Fetches
    /// latch bitmap and attribute bytes independently, including shadow paging.
    void beginRasterFrame(const FrameTimings &timings);
    void renderUntil(uint32_t frameTstate, const Mmu &mmu);
    void copyRasterScanline(uint32_t y, VideoFrame &frame) const;
    void endRasterFrame() noexcept
    {
        m_rasterActive = false;
    }

    void advanceFlashCounter() noexcept;
    bool flashInverted() const noexcept
    {
        return m_flashInvert;
    }
    const std::array<uint32_t, 16> &palette() const noexcept
    {
        return m_palette;
    }

    /// Floating bus emulation for 48K / 128K / +2 during active display.
    uint8_t readFloatingBus(const Clock &clock, const Mmu &mmu) const noexcept;

    void saveState(StateWriter &w) const;
    void loadState(StateReader &r);

private:
    BorderMargins m_margins;
    const MemoryBank &m_mmuBank(const Mmu &mmu) const;

    VideoFrame m_rasterFrame;
    FrameTimings m_rasterTimings{};
    uint32_t m_rasterCursor = 0;
    bool m_rasterActive = false;
    std::array<uint8_t, 2> m_bitmapLatch{};
    uint8_t m_attributeLatch = 0;

    uint8_t m_borderColor = 7;
    uint8_t m_scldReg = 0;
    VideoDisplayMode m_mode = VideoDisplayMode::StandardUla;
    BankId m_vramBankId = 0;
    uint16_t m_vramPage16k = 1;
    uint16_t m_vramOffsetBytes = 0;
    bool m_flashInvert = false;
    uint8_t m_flashFrameCounter = 0;
    std::array<uint32_t, 16> m_palette = kStandardZxPalette;
};

} // namespace zxkit
