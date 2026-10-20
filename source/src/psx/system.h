/*
 * system.h - runs the PS1 program (the game's main()) inside the PC port.
 */
#ifndef PSX_SYSTEM_H
#define PSX_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called once per PS1 frame (vertical blank, from VSync()): shows the
 * frame, handles the window and waits for the next 1/60 s.  Returns
 * FALSE to stop the game.
 */
typedef int (*PsxFrameHook)(void);

/*
 * The buttons of controller port 0 or 1 as the PS1 pad reports them
 * (active high, 0x0001 Select ... 0x8000 Square); -1: nothing connected.
 */
typedef int (*PsxPadReader)(int port);

void psx_set_pad_reader(PsxPadReader reader);

/* Receives the sound of one frame: interleaved stereo at 44.1 kHz. */
typedef void (*PsxAudioSink)(const int16_t *samples, int frames);

void psx_set_audio_sink(PsxAudioSink sink);

/*
 * Sound in real time: the sound device's own thread asks for each frame
 * of sound (psx_sound_frame()) when it needs it, so the music keeps its
 * pace even when a picture takes longer than a frame.  The game's code and
 * the sound never run at the same time: the game holds a lock and lets go
 * of it only while it waits for the vertical blank.  Off (the default),
 * one frame of sound is mixed at every vertical blank and handed to the
 * PsxAudioSink, in step with the game (recordings, tests).
 */
#define PSX_SOUND_FRAME_SAMPLES 735     /* 44100 / 60 */

void psx_set_sound_realtime(int on);

/* The next 1/60 s of sound (silence while no game runs). */
void psx_sound_frame(int16_t out[PSX_SOUND_FRAME_SAMPLES * 2]);

/* Volumes in percent (the settings); cd = the movies' and music's XA audio. */
void psx_set_sound_volumes(int master, int cd, int effects);

/* The sound effects' bass softened as by a TV speaker (a high-pass at
 * 150 Hz on the SPU; the movies and the music stay as they are). */
void psx_set_effects_bass_cut(int on);

/*
 * The game is paused (by the player, or while the settings are shown):
 * the sound stops where it is (the SPU's voices and the CD's sound wait),
 * and the CD's clock does not count the paused time, so a movie goes on
 * exactly where it stopped.  The game itself waits in VSync() meanwhile.
 */
void psx_set_paused(int paused);

/* The sound is mixed as always but not heard (the game runs on for a
 * frame or two to where a save state can be taken). */
void psx_set_muted(int muted);

/* The movies' colours upsampled smoothly (the settings; libpress.c). */
void psx_set_movie_smoothing(int on);

/* The Acclaim and WARP logos skipped (the settings; game_hooks.c). */
void psx_set_skip_logos(int on);

/* TRUE while the title screen waits for a button. */
int psx_on_title_screen(void);

/* TRUE when Start does nothing in the game right now (not in the intro, on
 * the title or in the first movie), so the PC side may use it to pause. */
int psx_start_is_free(void);

/* The debug font's stream `index` (font.c): its window (x, y, w, h in the
 * game's drawing coordinates) and the text it showed last (FntFlush).
 * FALSE past the last stream. */
int psx_font_text(int index, int rect[4], const char **text);

/* Frames that will not be shown (skipping the logos): the GPU does not
 * draw them (DrawOTag returns at once) and no sound is mixed. */
extern int g_psx_skip_frames;

/*
 * The images of the three discs (UTF-8 paths, NULL or "" if not chosen).
 * The game starts with the disc in `first` (1..3) in the drive; when it
 * asks for the next disc (the lid is opened and closed), the drive gets
 * that disc's image (src/psx/libcd.c).
 */
void psx_set_discs(const char *const paths[3], int first);

/* While the game waits for a disc that was not chosen: its number (2, 3),
 * otherwise 0.  The launcher then asks for the image; psx_insert_disc()
 * puts it in. */
int psx_missing_disc(void);
void psx_insert_disc(const char *path);

/* Maps the PS1 memory and copies the initial data; once at start-up.
 * FALSE if that fails. */
int psx_system_init(void);

/* Runs the game from power-on until the frame hook returns FALSE.  The
 * disc image must be open (disc_open()). */
void psx_run_game(PsxFrameHook hook);

/* One vertical blank (VSync() calls it). */
void psx_vblank(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SYSTEM_H */
