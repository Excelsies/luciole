#pragma once

// The loops and filter macros below are derived from subprojects/libbink2/src/bink2_dsp.c (libbink2, licensed under
// the GNU Lesser General Public License, version 2.1 or later; see subprojects/libbink2/LICENSE). They are a test
// reference and are not linked into the program.

// The motion compensation of the Bink 2 decoder, written the plain way (the loops as they were before they were
// reordered for the compiler), as the reference the tests compare bink2_mc_luma and bink2_mc_chroma with, byte for
// byte. The luma case with a fraction in both directions follows the reference decoder: no clipping between its passes.
#include <cstdint>
#include <cstring>

namespace test::bink2::reference {
  inline int clip8(int v) {
    if (v < 0) {
      return 0;
    }
    return v > 255 ? 255 : v;
  }

#define EDGE_STRIDE 32

  inline void edge_copy(
      unsigned char* dst, const unsigned char* src, int src_stride, int x0, int y0, int bw, int bh, int w, int h
  ) {
    int i, j;

    for (j = 0; j < bh; j++) {
      int sy = y0 + j;

      if (sy < 0) {
        sy = 0;
      } else if (sy >= h) {
        sy = h - 1;
      }
      for (i = 0; i < bw; i++) {
        int sx = x0 + i;

        if (sx < 0) {
          sx = 0;
        } else if (sx >= w) {
          sx = w - 1;
        }
        dst[j * EDGE_STRIDE + i] = src[sy * src_stride + sx];
      }
    }
  }

#define LH_FILTER(src)                                                                                                 \
  ((((((src)[0] + (src)[1]) * 19) >> 1) - ((src)[-1] + (src)[2]) * 2 + (((src)[-2] + (src)[3]) >> 1) + 8) >> 4)
#define LV_FILTER(src, i)                                                                                              \
  ((((((src)[0] + (src)[i]) * 19) >> 1)                                                                                \
    - ((src)[-(i)] + (src)[2 * (i)]) * 2                                                                               \
    + (((src)[-2 * (i)] + (src)[3 * (i)]) >> 1)                                                                        \
    + 8)                                                                                                               \
   >> 4)

  inline void ref_mc_luma(
      unsigned char* dst, int dst_stride, const unsigned char* src, int src_stride, int x, int y, int frame_w,
      int frame_h, int mvx, int mvy
  ) {
    unsigned char edge[EDGE_STRIDE * 24];
    const unsigned char* msrc;
    int ss = src_stride;
    int fw = (frame_w + 31) & ~31;
    int fh = (frame_h + 31) & ~31;
    int mx = x + (mvx >> 1);
    int my = y + (mvy >> 1);
    int hmode = mvx & 1;
    int vmode = mvy & 1;
    int i, j;

    if (mx < 2 || my < 2 || mx + 19 > fw || my + 19 > fh) {
      edge_copy(edge, src, src_stride, mx - 2, my - 2, 21, 21, fw, fh);
      msrc = edge + 2 + 2 * EDGE_STRIDE;
      ss = EDGE_STRIDE;
    } else {
      msrc = src + mx + my * src_stride;
    }

    if (hmode == 0 && vmode == 0) {
      for (j = 0; j < 16; j++) {
        std::memcpy(dst, msrc, 16);
        dst += dst_stride;
        msrc += ss;
      }
    } else if (vmode == 0) {
      for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
          dst[i] = (unsigned char)clip8(LH_FILTER(msrc + i));
        }
        dst += dst_stride;
        msrc += ss;
      }
    } else if (hmode == 0) {
      for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) {
          dst[j * dst_stride + i] = (unsigned char)clip8(LV_FILTER(msrc + j * ss + i, ss));
        }
      }
    } else {
      // The horizontal pass keeps its rounded value unclipped: it can lie outside 0..255, as in the reference decoder.
      std::int16_t temp[21 * 16];

      msrc -= 2 * ss;
      for (i = 0; i < 21; i++) {
        for (j = 0; j < 16; j++) {
          temp[i * 16 + j] = static_cast<std::int16_t>(LH_FILTER(msrc + j));
        }
        msrc += ss;
      }
      for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
          dst[i] = (unsigned char)clip8(LV_FILTER(temp + (j + 2) * 16 + i, 16));
        }
        dst += dst_stride;
      }
    }
  }

#define CH1_FILTER(src) (((src)[0] * 6 + (src)[1] * 2 + 4) >> 3)
#define CH2_FILTER(src) (((src)[0] + (src)[1] + 1) >> 1)
#define CH3_FILTER(src) (((src)[0] * 2 + (src)[1] * 6 + 4) >> 3)
#define CV1_FILTER(src, i) (((src)[0] * 6 + (src)[i] * 2 + 4) >> 3)
#define CV2_FILTER(src, i) (((src)[0] + (src)[i] + 1) >> 1)
#define CV3_FILTER(src, i) (((src)[0] * 2 + (src)[i] * 6 + 4) >> 3)

  inline int chroma_h(int mode, const unsigned char* src) {
    switch (mode) {
    case 1:
      return CH1_FILTER(src);
    case 2:
      return CH2_FILTER(src);
    case 3:
      return CH3_FILTER(src);
    default:
      return src[0];
    }
  }

  inline int chroma_v(int mode, const unsigned char* src, int stride) {
    switch (mode) {
    case 1:
      return CV1_FILTER(src, stride);
    case 2:
      return CV2_FILTER(src, stride);
    case 3:
      return CV3_FILTER(src, stride);
    default:
      return src[0];
    }
  }

  inline void ref_mc_chroma(
      unsigned char* dst, int dst_stride, const unsigned char* src, int src_stride, int x, int y, int frame_w,
      int frame_h, int mvx, int mvy
  ) {
    unsigned char edge[EDGE_STRIDE * 24];
    const unsigned char* msrc;
    int ss = src_stride;
    int fw = (frame_w + 15) & ~15;
    int fh = (frame_h + 15) & ~15;
    int mx = x + (mvx >> 2);
    int my = y + (mvy >> 2);
    int hmode = mvx & 3;
    int vmode = mvy & 3;
    int i, j;

    if (mx < 0 || my < 0 || mx + 9 > fw || my + 9 > fh) {
      edge_copy(edge, src, src_stride, mx, my, 9, 9, fw, fh);
      msrc = edge;
      ss = EDGE_STRIDE;
    } else {
      msrc = src + mx + my * src_stride;
    }

    if (hmode == 0 && vmode == 0) {
      for (j = 0; j < 8; j++) {
        std::memcpy(dst, msrc, 8);
        dst += dst_stride;
        msrc += ss;
      }
    } else if (vmode == 0) {
      for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++) {
          dst[i] = (unsigned char)clip8(chroma_h(hmode, msrc + i));
        }
        dst += dst_stride;
        msrc += ss;
      }
    } else if (hmode == 0) {
      for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
          dst[j * dst_stride + i] = (unsigned char)clip8(chroma_v(vmode, msrc + j * ss + i, ss));
        }
      }
    } else {
      /* The horizontal pass keeps its sum unrounded; the one rounding
       * happens after the vertical pass. */
      int temp[9 * 8];
      int hw = 4 - hmode;
      int vw = 4 - vmode;

      for (i = 0; i < 9; i++) {
        for (j = 0; j < 8; j++) {
          temp[i * 8 + j] = hw * msrc[j] + hmode * msrc[j + 1];
        }
        msrc += ss;
      }
      for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++) {
          dst[i] = (unsigned char)clip8((vw * temp[j * 8 + i] + vmode * temp[j * 8 + i + 8] + 8) >> 4);
        }
        dst += dst_stride;
      }
    }
  }

} // namespace test::bink2::reference

#undef EDGE_STRIDE
#undef LH_FILTER
#undef LV_FILTER
#undef CH1_FILTER
#undef CH2_FILTER
#undef CH3_FILTER
#undef CV1_FILTER
#undef CV2_FILTER
#undef CV3_FILTER
