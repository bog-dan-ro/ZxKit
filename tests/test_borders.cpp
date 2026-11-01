/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <zxkit/zxkit.h>
#include <algorithm>
#include <cassert>
#include <print>

using namespace zxkit;

void rasterCrops()
{
    Mmu mmu;
    const auto bank = mmu.registerBank("ram", 16384, true);
    auto bytes = mmu.bank(bank).bytes();
    for (std::size_t n = 0; n < bytes.size(); ++n)
        bytes[n] = uint8_t(n * 17 + n / 7);
    UlaRenderer ula;
    ula.setVramSource(bank, 0);
    ula.setBorderColor(3);
    ula.setBorderMargins({48, 56, 48, 56});
    VideoFrame full;
    ula.renderFullFrame(mmu, full);

    for (const auto margins : {BorderMargins{0, 0, 0, 0}, BorderMargins{1, 3, 7, 5},
                               BorderMargins{48, 56, 48, 56}, BorderMargins{32, 32, 32, 32}}) {
        ula.setBorderMargins(margins);
        ula.beginRasterFrame(kTimings48k);
        ula.renderUntil(kTimings48k.tstatesPerFrame(), mmu);
        VideoFrame frame;
        for (uint32_t y = 0; y < ula.outputHeight(); ++y)
            ula.copyRasterScanline(y, frame);
        assert(frame.width() == margins.left + 256u + margins.right);
        assert(frame.height() == margins.top + 192u + margins.bottom);
        for (uint32_t y = 0; y < frame.height(); ++y)
            for (uint32_t x = 0; x < frame.width(); ++x)
                assert(frame.pixel(x, y) == full.pixel(x + 48 - margins.left, y + 56 - margins.top));
    }

    // A colour transition in a region the old 32-line crop never captured.
    ula.setBorderMargins({48, 56, 48, 56});
    ula.setBorderColor(2);
    ula.beginRasterFrame(kTimings48k);
    const uint32_t first = kTimings48k.firstScreenTstate() - 56 * 224 - 24;
    ula.renderUntil(first + 10, mmu);
    ula.setBorderColor(1);
    ula.renderUntil(first + 176, mmu);
    VideoFrame frame;
    ula.copyRasterScanline(0, frame);
    for (unsigned x = 0; x < 352; ++x)
        assert(frame.pixel(x, 0) == kStandardZxPalette[x < 20 ? 2 : 1]);
}

void frameBoundaries()
{
    Emulator full({.fullBorder = true}), reference;
    full.runScanline();
    reference.runScanline();
    const auto time = full.clock().totalTstates();
    const auto pc = full.cpu().registers().PC;
    const auto refreshed = full.refreshFrame();
    assert(refreshed.readLock()->width() == 352 && refreshed.readLock()->height() == 311);
    assert(full.clock().totalTstates() == time && full.cpu().registers().PC == pc);
    full.runFrames(2);
    reference.runFrames(2);
    assert(full.clock().totalTstates() == reference.clock().totalTstates());
    assert(full.cpu().registers().PC == reference.cpu().registers().PC);
    assert(full.saveSnapshot() == reference.saveSnapshot());
    const auto paper = full.lastFrame().readLock()->paperRect();
    assert((paper == VideoFrame::PaperRect{48, 63, 256, 192}));
    const auto saved = full.saveSnapshot();
    full.loadSnapshot(saved, ".szx");
    assert(full.refreshFrame().readLock()->paperRect() == paper);
    assert(reference.lastFrame().readLock()->width() == 320);
}

void machineCrops()
{
    for (const char *id : {"48k", "128k", "plus3", "pentagon", "tc2048", "ts2068", "se", "cobra1_80k", "next"}) {
        Emulator normal({.machineId = id}), full({.machineId = id, .fullBorder = true});
        for (const bool hires : {false, true}) {
            if (hires && std::string_view(id) != "tc2048")
                continue;
            if (hires) {
                normal.ioBus().write(0xFF, 6);
                full.ioBus().write(0xFF, 6);
            }
            const auto native = normal.refreshFrame();
            const auto original = native.readLock();
            const auto enlarged = full.refreshFrame();
            const auto image = enlarged.readLock();
            const auto paper = image->paperRect();
            const uint32_t scaleX = paper.width / 256;
            const uint32_t scaleY = paper.height / 192;
            assert(image->width() >= original->width() && image->height() >= original->height());
            for (uint32_t y = 0; y < original->height(); ++y)
                for (uint32_t x = 0; x < original->width(); ++x)
                    assert(image->pixel(x + paper.x - 32 * scaleX, y + paper.y - 32 * scaleY) == original->pixel(x, y));
        }
        const auto running = full.runFrames();
        const auto paused = full.refreshFrame();
        assert(running.readLock()->width() == paused.readLock()->width());
        assert(running.readLock()->height() == paused.readLock()->height());
        assert(running.readLock()->paperRect() == paused.readLock()->paperRect());
    }
}

void hiddenCopper()
{
    for (const bool fullBorder : {false, true}) {
        Emulator e({.machineId = "next", .fullBorder = fullBorder});
        auto &next = dynamic_cast<ZxNextMachine &>(e.machine());
        auto &copper = next.copper();
        for (uint8_t byte : {0x80, 20, 0x4A, 0xE3, 0xFF, 0xFF})
            copper.writeData8(byte);
        copper.setControlHi(0x40);
        // End before the first paper row: this WAIT/MOVE happens in the hidden top border.
        for (unsigned n = 0; n < 53; ++n)
            e.runScanline();
        assert(next.nextReg().peek(0x4A) == 0xE3);
    }
}

int main()
{
    rasterCrops();
    frameBoundaries();
    machineCrops();
    hiddenCopper();
    std::println("Full border frames: raster effects, paper metadata, machine state and Copper passed");
}
