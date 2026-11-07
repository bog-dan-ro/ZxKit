/*
    Copyright (c) 2026, BogDan Vatra <bogdan@kde.org>
    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "zxkit/video/VideoFrame.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace zxkit {

/// Optional host-side software filters. Link ZX::VideoFilters to use this API.
enum class VideoFilter {
    Nearest,
    Bilinear,
    Scale, ///< AdvMAME/Scale2x, Scale3x; Scale4x is two Scale2x passes.
    Hq, ///< Maxim Stepin's HQ2x, HQ3x and HQ4x.
    Xbrz, ///< Zenju's xBRZ, 2x through 6x.
    Sai, ///< 2xSaI.
    SuperSai, ///< Super 2xSaI.
    SuperEagle,
    Tv, ///< Fuse-style bright/dim scanline blocks, 2x through 4x.
    DotMatrix, ///< Fuse's 2x dot-matrix mask.
    Scanlines, ///< Dims odd rows to 50%, without resizing.
    RgbMask ///< RGB phosphor stripes; other channels at 50%, no resizing.
};

struct VideoFilterInfo
{
    VideoFilter filter;
    std::string_view id;
    std::string_view name;
    uint32_t minScale;
    uint32_t maxScale;
};

/// Stable IDs and supported integer scales for menus and stored settings.
std::span<const VideoFilterInfo> videoFilterCatalog() noexcept;

struct VideoFilterStage
{
    VideoFilter filter;
    uint32_t scale; ///< Each stage multiplies the dimensions of its input.
    bool operator==(const VideoFilterStage &) const = default;
};

struct VideoFilterSize
{
    uint32_t width;
    uint32_t height;
    bool operator==(const VideoFilterSize &) const = default;
};

/// Ordered post-processing, separate from emulation and GPU rendering.
/// Active stages ignore input alpha and produce opaque 0xFFBBGGRR pixels.
/// Neighborhood filters repeat the nearest border pixel. No shared mutable state.
/// A pipeline owns reusable scratch buffers and must not be used concurrently;
/// independent pipelines may run concurrently.
class VideoFilterPipeline
{
public:
    /// Bound each logical input/intermediate/output frame to 64M pixels by default.
    /// Scratch buffers and neighborhood padding require additional memory.
    static constexpr std::size_t kDefaultMaxPixels = 64 * 1024 * 1024;

    explicit VideoFilterPipeline(std::vector<VideoFilterStage> stages = {},
                                 std::size_t maxPixels = kDefaultMaxPixels);

    /// Invalid stages throw InvalidArgumentException, preserving the old stages.
    void setStages(std::vector<VideoFilterStage> stages);
    std::span<const VideoFilterStage> stages() const noexcept
    {
        return m_stages;
    }
    std::size_t maxPixels() const noexcept
    {
        return m_maxPixels;
    }

    /// Preflight every stage without allocating. Rejects zero dimensions, overflow,
    /// unsupported stages and frames exceeding maxPixels (InvalidArgumentException).
    VideoFilterSize outputSize(VideoFilterSize input) const;

    /// Preserves source pixels and frameNumber. An empty pipeline returns source
    /// by reference, with no allocation or pixel conversion. Otherwise returns an
    /// owned buffer, valid until the next apply(), releaseBuffers() or destruction.
    /// Copy the result when it needs to outlive that interval. The previous result
    /// may itself be used as input. Allocation failures propagate as std::bad_alloc.
    const VideoFrame &apply(const VideoFrame &source);

    /// Release cached storage after disabling filters or changing resolution.
    void releaseBuffers();

private:
    void applyStage(VideoFilterStage stage, const VideoFrame &source, VideoFrame &target);
    void applyFuse(VideoFilterStage stage, const VideoFrame &source, VideoFrame &target);

    std::vector<VideoFilterStage> m_stages;
    std::size_t m_maxPixels;
    std::array<VideoFrame, 2> m_buffers{ VideoFrame(0, 0), VideoFrame(0, 0) };
    VideoFrame m_scaleScratch{ 0, 0 };
    std::vector<uint32_t> m_inputScratch;
};

} // namespace zxkit
