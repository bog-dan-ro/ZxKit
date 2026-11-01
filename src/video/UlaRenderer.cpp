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

#include "zxkit/video/UlaRenderer.h"

#include <algorithm>
#include <utility>

namespace zxkit {

UlaRenderer::UlaRenderer()
{
    reset();
}

void UlaRenderer::reset() noexcept
{
    m_rasterActive = false;
    m_borderColor = 7;
    m_scldReg = 0;
    m_mode = VideoDisplayMode::StandardUla;
    m_vramBankId = 0;
    m_vramPage16k = 1;
    m_vramOffsetBytes = 0;
    m_flashInvert = false;
    m_flashFrameCounter = 0;
    m_palette = kStandardZxPalette;
}

void UlaRenderer::setVramSource(BankId bankId, uint16_t page16k, uint16_t offsetBytes) noexcept
{
    m_vramBankId = bankId;
    m_vramPage16k = page16k;
    m_vramOffsetBytes = offsetBytes;
}

void UlaRenderer::setBorderColor(uint8_t color) noexcept
{
    m_borderColor = color & 0x07;
}

void UlaRenderer::setScldRegister(uint8_t val) noexcept
{
    m_scldReg = val;
    switch (val & 0x07) {
    case 0:
        m_mode = VideoDisplayMode::StandardUla;
        break;
    case 1:
        m_mode = VideoDisplayMode::SecondScreen;
        break;
    case 2:
    case 3:
        m_mode = VideoDisplayMode::TimexHiColor;
        break;
    default:
        m_mode = VideoDisplayMode::TimexHiRes;
        break;
    }
}

void UlaRenderer::renderScanline(uint32_t y, const Mmu &mmu, VideoFrame &frame) const
{
    if (y >= kTotalHeightLines)
        return;

    const bool hiRes = (m_mode == VideoDisplayMode::TimexHiRes);
    const uint32_t targetWidth = hiRes ? 640 : kTotalWidthPx;
    if (frame.width() != targetWidth || frame.height() != kTotalHeightLines)
        frame.resize(targetWidth, kTotalHeightLines);

    auto lineOut = frame.scanline(y);
    const uint32_t borderRgba = m_palette[m_borderColor & 0x07];

    if (y < kBorderTopLines || y >= kBorderTopLines + kScreenLines || m_vramBankId == 0) {
        std::ranges::fill(lineOut, borderRgba);
        return;
    }

    const uint32_t screenY = y - kBorderTopLines; // 0 .. 191
    const auto &ramBank = m_mmuBank(mmu);
    const auto vram16k = ramBank.span16k(m_vramPage16k);

    const uint32_t borderLeft = hiRes ? (kBorderLeftPx * 2) : kBorderLeftPx;
    for (uint32_t x = 0; x < borderLeft; ++x)
        lineOut[x] = borderRgba;

    const uint16_t pixelLineOffset = static_cast<uint16_t>(
        ((screenY & 0xC0) << 5) | ((screenY & 0x07) << 8) | ((screenY & 0x38) << 2));
    const uint16_t attrLineOffset = static_cast<uint16_t>(0x1800 + ((screenY >> 3) << 5));

    if (!hiRes) {
        const uint16_t baseShift = (m_mode == VideoDisplayMode::SecondScreen) ? 0x2000 : m_vramOffsetBytes;
        for (uint32_t col = 0; col < 32; ++col) {
            const uint8_t bitmap = vram16k[(baseShift + pixelLineOffset + col) & 0x3FFF];
            uint8_t attr = 0;
            if (m_mode == VideoDisplayMode::TimexHiColor) {
                attr = vram16k[(0x2000 + pixelLineOffset + col) & 0x3FFF];
            } else {
                attr = vram16k[(baseShift + attrLineOffset + col) & 0x3FFF];
            }

            const uint8_t bright = (attr & 0x40) ? 8 : 0;
            uint8_t inkIdx   = (attr & 0x07) + bright;
            uint8_t paperIdx = ((attr >> 3) & 0x07) + bright;
            if ((attr & 0x80) && m_flashInvert)
                std::swap(inkIdx, paperIdx);

            const uint32_t inkRgba   = m_palette[inkIdx];
            const uint32_t paperRgba = m_palette[paperIdx];
            const auto dst = lineOut.subspan(borderLeft + col * 8, 8);
            for (uint32_t x = 0; x < 8; ++x)
                dst[x] = ((bitmap << x) & 0x80) ? inkRgba : paperRgba;
        }
    } else {
        // Timex 512x192 Hi-Res mode: columns alternate between 0x0000 and 0x2000
        const uint8_t inkIdx = (m_scldReg >> 3) & 0x07;
        const uint8_t paperIdx = (~inkIdx) & 0x07;
        const uint32_t inkRgba   = m_palette[inkIdx + 8];
        const uint32_t paperRgba = m_palette[paperIdx + 8];

        for (uint32_t col = 0; col < 32; ++col) {
            const uint8_t bm0 = vram16k[(pixelLineOffset + col) & 0x3FFF];
            const uint8_t bm1 = vram16k[(0x2000 + pixelLineOffset + col) & 0x3FFF];
            const auto dst = lineOut.subspan(borderLeft + col * 16, 16);
            for (uint32_t x = 0; x < 8; ++x) {
                dst[x] = ((bm0 << x) & 0x80) ? inkRgba : paperRgba;
                dst[x + 8] = ((bm1 << x) & 0x80) ? inkRgba : paperRgba;
            }
        }
    }

    const uint32_t rightStart = borderLeft + (hiRes ? 512 : kScreenWidthPx);
    for (uint32_t x = rightStart; x < targetWidth; ++x)
        lineOut[x] = borderRgba;
}

void UlaRenderer::renderFullFrame(const Mmu &mmu, VideoFrame &frame)
{
    for (uint32_t y = 0; y < kTotalHeightLines; ++y)
        renderScanline(y, mmu, frame);
    advanceFlashCounter();
}

void UlaRenderer::beginRasterFrame(const FrameTimings &timings)
{
    m_rasterTimings = timings;
    m_rasterCursor = timings.firstScreenTstate() - kBorderTopLines * timings.tstatesPerLine
                     - kBorderLeftPx / 2;
    m_bitmapLatch.fill(0);
    m_rasterActive = true;
}

void UlaRenderer::renderUntil(uint32_t frameTstate, const Mmu &mmu)
{
    if (!m_rasterActive || frameTstate <= m_rasterCursor)
        return;
    const auto &t = m_rasterTimings;
    const uint32_t first = t.firstScreenTstate() - kBorderTopLines * t.tstatesPerLine
                           - kBorderLeftPx / 2;
    const uint32_t end = std::min(frameTstate, first + kTotalHeightLines * t.tstatesPerLine);
    const auto vram = m_vramBankId ? mmu.bank(m_vramBankId).span16k(m_vramPage16k)
                                  : std::span<const uint8_t>{};
    while (m_rasterCursor < end) {
        const uint32_t relative = m_rasterCursor - first;
        const uint32_t y = relative / t.tstatesPerLine;
        const uint32_t tick = relative % t.tstatesPerLine;
        if (tick >= kTotalWidthPx / 2) {
            m_rasterCursor += std::min(end - m_rasterCursor, t.tstatesPerLine - tick);
            continue;
        }
        auto row = m_rasterFrame.scanline(y);
        const int32_t screenY = int32_t(y) - kBorderTopLines;
        const int32_t screenTick = int32_t(tick) - kBorderLeftPx / 2;
        const bool screenLine = screenY >= 0 && screenY < int32_t(kScreenLines) && !vram.empty();
        if (!screenLine || screenTick < 0 || screenTick >= 128) {
            row[tick * 2] = row[tick * 2 + 1] = m_palette[m_borderColor];
        }
        // A pair of characters is fetched on four consecutive clocks followed
        // by four idle clocks. Attribute fetch completes each character latch;
        // later writes cannot change the eight pixels already in that latch.
        if (screenLine && screenTick >= -1 && screenTick < 123) {
            const uint32_t fetchTick = screenTick + 1;
            const uint32_t phase = fetchTick & 7;
            if (phase < 4) {
                const uint32_t col = (fetchTick / 8) * 2 + phase / 2;
                const uint16_t bitmapOffset = ((screenY & 0xc0) << 5) | ((screenY & 7) << 8)
                                              | ((screenY & 0x38) << 2) | col;
                if (!(phase & 1)) {
                    m_bitmapLatch[phase / 2] = vram[(m_vramOffsetBytes + bitmapOffset) & 0x3fff];
                } else {
                    const uint16_t attrOffset = 0x1800 + (screenY / 8) * 32 + col;
                    m_attributeLatch = vram[(m_vramOffsetBytes + attrOffset) & 0x3fff];
                    const uint8_t bright = (m_attributeLatch & 0x40) ? 8 : 0;
                    uint8_t ink = (m_attributeLatch & 7) + bright;
                    uint8_t paper = ((m_attributeLatch >> 3) & 7) + bright;
                    if ((m_attributeLatch & 0x80) && m_flashInvert)
                        std::swap(ink, paper);
                    const uint8_t bits = m_bitmapLatch[phase / 2];
                    for (uint32_t x = 0; x < 8; ++x)
                        row[kBorderLeftPx + col * 8 + x] = m_palette[(bits & (0x80 >> x)) ? ink : paper];
                }
            }
        }
        ++m_rasterCursor;
    }
}

void UlaRenderer::copyRasterScanline(uint32_t y, VideoFrame &frame) const
{
    if (frame.width() != kTotalWidthPx || frame.height() != kTotalHeightLines)
        frame.resize(kTotalWidthPx, kTotalHeightLines);
    std::ranges::copy(m_rasterFrame.scanline(y), frame.scanline(y).begin());
}

void UlaRenderer::advanceFlashCounter() noexcept
{
    if (++m_flashFrameCounter >= 16) {
        m_flashFrameCounter = 0;
        m_flashInvert = !m_flashInvert;
    }
}

uint8_t UlaRenderer::readFloatingBus(const Clock &clock, const Mmu &mmu) const noexcept
{
    if (m_vramBankId == 0)
        return 0xFF;

    const auto &t = clock.timings();
    const uint32_t now = clock.frameBaseTstates();
    const uint32_t firstFetch = t.firstScreenTstate() + 2;
    if (now < firstFetch)
        return 0xFF;
    const uint32_t elapsed = now - firstFetch;
    const uint32_t screenY = elapsed / t.tstatesPerLine;
    const uint32_t relTs = elapsed % t.tstatesPerLine;
    if (screenY >= t.screenLines || relTs >= t.screenTstates)
        return 0xFF;
    const uint8_t phase = relTs & 7;
    if (phase >= 4)
        return 0xFF;
    const uint16_t col = static_cast<uint16_t>((relTs / 8) * 2 + phase / 2);
    const auto vram16k = mmu.bank(m_vramBankId).span16k(m_vramPage16k);
    if ((phase & 1) == 0) {
        const uint16_t pixelOffset = static_cast<uint16_t>(
            ((screenY & 0xC0) << 5) | ((screenY & 0x07) << 8) | ((screenY & 0x38) << 2) | (col & 0x1F));
        return vram16k[pixelOffset & 0x3FFF];
    } else {
        const uint16_t attrOffset = static_cast<uint16_t>(0x1800 + ((screenY >> 3) << 5) + (col & 0x1F));
        return vram16k[attrOffset & 0x3FFF];
    }
}

void UlaRenderer::saveState(StateWriter &w) const
{
    w.write(m_borderColor);
    w.write(m_scldReg);
    w.write(static_cast<uint8_t>(m_mode));
    w.write(m_vramBankId);
    w.write(m_vramPage16k);
    w.write(m_vramOffsetBytes);
    w.writeBool(m_flashInvert);
    w.write(m_flashFrameCounter);
}

void UlaRenderer::loadState(StateReader &r)
{
    m_rasterActive = false;
    m_borderColor       = r.read<uint8_t>();
    m_scldReg           = r.read<uint8_t>();
    m_mode              = static_cast<VideoDisplayMode>(r.read<uint8_t>());
    m_vramBankId        = r.read<BankId>();
    m_vramPage16k       = r.read<uint16_t>();
    m_vramOffsetBytes   = r.read<uint16_t>();
    m_flashInvert       = r.readBool();
    m_flashFrameCounter = r.read<uint8_t>();
}

const MemoryBank &UlaRenderer::m_mmuBank(const Mmu &mmu) const
{
    return mmu.bank(m_vramBankId);
}

} // namespace zxkit
