/*
 * Added for Luciole, 2026-11-22. See bink2_idct.h.
 */
#include "bink2_idct.h"

#include "bink2_dsp.h"

#include <string.h>

static volatile int use_plain;

void bink2_idct_use_plain(int on)
{
    use_plain = on;
}

int bink2_idct_plain_requested(void)
{
    return use_plain;
}

void bink2_idct_put_plain(unsigned char *dst, int stride, const int16_t block[64])
{
    int32_t tmp[64];
    int i;

    for (i = 0; i < 64; i++)
        tmp[i] = block[i];
    bink2_idct_new(tmp);
    bink2_put_block(dst, stride, tmp);
}

void bink2_idct_add_plain(unsigned char *dst, int stride, const int16_t block[64])
{
    int32_t tmp[64];
    int i;

    for (i = 0; i < 64; i++)
        tmp[i] = block[i];
    bink2_idct_new(tmp);
    bink2_add_block(dst, stride, tmp);
}

/*
 * Whether only the first coefficient is not zero. Then every column pass leaves it where it is (the other
 * columns stay zero) and every row pass spreads it unchanged, so each of the 64 results is that coefficient
 * shifted right by 6, with the sign kept: no rounding, no overflow of the 16 bit steps.
 */
static int dc_only(const int16_t *block)
{
    int acc = 0;
    int i;

    for (i = 1; i < 64; i++)
        acc |= block[i];
    return acc == 0;
}

static int clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void bink2_idct_put(unsigned char *dst, int stride, const int16_t block[64])
{
    int row;

    if (use_plain || !dc_only(block)) {
        bink2_idct_put_plain(dst, stride, block);
        return;
    }
    for (row = 0; row < 8; row++)
        memset(dst + row * stride, clip8(block[0] >> 6), 8);
}

void bink2_idct_add(unsigned char *dst, int stride, const int16_t block[64])
{
    int row, col;
    int add;

    if (use_plain || !dc_only(block)) {
        bink2_idct_add_plain(dst, stride, block);
        return;
    }
    add = block[0] >> 6;
    if (add == 0)
        return;
    for (row = 0; row < 8; row++, dst += stride)
        for (col = 0; col < 8; col++)
            dst[col] = (unsigned char)clip8(dst[col] + add);
}
