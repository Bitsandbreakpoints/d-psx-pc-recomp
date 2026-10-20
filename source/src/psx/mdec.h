/*
 * mdec.h - the PS1's movie decoder (MDEC) and the VLC decoding of the
 * STR bitstream (LIBPRESS's DecDCTvlc), in software (mdec.c).
 */
#ifndef PSX_MDEC_H
#define PSX_MDEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDEC_MACROBLOCK         16      /* pixels per side */

/*
 * Decodes a frame's bitstream (STR version 2) of a width x height picture
 * into the MDEC's run-length codes: the MDEC header word (0x3800 << 16 |
 * number of code words), a word with the size, then the 16-bit codes of
 * all blocks.  `out_words` is the room in `out`.  Returns the words
 * written, 0 if the bitstream is not understood.
 */
int mdec_vlc_decode(const uint32_t *bitstream, int width, int height, uint32_t *out,
                    int out_words);

/*
 * Decodes the codes of a whole frame (macroblocks in columns, top to
 * bottom, left to right) into RGB pixels, 0xRRGGBB in `rgb` (room for
 * `max_pixels`); *width / *height get the size (whole macroblocks).  With
 * `smooth`, the colour difference planes (half resolution) are upsampled
 * bilinearly across the whole frame instead of repeated per pixel.
 * Returns FALSE if the frame is too large or has no size.
 */
int mdec_decode_frame(const uint32_t *codes, int smooth, uint32_t *rgb, int max_pixels,
                      int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MDEC_H */
