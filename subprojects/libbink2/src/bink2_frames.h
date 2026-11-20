/*
 * Added for Luciole, 2026-11-20. Decodes one frame in steps, so its slices
 * can be decoded on different threads into planes the caller owns.
 *
 *   bink2_frame_prepare()        read the packet and its slice table
 *   bink2_frame_decode_slice()   once per slice, in any order or at once
 *   bink2_frame_finish()         check the slices, decode the audio
 *
 * The result of the slices is the same, bit for bit, whatever the order and
 * the threads. Use only one frame at a time per decoder object.
 *
 * Part of the pinned libbink2 copy in this folder, and licensed like it: the
 * GNU Lesser General Public License, version 2.1 or (at your option) any
 * later version.
 */

#ifndef LIBBINK2_FRAMES_H
#define LIBBINK2_FRAMES_H

#include "bink2.h"
#include "bink2_video.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Row strides and byte sizes of the Y, U and V planes a frame decodes into,
 *  padded to whole 32 pixel blocks. 0 on success, -1 for a stream these
 *  functions cannot decode (one with an alpha plane). */
int bink2_frame_geometry(const bink2 object, int stride[3],
                         unsigned long size[3]);
/** 1 for a key frame, 0 for any other, -1 for a frame past the end */
int bink2_frame_is_key(const bink2 object, unsigned long frame);
/** The number of slices a frame is cut into (at most BINK2_MAX_SLICES). */
int bink2_frame_slices(const bink2 object);

/** The state one thread needs to decode slices. */
typedef struct bink2_scratch_t *bink2_scratch;
bink2_scratch bink2_scratch_create(const bink2 object);
void bink2_scratch_destroy(bink2_scratch scratch);

/**
 * Reads frame `frame`, enables video and audio and parses its slice table.
 * Returns BINK2_OK, BINK2_LAST for a frame past the end or BINK2_ERROR.
 */
int bink2_frame_prepare(bink2 object, unsigned long frame);
/**
 * Decodes slice `slice` of the prepared frame into `cur`. A key frame needs
 * no `ref`; any other needs the planes of the frame before it. Calls for
 * different slices with different scratch objects may run at the same time.
 * Returns 0 or BINK2_ERROR.
 */
int bink2_frame_decode_slice(bink2 object, bink2_scratch scratch, int slice,
                             const Bink2Planes *cur, const Bink2Planes *ref);
/** Once every slice returned 0: checks the slices and decodes the audio.
 *  Returns BINK2_MORE or BINK2_LAST on success, else BINK2_ERROR. */
int bink2_frame_finish(bink2 object);
/**
 * Reads frame `frame` and decodes only its sound, for a caller that starts the
 * sound before the picture it belongs to. Returns BINK2_OK, BINK2_LAST for a
 * frame past the end or BINK2_ERROR.
 */
int bink2_frame_prepare_sound(bink2 object, unsigned long frame);
/**
 * The bytes of sound that the chunk of `track` in frame `frame` decodes to, as
 * the chunk states them, read without decoding. 0 for a frame without sound.
 * Returns 0; 1 for a frame that reaches past the end of the file, which then
 * has no sound to read; -1 for a frame or track that does not exist or a
 * chunk that does not fit its frame.
 */
int bink2_frame_sound_bytes(const bink2 object, unsigned long frame, int track,
                            unsigned long *bytes);
/** Forgets the audio history, as at the start of a stream and after a seek. */
void bink2_frame_reset_audio(bink2 object);

#ifdef __cplusplus
}
#endif

#endif
