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
#define MUL_A(val) ((val) + ((val) >> 2))
#define MUL_B(val) ((val) >> 1)
#define MUL_C(val) ((val) - ((val) >> 2) - ((val) >> 4))
#define MUL_D(val) ((val) + ((val) >> 2) - ((val) >> 4))
#define MUL_E(val) ((val) >> 2)

/*
 * One pass of the transform over eight columns at once: the arithmetic of
 * idct_1d_new in bink2_dsp.c, with the column as the inner loop so the
 * compiler can vectorize it. Each result is cut to 16 bits, as there.
 */
static void pass(const int16_t in[8][8], int16_t out[8][8], int shift)
{
    int i;

    for (i = 0; i < 8; i++) {
        int tmp00 = in[3][i] + in[5][i];
        int tmp01 = in[3][i] - in[5][i];
        int tmp02 = MUL_A(in[2][i]) + MUL_B(in[6][i]);
        int tmp03 = MUL_B(in[2][i]) - MUL_A(in[6][i]);
        int tmp0  = (in[0][i] + in[4][i]) + tmp02;
        int tmp1  = (in[0][i] + in[4][i]) - tmp02;
        int tmp2  = in[0][i] - in[4][i];
        int tmp3  = in[1][i] + tmp00;
        int tmp4  = in[1][i] - tmp00;
        int tmp5  = tmp01 + in[7][i];
        int tmp6  = tmp01 - in[7][i];
        int tmp7  = tmp4 + MUL_C(tmp6);
        int tmp8  = MUL_C(tmp4) - tmp6;
        int tmp9  = MUL_D(tmp3) + MUL_E(tmp5);
        int tmp10 = MUL_E(tmp3) - MUL_D(tmp5);
        int tmp11 = tmp2 + tmp03;
        int tmp12 = tmp2 - tmp03;

        out[0][i] = (int16_t)((tmp0  + tmp9)  >> shift);
        out[1][i] = (int16_t)((tmp11 + tmp7)  >> shift);
        out[2][i] = (int16_t)((tmp12 + tmp8)  >> shift);
        out[3][i] = (int16_t)((tmp1  + tmp10) >> shift);
        out[4][i] = (int16_t)((tmp1  - tmp10) >> shift);
        out[5][i] = (int16_t)((tmp12 - tmp8)  >> shift);
        out[6][i] = (int16_t)((tmp11 - tmp7)  >> shift);
        out[7][i] = (int16_t)((tmp0  - tmp9)  >> shift);
    }
}

/* The picture block of `block`: result[r][c] is the byte (before clipping) at row r, column c. */
static void transform(const int16_t block[64], int16_t result[8][8])
{
    int16_t first[8][8];
    int16_t turned[8][8];
    int r, c;

    pass((const int16_t(*)[8])block, first, 0);
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            turned[c][r] = first[r][c];
    pass((const int16_t(*)[8])turned, result, 6);
}

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

static void put_flat(unsigned char *dst, int stride, int16_t dc)
{
    int row;

    for (row = 0; row < 8; row++)
        memset(dst + row * stride, clip8(dc >> 6), 8);
}

static void add_flat(unsigned char *dst, int stride, int16_t dc)
{
    int row, col;
    int add = dc >> 6;

    if (add == 0)
        return;
    for (row = 0; row < 8; row++, dst += stride)
        for (col = 0; col < 8; col++)
            dst[col] = (unsigned char)clip8(dst[col] + add);
}

void bink2_idct_put_dc(unsigned char *dst, int stride, int16_t dc)
{
    int16_t block[64] = {0};

    if (use_plain) {
        block[0] = dc;
        bink2_idct_put_plain(dst, stride, block);
        return;
    }
    put_flat(dst, stride, dc);
}

void bink2_idct_add_dc(unsigned char *dst, int stride, int16_t dc)
{
    int16_t block[64] = {0};

    if (use_plain) {
        block[0] = dc;
        bink2_idct_add_plain(dst, stride, block);
        return;
    }
    add_flat(dst, stride, dc);
}

void bink2_idct_put(unsigned char *dst, int stride, const int16_t block[64])
{
    int16_t result[8][8];
    int r, c;

    if (use_plain) {
        bink2_idct_put_plain(dst, stride, block);
    } else if (dc_only(block)) {
        put_flat(dst, stride, block[0]);
    } else {
        transform(block, result);
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++)
                dst[r * stride + c] = (unsigned char)clip8(result[r][c]);
    }
}

void bink2_idct_add(unsigned char *dst, int stride, const int16_t block[64])
{
    int16_t result[8][8];
    int r, c;

    if (use_plain) {
        bink2_idct_add_plain(dst, stride, block);
    } else if (dc_only(block)) {
        add_flat(dst, stride, block[0]);
    } else {
        transform(block, result);
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++)
                dst[r * stride + c] =
                    (unsigned char)clip8(dst[r * stride + c] + result[r][c]);
    }
}
