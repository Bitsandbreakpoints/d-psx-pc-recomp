/*
 * memory.h - the PS1's memory at its own addresses.
 */
#ifndef PSX_MEMORY_H
#define PSX_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_RAM_ADDRESS         0x80000000u     /* KSEG0 */
#define PSX_RAM_SIZE            0x200000u       /* 2 MB */
#define PSX_SCRATCHPAD_ADDRESS  0x1F800000u
#define PSX_SCRATCHPAD_SIZE     0x400u          /* 1 KB */

/* Maps main RAM and the scratchpad; FALSE if an address is taken.  Must
 * run early, before DLLs could take the addresses. */
int psx_memory_init(void);

/* Clears both (power-on state). */
void psx_memory_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MEMORY_H */
