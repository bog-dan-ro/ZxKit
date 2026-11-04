/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>
    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <zxkit/video/VideoFilters.h>
#include <zxkit/core/Exception.h>
#include "fixtures/video/FilterReference.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <future>
#include <limits>
#include <print>
#include <set>

#ifdef NDEBUG
#error Video filter tests require assertions, including in Release builds.
#endif

using namespace zxkit;

namespace {

template<class Function> void rejects(Function function)
{
    bool threw = false;
    try {
        function();
    } catch (const InvalidArgumentException&) {
        threw = true;
    }
    assert(threw);
}

void equalFrames(const VideoFrame& a, const VideoFrame& b)
{
    assert(a.width() == b.width() && a.height() == b.height());
    assert(a.frameNumber() == b.frameNumber());
    assert(std::ranges::equal(a.pixels(), b.pixels()));
}

VideoFrame pattern(uint32_t width, uint32_t height)
{
    VideoFrame source(width, height);
    uint32_t random = 0xC0FFEEu;
    for (auto& pixel : source.pixels()) {
        random = random * 1664525u + 1013904223u;
        pixel = kStandardZxPalette[(random >> 28) & 15];
    }
    source.setFrameNumber(12345);
    return source;
}

void catalogAndBoundaries()
{
    std::set<std::string_view> ids;
    std::set<VideoFilter> filters;
    std::size_t variants = 0;
    for (const auto& info : videoFilterCatalog()) {
        assert(!info.name.empty() && !info.id.empty());
        assert(ids.insert(info.id).second && filters.insert(info.filter).second);
        rejects([&] { VideoFilterPipeline pipeline({{info.filter, info.minScale - 1}}); });
        rejects([&] { VideoFilterPipeline pipeline({{info.filter, info.maxScale + 1}}); });
        for (auto scale = info.minScale; scale <= info.maxScale; ++scale) {
            ++variants;
            VideoFilterPipeline pipeline({{info.filter, scale}});
            assert(pipeline.stages().size() == 1);
            // Exercise every family with odd, asymmetric, single-row/column and
            // single-pixel input. Reuse each pipeline across resolution changes.
            for (const auto size : {VideoFilterSize{1, 1}, {1, 7}, {7, 1}, {3, 5}, {5, 3}}) {
                auto source = pattern(size.width, size.height);
                const auto before = source;
                const auto& result = pipeline.apply(source);
                assert(result.width() == size.width * scale);
                assert(result.height() == size.height * scale);
                assert(result.frameNumber() == source.frameNumber());
                assert(std::ranges::all_of(result.pixels(), [](uint32_t p) { return (p >> 24) == 255; }));
                equalFrames(source, before);
                const auto expected = result;
                equalFrames(pipeline.apply(source), expected);

                // Alpha must not influence color comparisons or leak to output.
                for (auto& pixel : source.pixels())
                    pixel &= 0x00FFFFFFu;
                equalFrames(pipeline.apply(source), expected);

                // Flat RGB must stay flat for all smoothing/resampling filters,
                // including corners; this also checks channel order and rounding.
                if (info.filter <= VideoFilter::SuperEagle) {
                    source.clear(packRgba(193, 59, 107));
                    const auto& flat = pipeline.apply(source);
                    assert(std::ranges::all_of(flat.pixels(), [](uint32_t p) {
                        return p == packRgba(193, 59, 107);
                    }));
                }
            }
        }
    }
    assert(variants == 32);

    VideoFilterPipeline pipeline({{VideoFilter::Hq, 4}});
    rejects([&] { pipeline.setStages({{VideoFilter::Tv, 0}}); });
    rejects([&] { pipeline.setStages({{static_cast<VideoFilter>(999), 2}}); });
    assert((pipeline.stages()[0] == VideoFilterStage{VideoFilter::Hq, 4}));
    rejects([] { VideoFilterPipeline pipeline({}, 0); });
    rejects([&] { pipeline.outputSize({0, 1}); });
    rejects([&] { pipeline.apply(VideoFrame(1, 0)); });
    rejects([&] { pipeline.outputSize({std::numeric_limits<uint32_t>::max(), 2}); });
    rejects([&] { pipeline.outputSize({8192, 8192}); });
    VideoFilterPipeline small({{VideoFilter::Nearest, 2}}, 16);
    assert((small.outputSize({2, 2}) == VideoFilterSize{4, 4}));
    rejects([&] { small.outputSize({3, 2}); });
    VideoFilterPipeline excessive(std::vector<VideoFilterStage>(40, {VideoFilter::Nearest, 6}));
    rejects([&] { excessive.outputSize({1, 1}); });
}

void numericResults()
{
    auto source = pattern(2, 2);
    VideoFilterPipeline nearest({{VideoFilter::Nearest, 3}});
    const auto& enlarged = nearest.apply(source);
    for (uint32_t y = 0; y < 6; ++y)
        for (uint32_t x = 0; x < 6; ++x)
            assert(enlarged.pixel(x, y) == source.pixel(x / 3, y / 3));

    source = VideoFrame(2, 1);
    source.putPixel(0, 0, packRgba(0, 0, 255));
    source.putPixel(1, 0, packRgba(255, 0, 0));
    VideoFilterPipeline bilinear({{VideoFilter::Bilinear, 2}});
    const auto& smooth = bilinear.apply(source);
    const std::array expected{packRgba(0, 0, 255), packRgba(64, 0, 191),
                              packRgba(191, 0, 64), packRgba(255, 0, 0)};
    assert(std::ranges::equal(smooth.scanline(0), expected));
    assert(std::ranges::equal(smooth.scanline(1), expected));

    // A red corner around a black center; Scale2x replaces exactly the upper
    // left subpixel. HQ blends the corner, while retaining the black interior.
    source = VideoFrame(3, 3);
    source.putPixel(1, 0, packRgba(255, 0, 0));
    source.putPixel(0, 1, packRgba(255, 0, 0));
    source.putPixel(2, 1, packRgba(255, 255, 255));
    source.putPixel(1, 2, packRgba(0, 0, 255));
    VideoFilterPipeline scale2({{VideoFilter::Scale, 2}});
    const auto& edged = scale2.apply(source);
    assert(edged.pixel(2, 2) == packRgba(255, 0, 0));
    assert(edged.pixel(3, 2) == packRgba(0, 0, 0));
    assert(edged.pixel(2, 3) == packRgba(0, 0, 0));
    assert(edged.pixel(3, 3) == packRgba(0, 0, 0));
    VideoFilterPipeline scale4({{VideoFilter::Scale, 4}});
    VideoFilterPipeline twice({{VideoFilter::Scale, 2}, {VideoFilter::Scale, 2}});
    equalFrames(scale4.apply(source), twice.apply(source));

    source = VideoFrame(1, 1);
    source.clear(packRgba(240, 160, 80));
    for (uint32_t factor = 2; factor <= 4; ++factor) {
        VideoFilterPipeline tv({{VideoFilter::Tv, factor}});
        const auto& result = tv.apply(source);
        for (uint32_t y = 0; y < factor; ++y) {
            const auto expected = y < (factor == 2 ? 1u : 2u)
                                ? packRgba(240, 160, 80) : packRgba(210, 140, 70);
            for (const auto pixel : result.scanline(y))
                assert(pixel == expected);
        }
    }
    VideoFilterPipeline dots({{VideoFilter::DotMatrix, 2}});
    const auto& dot = dots.apply(source);
    assert(dot.pixel(0, 0) == packRgba(240, 120, 80));
    assert(dot.pixel(1, 0) == packRgba(240, 160, 60));
    assert(dot.pixel(0, 1) == packRgba(180, 120, 60));
    assert(dot.pixel(1, 1) == packRgba(240, 160, 80));

    source = VideoFrame(3, 2);
    source.clear(packRgba(240, 160, 80));
    VideoFilterPipeline scanlines({{VideoFilter::Scanlines, 1}});
    const auto& scan = scanlines.apply(source);
    assert(scan.pixel(0, 0) == packRgba(240, 160, 80));
    assert(scan.pixel(0, 1) == packRgba(120, 80, 40));
    VideoFilterPipeline rgb({{VideoFilter::RgbMask, 1}});
    const auto& mask = rgb.apply(source);
    assert(mask.pixel(0, 0) == packRgba(240, 80, 40));
    assert(mask.pixel(1, 0) == packRgba(120, 160, 40));
    assert(mask.pixel(2, 0) == packRgba(120, 80, 80));
}

void upstreamReferences()
{
    // 16x16 tiles of 3x3 pixels: every binary neighborhood around a black
    // center, plus three rows of asymmetric RGB ramps for channel/YUV checks.
    VideoFrame source(48, 51);
    constexpr std::array positions{0u, 1u, 2u, 3u, 5u, 6u, 7u, 8u};
    for (uint32_t pattern = 0; pattern < 256; ++pattern) {
        const auto tx = (pattern % 16) * 3;
        const auto ty = (pattern / 16) * 3;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            const auto pos = positions[bit];
            const uint8_t value = (pattern & (1u << bit)) ? 255 : 0;
            source.putPixel(tx + pos % 3, ty + pos / 3, packRgba(value, value, value));
        }
    }
    for (uint32_t y = 48; y < 51; ++y)
        for (uint32_t x = 0; x < 48; ++x)
            source.putPixel(x, y, packRgba((x * 5 + y * 3) & 255,
                                          (x * 11 + y * 7) & 255,
                                          (x * 17 + y * 13) & 255));
    for (const auto& reference : kFilterReferences) {
        VideoFilterPipeline pipeline({reference.stage});
        const auto& output = pipeline.apply(source);
        uint64_t fingerprint = 14695981039346656037ull;
        for (const auto pixel : output.pixels()) {
            fingerprint ^= pixel;
            fingerprint *= 1099511628211ull;
        }
        if (fingerprint != reference.fingerprint)
            std::println(stderr, "Reference mismatch: filter {} scale {}: {:#x}, expected {:#x}",
                         static_cast<int>(reference.stage.filter), reference.stage.scale,
                         fingerprint, reference.fingerprint);
        assert(fingerprint == reference.fingerprint);
    }
}

void bilinearPixels()
{
    // Frozen output of the original integer kernel, before scale specialization
    // and cached column coordinates. Cover every scale, RGB/alpha, clipping,
    // single rows/columns and resolution changes without duplicating the kernel.
    constexpr std::array expected{
        0xb6d6f2951f5c43b0ull, 0x013b29a99786e40dull, 0x7183d95d0981b26cull,
        0x9afdddc4e9899eddull, 0x29ea760aab7eda68ull, 0x5ce18f3f92efedacull};
    for (uint32_t scale = 1; scale <= 6; ++scale) {
        uint64_t hash = 14695981039346656037ull;
        VideoFilterPipeline pipeline({{VideoFilter::Bilinear, scale}});
        for (auto size : {VideoFilterSize{1, 1}, {1, 7}, {7, 1}, {3, 5}, {5, 3}, {48, 51}}) {
            VideoFrame source(size.width, size.height);
            uint32_t random = 0xC0FFEEu;
            for (auto &pixel : source.pixels()) {
                random = random * 1664525u + 1013904223u;
                pixel = random;
            }
            for (auto pixel : pipeline.apply(source).pixels())
                hash = (hash ^ pixel) * 1099511628211ull;
        }
        assert(hash == expected[scale - 1]);
    }
    // Cached coordinates share scratch storage with the adapted kernels.
    // Chaining must not reuse stale coordinate/conversion data.
    const auto source = pattern(5, 3);
    VideoFilterPipeline bilinear({{VideoFilter::Bilinear, 3}}), hq({{VideoFilter::Hq, 2}});
    VideoFilterPipeline first({{VideoFilter::Bilinear, 3}, {VideoFilter::Hq, 2}});
    equalFrames(first.apply(source), hq.apply(bilinear.apply(source)));
    VideoFilterPipeline second({{VideoFilter::Hq, 2}, {VideoFilter::Bilinear, 3}});
    equalFrames(second.apply(source), bilinear.apply(hq.apply(source)));
}

void compositionAndOwnership()
{
    auto source = pattern(7, 5);
    const auto before = source;
    VideoFilterPipeline disabled;
    assert(&disabled.apply(source) == &source);
    VideoFilterPipeline chain({{VideoFilter::Hq, 4}, {VideoFilter::Tv, 4}});
    assert((chain.outputSize({320, 256}) == VideoFilterSize{5120, 4096}));
    VideoFilterPipeline hq({{VideoFilter::Hq, 4}});
    VideoFilterPipeline tv({{VideoFilter::Tv, 4}});
    const auto expected = tv.apply(hq.apply(source));
    equalFrames(chain.apply(source), expected);
    const auto data = chain.apply(source).pixels().data();
    assert(chain.apply(source).pixels().data() == data);
    VideoFilterPipeline reversed({{VideoFilter::Tv, 4}, {VideoFilter::Hq, 4}});
    assert(!std::ranges::equal(reversed.apply(source).pixels(), expected.pixels()));
    equalFrames(source, before);

    // Feeding a returned buffer back into the same pipeline must not resize or
    // overwrite its input before it has been read.
    const auto first = hq.apply(source);
    const auto& previous = hq.apply(source);
    VideoFilterPipeline reference({{VideoFilter::Hq, 4}});
    equalFrames(hq.apply(previous), reference.apply(first));
    chain.releaseBuffers();
    equalFrames(chain.apply(source), expected);
    chain.setStages({});
    assert(&chain.apply(source) == &source);
    chain.setStages({{VideoFilter::Nearest, 1}});
    equalFrames(chain.apply(source), source);

    // Independent pipelines must not share global format selectors or scratch.
    auto worker = [source] {
        VideoFilterPipeline pipeline({{VideoFilter::Xbrz, 3}, {VideoFilter::Hq, 2}});
        return VideoFrame(pipeline.apply(source));
    };
    auto a = std::async(std::launch::async, worker);
    auto b = std::async(std::launch::async, worker);
    equalFrames(a.get(), b.get());
}

} // namespace

int main()
{
    catalogAndBoundaries();
    numericResults();
    bilinearPixels();
    upstreamReferences();
    compositionAndOwnership();
    std::println("Video filter tests passed (32 variants, composition, borders and ownership)");
}
