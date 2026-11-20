/*
 * Modified for Luciole, 2026-11-20: the object holds the read function of a
 * callback-opened file and the frame being decoded slice by slice; a frame
 * packet is limited to BINK2_MAX_FRAME_BYTES; bink2_internal_read is declared.
 * Original: libbink2 commit ae2e413.
 */
#ifndef LIBBINK2_INTERNAL_H
#define LIBBINK2_INTERNAL_H

#include "bink2.h"
#include "bink2_video.h"

#define BINK2_MAX_AUDIO_TRACKS 7
#define BINK2_MAX_FRAMES 1000000UL
#define BINK2_MAX_WIDTH 7680UL
#define BINK2_MAX_HEIGHT 4800UL
/* The largest frame packet read, far above any real one. */
#define BINK2_MAX_FRAME_BYTES (64UL << 20)

struct Bink2Audio;
struct Bink2Video;

typedef struct {
    unsigned long rate;
    unsigned char channels;
    unsigned char bitdepth;
    unsigned char flags;
    unsigned char enabled;
    unsigned long max_decoded;
    struct Bink2Audio *decoder;
    const unsigned char *payload;
    unsigned long payload_size;
} Bink2AudioTrack;

struct bink2_t {
    /* input source */
    FILE *file;
    unsigned char *mem;
    unsigned long mem_size;
    unsigned char mode;
    unsigned char owns_file;
    bink2_read_fn read;
    void *read_user;

    /* container metadata */
    unsigned long file_size;
    unsigned long actual_size;
    int version; /* 'f' .. 'n' */
    unsigned long width;
    unsigned long height;
    unsigned long frame_count;
    unsigned long largest_frame;
    unsigned long fps_num;
    unsigned long fps_den;
    unsigned long flags;
    int audio_tracks;
    Bink2AudioTrack audio[BINK2_MAX_AUDIO_TRACKS];

    /* frame index */
    unsigned long *frame_offsets;
    unsigned long *frame_sizes;
    unsigned char *frame_keyframes;

    /* current frame packet */
    unsigned char *frame_data;
    unsigned long frame_data_size;
    const unsigned char *video_data;
    unsigned long video_size;

    /* playback state */
    unsigned long current_frame;
    unsigned char video_enabled;
    struct Bink2Video *video;

    /* the frame being decoded slice by slice (bink2_frames.h) */
    Bink2FrameJob job;
    int job_ready;
};

int bink2_internal_load_frame(bink2 object, unsigned long frame);
/* 0 when `count` bytes at `offset` were read. */
int bink2_internal_read(const bink2 object, unsigned long offset, void *dst,
                        unsigned long count);

#endif
