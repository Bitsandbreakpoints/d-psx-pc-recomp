/*
 * game_dll.c - linked into game.dll with the game's code (../../src):
 * the bounds of the game's initialised variables, for src/psx/game_data.c
 * in d.exe (which sets them back at every start and saves them in the
 * save states).
 *
 * pc_game.h puts the game's variables into the section .data$game_1 (the
 * game code is compiled with -fno-zero-initialized-in-bss, so there is no
 * .bss part); the markers here, in the sections _0 and _2, come before and
 * after it (the linker sorts sections with a '$' by name).
 */
#define MARKER(name, sect)      __attribute__((section(sect), used)) char name = 0

MARKER(s_game_data_start, ".data$game_0");
MARKER(s_game_data_end, ".data$game_2");

/* (exported: game.def) */
void game_data_bounds(char **start, char **end)
{
    *start = &s_game_data_start;
    *end = &s_game_data_end;
}
