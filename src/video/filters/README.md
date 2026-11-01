# Software video filters

These algorithms form a separate host presentation library, `ZX::VideoFilters`.
The emulator does not call them. Their code is bundled and built with ZxKit;
consumers link this target only when they want CPU post-processing.

## Algorithms and selection

| Family | Scales | Typical use |
| --- | --- | --- |
| Nearest neighbour | 1–6 | Crisp Spectrum pixels and text; lowest processing cost |
| Bilinear | 1–6 | Conventional soft interpolation; blends all edges |
| Scale / AdvMAME | 2, 3, 4 | Fast edge smoothing without introducing new colors |
| HQ | 2, 3, 4 | Pattern-based smoothing with perceptual YUV edge detection |
| xBRZ | 2–6 | Detailed pixel-art smoothing and larger integer enlargement |
| 2xSaI | 2 | Classic interpolated smoothing |
| Super 2xSaI | 2 | Alternative SaI edge and texture treatment |
| SuperEagle | 2 | Stronger classic smoothing |
| TV | 2, 3, 4 | Fuse's repeated pixels with dimmed scanline rows |
| Dot Matrix | 2 | Fuse's repeating 4×4 RGB dot pattern |
| Scanlines | 1 | Dim alternate output rows without further enlargement |
| RGB phosphor mask | 1 | RGB stripes without further enlargement |

There are 32 family/scale combinations. There is no universal best filter:
nearest preserves small text, Scale keeps the palette, and HQ/xBRZ smooth game
art with blended colors. Compare them on the content you intend to show.
TV and masks are stylistic effects, not simulations of analog video signals.
Fuse's PAL/NTSC composite filters and Timex-specific fractional resizing are
not included. The existing emulator output and hardware rendering are unaffected.

## Composing stages

```cmake
target_link_libraries(my_software_renderer PRIVATE ZX::VideoFilters)
```

```cpp
#include <zxkit/video/VideoFilters.h>

zxkit::VideoFilterPipeline filters({
    {zxkit::VideoFilter::Hq, 4},
    {zxkit::VideoFilter::Tv, 4},
});

// Each factor applies to the previous stage: 320×256 becomes 5120×4096.
auto size = filters.outputSize({320, 256});
const zxkit::VideoFrame& output = filters.apply(input);
// Upload output.pixels() or pass output to encodePng().
```

For HQ4x with scanlines at the same final resolution, use `{Hq, 4}` followed
by `{Scanlines, 1}`. Add `{RgbMask, 1}` for phosphor stripes. `videoFilterCatalog()`
returns stable family IDs, display names and valid factor ranges for UI/settings.
Construct a stage with an enum value and its factor; all factors are integers.

The input is read-only and `frameNumber` is retained. Active filters ignore
input alpha and produce opaque numeric `0xFFBBGGRR` pixels, matching the
Spectrum screen's `packRgba()` representation on every endianness. These are
screen filters, not general-purpose transparent image resamplers. An empty
pipeline returns the original frame reference with no allocation or conversion.

Buffers are reused between frames. The returned reference is valid until the
next `apply()`, `releaseBuffers()`, or pipeline destruction. An empty pipeline's
reference instead follows the source's lifetime. Copy the result to retain it.
The previous result may be fed back into the same pipeline. Use one pipeline
per rendering thread; independent instances have no shared mutable state.
`setStages()` validates the entire replacement before changing the current list.
`releaseBuffers()` frees cached frame and padding storage without changing stages.

Every input/intermediate/output dimension is checked before processing. Zero
dimensions, invalid stages, arithmetic overflow, kernel size limits and frames
over the configurable pixel limit throw `InvalidArgumentException`. The default
limit is 64M pixels per logical frame, not a total memory budget. The pipeline
retains up to two output/intermediate buffers, an extra frame for Scale4x, and
an input/padding buffer. Large chains can use several hundred MiB and have a
substantial CPU cost. `outputSize()` lets a host validate dimensions before
allocating or presenting UI choices. Allocation failures propagate normally.

## Provenance and licenses

### Fuse / ScummVM / HiEnd3D

The files in `fuse/` are adapted from Fuse's `ui/scaler/scalers.c` and
`scaler_hq{2,3,4}x.c`, revision
`df2811c51c6e9ba7b7fa24254c808fad452051e7`, inspected on 2026-11-01.
Project: <https://fuse-emulator.sourceforge.net/>.

Original copyright holders include Fredrick Meunier, Philip Kendall,
Gergely Szasz, Ludvig Strigeus, the ScummVM project, Maxim Stepin (MaxSt), and
Sergio Baldoví. Original copyright and GPL-2.0-or-later notices remain in the
implementation and HQ pattern tables. ZxKit uses these under GPLv3, as allowed
by their later-version clause; see the repository's `LICENSE`.

Adaptations: retained only the stateless 32-bit SaI, Eagle, Scale, HQ, TV and
dot-matrix algorithms; fixed numeric RGB channel masks; removed frontend,
settings, libspectrum, 16-bit and composite dependencies; replaced libspectrum
integer aliases and obsolete `register`; introduced a private C++ namespace;
used signed neighborhood strides; stopped HQ pointer advancement after each
row's last output pixel. HQ switch tables are unchanged except for
their `.inc` filenames. The public wrapper normalizes alpha, replicates borders
and validates dimensions. Scale4x applies Scale2x twice, as described by the
[Scale2x author](https://www.scale2x.it/algorithm).

### xBRZ

`xbrz/` contains Zenju's xBRZ 1.8 implementation, copied from
<https://github.com/atheros/xbrzscale/tree/a2d8dce723e8fab548bf8460b46eeebfc64abca6/xbrz>.
Original project: <https://sourceforge.net/projects/xbrz/>.
The original copyright, GPLv3 license with upstream exceptions, and changelog
are retained. See `xbrz/License.txt` and the source file notices. These files
retain their upstream licensing; ZxKit's own wrappers are GPL-3.0-or-later.

Adaptations: normalized line endings/text encoding, moved public symbols into
`zxkit::detail::xbrz`, and changed the unbuffered ARGB path's border reader to
repeat edge pixels for opaque screens. The wrapper converts red/blue channel
order before and after filtering. It selects the unbuffered path so first use
does not allocate or initialize a process-wide color-distance lookup table.
