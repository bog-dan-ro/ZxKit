/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>
    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "zxkit/video/VideoFilters.h"
#include "zxkit/core/Exception.h"
#include "fuse/FuseScalers.h"
#include "xbrz/xbrz.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace zxkit {
namespace {

constexpr std::array kCatalog{
    VideoFilterInfo{VideoFilter::Nearest, "nearest", "Nearest neighbour", 1, 6},
    VideoFilterInfo{VideoFilter::Bilinear, "bilinear", "Bilinear", 1, 6},
    VideoFilterInfo{VideoFilter::Scale, "scale", "Scale / AdvMAME", 2, 4},
    VideoFilterInfo{VideoFilter::Hq, "hq", "HQ", 2, 4},
    VideoFilterInfo{VideoFilter::Xbrz, "xbrz", "xBRZ", 2, 6},
    VideoFilterInfo{VideoFilter::Sai, "2xsai", "2xSaI", 2, 2},
    VideoFilterInfo{VideoFilter::SuperSai, "super2xsai", "Super 2xSaI", 2, 2},
    VideoFilterInfo{VideoFilter::SuperEagle, "supereagle", "SuperEagle", 2, 2},
    VideoFilterInfo{VideoFilter::Tv, "tv", "TV", 2, 4},
    VideoFilterInfo{VideoFilter::DotMatrix, "dotmatrix", "Dot Matrix", 2, 2},
    VideoFilterInfo{VideoFilter::Scanlines, "scanlines", "Scanlines", 1, 1},
    VideoFilterInfo{VideoFilter::RgbMask, "rgbmask", "RGB phosphor mask", 1, 1},
};

void validateStage(VideoFilterStage stage)
{
    const auto info = std::ranges::find(kCatalog, stage.filter, &VideoFilterInfo::filter);
    if (info == kCatalog.end() || stage.scale < info->minScale || stage.scale > info->maxScale)
        throw InvalidArgumentException("Unsupported video filter or scale");
}

void validateSize(uint64_t width, uint64_t height, std::size_t maxPixels)
{
    // The adapted kernels use signed int coordinates and 32-bit byte pitches.
    // Bound padded byte counts too, so all kernel offset arithmetic fits.
    constexpr uint64_t maxKernelPixels = std::numeric_limits<int>::max() / sizeof(uint32_t);
    if (!width || !height || width > maxKernelPixels || height > maxKernelPixels
        || width * height > maxPixels || (width + 4) * (height + 4) > maxKernelPixels)
        throw InvalidArgumentException("Video filter dimensions exceed the pixel or kernel limit");
}

uint32_t swapRedBlue(uint32_t pixel)
{
    return (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) | ((pixel >> 16) & 0xFFu);
}

void prepareFrame(VideoFrame& frame, uint32_t width, uint32_t height)
{
    // Allocate before replacing dimensions: a failed allocation must leave the
    // pipeline reusable, with each frame's dimensions matching its storage.
    if (frame.width() != width || frame.height() != height)
        frame = VideoFrame(width, height);
}

// Pixel-center-aligned bilinear sampling, using integer weights for repeatability.
struct Sample {
    uint32_t first;
    uint32_t second;
    uint32_t fraction;
};

Sample sample(uint32_t coordinate, uint32_t sourceSize, uint32_t scale)
{
    const auto denominator = 2 * scale;
    const auto position = std::clamp<int64_t>(2 * int64_t(coordinate) + 1 - scale,
                                             0, int64_t(sourceSize - 1) * denominator);
    const auto first = static_cast<uint32_t>(position / denominator);
    return {first, std::min(first + 1, sourceSize - 1),
            static_cast<uint32_t>(position % denominator)};
}

template<uint32_t Scale>
void bilinear(const VideoFrame& source, VideoFrame& target, std::vector<uint32_t>& columns)
{
    constexpr uint32_t denominator = 2 * Scale;
    constexpr uint32_t divisor = denominator * denominator;
    // The horizontal sampling positions repeat for every row. Reuse the
    // pipeline's scratch storage rather than recomputing them per output pixel.
    columns.resize(std::size_t(target.width()) * 2);
    for (uint32_t x = 0; x < target.width(); ++x) {
        const auto sx = sample(x, source.width(), Scale);
        columns[x * 2] = sx.first;
        columns[x * 2 + 1] = sx.fraction;
    }
    for (uint32_t y = 0; y < target.height(); ++y) {
        const auto sy = sample(y, source.height(), Scale);
        const auto top = source.scanline(sy.first);
        const auto bottom = source.scanline(sy.second);
        auto row = target.scanline(y);
        for (uint32_t x = 0; x < target.width(); ++x) {
            const Sample sx{columns[x * 2], std::min(columns[x * 2] + 1, source.width() - 1),
                            columns[x * 2 + 1]};
            uint32_t pixel = 0xFF000000u;
            for (uint32_t shift = 0; shift < 24; shift += 8) {
                const auto a = (top[sx.first] >> shift) & 0xFFu;
                const auto b = (top[sx.second] >> shift) & 0xFFu;
                const auto c = (bottom[sx.first] >> shift) & 0xFFu;
                const auto d = (bottom[sx.second] >> shift) & 0xFFu;
                const auto value = a * (denominator - sx.fraction) * (denominator - sy.fraction)
                                 + b * sx.fraction * (denominator - sy.fraction)
                                 + c * (denominator - sx.fraction) * sy.fraction
                                 + d * sx.fraction * sy.fraction;
                pixel |= ((value + divisor / 2) / divisor) << shift;
            }
            row[x] = pixel;
        }
    }
}

} // namespace

std::span<const VideoFilterInfo> videoFilterCatalog() noexcept
{
    return kCatalog;
}

VideoFilterPipeline::VideoFilterPipeline(std::vector<VideoFilterStage> stages, std::size_t maxPixels)
    : m_maxPixels(maxPixels)
{
    if (!maxPixels)
        throw InvalidArgumentException("Video filter pixel limit must be positive");
    setStages(std::move(stages));
}

void VideoFilterPipeline::setStages(std::vector<VideoFilterStage> stages)
{
    for (const auto stage : stages)
        validateStage(stage);
    m_stages = std::move(stages);
}

VideoFilterSize VideoFilterPipeline::outputSize(VideoFilterSize input) const
{
    uint64_t width = input.width;
    uint64_t height = input.height;
    validateSize(width, height, m_maxPixels);
    for (const auto stage : m_stages) {
        width *= stage.scale;
        height *= stage.scale;
        validateSize(width, height, m_maxPixels);
    }
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

const VideoFrame& VideoFilterPipeline::apply(const VideoFrame& source)
{
    outputSize({source.width(), source.height()});
    // Choose a buffer distinct from source, including when reusing our last result.
    std::size_t index = &source == &m_buffers[0] ? 1 : 0;
    const VideoFrame* current = &source;
    for (const auto stage : m_stages) {
        auto& target = m_buffers[index];
        prepareFrame(target, current->width() * stage.scale, current->height() * stage.scale);
        applyStage(stage, *current, target);
        target.setFrameNumber(current->frameNumber());
        current = &target;
        index ^= 1;
    }
    return *current;
}

void VideoFilterPipeline::releaseBuffers()
{
    for (auto& frame : m_buffers)
        frame = VideoFrame(0, 0);
    m_scaleScratch = VideoFrame(0, 0);
    std::vector<uint32_t>().swap(m_inputScratch);
}

void VideoFilterPipeline::applyStage(VideoFilterStage stage, const VideoFrame& source, VideoFrame& target)
{
    switch (stage.filter) {
    case VideoFilter::Nearest:
        for (uint32_t y = 0; y < target.height(); ++y) {
            const auto input = source.scanline(y / stage.scale);
            auto output = target.scanline(y);
            for (uint32_t x = 0; x < source.width(); ++x)
                std::ranges::fill(output.subspan(std::size_t(x) * stage.scale, stage.scale),
                                  input[x] | 0xFF000000u);
        }
        return;
    case VideoFilter::Bilinear:
        // Fixed denominators preserve the integer weights and rounding while
        // avoiding division instructions for every output pixel/channel.
        switch (stage.scale) {
        case 1: bilinear<1>(source, target, m_inputScratch); break;
        case 2: bilinear<2>(source, target, m_inputScratch); break;
        case 3: bilinear<3>(source, target, m_inputScratch); break;
        case 4: bilinear<4>(source, target, m_inputScratch); break;
        case 5: bilinear<5>(source, target, m_inputScratch); break;
        case 6: bilinear<6>(source, target, m_inputScratch); break;
        }
        return;
    case VideoFilter::Xbrz:
        // xBRZ measures luminance in 0xAARRGGBB; VideoFrame uses 0xAABBGGRR.
        m_inputScratch.resize(source.pixels().size());
        std::ranges::transform(source.pixels(), m_inputScratch.begin(), [](uint32_t pixel) {
            return swapRedBlue(pixel) | 0xFF000000u;
        });
        detail::xbrz::scale(stage.scale, m_inputScratch.data(), target.pixels().data(),
                            static_cast<int>(source.width()), static_cast<int>(source.height()),
                            detail::xbrz::ColorFormat::ARGB_UNBUFFERED);
        std::ranges::transform(target.pixels(), target.pixels().begin(), [](uint32_t pixel) {
            return swapRedBlue(pixel) | 0xFF000000u;
        });
        return;
    case VideoFilter::Scanlines:
    case VideoFilter::RgbMask:
        for (uint32_t y = 0; y < source.height(); ++y) {
            const auto input = source.scanline(y);
            auto output = target.scanline(y);
            for (uint32_t x = 0; x < source.width(); ++x) {
                const uint32_t pixel = input[x] & 0x00FFFFFFu;
                const uint32_t dim = (pixel >> 1) & 0x007F7F7Fu;
                if (stage.filter == VideoFilter::Scanlines)
                    output[x] = ((y & 1) ? dim : pixel) | 0xFF000000u;
                else {
                    const uint32_t channel = 0xFFu << (8 * (x % 3));
                    output[x] = (pixel & channel) | (dim & ~channel) | 0xFF000000u;
                }
            }
        }
        return;
    case VideoFilter::Scale:
        if (stage.scale == 4) {
            prepareFrame(m_scaleScratch, source.width() * 2, source.height() * 2);
            applyFuse({VideoFilter::Scale, 2}, source, m_scaleScratch);
            applyFuse({VideoFilter::Scale, 2}, m_scaleScratch, target);
            return;
        }
        [[fallthrough]];
    default:
        applyFuse(stage, source, target);
    }
}

void VideoFilterPipeline::applyFuse(VideoFilterStage stage, const VideoFrame& source, VideoFrame& target)
{
    using namespace detail::fuse;
    using Kernel = void (*)(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
    Kernel kernel = nullptr;
    switch (stage.filter) {
    case VideoFilter::Scale:
        kernel = stage.scale == 2 ? scaler_AdvMame2x : scaler_AdvMame3x;
        break;
    case VideoFilter::Hq:
        kernel = stage.scale == 2 ? scaler_HQ2x : stage.scale == 3 ? scaler_HQ3x : scaler_HQ4x;
        break;
    case VideoFilter::Sai: kernel = scaler_2xSaI; break;
    case VideoFilter::SuperSai: kernel = scaler_Super2xSaI; break;
    case VideoFilter::SuperEagle: kernel = scaler_SuperEagle; break;
    case VideoFilter::Tv:
        kernel = stage.scale == 2 ? scaler_TV2x : stage.scale == 3 ? scaler_TV3x : scaler_TV4x;
        break;
    case VideoFilter::DotMatrix: kernel = scaler_DotMatrix; break;
    default: throw InvalidArgumentException("Unsupported Fuse filter");
    }

    // SaI needs two extra pixels to the right/bottom. HQ also reads ahead after
    // its last output pixel. Replicate a two-pixel border on all sides.
    const std::size_t pitch = std::size_t(source.width()) + 4;
    m_inputScratch.resize(pitch * (std::size_t(source.height()) + 4));
    for (std::size_t y = 0; y < source.height() + 4u; ++y) {
        const auto sy = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(y) - 2, 0, source.height() - 1));
        const auto input = source.scanline(sy);
        const auto row = std::span(m_inputScratch).subspan(y * pitch, pitch);
        row[0] = row[1] = input.front() & 0x00FFFFFFu;
        std::ranges::transform(input, row.begin() + 2, [](uint32_t pixel) { return pixel & 0x00FFFFFFu; });
        row[pitch - 2] = row[pitch - 1] = input.back() & 0x00FFFFFFu;
    }
    kernel(reinterpret_cast<const uint8_t*>(m_inputScratch.data() + 2 * pitch + 2),
           static_cast<uint32_t>(pitch * sizeof(uint32_t)),
           reinterpret_cast<uint8_t*>(target.pixels().data()),
           target.width() * sizeof(uint32_t), static_cast<int>(source.width()),
           static_cast<int>(source.height()));
    for (auto& pixel : target.pixels())
        pixel |= 0xFF000000u;
}

} // namespace zxkit
