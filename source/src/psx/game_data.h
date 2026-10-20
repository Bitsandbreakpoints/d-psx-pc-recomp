/*
 * game_data.h - the game's initialised variables, set back to their
 * initial values when the game is started again from the launcher.
 */
#ifndef PSX_GAME_DATA_H
#define PSX_GAME_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Keeps a copy of the initial values; once at start-up.  FALSE on error. */
int pc_game_data_init(void);

/* Sets the variables back to their initial values. */
void pc_game_data_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GAME_DATA_H */
