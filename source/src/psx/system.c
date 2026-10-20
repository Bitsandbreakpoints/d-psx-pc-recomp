/*
 * system.c - runs the PS1 program (the game's main()) inside the PC port.
 *
 * The game's code runs on the PC's main thread.  It never returns from
 * main(), but it waits for the vertical blank (VSync()) at least once per
 * frame; that is where the PC side gets control: psx_vblank() calls the
 * frame hook, and when the hook says stop, it jumps back out of the game
 * (longjmp) to psx_run_game().  Starting again resets all memory and data.
 */
#include <setjmp.h>
#include <stdio.h>
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/memory.h"
#include "psx/game_data.h"
#include "psx/gpu.h"
#include "disc/disc.h"

void libetc_reset(void);
void libetc_game_running(int running);
void libetc_game_returned(void);
void libcd_reset(void);
void libpad_reset(void);
void libsnd_reset(void);
void libpress_reset(void);
void font_reset(void);
void savestate_game_started(void);

/* the game's executable on the disc (a PS-X EXE: 2 KB header, then the image) */
#define EXE_HEADER_SIZE         0x800
#define EXE_TEXT_ADDRESS        0x18    /* header fields */
#define EXE_TEXT_SIZE           0x1C
#define SECTOR_SIZE             2048

static jmp_buf s_stop;
static PsxFrameHook s_hook;

/*
 * Loads the game's executable (named in SYSTEM.CNF; the same program on
 * all three discs) to its place in PS1 memory.  Its code is not run (the
 * PC port has it compiled), but the library replacements read Sony's
 * tables (the sine table of rsin()) there.
 */
static int load_executable(void)
{
    char name[64], path[80];
    uint32_t lba, size;
    uint8_t header[SECTOR_SIZE];
    if (!disc_boot_file(name, sizeof(name))) {
        psx_log("SYSTEM.CNF names no executable");
        return 0;
    }
    snprintf(path, sizeof(path), "\%s;1", name);
    if (!disc_find_file(path, &lba, &size) || !disc_read_sectors(lba, 1, header)) {
        psx_log("cannot read %s", path);
        return 0;
    }
    uint32_t address, text_size;
    memcpy(&address, header + EXE_TEXT_ADDRESS, 4);
    memcpy(&text_size, header + EXE_TEXT_SIZE, 4);
    if (address < PSX_RAM_ADDRESS || address + text_size > PSX_RAM_ADDRESS + PSX_RAM_SIZE
        || text_size > size - EXE_HEADER_SIZE + SECTOR_SIZE) {
        psx_log("%s: unexpected header", path);
        return 0;
    }
    uint32_t sectors = (text_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    return disc_read_sectors(lba + EXE_HEADER_SIZE / SECTOR_SIZE, sectors, (void *) (uintptr_t) address);
}

int psx_system_init(void)
{
    if (!psx_memory_init()) {
        psx_log("cannot map the PS1 memory at 0x80000000 / 0x1F800000");
        return 0;
    }
    return pc_game_data_init();
}

void psx_run_game(PsxFrameHook hook)
{
    s_hook = hook;
    libetc_game_running(1);             /* the sound waits while the game's code runs */
    psx_memory_clear();
    load_executable();
    pc_game_data_reset();
    gpu_reset();
    libetc_reset();
    libcd_reset();
    libpad_reset();
    libsnd_reset();
    libpress_reset();
    font_reset();
    savestate_game_started();
    psx_log("game started");
    if (setjmp(s_stop) == 0) {
        psx_main();
        psx_log("main() returned");
        libetc_game_returned();
    }
    libetc_game_running(0);
    psx_log("game stopped");
}

void psx_vblank(void)
{
    if (!s_hook()) {
        longjmp(s_stop, 1);
    }
}

/* exit() of the game (it gives up when a file is missing) */
void psx_exit(void)
{
    psx_log("the game called exit()");
    libetc_game_returned();
    longjmp(s_stop, 1);
}
