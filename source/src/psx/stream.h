/*
 * stream.h - continuous CD reading (CdRead2()) for the movies and the XA
 * music (stream.c).
 */
#ifndef PSX_STREAM_H
#define PSX_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

void stream_reset(void);

/* Around reads of the disc image: the sound thread may run meanwhile
 * (libetc.c). */
void libetc_io_begin(void);
void libetc_io_end(void);

/* Waits a while with the sound running (libetc.c). */
void libetc_wait(int milliseconds);

/* Starts reading at `lba` with the CdlSetmode bits `mode` (CdRead2()). */
void stream_start(int lba, int mode);

/* Stops reading (CdlPause, CdlStop, an ordinary CdRead()). */
void stream_stop(void);

/* The game is paused (psx_set_paused()): the drive's clock stops. */
void stream_pause(int paused);

/* Reads the sectors due by now (every vertical blank). */
void stream_tick(void);

/* The size of the movie frame StGetNext() handed out last (DecDCTvlc). */
void stream_frame_size(int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif /* PSX_STREAM_H */
