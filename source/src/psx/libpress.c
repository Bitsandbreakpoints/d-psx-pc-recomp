/*
 * libpress.c - LIBPRESS: the MDEC, which decodes the movies' frames
 * (mdec.c does the work).
 *
 * The game's movie player (../src/movie.c) works as Sony's example: it
 * VLC-decodes a frame (DecDCTvlc), gives the codes to the MDEC
 * (DecDCTin), and asks for the picture one column of macroblocks at a time
 * (DecDCTout); when a column is done the MDEC interrupt calls the game's
 * callback, which copies it to VRAM and asks for the next column.
 *
 * Here the whole frame is decoded at the first DecDCTout() after
 * DecDCTin() (so the colours can be upsampled smoothly across it), and
 * each DecDCTout() hands out the next column at once and calls the
 * callback.  A DecDCTout() made by the callback is not run inside it but
 * right after it returns, as the interrupt would.
 */
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/mdec.h"
#include "psx/savestate.h"
#include "psx/stream.h"

#define VLC_BUFFER_WORDS        (0x28000 / 4)   /* g_MovieVlc[] (game.h) */
#define MAX_FRAME_PIXELS        (640 * 480)
#define MODE_24BIT              0x01            /* DecDCTin() mode bit */
#define BYTES_24BIT             3
#define BYTES_15BIT             2

static void (*s_out_callback)(void);
static const uint32_t *s_input;         /* DecDCTin() */
static int s_mode;
static int s_decoded;                   /* the frame is in s_frame */
static uint32_t s_frame[MAX_FRAME_PIXELS];
static int s_width, s_height;
static int s_column_x;                  /* the next column handed out */
static int s_in_callback;
static uint32_t *s_pending_data;        /* DecDCTout() made by the callback */
static size_t s_pending_length;
static int s_smooth = 1;

void psx_set_movie_smoothing(int on)
{
    s_smooth = on;
}

void libpress_reset(void)
{
    s_out_callback = NULL;
    s_input = NULL;
    s_decoded = 0;
    s_in_callback = 0;
    s_pending_data = NULL;
}

void DecDCTReset(int mode)
{
    s_input = NULL;
    s_decoded = 0;
}

int DecDCTvlc(const uint32_t *bs, uint32_t *buf)
{
    int width, height;
    stream_frame_size(&width, &height);
    if (!mdec_vlc_decode(bs, width, height, buf, VLC_BUFFER_WORDS)) {
        buf[0] = 0;
        buf[1] = 0;
    }
    return 0;
}

void DecDCTin(const uint32_t *data, int mode)
{
    s_input = data;
    s_mode = mode;
    s_decoded = 0;
    s_column_x = 0;
}

/* The next column of the frame, `length` words, into `data`. */
static void hand_out_column(uint32_t *data, size_t length)
{
    if (!s_decoded) {
        s_decoded = s_input
                    && mdec_decode_frame(s_input, s_smooth, s_frame, MAX_FRAME_PIXELS, &s_width,
                                         &s_height);
        if (!s_decoded) {
            s_width = s_height = 0;
        }
        s_column_x = 0;
    }
    int bytes_per_pixel = s_mode & MODE_24BIT ? BYTES_24BIT : BYTES_15BIT;
    int rows = (int) (length * 4 / (MDEC_MACROBLOCK * bytes_per_pixel));
    uint8_t *out = (uint8_t *) data;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < MDEC_MACROBLOCK; x++) {
            int px = s_column_x + x;
            uint32_t rgb = px < s_width && y < s_height ? s_frame[y * s_width + px] : 0;
            uint8_t r = (uint8_t) (rgb >> 16), g = (uint8_t) (rgb >> 8), b = (uint8_t) rgb;
            if (bytes_per_pixel == BYTES_24BIT) {
                *out++ = r;
                *out++ = g;
                *out++ = b;
            } else {
                uint16_t p = (uint16_t) (r >> 3 | (g >> 3) << 5 | (b >> 3) << 10);
                *out++ = (uint8_t) p;
                *out++ = (uint8_t) (p >> 8);
            }
        }
    }
    s_column_x += MDEC_MACROBLOCK;
}

void DecDCTout(uint32_t *data, size_t length)
{
    if (s_in_callback) {
        s_pending_data = data;          /* (run when the callback returns) */
        s_pending_length = length;
        return;
    }
    for (;;) {
        hand_out_column(data, length);
        if (!s_out_callback) {
            return;
        }
        s_pending_data = NULL;
        s_in_callback = 1;
        s_out_callback();
        s_in_callback = 0;
        if (!s_pending_data) {
            return;
        }
        data = s_pending_data;
        length = s_pending_length;
    }
}

void DecDCToutCallback(void (*func)(void))
{
    s_out_callback = func;
}

/* (a save state: savestate.c; DecDCTout() hands out a whole frame at once
 * - its callback asks for the next column - so between frames nothing of
 * the decoded picture is left to keep) */
void libpress_state(StateIO *io)
{
    state_section(io, "MDEC");
    STATE(io, s_out_callback);
    STATE(io, s_input);
    STATE(io, s_mode);
    STATE(io, s_column_x);
    if (io->loading) {
        s_decoded = 0;
        s_in_callback = 0;
        s_pending_data = NULL;
    }
}
