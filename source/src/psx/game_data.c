/*
 * game_data.c - the game's initialised variables, so that they can be set
 * back to their initial values when the game is started again.
 *
 * The variables are in game.dll, in a section of their own, whose bounds
 * game_data_bounds() gives (game/game_dll.c).  The uninitialised variables
 * are in the PS1 memory, which is cleared at every start
 * (psx_memory_clear()).
 */
#include <stdlib.h>
#include <string.h>
#include "psx/psx.h"
#include "psx/game_data.h"
#include "psx/savestate.h"

void game_data_bounds(char **start, char **end);      /* game.dll */

static char *s_start, *s_end;
static char *s_initial;                 /* copy of the data at start-up */

static size_t data_size(void)
{
    return (size_t) (s_end - s_start);
}

int pc_game_data_init(void)
{
    game_data_bounds(&s_start, &s_end);
    if (s_end < s_start) {
        psx_log("game data: the sections are out of order");
        return 0;
    }
    s_initial = (char *) malloc(data_size() ? data_size() : 1);
    if (!s_initial) {
        return 0;
    }
    memcpy(s_initial, s_start, data_size());
    psx_log("game data: %u bytes of variables", (unsigned) data_size());
    return 1;
}

void pc_game_data_reset(void)
{
    memcpy(s_start, s_initial, data_size());
}

/* the game's variables as they are now (a save state: savestate.c) */
void game_data_state(StateIO *io)
{
    state_section(io, "DATA");
    state_bytes(io, s_start, data_size());
}
