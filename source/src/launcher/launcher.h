/*
 * launcher.h - the first screen: choose the disc images, change the
 * settings, start the game.
 */
#ifndef LAUNCHER_LAUNCHER_H
#define LAUNCHER_LAUNCHER_H

/* launcher_frame() result */
enum LauncherAction {
    LAUNCHER_STAY = 0,
    LAUNCHER_PLAY,
    LAUNCHER_LOAD_STATE,                /* play from the save state launcher_state_slot() */
    LAUNCHER_QUIT
};

/* Checks the discs of the settings (if any). */
void launcher_init(void);

/* The image of disc `number` (1..3) if it can be used, else NULL. */
const char *launcher_disc_path(int number);

/* The save state chosen on the "Load game" page (LAUNCHER_LOAD_STATE). */
int launcher_state_slot(void);

/*
 * Checking disc images runs in the background (a thread; the first time a
 * disc's checksum takes a few seconds): launcher_check_discs() starts it
 * for chosen images (and the missing discs found next to them), FALSE if
 * a check is still running; launcher_poll() (every frame) takes the
 * results over into the settings once it is done; launcher_checking() is
 * TRUE until then.  launcher_checking_text() gives the loading line.
 */
#include "imgui.h"
#include "platform/platform.h"
int launcher_check_discs(char paths[][PLATFORM_PATH_MAX], int count);
void launcher_poll(void);
int launcher_checking(void);
int launcher_checking_text(char *out, int size);

/* The loading indicator: a spinning ring of dots. */
void launcher_draw_spinner(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour);
void launcher_shutdown(void);

/* Builds one frame of the launcher UI (between ui_begin_frame and
 * ui_end_frame); returns an enum LauncherAction. */
int launcher_frame(void);

/* TRUE while the game runs: the display settings apply to the window at
 * once (in the launcher it stays a splash window). */
void launcher_set_in_game(int in_game);

/* The settings screen alone (also used over the paused game). Returns
 * FALSE once the player closes it. */
int settings_screen(void);

/* TRUE while the settings wait for a key or button to assign (Esc then
 * cancels that, not the screen). */
int settings_capturing(void);

#endif /* LAUNCHER_LAUNCHER_H */
