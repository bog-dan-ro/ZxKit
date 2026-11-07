// Numeric fingerprints from independent upstream implementations.
// See README.md for provenance, input construction and fingerprint definition.
#pragma once

#include <zxkit/video/VideoFilters.h>
#include <array>
#include <cstdint>

struct FilterReference
{
    zxkit::VideoFilterStage stage;
    uint64_t fingerprint;
};

inline constexpr std::array kFilterReferences{
    FilterReference{ { zxkit::VideoFilter::Scale, 2 }, 0xcf05d49175c76106ull },
    FilterReference{ { zxkit::VideoFilter::Scale, 3 }, 0x8a285265594b31d4ull },
    FilterReference{ { zxkit::VideoFilter::Hq, 2 }, 0x00f6ac1ffaa80405ull },
    FilterReference{ { zxkit::VideoFilter::Hq, 3 }, 0xc0f04e149dd1c273ull },
    FilterReference{ { zxkit::VideoFilter::Hq, 4 }, 0x25a1c5b9886c25a0ull },
    FilterReference{ { zxkit::VideoFilter::Sai, 2 }, 0xe1c9c664ac6be3edull },
    FilterReference{ { zxkit::VideoFilter::SuperSai, 2 }, 0x241843e554697b27ull },
    FilterReference{ { zxkit::VideoFilter::SuperEagle, 2 }, 0x84dba9a96bc4d3d3ull },
    FilterReference{ { zxkit::VideoFilter::Tv, 2 }, 0x8c053bb45fa309edull },
    FilterReference{ { zxkit::VideoFilter::Tv, 3 }, 0x8d8d689706d5fca5ull },
    FilterReference{ { zxkit::VideoFilter::Tv, 4 }, 0x112f63a31e5c8005ull },
    FilterReference{ { zxkit::VideoFilter::DotMatrix, 2 }, 0x29c0a97c8653ea41ull },
    FilterReference{ { zxkit::VideoFilter::Xbrz, 2 }, 0x682210d3249269e1ull },
    FilterReference{ { zxkit::VideoFilter::Xbrz, 3 }, 0x2254e4dbee0d5ad8ull },
    FilterReference{ { zxkit::VideoFilter::Xbrz, 4 }, 0xe040608217b4b051ull },
    FilterReference{ { zxkit::VideoFilter::Xbrz, 5 }, 0xc93850cf843ab634ull },
    FilterReference{ { zxkit::VideoFilter::Xbrz, 6 }, 0xa65b999784e285caull },
};
