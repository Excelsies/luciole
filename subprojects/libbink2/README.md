# libbink2 (pinned copy)

A C decoder for Bink 2 video (`KB2f` to `KB2n`) and Bink Audio 2, copied into Luciole and built as a Meson
subproject. Only the decoder is kept: no encoder, tools, players or tests.

| | |
| --- | --- |
| Origin | https://github.com/raynorpat/libbink2 |
| Commit | `ae2e41335235610ba6eff1c5124b7ee8c77165f9` (2026-11-17) |
| License | GNU Lesser General Public License, version 2.1 or later; see `LICENSE` |

The decoder is derived from the unmerged FFmpeg Bink 2 patches by Konstantin Shishkov (copyright 2014) and
Paul B Mahol (copyright 2019), and from FFmpeg's `binkaudio.c` by Peter Ross (copyright 2007-2011) and Daniel
Verkamp (copyright 2009), all under the LGPL, version 2.1 or later.

Luciole links this library statically. The sources here are the complete corresponding sources of the copy that is
linked, so it can be rebuilt with a modified copy of them.

## Local changes

Every changed file begins with a notice that says so. The changes:

- `src/bink2_dsp.c`: the luma motion compensation with a fraction in both directions keeps the rounded result of its
  horizontal pass unclipped between the passes, as the reference decoder does, for the older and the newer streams
  alike. Chroma motion compensation with a fraction in both directions rounds once, after the vertical pass, as the
  reference decoder does. The loops of the motion compensation with a vector in one direction run along the rows, so
  the compiler can vectorize them.
- `src/bink2_video.c`, `src/bink2_video.h`: one frame is decoded in steps (`bink2_video_job_begin`,
  `bink2_video_job_slice` once per slice, `bink2_video_job_end`) into planes the caller owns, with the row buffers
  of a slice in a `Bink2Scratch` of its own. Slices can then run on different threads and the pictures are the same,
  bit for bit, as with one thread. `bink2_video_decode` runs the same steps in order. `bink2_video_init_external`
  allocates no picture planes.
- `src/bink2.c`, `src/bink2.h`, `src/bink2_internal.h`: `bink2_open_callbacks` reads the file through a function of
  the caller; frame packets are followed by 8 zero bytes and refused above 64 MiB.
- `src/bink2_audio.c`: a sound chunk decodes only the bytes of sound it states at its start, and fails when it states
  none, more than the track's maximum, or more than its bits hold; the PCM is exactly that length. Decoded sound is
  bounded by the claim, not by the bits that follow it.
- `src/bink2_frames.c`, `src/bink2_frames.h` (new): the frame-at-a-time interface for a caller with threads of its
  own: `bink2_frame_prepare`, `bink2_frame_decode_slice`, `bink2_frame_finish`, the plane layout, key frame flags
  and a failing audio chunk reported as an error.
- `src/bink2_idct.c`, `src/bink2_idct.h` (new): the inverse transform of the "new" blocks, put or added into a
  picture, in one call on the 16 bit coefficients. It gives the same bytes as the plain C path of `bink2_dsp.c`
  (`bink2_idct_use_plain` runs that path, for tests). A block with no coefficient but its DC is a flat fill; the
  others go through the same arithmetic with the eight columns as the inner loop of each pass, so the compiler can
  vectorize it.
- `src/bink2_video_new.c`: the DC of an inter block is limited to 2047 when the frame flag 0x40000 is set and to
  1023 otherwise, as in the reference decoder (`bink2_new_inter_dc_limit`; the older streams use 1023). The file
  calls `bink2_idct.c` and dequantizes only the coefficients that were read; a block with none is not cleared or
  scanned. The plain path, kept for the comparison tests, dequantizes all 64 coefficients of each block.
- `meson.build` (new): builds the decoder sources, with the compiler flags upstream uses and at -O3 in every build type.

The library starts no threads. Streams older than `KB2g` and streams with an alpha plane (refused at open for
`KB2g` and later) have not been tried on real files here.
