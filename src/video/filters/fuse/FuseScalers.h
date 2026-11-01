// Private adapter declarations; implementation and notices in FuseScalers.cpp.
#pragma once
#include <cstdint>
namespace zxkit::detail::fuse {
void scaler_Super2xSaI(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_SuperEagle(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_2xSaI(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_AdvMame2x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_AdvMame3x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_TV2x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_TV3x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_TV4x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_DotMatrix(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_HQ2x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_HQ3x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
void scaler_HQ4x(const uint8_t*, uint32_t, uint8_t*, uint32_t, int, int);
}
