/*
 * libetc.c - LIBETC/LIBAPI: the vertical blank and interrupt callbacks.
 *
 * VSync() is where the game waits for the next frame, so that is where
 * the PC port shows the frame and keeps the 60 Hz pace (psx_vblank()).
 * The vertical blank callback runs once per frame, as the interrupt does
 * on the PS1.
 */
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/spu.h"
#include "psx/savestate.h"
#include "psx/stream.h"
#include "platform/threads.h"

void libpad_update(void);
void libsnd_tick(void);
void libsnd_mix_cd(int16_t *out, int frames);
void libsnd_filter_effects(int16_t *out, int frames);
void game_hooks_vblank(int vblank);

/* one frame of sound: 44100 / 60 samples */
#define SOUND_FRAME_SAMPLES     PSX_SOUND_FRAME_SAMPLES

static PsxAudioSink s_audio_sink;
static int16_t s_sound_frame[SOUND_FRAME_SAMPLES * 2];
static int s_sound_realtime;            /* the sound device's thread mixes */
static ThreadsLock *s_sound_lock;       /* held by the game's code (see system.h) */
static int s_game_running;              /* (under the lock) */
static volatile int s_paused;           /* psx_set_paused() */
static volatile int s_muted;            /* psx_set_muted() */

void psx_set_sound_realtime(int on)
{
    s_sound_realtime = on;
}

static ThreadsLock *sound_lock(void)
{
    if (!s_sound_lock) {
        s_sound_lock = threads_lock_create();
    }
    return s_sound_lock;
}

/* The game's code starts (lock taken) or has stopped (lock free). */
void libetc_game_running(int running)
{
    if (running) {
        threads_lock(sound_lock());
        s_game_running = 1;
    } else {
        threads_lock(sound_lock());
        s_game_running = 0;
        threads_unlock(sound_lock());
    }
}

void libetc_game_returned(void)
{
    threads_unlock(sound_lock());       /* (main() returned: still held) */
}

void psx_set_paused(int paused)
{
    s_paused = paused;
    stream_pause(paused);
}

void psx_set_muted(int muted)
{
    s_muted = muted;
}

void psx_sound_frame(int16_t out[PSX_SOUND_FRAME_SAMPLES * 2])
{
    threads_lock(sound_lock());
    if (s_game_running && !s_paused) {
        /* LIBSND's sequencer ticks 60 times a second (SS_TICK60) */
        libsnd_tick();
        spu_render(out, SOUND_FRAME_SAMPLES);
        libsnd_filter_effects(out, SOUND_FRAME_SAMPLES);
        libsnd_mix_cd(out, SOUND_FRAME_SAMPLES);
        if (s_muted) {
            memset(out, 0, SOUND_FRAME_SAMPLES * 2 * sizeof(int16_t));
        }
    } else {
        memset(out, 0, SOUND_FRAME_SAMPLES * 2 * sizeof(int16_t));
    }
    threads_unlock(sound_lock());
}

void psx_set_audio_sink(PsxAudioSink sink)
{
    s_audio_sink = sink;
}

static void (*s_vsync_callback)(void);
static int s_vblank_count;

void libetc_reset(void)
{
    s_vsync_callback = NULL;
    s_vblank_count = 0;
}

/*
 * Reading the disc image can take a while (the first read of a part of a
 * CHD, or of a file Windows has not cached yet: tens of milliseconds), and
 * the sound device only keeps about 50 ms queued.  The reads touch nothing
 * of the sound, so the sound thread may mix while they run (libcd.c,
 * stream.c call these around them).
 */
void libetc_io_begin(void)
{
    threads_unlock(sound_lock());
}

void libetc_io_end(void)
{
    threads_lock(sound_lock());
}

/* The game waits for something that comes in its own time (the CD): the
 * sound may run meanwhile. */
void libetc_wait(int milliseconds)
{
    threads_unlock(sound_lock());
    threads_sleep_ms(milliseconds);
    threads_lock(sound_lock());
}

static void vblank(void)
{
    /* (the PC shows the frame and waits: the sound may run meanwhile; the
     * frame hook may also leave the game here, with the lock free) */
    threads_unlock(sound_lock());
    psx_vblank();
    threads_lock(sound_lock());
    libpad_update();                    /* the controllers answer at the blank */
    stream_tick();                      /* the CD goes on reading */
    s_vblank_count++;
    game_hooks_vblank(s_vblank_count);  /* skipping the logos, tests */
    if (s_vsync_callback) {
        s_vsync_callback();
    }
    if (s_sound_realtime) {
        return;                         /* (psx_sound_frame()) */
    }
    /* LIBSND's sequencer ticks at the vertical blank (SS_TICK60) */
    libsnd_tick();
    if (g_psx_skip_frames) {
        return;                         /* (nothing is heard while skipping) */
    }
    spu_render(s_sound_frame, SOUND_FRAME_SAMPLES);
    libsnd_filter_effects(s_sound_frame, SOUND_FRAME_SAMPLES);
    libsnd_mix_cd(s_sound_frame, SOUND_FRAME_SAMPLES);
    if (s_muted) {
        memset(s_sound_frame, 0, sizeof(s_sound_frame));
    }
    if (s_audio_sink) {
        s_audio_sink(s_sound_frame, SOUND_FRAME_SAMPLES);
    }
}

int ResetCallback(void)
{
    s_vsync_callback = NULL;
    return 0;
}

void StopCallback(void)
{
    s_vsync_callback = NULL;
}

int VSync(int mode)
{
    if (mode < 0) {
        return s_vblank_count;          /* frames since start-up */
    }
    if (mode == 1) {
        return 0;                       /* lines since the last vertical blank */
    }
    /* 0: wait for the next vertical blank; n > 1: n of them */
    for (int i = 0; i < (mode == 0 ? 1 : mode); i++) {
        vblank();
    }
    return 0;
}

void *VSyncCallback(void (*func)(void))
{
    void (*old)(void) = s_vsync_callback;
    s_vsync_callback = func;
    return (void *) old;
}

/* (a save state: savestate.c) */
void libetc_state(StateIO *io)
{
    state_section(io, "ETC ");
    STATE(io, s_vsync_callback);
    STATE(io, s_vblank_count);
}
