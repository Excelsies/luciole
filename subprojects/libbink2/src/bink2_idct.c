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

void bink2_idct_put(unsigned char *dst, int stride, const int16_t block[64])
{
    bink2_idct_put_plain(dst, stride, block);
}

void bink2_idct_add(unsigned char *dst, int stride, const int16_t block[64])
{
    bink2_idct_add_plain(dst, stride, block);
}
