/*
 * memory.c - the PS1's memory at its own addresses.
 *
 * The game code uses fixed addresses (the R04 door textures and the sound
 * bank's work area at 0x80180000), and its uninitialised variables live at
 * their original addresses (build/gen/bss_symbols.c).  The PC port
 * is a 32-bit process linked with --large-address-aware, so it can map
 * the 2 MB of main RAM at 0x80000000 and the scratchpad at 0x1F800000 and
 * use those addresses unchanged.
 */
#include <windows.h>
#include <string.h>
#include "psx/memory.h"
#include "psx/savestate.h"

/* (VirtualAlloc works in pages: the scratchpad gets a whole one) */
#define PAGE_SIZE               0x1000u

int psx_memory_init(void)
{
    void *ram = VirtualAlloc((void *) PSX_RAM_ADDRESS, PSX_RAM_SIZE, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
    void *scratchpad = VirtualAlloc((void *) PSX_SCRATCHPAD_ADDRESS, PAGE_SIZE,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return ram == (void *) PSX_RAM_ADDRESS && scratchpad == (void *) PSX_SCRATCHPAD_ADDRESS;
}

void psx_memory_clear(void)
{
    memset((void *) PSX_RAM_ADDRESS, 0, PSX_RAM_SIZE);
    memset((void *) PSX_SCRATCHPAD_ADDRESS, 0, PSX_SCRATCHPAD_SIZE);
}

/* (a save state: savestate.c) */
void memory_state(StateIO *io)
{
    state_section(io, "RAM ");
    state_bytes(io, (void *) PSX_RAM_ADDRESS, PSX_RAM_SIZE);
    state_bytes(io, (void *) PSX_SCRATCHPAD_ADDRESS, PSX_SCRATCHPAD_SIZE);
}
