/*
 * game_hooks.c - the PC port's own additions that look at the game's
 * state, run at every vertical blank (libetc.c):
 *
 *  - skipping the company logos (the Acclaim and WARP movies before the
 *    title, a setting): the game's own "already seen" flag
 *    (g_Story.skip_movie, see GetNextFrame() in ../src/movie.c) stops the
 *    movie at its second frame;
 *  - whether the title screen waits for a button (Esc / Circle there
 *    closes the program, main_pc.cpp);
 *  - whether Start does anything in the game right now (if not, it pauses,
 *    main_pc.cpp);
 *  - tests: D_EVENT=<vblank>:<event>[@<location>][,...] starts a system
 *    screen or puzzle at the given vertical blank, the way the game's
 *    scene events do (CheckLocationEvent), e.g. "2400:75" for the disc
 *    change (EVENT_DISKGAME); with @<location> the player stands there
 *    first (puzzles that look at the location, e.g. "3000:57@118").  The system screens are run by RunSystemScreen(), the
 *    puzzles (and the title, the compact, the ending) by RunPuzzle().
 */
#include <stdlib.h>
#include "psx/psx.h"
#include "psx/system.h"
#include "game.h"

#define SKIP_THIS_MOVIE         1       /* g_Story.skip_movie */

/* RunTitle()'s steps while the title is shown (../src/title.c): after the
 * intro movie, until a button was pressed */
#define TITLE_SHOWN_FIRST       2
#define TITLE_SHOWN_LAST        8

int psx_on_title_screen(void)
{
    return g_Event.id == EVENT_TITLE && g_Event.running == 1 && g_Game.phase == PHASE_PUZZLE
           && g_Event.step >= TITLE_SHOWN_FIRST && g_Event.step <= TITLE_SHOWN_LAST;
}

static int s_skip_logos;

void psx_set_skip_logos(int on)
{
    s_skip_logos = on;
}

static void skip_logos(void)
{
    if (s_skip_logos && g_Game.phase == PHASE_MOVIE && g_Movie.state == MOVIE_PLAYING
        && (g_Game.movie == MOVIE_ACCLAIM || g_Game.movie == MOVIE_WARP)) {
        g_Story.skip_movie = SKIP_THIS_MOVIE;
    }
}

static int is_system_screen(int event)
{
    return event == EVENT_R04LOOP || event == EVENT_R05FIGHT || event == EVENT_DISKGAME
           || event == EVENT_DISC_START || event == EVENT_DISC1_TITLE;
}

static void test_events(int vblank)
{
    static int checked, enabled;
    static char script[256];
    if (!checked) {
        const char *value = getenv("D_EVENT");
        checked = 1;
        enabled = value != NULL;
        if (enabled) {
            snprintf(script, sizeof(script), "%s", value);
        }
    }
    if (!enabled) {
        return;
    }
    for (const char *p = script; *p;) {
        int at = atoi(p);
        const char *colon = strchr(p, ':');
        if (!colon) {
            break;
        }
        if (at == vblank) {
            int event = atoi(colon + 1);
            const char *at_sign = strchr(colon, '@');
            const char *next = strchr(colon, ',');
            if (at_sign && (!next || at_sign < next)) {
                g_Game.location = (uint32_t) atoi(at_sign + 1);
            }
            psx_log("D_EVENT: event %d at vblank %d (disc %u, location %u)", event, vblank,
                    g_System.disc, g_Game.location);
            g_Event.id = event;
            g_Game.phase = is_system_screen(event) ? PHASE_SYSTEM : PHASE_PUZZLE;
            g_System.mode = MODE_SETUP_PHASE;
        }
        const char *comma = strchr(colon, ',');
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
}

/*
 * Start only does something in the intro: the logos (EVENT_DISC1_TITLE),
 * the title screen and its opening movie (EVENT_TITLE, ../src/title.c),
 * the screen that starts the game on a disc (EVENT_DISC_START), and the
 * game's first movie, which it skips (OnMovieStart() in ../src/movie.c).
 * Everywhere else - the castle, the inventory, the other movies, the
 * puzzles, the ending - the game ignores it.  (The system screens set
 * g_Event.id back to 0 when they are done.)
 */
int psx_start_is_free(void)
{
    switch (g_Event.id) {
    case EVENT_DISC1_TITLE:
    case EVENT_TITLE:
    case EVENT_DISC_START:
        return 0;
    default:
        break;
    }
    return !(g_Game.phase == PHASE_MOVIE && g_Game.movie == MOVIE_R10M00);
}

/* the log says when Start changes between the game's and the pause */
static void log_start_use(int vblank)
{
    static int last = -1;
    int free_now = psx_start_is_free();
    if (free_now != last) {
        psx_log("Start %s at vblank %d (event %d, phase %d, movie %d)",
                free_now ? "pauses" : "is the game's", vblank, g_Event.id, g_Game.phase,
                g_Game.movie);
        last = free_now;
    }
}

void game_hooks_vblank(int vblank)
{
    log_start_use(vblank);
    skip_logos();
    test_events(vblank);
}
