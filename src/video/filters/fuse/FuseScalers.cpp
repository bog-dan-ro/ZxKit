/* scalers.c: the actual graphics scalers
 * Copyright (C) 2003-2026 Fredrick Meunier, Philip Kendall, Gergely Szasz
 *
 * Originally taken from ScummVM - Scumm Interpreter
 * Copyright (C) 2001  Ludvig Strigeus
 * Copyright (C) 2001-2003 The ScummVM project
 *
 * HQ2x, HQ3x and HQ4x scalers taken from HiEnd3D Demos (http://www.hiend3d.com)
 * Copyright (C) 2003 MaxSt ( maxst@hiend3d.com )
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// Adapted for ZxKit: fixed RGB format, C++ namespace, stateless subset.
#include "FuseScalers.h"
#include <cstddef>

namespace zxkit::detail::fuse {
#define ABS(x) ((x) >= 0 ? (x) : -(x))
using scaler_data_type = uint32_t;
static const uint32_t colorMask = 0x00FEFEFE;
static const uint32_t lowPixelMask = 0x00010101;
static const uint32_t qcolorMask = 0x00FCFCFC;
static const uint32_t qlowpixelMask = 0x00030303;
static const uint64_t redblueMask = 0x00FF00FF;
static const uint32_t redblue8_Mask = 0x07F807F8;
static const uint32_t redblue16_Mask = 0x0FF00FF0;
static const uint32_t redMask =   0x000000FF;
static const uint32_t greenMask = 0x0000FF00;
static const uint32_t blueMask =  0x00FF0000;
static const uint32_t green8_Mask = 0x0007F800;
static const uint32_t green16_Mask = 0x000FF000;

static const uint32_t dotmatrix[16] = {
  0x00003F00, 0x003F0000, 0x0000003F, 0x00000000,
  0x003F3F3F, 0x00000000, 0x003F3F3F, 0x00000000,
  0x0000003F, 0x00000000, 0x00003F00, 0x003F0000,
  0x003F3F3F, 0x00000000, 0x003F3F3F, 0x00000000
};

static inline int 
GetResult( uint32_t A, uint32_t B, uint32_t C,
	   uint32_t D )
{
  const int ac = (A==C);
  const int bc = (B==C);
  const int x1 = ac;
  const int y1 = (bc & !ac);
  const int ad = (A==D);
  const int bd = (B==D);
  const int x2 = ad;
  const int y2 = (bd & !ad);
  const int x = x1+x2;
  const int y = y1+y2;
  static const int rmap[3][3] = {
      {0, 0, -1},
      {0, 0, -1},
      {1, 1,  0}
    };
  return rmap[y][x];
}

static inline uint32_t 
INTERPOLATE( uint32_t A, uint32_t B )
{
  if (A != B) {
    return (((A & colorMask) >> 1) + ((B & colorMask) >> 1) + (A & B & lowPixelMask));
  } else
    return A;
}

static inline uint32_t 
Q_INTERPOLATE( uint32_t A, uint32_t B, uint32_t C,
	       uint32_t D )
{
  uint32_t x = ((A & qcolorMask) >> 2) +
  ((B & qcolorMask) >> 2) + ((C & qcolorMask) >> 2) + ((D & qcolorMask) >> 2);
  uint32_t y = (A & qlowpixelMask) +
  (B & qlowpixelMask) + (C & qlowpixelMask) + (D & qlowpixelMask);

  y = (y >> 2) & qlowpixelMask;
  return x + y;
}

/* HQ scalers */
#define HQ_INTERPOLATE_1(A,B) Q_INTERPOLATE(A,A,A,B)

#define HQ_INTERPOLATE_2(A,B,C) Q_INTERPOLATE(A,A,B,C)

static inline uint32_t
HQ_INTERPOLATE_3( uint32_t A, uint32_t B )
{
  return ( ( ( ( A & greenMask ) * 7 + ( B & greenMask) ) & green8_Mask ) +
         ( ( ( A & redblueMask ) * 7 + ( B & redblueMask ) ) & redblue8_Mask )
         ) >> 3;
}

static inline uint32_t
HQ_INTERPOLATE_4( uint32_t A, uint32_t B,
                  uint32_t C )
{
  return ( ( ( ( A & greenMask ) * 2 + ( ( B & greenMask) + 
		( C & greenMask ) ) * 7 ) & green16_Mask) +
         ( ( ( A & redblueMask ) * 2 + ( ( B & redblueMask ) +
		( C & redblueMask ) ) * 7 ) & redblue16_Mask ) ) >> 4;
}

#define HQ_INTERPOLATE_5(A,B) INTERPOLATE(A,B)

static inline uint32_t
HQ_INTERPOLATE_6( uint32_t A, uint32_t B,
                  uint32_t C )
{
  return ( ( ( ( A & greenMask ) * 5 + ( B & greenMask) * 2 + 
		( C & greenMask ) ) & green8_Mask) +
         ( ( ( A & redblueMask ) * 5 + ( B & redblueMask ) * 2 +
		( C & redblueMask ) ) & redblue8_Mask ) ) >> 3;
}

static inline uint32_t
HQ_INTERPOLATE_7( uint32_t A, uint32_t B,
                  uint32_t C )
{
  return ( ( ( ( A & greenMask ) * 6 + ( B & greenMask) + 
		( C & greenMask ) ) & green8_Mask) +
         ( ( ( A & redblueMask ) * 6 + ( B & redblueMask ) +
		( C & redblueMask ) ) & redblue8_Mask ) ) >> 3;
}

static inline uint32_t
HQ_INTERPOLATE_8( uint32_t A, uint32_t B )
{
  return ( ( ( ( A & greenMask ) * 5 + ( B & greenMask) * 3 ) & green8_Mask ) +
         ( ( ( A & redblueMask ) * 5 + ( B & redblueMask ) * 3 ) & redblue8_Mask )
         ) >> 3;
}

static inline uint32_t
HQ_INTERPOLATE_9( uint32_t A, uint32_t B,
                  uint32_t C )
{
  return ( ( ( ( A & greenMask ) * 2 + ( ( B & greenMask) + 
		( C & greenMask ) ) * 3 ) & green8_Mask) +
         ( ( ( A & redblueMask ) * 2 + ( ( B & redblueMask ) +
		( C & redblueMask ) ) * 3 ) & redblue8_Mask ) ) >> 3;
}

static inline uint32_t
HQ_INTERPOLATE_10( uint32_t A, uint32_t B,
                   uint32_t C )
{
  return ( ( ( ( A & greenMask ) * 14 + ( B & greenMask) + 
		( C & greenMask ) ) & green16_Mask) +
         ( ( ( A & redblueMask ) * 14 + ( B & redblueMask ) +
		( C & redblueMask ) ) & redblue16_Mask ) ) >> 4;
}

/* dstPtr */
#define HQ_PIXEL00_0     w[5]
#define HQ_PIXEL00_10    HQ_INTERPOLATE_1(w[5], w[1])
#define HQ_PIXEL00_11    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ_PIXEL00_12    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ_PIXEL00_20    HQ_INTERPOLATE_2(w[5], w[4], w[2])
#define HQ_PIXEL00_21    HQ_INTERPOLATE_2(w[5], w[1], w[2])
#define HQ_PIXEL00_22    HQ_INTERPOLATE_2(w[5], w[1], w[4])
#define HQ_PIXEL00_60    HQ_INTERPOLATE_6(w[5], w[2], w[4])
#define HQ_PIXEL00_61    HQ_INTERPOLATE_6(w[5], w[4], w[2])
#define HQ_PIXEL00_70    HQ_INTERPOLATE_7(w[5], w[4], w[2])
#define HQ_PIXEL00_90    HQ_INTERPOLATE_9(w[5], w[4], w[2])
#define HQ_PIXEL00_100   HQ_INTERPOLATE_10(w[5], w[4], w[2])
/* +1 */
#define HQ_PIXEL01_0     w[5]
#define HQ_PIXEL01_10    HQ_INTERPOLATE_1(w[5], w[3])
#define HQ_PIXEL01_11    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ_PIXEL01_12    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ_PIXEL01_20    HQ_INTERPOLATE_2(w[5], w[2], w[6])
#define HQ_PIXEL01_21    HQ_INTERPOLATE_2(w[5], w[3], w[6])
#define HQ_PIXEL01_22    HQ_INTERPOLATE_2(w[5], w[3], w[2])
#define HQ_PIXEL01_60    HQ_INTERPOLATE_6(w[5], w[6], w[2])
#define HQ_PIXEL01_61    HQ_INTERPOLATE_6(w[5], w[2], w[6])
#define HQ_PIXEL01_70    HQ_INTERPOLATE_7(w[5], w[2], w[6])
#define HQ_PIXEL01_90    HQ_INTERPOLATE_9(w[5], w[2], w[6])
#define HQ_PIXEL01_100   HQ_INTERPOLATE_10(w[5], w[2], w[6])
/* +dstPitch */
#define HQ_PIXEL10_0     w[5]
#define HQ_PIXEL10_10    HQ_INTERPOLATE_1(w[5], w[7])
#define HQ_PIXEL10_11    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ_PIXEL10_12    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ_PIXEL10_20    HQ_INTERPOLATE_2(w[5], w[8], w[4])
#define HQ_PIXEL10_21    HQ_INTERPOLATE_2(w[5], w[7], w[4])
#define HQ_PIXEL10_22    HQ_INTERPOLATE_2(w[5], w[7], w[8])
#define HQ_PIXEL10_60    HQ_INTERPOLATE_6(w[5], w[4], w[8])
#define HQ_PIXEL10_61    HQ_INTERPOLATE_6(w[5], w[8], w[4])
#define HQ_PIXEL10_70    HQ_INTERPOLATE_7(w[5], w[8], w[4])
#define HQ_PIXEL10_90    HQ_INTERPOLATE_9(w[5], w[8], w[4])
#define HQ_PIXEL10_100   HQ_INTERPOLATE_10(w[5], w[8], w[4])
/* +dstPitch+1*/
#define HQ_PIXEL11_0     w[5]
#define HQ_PIXEL11_10    HQ_INTERPOLATE_1(w[5], w[9])
#define HQ_PIXEL11_11    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ_PIXEL11_12    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ_PIXEL11_20    HQ_INTERPOLATE_2(w[5], w[6], w[8])
#define HQ_PIXEL11_21    HQ_INTERPOLATE_2(w[5], w[9], w[8])
#define HQ_PIXEL11_22    HQ_INTERPOLATE_2(w[5], w[9], w[6])
#define HQ_PIXEL11_60    HQ_INTERPOLATE_6(w[5], w[8], w[6])
#define HQ_PIXEL11_61    HQ_INTERPOLATE_6(w[5], w[6], w[8])
#define HQ_PIXEL11_70    HQ_INTERPOLATE_7(w[5], w[6], w[8])
#define HQ_PIXEL11_90    HQ_INTERPOLATE_9(w[5], w[6], w[8])
#define HQ_PIXEL11_100   HQ_INTERPOLATE_10(w[5], w[6], w[8])

/* dstPtr */
#define HQ_PIXEL00_1M  HQ_INTERPOLATE_1(w[5], w[1])
#define HQ_PIXEL00_1U  HQ_INTERPOLATE_1(w[5], w[2])
#define HQ_PIXEL00_1L  HQ_INTERPOLATE_1(w[5], w[4])
#define HQ_PIXEL00_2   HQ_INTERPOLATE_2(w[5], w[4], w[2])
#define HQ_PIXEL00_4   HQ_INTERPOLATE_4(w[5], w[4], w[2])
#define HQ_PIXEL00_5   HQ_INTERPOLATE_5(w[4], w[2])
#define HQ_PIXEL00_C   w[5]
/* dstPtr+1*/
#define HQ_PIXEL01_1   HQ_INTERPOLATE_1(w[5], w[2])
#define HQ_PIXEL01_3   HQ_INTERPOLATE_3(w[5], w[2])
#define HQ_PIXEL01_6   HQ_INTERPOLATE_1(w[2], w[5])
#define HQ_PIXEL01_C   w[5]
/* dstPtr+2*/
#define HQ_PIXEL02_1M  HQ_INTERPOLATE_1(w[5], w[3])
#define HQ_PIXEL02_1U  HQ_INTERPOLATE_1(w[5], w[2])
#define HQ_PIXEL02_1R  HQ_INTERPOLATE_1(w[5], w[6])
#define HQ_PIXEL02_2   HQ_INTERPOLATE_2(w[5], w[2], w[6])
#define HQ_PIXEL02_4   HQ_INTERPOLATE_4(w[5], w[2], w[6])
#define HQ_PIXEL02_5   HQ_INTERPOLATE_5(w[2], w[6])
#define HQ_PIXEL02_C   w[5]
/* + dstPitch*/
#define HQ_PIXEL10_1   HQ_INTERPOLATE_1(w[5], w[4])
#define HQ_PIXEL10_3   HQ_INTERPOLATE_3(w[5], w[4])
#define HQ_PIXEL10_6   HQ_INTERPOLATE_1(w[4], w[5])
#define HQ_PIXEL10_C   w[5]
/* + dstPitch + 1 */
#define HQ_PIXEL11     w[5]
/* + dstPitch + 2 */
#define HQ_PIXEL12_1   HQ_INTERPOLATE_1(w[5], w[6])
#define HQ_PIXEL12_3   HQ_INTERPOLATE_3(w[5], w[6])
#define HQ_PIXEL12_6   HQ_INTERPOLATE_1(w[6], w[5])
#define HQ_PIXEL12_C   w[5]
/* + 2*dstPitch */
#define HQ_PIXEL20_1M  HQ_INTERPOLATE_1(w[5], w[7])
#define HQ_PIXEL20_1D  HQ_INTERPOLATE_1(w[5], w[8])
#define HQ_PIXEL20_1L  HQ_INTERPOLATE_1(w[5], w[4])
#define HQ_PIXEL20_2   HQ_INTERPOLATE_2(w[5], w[8], w[4])
#define HQ_PIXEL20_4   HQ_INTERPOLATE_4(w[5], w[8], w[4])
#define HQ_PIXEL20_5   HQ_INTERPOLATE_5(w[8], w[4])
#define HQ_PIXEL20_C   w[5]
/* + 2*dstPitch + 1 */
#define HQ_PIXEL21_1   HQ_INTERPOLATE_1(w[5], w[8])
#define HQ_PIXEL21_3   HQ_INTERPOLATE_3(w[5], w[8])
#define HQ_PIXEL21_6   HQ_INTERPOLATE_1(w[8], w[5])
#define HQ_PIXEL21_C   w[5]
/* + 2*dstPitch + 2 */
#define HQ_PIXEL22_1M  HQ_INTERPOLATE_1(w[5], w[9])
#define HQ_PIXEL22_1D  HQ_INTERPOLATE_1(w[5], w[8])
#define HQ_PIXEL22_1R  HQ_INTERPOLATE_1(w[5], w[6])
#define HQ_PIXEL22_2   HQ_INTERPOLATE_2(w[5], w[6], w[8])
#define HQ_PIXEL22_4   HQ_INTERPOLATE_4(w[5], w[6], w[8])
#define HQ_PIXEL22_5   HQ_INTERPOLATE_5(w[6], w[8])
#define HQ_PIXEL22_C   w[5]

/* dstPtr */
#define HQ4X_PIXEL00_0     w[5]
#define HQ4X_PIXEL00_11    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ4X_PIXEL00_12    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ4X_PIXEL00_20    HQ_INTERPOLATE_2(w[5], w[2], w[4])
#define HQ4X_PIXEL00_50    HQ_INTERPOLATE_5(w[2], w[4])
#define HQ4X_PIXEL00_80    HQ_INTERPOLATE_8(w[5], w[1])
#define HQ4X_PIXEL00_81    HQ_INTERPOLATE_8(w[5], w[4])
#define HQ4X_PIXEL00_82    HQ_INTERPOLATE_8(w[5], w[2])
#define HQ4X_PIXEL01_0     w[5]
#define HQ4X_PIXEL01_10    HQ_INTERPOLATE_1(w[5], w[1])
#define HQ4X_PIXEL01_12    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ4X_PIXEL01_14    HQ_INTERPOLATE_1(w[2], w[5])
#define HQ4X_PIXEL01_21    HQ_INTERPOLATE_2(w[2], w[5], w[4])
#define HQ4X_PIXEL01_31    HQ_INTERPOLATE_3(w[5], w[4])
#define HQ4X_PIXEL01_50    HQ_INTERPOLATE_5(w[2], w[5])
#define HQ4X_PIXEL01_60    HQ_INTERPOLATE_6(w[5], w[2], w[4])
#define HQ4X_PIXEL01_61    HQ_INTERPOLATE_6(w[5], w[2], w[1])
#define HQ4X_PIXEL01_82    HQ_INTERPOLATE_8(w[5], w[2])
#define HQ4X_PIXEL01_83    HQ_INTERPOLATE_8(w[2], w[4])
#define HQ4X_PIXEL02_0     w[5]
#define HQ4X_PIXEL02_10    HQ_INTERPOLATE_1(w[5], w[3])
#define HQ4X_PIXEL02_11    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ4X_PIXEL02_13    HQ_INTERPOLATE_1(w[2], w[5])
#define HQ4X_PIXEL02_21    HQ_INTERPOLATE_2(w[2], w[5], w[6])
#define HQ4X_PIXEL02_32    HQ_INTERPOLATE_3(w[5], w[6])
#define HQ4X_PIXEL02_50    HQ_INTERPOLATE_5(w[2], w[5])
#define HQ4X_PIXEL02_60    HQ_INTERPOLATE_6(w[5], w[2], w[6])
#define HQ4X_PIXEL02_61    HQ_INTERPOLATE_6(w[5], w[2], w[3])
#define HQ4X_PIXEL02_81    HQ_INTERPOLATE_8(w[5], w[2])
#define HQ4X_PIXEL02_83    HQ_INTERPOLATE_8(w[2], w[6])
#define HQ4X_PIXEL03_0     w[5]
#define HQ4X_PIXEL03_11    HQ_INTERPOLATE_1(w[5], w[2])
#define HQ4X_PIXEL03_12    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ4X_PIXEL03_20    HQ_INTERPOLATE_2(w[5], w[2], w[6])
#define HQ4X_PIXEL03_50    HQ_INTERPOLATE_5(w[2], w[6])
#define HQ4X_PIXEL03_80    HQ_INTERPOLATE_8(w[5], w[3])
#define HQ4X_PIXEL03_81    HQ_INTERPOLATE_8(w[5], w[2])
#define HQ4X_PIXEL03_82    HQ_INTERPOLATE_8(w[5], w[6])
/* +dstPitch */
#define HQ4X_PIXEL10_0     w[5]
#define HQ4X_PIXEL10_10    HQ_INTERPOLATE_1(w[5], w[1])
#define HQ4X_PIXEL10_11    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ4X_PIXEL10_13    HQ_INTERPOLATE_1(w[4], w[5])
#define HQ4X_PIXEL10_21    HQ_INTERPOLATE_2(w[4], w[5], w[2])
#define HQ4X_PIXEL10_32    HQ_INTERPOLATE_3(w[5], w[2])
#define HQ4X_PIXEL10_50    HQ_INTERPOLATE_5(w[4], w[5])
#define HQ4X_PIXEL10_60    HQ_INTERPOLATE_6(w[5], w[4], w[2])
#define HQ4X_PIXEL10_61    HQ_INTERPOLATE_6(w[5], w[4], w[1])
#define HQ4X_PIXEL10_81    HQ_INTERPOLATE_8(w[5], w[4])
#define HQ4X_PIXEL10_83    HQ_INTERPOLATE_8(w[4], w[2])
#define HQ4X_PIXEL11_0     w[5]
#define HQ4X_PIXEL11_30    HQ_INTERPOLATE_3(w[5], w[1])
#define HQ4X_PIXEL11_31    HQ_INTERPOLATE_3(w[5], w[4])
#define HQ4X_PIXEL11_32    HQ_INTERPOLATE_3(w[5], w[2])
#define HQ4X_PIXEL11_70    HQ_INTERPOLATE_7(w[5], w[4], w[2])
#define HQ4X_PIXEL12_0     w[5]
#define HQ4X_PIXEL12_30    HQ_INTERPOLATE_3(w[5], w[3])
#define HQ4X_PIXEL12_31    HQ_INTERPOLATE_3(w[5], w[2])
#define HQ4X_PIXEL12_32    HQ_INTERPOLATE_3(w[5], w[6])
#define HQ4X_PIXEL12_70    HQ_INTERPOLATE_7(w[5], w[6], w[2])
#define HQ4X_PIXEL13_0     w[5]
#define HQ4X_PIXEL13_10    HQ_INTERPOLATE_1(w[5], w[3])
#define HQ4X_PIXEL13_12    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ4X_PIXEL13_14    HQ_INTERPOLATE_1(w[6], w[5])
#define HQ4X_PIXEL13_21    HQ_INTERPOLATE_2(w[6], w[5], w[2])
#define HQ4X_PIXEL13_31    HQ_INTERPOLATE_3(w[5], w[2])
#define HQ4X_PIXEL13_50    HQ_INTERPOLATE_5(w[6], w[5])
#define HQ4X_PIXEL13_60    HQ_INTERPOLATE_6(w[5], w[6], w[2])
#define HQ4X_PIXEL13_61    HQ_INTERPOLATE_6(w[5], w[6], w[3])
#define HQ4X_PIXEL13_82    HQ_INTERPOLATE_8(w[5], w[6])
#define HQ4X_PIXEL13_83    HQ_INTERPOLATE_8(w[6], w[2])
/* +dstPitch +dstPitch */
#define HQ4X_PIXEL20_0     w[5]
#define HQ4X_PIXEL20_10    HQ_INTERPOLATE_1(w[5], w[7])
#define HQ4X_PIXEL20_12    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ4X_PIXEL20_14    HQ_INTERPOLATE_1(w[4], w[5])
#define HQ4X_PIXEL20_21    HQ_INTERPOLATE_2(w[4], w[5], w[8])
#define HQ4X_PIXEL20_31    HQ_INTERPOLATE_3(w[5], w[8])
#define HQ4X_PIXEL20_50    HQ_INTERPOLATE_5(w[4], w[5])
#define HQ4X_PIXEL20_60    HQ_INTERPOLATE_6(w[5], w[4], w[8])
#define HQ4X_PIXEL20_61    HQ_INTERPOLATE_6(w[5], w[4], w[7])
#define HQ4X_PIXEL20_82    HQ_INTERPOLATE_8(w[5], w[4])
#define HQ4X_PIXEL20_83    HQ_INTERPOLATE_8(w[4], w[8])
#define HQ4X_PIXEL21_0     w[5]
#define HQ4X_PIXEL21_30    HQ_INTERPOLATE_3(w[5], w[7])
#define HQ4X_PIXEL21_31    HQ_INTERPOLATE_3(w[5], w[8])
#define HQ4X_PIXEL21_32    HQ_INTERPOLATE_3(w[5], w[4])
#define HQ4X_PIXEL21_70    HQ_INTERPOLATE_7(w[5], w[4], w[8])
#define HQ4X_PIXEL22_0     w[5]
#define HQ4X_PIXEL22_30    HQ_INTERPOLATE_3(w[5], w[9])
#define HQ4X_PIXEL22_31    HQ_INTERPOLATE_3(w[5], w[6])
#define HQ4X_PIXEL22_32    HQ_INTERPOLATE_3(w[5], w[8])
#define HQ4X_PIXEL22_70    HQ_INTERPOLATE_7(w[5], w[6], w[8])
#define HQ4X_PIXEL23_0     w[5]
#define HQ4X_PIXEL23_10    HQ_INTERPOLATE_1(w[5], w[9])
#define HQ4X_PIXEL23_11    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ4X_PIXEL23_13    HQ_INTERPOLATE_1(w[6], w[5])
#define HQ4X_PIXEL23_21    HQ_INTERPOLATE_2(w[6], w[5], w[8])
#define HQ4X_PIXEL23_32    HQ_INTERPOLATE_3(w[5], w[8])
#define HQ4X_PIXEL23_50    HQ_INTERPOLATE_5(w[6], w[5])
#define HQ4X_PIXEL23_60    HQ_INTERPOLATE_6(w[5], w[6], w[8])
#define HQ4X_PIXEL23_61    HQ_INTERPOLATE_6(w[5], w[6], w[9])
#define HQ4X_PIXEL23_81    HQ_INTERPOLATE_8(w[5], w[6])
#define HQ4X_PIXEL23_83    HQ_INTERPOLATE_8(w[6], w[8])
/* +dstPitch +dstPitch +dstPitch */
#define HQ4X_PIXEL30_0     w[5]
#define HQ4X_PIXEL30_11    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ4X_PIXEL30_12    HQ_INTERPOLATE_1(w[5], w[4])
#define HQ4X_PIXEL30_20    HQ_INTERPOLATE_2(w[5], w[8], w[4])
#define HQ4X_PIXEL30_50    HQ_INTERPOLATE_5(w[8], w[4])
#define HQ4X_PIXEL30_80    HQ_INTERPOLATE_8(w[5], w[7])
#define HQ4X_PIXEL30_81    HQ_INTERPOLATE_8(w[5], w[8])
#define HQ4X_PIXEL30_82    HQ_INTERPOLATE_8(w[5], w[4])
#define HQ4X_PIXEL31_0     w[5]
#define HQ4X_PIXEL31_10    HQ_INTERPOLATE_1(w[5], w[7])
#define HQ4X_PIXEL31_11    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ4X_PIXEL31_13    HQ_INTERPOLATE_1(w[8], w[5])
#define HQ4X_PIXEL31_21    HQ_INTERPOLATE_2(w[8], w[5], w[4])
#define HQ4X_PIXEL31_32    HQ_INTERPOLATE_3(w[5], w[4])
#define HQ4X_PIXEL31_50    HQ_INTERPOLATE_5(w[8], w[5])
#define HQ4X_PIXEL31_60    HQ_INTERPOLATE_6(w[5], w[8], w[4])
#define HQ4X_PIXEL31_61    HQ_INTERPOLATE_6(w[5], w[8], w[7])
#define HQ4X_PIXEL31_81    HQ_INTERPOLATE_8(w[5], w[8])
#define HQ4X_PIXEL31_83    HQ_INTERPOLATE_8(w[8], w[4])
#define HQ4X_PIXEL32_0     w[5]
#define HQ4X_PIXEL32_10    HQ_INTERPOLATE_1(w[5], w[9])
#define HQ4X_PIXEL32_12    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ4X_PIXEL32_14    HQ_INTERPOLATE_1(w[8], w[5])
#define HQ4X_PIXEL32_21    HQ_INTERPOLATE_2(w[8], w[5], w[6])
#define HQ4X_PIXEL32_31    HQ_INTERPOLATE_3(w[5], w[6])
#define HQ4X_PIXEL32_50    HQ_INTERPOLATE_5(w[8], w[5])
#define HQ4X_PIXEL32_60    HQ_INTERPOLATE_6(w[5], w[8], w[6])
#define HQ4X_PIXEL32_61    HQ_INTERPOLATE_6(w[5], w[8], w[9])
#define HQ4X_PIXEL32_82    HQ_INTERPOLATE_8(w[5], w[8])
#define HQ4X_PIXEL32_83    HQ_INTERPOLATE_8(w[8], w[6])
#define HQ4X_PIXEL33_0     w[5]
#define HQ4X_PIXEL33_11    HQ_INTERPOLATE_1(w[5], w[6])
#define HQ4X_PIXEL33_12    HQ_INTERPOLATE_1(w[5], w[8])
#define HQ4X_PIXEL33_20    HQ_INTERPOLATE_2(w[5], w[8], w[6])
#define HQ4X_PIXEL33_50    HQ_INTERPOLATE_5(w[8], w[6])
#define HQ4X_PIXEL33_80    HQ_INTERPOLATE_8(w[5], w[9])
#define HQ4X_PIXEL33_81    HQ_INTERPOLATE_8(w[5], w[6])
#define HQ4X_PIXEL33_82    HQ_INTERPOLATE_8(w[5], w[8])

#define HQ_trY 0x00000030
#define HQ_trU 0x00000007
#define HQ_trV 0x00000006

#define HQ_YUVDIFF(y1,u1,v1,y2,u2,v2) \
  ( ( ABS( y1 - y2 ) > HQ_trY ) || \
    ( ABS( u1 - u2 ) > HQ_trU ) || \
    ( ABS( v1 - v2 ) > HQ_trV ) )

void 
scaler_Super2xSaI( const uint8_t *srcPtr,
			       uint32_t srcPitch,
			       uint8_t *dstPtr,
			       uint32_t dstPitch,
			       int width, int height )
{
  const scaler_data_type *bP;
  scaler_data_type *dP;

  {
    const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
    uint32_t nextDstLine = dstPitch / sizeof( scaler_data_type );
    size_t i;

    while (height--) {
      bP = (const scaler_data_type*)srcPtr;
      dP = (scaler_data_type*)dstPtr;

      for( i = 0; i < width; ++i ) {
	uint32_t color4, color5, color6;
	uint32_t color1, color2, color3;
	uint32_t colorA0, colorA1, colorA2, colorA3, colorB0, colorB1, colorB2,
	 colorB3, colorS1, colorS2;
	uint32_t product1a, product1b, product2a, product2b;

/*---------------------------------------    B1 B2
                                           4  5  6 S2
                                           1  2  3 S1
	                                     A1 A2
*/

        colorB0 = *(bP - nextlineSrc - 1);
	colorB1 = *(bP - nextlineSrc);
	colorB2 = *(bP - nextlineSrc + 1);
	colorB3 = *(bP - nextlineSrc + 2);

	color4 = *(bP - 1);
	color5 = *(bP);
	color6 = *(bP + 1);
	colorS2 = *(bP + 2);

	color1 = *(bP + nextlineSrc - 1);
	color2 = *(bP + nextlineSrc);
	color3 = *(bP + nextlineSrc + 1);
	colorS1 = *(bP + nextlineSrc + 2);

	colorA0 = *(bP + 2 * nextlineSrc - 1);
	colorA1 = *(bP + 2 * nextlineSrc);
	colorA2 = *(bP + 2 * nextlineSrc + 1);
	colorA3 = *(bP + 2 * nextlineSrc + 2);

/*--------------------------------------*/
	if (color2 == color6 && color5 != color3) {
	  product2b = product1b = color2;
	} else if (color5 == color3 && color2 != color6) {
	  product2b = product1b = color5;
	} else if (color5 == color3 && color2 == color6) {
	  int r = 0;

	  r += GetResult(color6, color5, color1, colorA1);
	  r += GetResult(color6, color5, color4, colorB1);
	  r += GetResult(color6, color5, colorA2, colorS1);
	  r += GetResult(color6, color5, colorB2, colorS2);

	  if (r > 0)
	    product2b = product1b = color6;
	  else if (r < 0)
	    product2b = product1b = color5;
	  else {
	    product2b = product1b = INTERPOLATE(color5, color6);
	  }
	} else {
	  if (color6 == color3 && color3 == colorA1 && color2 != colorA2 && color3 != colorA0)
	    product2b = Q_INTERPOLATE(color3, color3, color3, color2);
	  else if (color5 == color2 && color2 == colorA2 && colorA1 != color3 && color2 != colorA3)
	    product2b = Q_INTERPOLATE(color2, color2, color2, color3);
	  else
	    product2b = INTERPOLATE(color2, color3);

	  if (color6 == color3 && color6 == colorB1 && color5 != colorB2 && color6 != colorB0)
	    product1b = Q_INTERPOLATE(color6, color6, color6, color5);
	  else if (color5 == color2 && color5 == colorB2 && colorB1 != color6 && color5 != colorB3)
	    product1b = Q_INTERPOLATE(color6, color5, color5, color5);
	  else
	    product1b = INTERPOLATE(color5, color6);
	}

	if (color5 == color3 && color2 != color6 && color4 == color5 && color5 != colorA2)
	  product2a = INTERPOLATE(color2, color5);
	else if (color5 == color1 && color6 == color5 && color4 != color2 && color5 != colorA0)
	  product2a = INTERPOLATE(color2, color5);
	else
	  product2a = color2;

	if (color2 == color6 && color5 != color3 && color1 == color2 && color2 != colorB2)
	  product1a = INTERPOLATE(color2, color5);
	else if (color4 == color2 && color3 == color2 && color1 != color5 && color2 != colorB0)
	  product1a = INTERPOLATE(color2, color5);
	else
	  product1a = color5;

	*dP = (scaler_data_type)product1a;
	*(dP+nextDstLine) = (scaler_data_type)product2a;
	dP++;
	*dP = (scaler_data_type)product1b;
	*(dP+nextDstLine) = (scaler_data_type)product2b;
	dP++;

	bP++;
      }

      srcPtr += srcPitch;
      dstPtr += dstPitch * 2;
    }
  }
}

void 
scaler_SuperEagle( const uint8_t *srcPtr,
			       uint32_t srcPitch,
			       uint8_t *dstPtr,
			       uint32_t dstPitch,
			       int width, int height )
{
  const scaler_data_type *bP;
  scaler_data_type *dP;

  {
    size_t i;
    const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
    uint32_t nextDstLine = dstPitch / sizeof( scaler_data_type );

    while (height--) {
      bP = (const scaler_data_type*)srcPtr;
      dP = (scaler_data_type*)dstPtr;
      for( i = 0; i < width; ++i ) {
	uint32_t color4, color5, color6;
	uint32_t color1, color2, color3;
	uint32_t colorA1, colorA2, colorB1, colorB2, colorS1, colorS2;
	uint32_t product1a, product1b, product2a, product2b;

	colorB1 = *(bP - nextlineSrc);
	colorB2 = *(bP - nextlineSrc + 1);

	color4 = *(bP - 1);
	color5 = *(bP);
	color6 = *(bP + 1);
	colorS2 = *(bP + 2);

	color1 = *(bP + nextlineSrc - 1);
	color2 = *(bP + nextlineSrc);
	color3 = *(bP + nextlineSrc + 1);
	colorS1 = *(bP + nextlineSrc + 2);

	colorA1 = *(bP + 2 * nextlineSrc);
	colorA2 = *(bP + 2 * nextlineSrc + 1);

	/* -------------------------------------- */
        if (color5 != color3 )
        {
          if (color2 == color6)
          {
            product1b = product2a = color2;
            if ((color1 == color2) || (color6 == colorB2)) {
              product1a = INTERPOLATE(color2, color5);
              product1a = INTERPOLATE(color2, product1a);
            } else {
              product1a = INTERPOLATE(color5, color6);
            }

            if ((color6 == colorS2) || (color2 == colorA1)) {
              product2b = INTERPOLATE(color2, color3);
              product2b = INTERPOLATE(color2, product2b);
            } else {
              product2b = INTERPOLATE(color2, color3);
            }
          }
          else
          {
            product2b = product1a = INTERPOLATE(color2, color6);
            product2b = Q_INTERPOLATE(color3, color3, color3, product2b);
            product1a = Q_INTERPOLATE(color5, color5, color5, product1a);

            product2a = product1b = INTERPOLATE(color5, color3);
            product2a = Q_INTERPOLATE(color2, color2, color2, product2a);
            product1b = Q_INTERPOLATE(color6, color6, color6, product1b);
          }
        }
        else /*if (color5 == color3) */
        {
          if (color2 != color6)
          {
            product2b = product1a = color5;

            if ((colorB1 == color5) || (color3 == colorS1)) {
              product1b = INTERPOLATE(color5, color6);
              product1b = INTERPOLATE(color5, product1b);
            } else {
              product1b = INTERPOLATE(color5, color6);
            }

            if ((color3 == colorA2) || (color4 == color5)) {
              product2a = INTERPOLATE(color5, color2);
              product2a = INTERPOLATE(color5, product2a);
            } else {
              product2a = INTERPOLATE(color2, color3);
            }

          }
          else /* if (color2 != color6) */
          {
            int r = 0;

            r += GetResult(color6, color5, color1, colorA1);
            r += GetResult(color6, color5, color4, colorB1);
            r += GetResult(color6, color5, colorA2, colorS1);
            r += GetResult(color6, color5, colorB2, colorS2);

            if (r > 0) {
              product1b = product2a = color2;
              product1a = product2b = INTERPOLATE(color5, color6);
            } else if (r < 0) {
              product2b = product1a = color5;
              product1b = product2a = INTERPOLATE(color5, color6);
            } else {
              product2b = product1a = color5;
              product1b = product2a = color2;
            }
          }
	}

	*dP = (scaler_data_type)product1a;
	*(dP+nextDstLine) = (scaler_data_type)product2a;
	dP++;
	*dP = (scaler_data_type)product1b;
	*(dP+nextDstLine) = (scaler_data_type)product2b;
	dP++;

	bP++;
      }

      srcPtr += srcPitch;
      dstPtr += dstPitch * 2;
    }
  }
}

void 
scaler_2xSaI( const uint8_t *srcPtr,
			  uint32_t srcPitch, uint8_t *dstPtr,
			  uint32_t dstPitch,
			  int width, int height )
{
  const scaler_data_type *bP;
  scaler_data_type *dP;

  {
    uint32_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
    uint32_t nextDstLine = dstPitch / sizeof( scaler_data_type );

    while (height--) {
      size_t i;
      bP = (const scaler_data_type*)srcPtr;
      dP = (scaler_data_type*)dstPtr;

      for( i = 0; i < width; ++i ) {

	uint32_t colorA, colorB;
	uint32_t colorC, colorD, colorE, colorF, colorG, colorH,
	  colorI, colorJ, colorK, colorL, colorM, colorN, colorO;
	uint32_t product, product1, product2;

/*---------------------------------------
   Map of the pixels:                    I|E F|J
                                         G|A B|K
                                         H|C D|L
                                         M|N O|P

   Note that P does not contribute to the algorithm
*/
	colorI = *(bP - nextlineSrc - 1);
	colorE = *(bP - nextlineSrc);
	colorF = *(bP - nextlineSrc + 1);
	colorJ = *(bP - nextlineSrc + 2);

	colorG = *(bP - 1);
	colorA = *(bP);
	colorB = *(bP + 1);
	colorK = *(bP + 2);

	colorH = *(bP + nextlineSrc - 1);
	colorC = *(bP + nextlineSrc);
	colorD = *(bP + nextlineSrc + 1);
	colorL = *(bP + nextlineSrc + 2);

	colorM = *(bP + 2 * nextlineSrc - 1);
	colorN = *(bP + 2 * nextlineSrc);
	colorO = *(bP + 2 * nextlineSrc + 1);

	if ((colorA == colorD) && (colorB != colorC)) {
	  if (((colorA == colorE) && (colorB == colorL)) ||
              ((colorA == colorC) && (colorA == colorF) && (colorB != colorE) && (colorB == colorJ))) {
	    product = colorA;
	  } else {
	    product = INTERPOLATE(colorA, colorB);
	  }

	  if (((colorA == colorG) && (colorC == colorO)) ||
              ((colorA == colorB) && (colorA == colorH) && (colorG != colorC) && (colorC == colorM))) {
	    product1 = colorA;
	  } else {
	    product1 = INTERPOLATE(colorA, colorC);
	  }
	  product2 = colorA;
	} else if ((colorB == colorC) && (colorA != colorD)) {
	  if (((colorB == colorF) && (colorA == colorH)) ||
              ((colorB == colorE) && (colorB == colorD) && (colorA != colorF) && (colorA == colorI))) {
	    product = colorB;
	  } else {
	    product = INTERPOLATE(colorA, colorB);
	  }

	  if (((colorC == colorH) && (colorA == colorF)) ||
              ((colorC == colorG) && (colorC == colorD) && (colorA != colorH) && (colorA == colorI))) {
	    product1 = colorC;
	  } else {
	    product1 = INTERPOLATE(colorA, colorC);
	  }
	  product2 = colorB;
	} else if ((colorA == colorD) && (colorB == colorC)) {
	  if (colorA == colorB) {
	    product = colorA;
	    product1 = colorA;
	    product2 = colorA;
	  } else {
	    int r = 0;

	    product1 = INTERPOLATE(colorA, colorC);
	    product = INTERPOLATE(colorA, colorB);

	    r += GetResult(colorA, colorB, colorG, colorE);
	    r -= GetResult(colorB, colorA, colorK, colorF);
	    r -= GetResult(colorB, colorA, colorH, colorN);
	    r += GetResult(colorA, colorB, colorL, colorO);

	    if (r > 0)
	      product2 = colorA;
	    else if (r < 0)
	      product2 = colorB;
	    else {
	      product2 = Q_INTERPOLATE(colorA, colorB, colorC, colorD);
	    }
	  }
	} else {
	  product2 = Q_INTERPOLATE(colorA, colorB, colorC, colorD);

	  if ((colorA == colorC) && (colorA == colorF)
	      && (colorB != colorE) && (colorB == colorJ)) {
	    product = colorA;
	  } else if ((colorB == colorE) && (colorB == colorD)
		     && (colorA != colorF) && (colorA == colorI)) {
	    product = colorB;
	  } else {
	    product = INTERPOLATE(colorA, colorB);
	  }

	  if ((colorA == colorB) && (colorA == colorH)
	      && (colorG != colorC) && (colorC == colorM)) {
	    product1 = colorA;
	  } else if ((colorC == colorG) && (colorC == colorD)
		     && (colorA != colorH) && (colorA == colorI)) {
	    product1 = colorC;
	  } else {
	    product1 = INTERPOLATE(colorA, colorC);
	  }
	}

	*dP = (scaler_data_type)colorA;
	*(dP+nextDstLine) = (scaler_data_type)product1;
	dP++;
	*dP = (scaler_data_type)product;
	*(dP+nextDstLine) = (scaler_data_type)product2;
	dP++;

	bP++;
      }

      srcPtr += srcPitch;
      dstPtr += dstPitch * 2;
    }
  }
}

void 
scaler_AdvMame2x( const uint8_t *srcPtr,
			      uint32_t srcPitch,
			      uint8_t *dstPtr,
			      uint32_t dstPitch,
			      int width, int height )
{
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type*) srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type*) dstPtr;

  scaler_data_type /* A, */ B, C;
  scaler_data_type D, E, F;
  scaler_data_type /* G, */ H, I;

  while (height--) {
    int i;

    /* B = *(p - 1 - nextlineSrc); */
    E = *(p - 1);
    /* H = *(p - 1 + nextlineSrc); */
    C = *(p - nextlineSrc);
    F = *(p);
    I = *(p + nextlineSrc);

    for (i = 0; i < width; ++i) {
      p++;
      /* A = B; */ B = C; C = *(p - nextlineSrc);
      D = E; E = F; F = *(p);
      /* G = H; */ H = I; I = *(p + nextlineSrc);

      *(q) = D == B && B != F && D != H ? D : E;
      *(q + 1) = B == F && B != D && F != H ? F : E;
      *(q + nextlineDst) = D == H && D != B && H != F ? D : E;
      *(q + nextlineDst + 1) = H == F && D != H && B != F ? F : E;
      q += 2;
    }
    p += nextlineSrc - width;
    q += (nextlineDst - width) << 1;
  }
}

void 
scaler_AdvMame3x( const uint8_t *srcPtr,
			      uint32_t srcPitch,
			      uint8_t *dstPtr,
			      uint32_t dstPitch,
			      int width, int height )
{
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type*) srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type*) dstPtr;

  scaler_data_type A, B, C;
  scaler_data_type D, E, F;
  scaler_data_type G, H, I;

  while (height--) {
    int i;

    B = *(p - 1 - nextlineSrc);
    E = *(p - 1);
    H = *(p - 1 + nextlineSrc);
    C = *(p - nextlineSrc);
    F = *(p);
    I = *(p + nextlineSrc);

    for (i = 0; i < width; ++i) {
      p++;
      A = B; B = C; C = *(p - nextlineSrc);
      D = E; E = F; F = *(p);
      G = H; H = I; I = *(p + nextlineSrc);

      *(q) = D == B && B != F && D != H ? D : E;
      *(q + 1) = (D == B && B != F && D != H && E != C) || (B == F && B != D && F != H && E != A) ? B : E;
      *(q + 2) = B == F && B != D && F != H ? F : E;
      *(q + nextlineDst) = (D == B && B != F && D != H && E != G) || (D == H && D != B && H != F && E != A) ? D : E;
      *(q + nextlineDst + 1) = E;
      *(q + nextlineDst + 2) = (B == F && B != D && F != H && E != I) || (H == F && D != H && B != F && E != C) ? F : E;
      *(q + 2 * nextlineDst) = D == H && D != B && H != F ? D : E;
      *(q + 2 * nextlineDst + 1) = (D == H && D != B && H != F && E != I) || (H == F && D != H && B != F && E != G) ? H : E;
      *(q + 2 * nextlineDst + 2) = H == F && D != H && B != F ? F : E;
      q += 3;
    }
    p += nextlineSrc - width;
    q += (nextlineDst - width) * 3;
  }
}

void
scaler_TV2x( const uint8_t *srcPtr,
			 uint32_t srcPitch, uint8_t *dstPtr,
			 uint32_t dstPitch,
			 int width, int height )
{
  int i, j;
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type*)srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type*)dstPtr;

  while(height--) {
    for (i = 0, j = 0; i < width; ++i, j += 2) {
      scaler_data_type p1 = *(p + i);
      scaler_data_type pi;

      pi  = (((p1 & redblueMask) * 7) >> 3) & redblueMask;
      pi |= (((p1 & greenMask  ) * 7) >> 3) & greenMask;

      *(q + j) = p1;
      *(q + j + 1) = p1;
      *(q + j + nextlineDst) = pi;
      *(q + j + nextlineDst + 1) = pi;
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void
scaler_TV3x( const uint8_t *srcPtr,
                         uint32_t srcPitch, uint8_t *dstPtr,
                         uint32_t dstPitch,
                         int width, int height )
{
  int i, j;
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type*)srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type*)dstPtr;

  while(height--) {
    for (i = 0, j = 0; i < width; ++i, j += 3) {
      scaler_data_type p1 = *(p + i);
      scaler_data_type pi;

      pi  = (((p1 & redblueMask) * 7) >> 3) & redblueMask;
      pi |= (((p1 & greenMask  ) * 7) >> 3) & greenMask;

      *(q + j) = p1;
      *(q + j + 1) = p1;
      *(q + j + 2) = p1;

      *(q + j + nextlineDst) = p1;
      *(q + j + nextlineDst + 1) = p1;
      *(q + j + nextlineDst + 2) = p1;

      *(q + j + (nextlineDst << 1)) = pi;
      *(q + j + (nextlineDst << 1) + 1) = pi;
      *(q + j + (nextlineDst << 1) + 2) = pi;
    }
    p += nextlineSrc;
    q += nextlineDst * 3;
  }
}

void
scaler_TV4x( const uint8_t *srcPtr,
                         uint32_t srcPitch, uint8_t *dstPtr,
                         uint32_t dstPitch,
                         int width, int height )
{
  int i, j;
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type*)srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type*)dstPtr;

  while(height--) {
    for (i = 0, j = 0; i < width; ++i, j += 4) {
      scaler_data_type p1 = *(p + i);
      scaler_data_type pi;

      pi  = (((p1 & redblueMask) * 7) >> 3) & redblueMask;
      pi |= (((p1 & greenMask  ) * 7) >> 3) & greenMask;

      *(q + j) = p1;
      *(q + j + 1) = p1;
      *(q + j + 2) = p1;
      *(q + j + 3) = p1;

      *(q + j + nextlineDst) = p1;
      *(q + j + nextlineDst + 1) = p1;
      *(q + j + nextlineDst + 2) = p1;
      *(q + j + nextlineDst + 3) = p1;

      *(q + j + (nextlineDst << 1)) = pi;
      *(q + j + (nextlineDst << 1) + 1) = pi;
      *(q + j + (nextlineDst << 1) + 2) = pi;
      *(q + j + (nextlineDst << 1) + 3) = pi;

      *(q + j + (nextlineDst * 3)) = pi;
      *(q + j + (nextlineDst * 3) + 1) = pi;
      *(q + j + (nextlineDst * 3) + 2) = pi;
      *(q + j + (nextlineDst * 3) + 3) = pi;
    }
    p += nextlineSrc;
    q += nextlineDst * 4;
  }
}

static inline scaler_data_type DOT_16(scaler_data_type c, int j, int i) {
  return c - ((c >> 2) & *(dotmatrix + ((j & 3) << 2) + (i & 3)));
}

void
scaler_DotMatrix( const uint8_t *srcPtr,
			      uint32_t srcPitch,
			      uint8_t *dstPtr,
			      uint32_t dstPitch,
			      int width, int height )
{
  int i, j, ii, jj;
  const std::ptrdiff_t nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p = (const scaler_data_type *)srcPtr;

  unsigned int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q = (scaler_data_type *)dstPtr;

  for (j = 0, jj = 0; j < height; ++j, jj += 2) {
    for (i = 0, ii = 0; i < width; ++i, ii += 2) {
      scaler_data_type c = *(p + i);
      *(q + ii) = DOT_16(c, jj, ii);
      *(q + ii + 1) = DOT_16(c, jj, ii + 1);
      *(q + ii + nextlineDst) = DOT_16(c, jj + 1, ii);
      *(q + ii + nextlineDst + 1) = DOT_16(c, jj + 1, ii + 1);
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

#define RGB_TO_Y(r, g, b) ( ( 2449L * r + 4809L * g + 934L * b + 1024 ) >> 11 )
#define RGB_TO_U(r, g, b) ( ( 4096L * b - 1383L * r - 2713L * g + 1024 ) >> 11 )
#define RGB_TO_V(r, g, b) ( ( 4096L * r - 3430L * g - 666L * b +  1024 ) >> 11 )

#define prevline (-nextlineSrc)
#define nextline nextlineSrc
#define MOVE_B_TO_A(A,B) \
		w[A] = w[B]; y[A] = y[B]; u[A] = u[B]; v[A] = v[B];
#define MOVE_P_RIGHT \
	MOVE_B_TO_A(1,2) \
	MOVE_B_TO_A(4,5) \
	MOVE_B_TO_A(7,8) \
	MOVE_B_TO_A(2,3) \
	MOVE_B_TO_A(5,6) \
	MOVE_B_TO_A(8,9)

void
scaler_HQ2x ( const uint8_t *srcPtr,
                          uint32_t srcPitch,
                          uint8_t *dstPtr,
                          uint32_t dstPitch,
                          int width, int height )
{
  int i, j, k, pattern;
  int nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p, *p0 = (const scaler_data_type *)srcPtr;
  int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q, *q1, *qN, *qN1, *q0 = (scaler_data_type *)dstPtr;
  uint64_t w[10];
  
  uint8_t  r, g, b;
  int32_t y[10], u[10], v[10];

  /*   +----+----+----+
       |    |    |    |
       | w1 | w2 | w3 |
       +----+----+----+
       |    |    |    |
       | w4 | w5 | w6 |
       +----+----+----+
       |    |    |    |
       | w7 | w8 | w9 |
       +----+----+----+ */
  for( j = 0; j < height; j++ ) {
    p = p0;
    q = q0; q1 = q + 1;
    qN = q + nextlineDst; qN1 = qN + 1;
    w[2] = *(p + prevline);
    w[5] = *p;
    w[8] = *(p + nextline);
    w[1] = *(p + prevline - 1);
    w[4] = *(p - 1);
    w[7] = *(p + nextline - 1);
    w[3] = *(p + prevline + 1);
    w[6] = *(p + 1);
    w[9] = *(p + nextline + 1);
    for( k = 1; k <= 9; k++ ) {
      r =  w[k] & redMask;
      g = (w[k] & greenMask) >> 8;
      b = (w[k] & blueMask) >> 16;

      y[k] = RGB_TO_Y( r, g, b );
      u[k] = RGB_TO_U( r, g, b );
      v[k] = RGB_TO_V( r, g, b );
    }

    for( i = 0; i < width; i++ ) {
      pattern = 0;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[1], u[1], v[1] ) ) pattern |= 0x01;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[2], u[2], v[2] ) ) pattern |= 0x02;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[3], u[3], v[3] ) ) pattern |= 0x04;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[4], u[4], v[4] ) ) pattern |= 0x08;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[6], u[6], v[6] ) ) pattern |= 0x10;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[7], u[7], v[7] ) ) pattern |= 0x20;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[8], u[8], v[8] ) ) pattern |= 0x40;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[9], u[9], v[9] ) ) pattern |= 0x80;

#include "scaler_hq2x.inc"

      // The rightmost output pointers must not advance beyond one-past-end.
      if( i + 1 == width ) break;
      p++;
      q  += 2; q1  += 2;
      qN += 2; qN1 += 2;
      MOVE_P_RIGHT
      w[3] = *(p + prevline + 1);
      w[6] = *(p + 1);
      w[9] = *(p + nextline + 1);
      for( k = 3; k <= 9; k += 3 ) {
        r =   w[k] & redMask;
        g = ( w[k] & greenMask ) >> 8;
        b = ( w[k] & blueMask  ) >> 16;

        y[k] = RGB_TO_Y( r, g, b );
        u[k] = RGB_TO_U( r, g, b );
        v[k] = RGB_TO_V( r, g, b );
      }
    }
    p0 += nextlineSrc;
    q0 += nextlineDst << 1;
  }
}

void
scaler_HQ3x ( const uint8_t *srcPtr,
                          uint32_t srcPitch,
                          uint8_t *dstPtr,
                          uint32_t dstPitch,
                          int width, int height )
{
  int i, j, k, pattern;
  int nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p, *p0 = (const scaler_data_type *)srcPtr;
  int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q, *qN, *qNN, *q1, *qN1, *qNN1, *q2, *qN2, *qNN2, 
		   *q0 = (scaler_data_type *)dstPtr;
  uint64_t w[10];
  
  uint8_t  r, g, b;
  int32_t y[10], u[10], v[10];

  /*   +----+----+----+
       |    |    |    |
       | w1 | w2 | w3 |
       +----+----+----+
       |    |    |    |
       | w4 | w5 | w6 |
       +----+----+----+
       |    |    |    |
       | w7 | w8 | w9 |
       +----+----+----+ */
  for( j = 0; j < height; j++ ) {
    p = p0;
    q = q0;
    q1 = q + 1; q2 = q + 2;
    qN = q + nextlineDst; qN1 = qN + 1; qN2 = qN + 2;
    qNN = qN + nextlineDst;  qNN1 = qNN + 1; qNN2 = qNN + 2;

    w[2] = *(p + prevline);
    w[5] = *p;
    w[8] = *(p + nextline);
    w[1] = *(p + prevline - 1);
    w[4] = *(p - 1);
    w[7] = *(p + nextline - 1);
    w[3] = *(p + prevline + 1);
    w[6] = *(p + 1);
    w[9] = *(p + nextline + 1);
    for( k = 1; k <= 9; k++ ) {
      r =  w[k] & redMask;
      g = (w[k] & greenMask) >> 8;
      b = (w[k] & blueMask) >> 16;

      y[k] = RGB_TO_Y( r, g, b );
      u[k] = RGB_TO_U( r, g, b );
      v[k] = RGB_TO_V( r, g, b );
    }

    for( i = 0; i < width; i++ ) {
      pattern = 0;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[1], u[1], v[1] ) ) pattern |= 0x01;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[2], u[2], v[2] ) ) pattern |= 0x02;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[3], u[3], v[3] ) ) pattern |= 0x04;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[4], u[4], v[4] ) ) pattern |= 0x08;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[6], u[6], v[6] ) ) pattern |= 0x10;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[7], u[7], v[7] ) ) pattern |= 0x20;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[8], u[8], v[8] ) ) pattern |= 0x40;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[9], u[9], v[9] ) ) pattern |= 0x80;

#include "scaler_hq3x.inc"

      if( i + 1 == width ) break;
      p++;
      q   += 3; q1   += 3; q2   += 3;
      qN  += 3; qN1  += 3; qN2  += 3;
      qNN += 3; qNN1 += 3; qNN2 += 3;
      MOVE_P_RIGHT
      w[3] = *(p + prevline + 1);
      w[6] = *(p + 1);
      w[9] = *(p + nextline + 1);
      for( k = 3; k <= 9; k += 3 ) {
        r =   w[k] & redMask;
        g = ( w[k] & greenMask ) >> 8;
        b = ( w[k] & blueMask  ) >> 16;

        y[k] = RGB_TO_Y( r, g, b );
        u[k] = RGB_TO_U( r, g, b );
        v[k] = RGB_TO_V( r, g, b );
      }
    }
    p0 += nextlineSrc;
    q0 += ( nextlineDst << 1 ) + nextlineDst;
  }
}

void
scaler_HQ4x ( const uint8_t *srcPtr,
                          uint32_t srcPitch,
                          uint8_t *dstPtr,
                          uint32_t dstPitch,
                          int width, int height )
{
  int i, j, k, pattern;
  int nextlineSrc = srcPitch / sizeof( scaler_data_type );
  const scaler_data_type *p, *p0 = (const scaler_data_type *)srcPtr;
  int nextlineDst = dstPitch / sizeof( scaler_data_type );
  scaler_data_type *q,  *qN,  *qNN,  *qNNN,
                   *q1, *qN1, *qNN1, *qNNN1,
                   *q2, *qN2, *qNN2, *qNNN2,
                   *q3, *qN3, *qNN3, *qNNN3,
                   *q0 = (scaler_data_type *)dstPtr;
  uint64_t w[10];

  uint8_t  r, g, b;
  int32_t y[10], u[10], v[10];

  /*   +----+----+----+
       |    |    |    |
       | w1 | w2 | w3 |
       +----+----+----+
       |    |    |    |
       | w4 | w5 | w6 |
       +----+----+----+
       |    |    |    |
       | w7 | w8 | w9 |
       +----+----+----+ */
  for( j = 0; j < height; j++ ) {
    p = p0;
    q = q0;
    q1 = q + 1; q2 = q + 2; q3 = q + 3;
    qN = q + nextlineDst; qN1 = qN + 1; qN2 = qN + 2; qN3 = qN + 3;
    qNN = qN + nextlineDst; qNN1 = qNN + 1; qNN2 = qNN + 2; qNN3 = qNN + 3;
    qNNN = qNN + nextlineDst; qNNN1 = qNNN + 1; qNNN2 = qNNN + 2; qNNN3 = qNNN + 3;

    w[2] = *(p + prevline);
    w[5] = *p;
    w[8] = *(p + nextline);
    w[1] = *(p + prevline - 1);
    w[4] = *(p - 1);
    w[7] = *(p + nextline - 1);
    w[3] = *(p + prevline + 1);
    w[6] = *(p + 1);
    w[9] = *(p + nextline + 1);
    for( k = 1; k <= 9; k++ ) {
      r =  w[k] & redMask;
      g = (w[k] & greenMask) >> 8;
      b = (w[k] & blueMask) >> 16;

      y[k] = RGB_TO_Y( r, g, b );
      u[k] = RGB_TO_U( r, g, b );
      v[k] = RGB_TO_V( r, g, b );
    }

    for( i = 0; i < width; i++ ) {
      pattern = 0;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[1], u[1], v[1] ) ) pattern |= 0x01;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[2], u[2], v[2] ) ) pattern |= 0x02;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[3], u[3], v[3] ) ) pattern |= 0x04;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[4], u[4], v[4] ) ) pattern |= 0x08;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[6], u[6], v[6] ) ) pattern |= 0x10;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[7], u[7], v[7] ) ) pattern |= 0x20;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[8], u[8], v[8] ) ) pattern |= 0x40;
      if( HQ_YUVDIFF( y[5], u[5], v[5], y[9], u[9], v[9] ) ) pattern |= 0x80;

#include "scaler_hq4x.inc"

      if( i + 1 == width ) break;
      p++;
      q    += 4; q1    += 4; q2    += 4; q3    += 4;
      qN   += 4; qN1   += 4; qN2   += 4; qN3   += 4;
      qNN  += 4; qNN1  += 4; qNN2  += 4; qNN3  += 4;
      qNNN += 4; qNNN1 += 4; qNNN2 += 4; qNNN3 += 4;
      MOVE_P_RIGHT
      w[3] = *(p + prevline + 1);
      w[6] = *(p + 1);
      w[9] = *(p + nextline + 1);
      for( k = 3; k <= 9; k += 3 ) {
        r =   w[k] & redMask;
        g = ( w[k] & greenMask ) >> 8;
        b = ( w[k] & blueMask  ) >> 16;

        y[k] = RGB_TO_Y( r, g, b );
        u[k] = RGB_TO_U( r, g, b );
        v[k] = RGB_TO_V( r, g, b );
      }
    }
    p0 += nextlineSrc;
    q0 += ( nextlineDst << 2 );
  }
}


} // namespace zxkit::detail::fuse
