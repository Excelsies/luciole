/*
 * Added for Luciole, 2026-11-22: the inverse transform of the "new" Bink 2 blocks, put or added into a picture, in
 * one call that takes the 16 bit coefficients as the decoder holds them. Fast paths give, byte for byte, what the
 * plain C path gives; the plain C path is the transform of bink2_dsp.c (bink2_idct_new with bink2_put_block or
 * bink2_add_block) and is what other architectures run.
 */
#ifndef LIBBINK2_IDCT_H
#define LIBBINK2_IDCT_H

#include <stdint.h>

/* Writes the picture block of `block` (coefficients in raster order) at dst. */
void bink2_idct_put(unsigned char *dst, int stride, const int16_t block[64]);
/* Adds the picture block of `block` to the 8x8 bytes at dst, saturating. */
void bink2_idct_add(unsigned char *dst, int stride, const int16_t block[64]);

/* The plain C path, always. */
void bink2_idct_put_plain(unsigned char *dst, int stride, const int16_t block[64]);
void bink2_idct_add_plain(unsigned char *dst, int stride, const int16_t block[64]);

/* Makes bink2_idct_put and bink2_idct_add run the plain C path while `on` is not zero. For tests; set it while no
 * frame is being decoded. */
void bink2_idct_use_plain(int on);

#endif
