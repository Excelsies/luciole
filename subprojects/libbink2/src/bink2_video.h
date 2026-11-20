/*
 * Modified for Luciole, 2026-11-20: a frame can be decoded in separate
 * steps (job begin, one call per slice, job end) into caller-supplied
 * planes, with the per-slice state in a Bink2Scratch of its own, so
 * slices may run on different threads. bink2_video_decode runs the same
 * steps in order. Original: libbink2 commit ae2e413.
 */
#ifndef LIBBINK2_VIDEO_H
#define LIBBINK2_VIDEO_H

#include "bink2.h"
#include "bink2_bits.h"

#include <stddef.h>
#include <stdint.h>

#define BINK2_RANGE_FULL   0
#define BINK2_RANGE_STUDIO 1

typedef struct {
    float dc[4][16];
    int block_type;
} Bink2OldDc;

typedef struct {
    int8_t intra_q;
    int8_t inter_q;
} Bink2Quant;

extern const uint8_t bink2_luma_repos[16];
extern const uint8_t bink2_next_skips[16];

typedef struct Bink2Video {
    unsigned long width;
    unsigned long height;
    unsigned long aligned_w;
    unsigned long aligned_h;
    int version;
    unsigned long flags;
    int has_alpha;
    int has_prev;
    int range;

    unsigned char *cur[4];
    unsigned char *ref[4];
    int stride[4];
    int plane_w[4];
    int plane_h[4];

    int mb_w;
    int mb_h;

    int16_t *mv[2];
    unsigned char *mv_count;
    size_t mv_capacity;

    unsigned char *rgb;
    size_t rgb_size;
    int rgb_valid;

    Bink2OldDc *old_dc_cur;
    Bink2OldDc *old_dc_prev;
    int32_t *old_mv_cur;
    int32_t *old_mv_prev;
    int old_mb_pos;
    int old_comp;

    uint32_t frame_flags;
    Bink2Quant *new_q_cur;
    Bink2Quant *new_q_prev;
} Bink2Video;

#define BINK2_MAX_SLICES 8

/* The state one slice needs besides the shared picture: the row buffers of
 * the slice decoder. One per thread that decodes slices. */
typedef struct Bink2Scratch {
    Bink2OldDc *old_dc_cur;
    Bink2OldDc *old_dc_prev;
    int32_t *old_mv_cur;
    int32_t *old_mv_prev;
    Bink2Quant *new_q_cur;
    Bink2Quant *new_q_prev;
} Bink2Scratch;

/* The planes a frame decodes into (Y, U, V as the decoder numbers them), with
 * the strides of bink2_video_stride. */
typedef struct {
    unsigned char *plane[3];
} Bink2Planes;

/* One frame split into slices, from bink2_video_job_begin. */
typedef struct {
    const unsigned char *data; /* the packet, followed by 8 zero bytes */
    size_t real_size;
    uint32_t frame_flags;
    int is_keyframe;
    int num_slices;
    int y0[BINK2_MAX_SLICES];
    int y1[BINK2_MAX_SLICES];
    size_t start_bit[BINK2_MAX_SLICES];
    size_t end_bit[BINK2_MAX_SLICES]; /* set by each slice */
} Bink2FrameJob;

int bink2_video_init(Bink2Video *v, unsigned long width, unsigned long height,
                     unsigned long flags, int version);
/* Like bink2_video_init, but allocates no picture planes and no RGB buffer:
 * the caller passes planes to bink2_video_job_slice. */
int bink2_video_init_external(Bink2Video *v, unsigned long width,
                              unsigned long height, unsigned long flags,
                              int version);
int bink2_scratch_init(Bink2Scratch *s, int mb_w);
void bink2_scratch_uninit(Bink2Scratch *s);
/* Reads the frame header and slice table. `data` holds `real_size` bytes of
 * packet and 8 zero bytes after them. */
int bink2_video_job_begin(Bink2Video *v, Bink2FrameJob *job,
                          const unsigned char *data, size_t real_size,
                          int is_keyframe);
/* Decodes one slice of the job into `cur`, reading `ref` (NULL when there is
 * no earlier picture). Calls for different slices, each with its own scratch,
 * may run at the same time; nothing else may touch the video then. */
int bink2_video_job_slice(Bink2Video *v, Bink2Scratch *scratch,
                          Bink2FrameJob *job, int slice,
                          const Bink2Planes *cur, const Bink2Planes *ref);
/* Checks the slices fit their offsets. Call once every slice returned 0. */
int bink2_video_job_end(Bink2Video *v, Bink2FrameJob *job);
void bink2_video_uninit(Bink2Video *v);
void bink2_video_reset(Bink2Video *v);
int bink2_video_decode(Bink2Video *v, const unsigned char *data, size_t size,
                       int is_keyframe);
const unsigned char *bink2_video_plane(Bink2Video *v, int plane);
int bink2_video_stride(const Bink2Video *v, int plane);
int bink2_video_plane_width(const Bink2Video *v, int plane);
int bink2_video_plane_height(const Bink2Video *v, int plane);
const unsigned char *bink2_video_rgb(Bink2Video *v);
int bink2_video_motion(Bink2Video *v, unsigned long *mb_w, unsigned long *mb_h,
                       const int16_t **mv_x, const int16_t **mv_y,
                       const unsigned char **mv_count);

int bink2_video_decode_slice_old(Bink2Video *v, Bink2Bits *bits,
                                 int is_keyframe, int y0, int y1);
int bink2_video_decode_slice_new(Bink2Video *v, Bink2Bits *bits,
                                 int is_keyframe, int y0, int y1);
void bink2_video_predict_dc(Bink2Video *v, int is_luma, float mindc,
                            float maxdc, int flags, float tdc[16]);
int bink2_video_slice_heights(int version, unsigned long flags,
                              unsigned long height, int heights[8]);
void bink2_video_mc_luma(Bink2Video *v, int x, int y);
void bink2_video_mc_chroma(Bink2Video *v, int x, int y);
void bink2_video_copy_mb(Bink2Video *v, int x, int y);
void bink2_video_store_motion(Bink2Video *v, int x, int y, int nb);

#endif
