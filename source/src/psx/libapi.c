/*
 * libapi.c - LIBAPI: root counters, events and critical sections.
 *
 * D starts root counter 1 (horizontal blanks) at boot and reads it as a
 * source of randomness (the random choices of a playthrough, game_flow.c).
 * Here it advances with the frames (263 lines each, NTSC) plus a little at
 * every read, so its value depends on when the player presses a button,
 * as on the console.  The events only exist to be opened and enabled.
 */
#include "psx/psx.h"
#include "psx/savestate.h"

#define LINES_PER_FRAME         263     /* NTSC */
#define COUNTER_MASK            0xFFFFu /* 16-bit counters */
#define READ_ADVANCE            37      /* lines between two reads in a frame */

#define COUNTER_ID_MASK         0xF     /* RCntCNT0..3 = DescRC | 0..3 */
#define HBLANK_COUNTER          1

/* the BIOS's rand(): x = x * 41C64E6Dh + 3039h, bits 16..30 */
#define RAND_MULTIPLIER         0x41C64E6Du
#define RAND_INCREMENT          0x3039u
#define RAND_SHIFT              16
#define RAND_MASK               0x7FFF

static uint32_t s_counter_reads;
static int s_next_event = 1;
static uint32_t s_rand_seed;            /* (the kernel's seed after boot: 0, as here) */

int SetRCnt(int spec, uint16_t target, int mode)
{
    return 1;
}

int StartRCnt(int spec)
{
    return 1;
}

int ResetRCnt(int spec)
{
    s_counter_reads = 0;
    return 1;
}

int GetRCnt(int spec)
{
    if ((spec & COUNTER_ID_MASK) != HBLANK_COUNTER) {
        return 0;
    }
    uint32_t lines = (uint32_t) VSync(-1) * LINES_PER_FRAME + s_counter_reads++ * READ_ADVANCE;
    return (int) (lines & COUNTER_MASK);
}

/* rand() of the game (pc_game.h): the BIOS's A(2Fh) */
int psx_rand(void)
{
    s_rand_seed = s_rand_seed * RAND_MULTIPLIER + RAND_INCREMENT;
    return (int) (s_rand_seed >> RAND_SHIFT & RAND_MASK);
}

int OpenEvent(uint32_t cl, uint32_t spec, int mode, void (*func)())
{
    return s_next_event++;
}

int CloseEvent(int event)
{
    return 1;
}

int EnableEvent(int event)
{
    return 1;
}

int EnterCriticalSection(void)
{
    return 1;
}

void ExitCriticalSection(void)
{
}

/* (a save state: savestate.c) */
void libapi_state(StateIO *io)
{
    state_section(io, "API ");
    STATE(io, s_counter_reads);
    STATE(io, s_next_event);
    STATE(io, s_rand_seed);
}
