/*
 * pc_game.h - included first in every file of the game code (../src) when
 * it is compiled for the PC port (-include, see configure.py).
 */
#ifndef PC_GAME_H
#define PC_GAME_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* The PS1's exit() takes no argument; in the PC port it ends the game and
 * goes back to the launcher (src/psx/system.c). */
void psx_exit(void);
#define exit(...) psx_exit()

/* The PS1's rand() is the BIOS's (A(2Fh)); the PC port has its own, whose
 * seed is part of the save states (src/psx/libapi.c), not the C library's. */
int psx_rand(void);
#define rand psx_rand

/*
 * The game's initialised variables (and, with -fno-zero-initialized-in-bss,
 * the zero ones) go into their own data section, so that the PC port can
 * set them back to their initial values when the game is started again
 * from the launcher (src/psx/game_data.c).  The uninitialised ones (.bss)
 * live in the PS1 memory at their original addresses (build/gen/bss_symbols.c).
 */
#pragma clang section data=".data$game_1"

#ifdef __cplusplus
extern "C" {
#endif

/* main() of the PS1 program (../src/main.c, renamed by configure.py) */
int psx_main(void);

/* The top of the game's main loop (../include/common.h): save states are
 * taken and restored there (src/psx/savestate.c). */
void psx_safe_point(void);
#undef PC_SAFE_POINT                    /* (a library file may have seen common.h first) */
#define PC_SAFE_POINT() psx_safe_point()

#ifdef __cplusplus
}
#endif

#endif /* PC_GAME_H */
