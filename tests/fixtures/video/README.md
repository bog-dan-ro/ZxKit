# Software filter reference fingerprints

`FilterReference.h` contains numeric output fingerprints obtained on 2026-11-01
by running the independent upstream Fuse C and xBRZ C++ implementations, not
ZxKit's adapted kernels. The tests need only this bundled data and the generated
input; no external checkout, ROMs, images or network access are required.

Sources and original attribution:

- Fuse `ui/scaler/scalers.c` and its original HQ tables, revision
  `df2811c51c6e9ba7b7fa24254c808fad452051e7`,
  <https://fuse-emulator.sourceforge.net/>. Copyright Fredrick Meunier,
  Philip Kendall, Gergely Szasz, Ludvig Strigeus, the ScummVM project,
  Maxim Stepin (MaxSt) and Sergio Baldoví; GPL-2.0-or-later.
- Zenju's xBRZ 1.8, as distributed in `atheros/xbrzscale`, revision
  `a2d8dce723e8fab548bf8460b46eeebfc64abca6`,
  <https://github.com/atheros/xbrzscale>. Copyright Zenju; GPLv3 with the
  exceptions retained in [the bundled source notices](../../../src/video/filters/xbrz/xbrz.h).

The generated 48×51 input starts black. Its upper 48 rows contain 16×16 tiles,
each 3×3 pixels. For pattern `p` (0 through 255), tile origin is
`((p % 16) * 3, (p / 16) * 3)`. Its center is black. The other eight pixels,
in raster order, are white when the corresponding bit of `p` is set and black
otherwise. The final three rows (`y` = 48 through 50) contain RGB ramps:
`R = (5*x + 3*y) & 255`, `G = (11*x + 7*y) & 255`,
`B = (17*x + 13*y) & 255`. This exercises all binary neighborhood patterns
and adds asymmetric color transitions for channel-order and YUV checks.

Fuse was compiled as C with `SCALER_DATA_SIZE=4` in its default numeric
`0x00BBGGRR` format. A duplicated two-pixel halo surrounded the input, and
the original entry points processed the central 48×51 area. Minimal header
stubs supplied libspectrum integer aliases and frontend/settings declarations;
no algorithm code or pattern tables were changed. The unused composite
code was linked with Fuse's bundled `snes_ntsc.c`.

For xBRZ the same input and duplicated halo were converted to opaque numeric
`0xAARRGGBB` format. The original `ARGB_UNBUFFERED` path processed the padded
image, and its output was cropped by `2 * scale` pixels on each side, then
converted back to `0xFFBBGGRR`. This establishes clamped-border reference
behavior independently of ZxKit's adapted border reader.

Fingerprint calculation uses unsigned 64-bit wraparound. Start with
`14695981039346656037`. For each numeric output pixel in raster order,
force alpha to `0xFF`, XOR the **whole uint32 pixel value** into the hash,
and multiply by `1099511628211`. It is a word-based FNV-style regression
fingerprint, not a byte-based FNV-1a file checksum, and is endian-independent.
Explicit numeric pixel assertions additionally cover resampling and masks.
