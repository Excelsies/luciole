/*
 * Added for Luciole, 2026-11-20. See bink2_frames.h.
 *
 * Part of the pinned libbink2 copy in this folder, and licensed like it: the
 * GNU Lesser General Public License, version 2.1 or (at your option) any
 * later version.
 */

#include "bink2_frames.h"

#include "bink2_audio.h"
#include "bink2_internal.h"

#include <stdlib.h>

struct bink2_scratch_t {
    Bink2Scratch s;
};

static int ensure_video(bink2 object)
{
    if (object->video != NULL)
        return 0;
    if (object->flags & BINK2_FLAG_ALPHA)
        return -1;
    object->video = (Bink2Video *)calloc(1, sizeof(Bink2Video));
    if (object->video == NULL)
        return -1;
    if (bink2_video_init_external(object->video, object->width,
                                  object->height, object->flags,
                                  object->version) != 0) {
        free(object->video);
        object->video = NULL;
        return -1;
    }
    return 0;
}

int bink2_frame_geometry(const bink2 object, int stride[3],
                         unsigned long size[3])
{
    unsigned long aligned_w, aligned_h, chroma_stride;

    if (object == NULL || (object->flags & BINK2_FLAG_ALPHA))
        return -1;
    aligned_w = (object->width + 31) & ~31UL;
    aligned_h = (object->height + 31) & ~31UL;
    chroma_stride = (((object->width + 1) / 2) + 15) & ~15UL;
    stride[0] = (int)aligned_w;
    stride[1] = (int)chroma_stride;
    stride[2] = (int)chroma_stride;
    size[0] = aligned_w * aligned_h;
    size[1] = chroma_stride * (aligned_h / 2);
    size[2] = size[1];
    return 0;
}

int bink2_frame_is_key(const bink2 object, unsigned long frame)
{
    if (object == NULL || frame >= object->frame_count)
        return -1;
    return object->frame_keyframes[frame] ? 1 : 0;
}

int bink2_frame_slices(const bink2 object)
{
    return object != NULL && object->job_ready ? object->job.num_slices : 0;
}

bink2_scratch bink2_scratch_create(const bink2 object)
{
    bink2_scratch scratch;

    if (object == NULL || object->width == 0)
        return NULL;
    scratch = (bink2_scratch)calloc(1, sizeof(*scratch));
    if (scratch == NULL)
        return NULL;
    if (bink2_scratch_init(&scratch->s,
                           (int)(((object->width + 31) & ~31UL) / 32)) != 0) {
        free(scratch);
        return NULL;
    }
    return scratch;
}

void bink2_scratch_destroy(bink2_scratch scratch)
{
    if (scratch == NULL)
        return;
    bink2_scratch_uninit(&scratch->s);
    free(scratch);
}

int bink2_frame_prepare(bink2 object, unsigned long frame)
{
    int status;

    if (object == NULL)
        return BINK2_ERROR;
    object->job_ready = 0;
    object->video_enabled = 1;
    status = bink2_internal_load_frame(object, frame);
    if (status != BINK2_OK)
        return status;
    if (ensure_video(object) != 0)
        return BINK2_ERROR;
    if (bink2_video_job_begin(object->video, &object->job, object->video_data,
                              object->video_size,
                              object->frame_keyframes[frame]) != 0)
        return BINK2_ERROR;
    object->job_ready = 1;
    object->current_frame = frame;
    return BINK2_OK;
}

int bink2_frame_decode_slice(bink2 object, bink2_scratch scratch, int slice,
                             const Bink2Planes *cur, const Bink2Planes *ref)
{
    if (object == NULL || scratch == NULL || !object->job_ready)
        return BINK2_ERROR;
    return bink2_video_job_slice(object->video, &scratch->s, &object->job,
                                 slice, cur, ref);
}

static int decode_sound(bink2 object)
{
    int i;

    for (i = 0; i < object->audio_tracks; i++) {
        Bink2AudioTrack *track = &object->audio[i];

        if (!track->enabled || track->decoder == NULL)
            continue;
        if (bink2_audio_decode(track->decoder, track->payload,
                               track->payload_size) != 0)
            return BINK2_ERROR;
    }
    return BINK2_OK;
}

int bink2_frame_finish(bink2 object)
{
    if (object == NULL || !object->job_ready)
        return BINK2_ERROR;
    object->job_ready = 0;
    if (bink2_video_job_end(object->video, &object->job) != 0)
        return BINK2_ERROR;
    if (decode_sound(object) != BINK2_OK)
        return BINK2_ERROR;
    return object->current_frame + 1 < object->frame_count ? BINK2_MORE
                                                            : BINK2_LAST;
}

int bink2_frame_prepare_sound(bink2 object, unsigned long frame)
{
    int status;

    if (object == NULL)
        return BINK2_ERROR;
    object->job_ready = 0;
    status = bink2_internal_load_frame(object, frame);
    if (status != BINK2_OK)
        return status;
    if (decode_sound(object) != BINK2_OK)
        return BINK2_ERROR;
    return BINK2_OK;
}

int bink2_frame_sound_bytes(const bink2 object, unsigned long frame, int track,
                            unsigned long *bytes)
{
    unsigned long pos, end;
    int t;

    if (object == NULL || bytes == NULL || frame >= object->frame_count ||
        track < 0 || track >= object->audio_tracks)
        return -1;
    pos = object->frame_offsets[frame];
    end = pos + object->frame_sizes[frame];
    if (end < pos || end > object->actual_size)
        return 1;
    for (t = 0; t <= track; t++) {
        unsigned char head[4];
        unsigned long chunk;

        if (end - pos < 4 || bink2_internal_read(object, pos, head, 4) != 0)
            return -1;
        chunk = (unsigned long)head[0] | ((unsigned long)head[1] << 8) |
                ((unsigned long)head[2] << 16) | ((unsigned long)head[3] << 24);
        pos += 4;
        if (chunk > end - pos)
            return -1;
        if (t == track) {
            unsigned char size[4];

            if (chunk == 0) {
                *bytes = 0;
                return 0;
            }
            if (chunk < 4 || bink2_internal_read(object, pos, size, 4) != 0)
                return -1;
            *bytes = (unsigned long)size[0] | ((unsigned long)size[1] << 8) |
                     ((unsigned long)size[2] << 16) |
                     ((unsigned long)size[3] << 24);
            return *bytes <= object->audio[track].max_decoded ? 0 : -1;
        }
        pos += chunk;
    }
    return -1;
}

void bink2_frame_reset_audio(bink2 object)
{
    int i;

    if (object == NULL)
        return;
    for (i = 0; i < object->audio_tracks; i++)
        bink2_audio_reset(object->audio[i].decoder);
}
