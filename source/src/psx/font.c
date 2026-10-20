/*
 * font.c - LIBGPU's debug font: FntLoad / FntOpen / FntPrint / FntFlush.
 *
 * A stream (FntOpen) is a text window on the screen holding up to n
 * characters; FntPrint adds formatted text to it, FntFlush draws it and
 * empties it.  D opens its windows and prints in the puzzles' debug screen
 * selector, but never flushes them (only PrintPuzzleDebug() would, and
 * nothing calls it), so on the console the text never shows.  Here the
 * streams behave the same; text that is flushed is kept as the stream's
 * picture, and the PC side draws it over the game (psx_font_text()), in
 * the window's place, instead of Sony's font texture.
 */
#include <stdarg.h>
#include <stdio.h>
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/savestate.h"

#define FONT_STREAMS            8
#define FONT_TEXT_MAX           1024    /* characters per stream (n of FntOpen, at most) */

typedef struct {
    int open;
    int x, y, w, h;                     /* the window, in screen pixels */
    int limit;                          /* characters it holds */
    char text[FONT_TEXT_MAX + 1];       /* printed since the last flush */
    int length;
    char shown[FONT_TEXT_MAX + 1];      /* the last flush */
} FontStream;

static FontStream s_streams[FONT_STREAMS];
static int s_stream_count;

void font_reset(void)
{
    memset(s_streams, 0, sizeof(s_streams));
    s_stream_count = 0;
}

/* The font's texture lives in the library's data; the text is drawn by
 * the PC side, so there is nothing to load. */
void FntLoad(int x, int y)
{
}

int FntOpen(int x, int y, int w, int h, int isbg, int n)
{
    if (s_stream_count >= FONT_STREAMS) {
        return -1;
    }
    FontStream *s = &s_streams[s_stream_count];
    memset(s, 0, sizeof(*s));
    s->open = 1;
    s->x = x;
    s->y = y;
    s->w = w;
    s->h = h;
    s->limit = n < FONT_TEXT_MAX ? n : FONT_TEXT_MAX;
    return s_stream_count++;
}

int FntPrint(int id, const char *fmt, ...)
{
    if (id < 0 || id >= s_stream_count || !s_streams[id].open) {
        return 0;
    }
    FontStream *s = &s_streams[id];
    va_list args;
    va_start(args, fmt);
    int room = s->limit - s->length;
    int written = room > 0 ? vsnprintf(s->text + s->length, (size_t) room + 1, fmt, args) : 0;
    va_end(args);
    if (written > 0) {
        s->length += written < room ? written : room;
    }
    return written;
}

void *FntFlush(int id)
{
    if (id < 0 || id >= s_stream_count || !s_streams[id].open) {
        return NULL;
    }
    FontStream *s = &s_streams[id];
    memcpy(s->shown, s->text, (size_t) s->length + 1);
    s->length = 0;
    s->text[0] = '\0';
    return NULL;
}

int psx_font_text(int index, int rect[4], const char **text)
{
    if (index < 0 || index >= s_stream_count) {
        return 0;
    }
    const FontStream *s = &s_streams[index];
    rect[0] = s->x;
    rect[1] = s->y;
    rect[2] = s->w;
    rect[3] = s->h;
    *text = s->shown;
    return 1;
}

/* (a save state: savestate.c) */
void font_state(StateIO *io)
{
    state_section(io, "FONT");
    STATE(io, s_streams);
    STATE(io, s_stream_count);
}
