/*
 * psx.h - common header of the PS1 library replacements (src/psx/): the
 * library headers the game code sees (the project's common.h: PSn00bSDK
 * plus the Psy-Q declarations in ../include/psyq), and logging.
 */
#ifndef PSX_PSX_H
#define PSX_PSX_H

#include "common.h"
#include "pc_game.h"
#include "psx/log.h"
#include "psx/memory.h"

/*
 * Ordering tables and primitive packets link to each other with 24-bit
 * addresses, which the PS1 also dereferences directly (main RAM is
 * mirrored from address 0).  The PC port turns them back into pointers:
 * below 2 MB they are PS1 RAM at 0x80000000, otherwise PC addresses (the
 * exe and its data lie below 16 MB, configure.py: --disable-dynamicbase).
 */
#define PSX_LINK_MASK           0x00FFFFFFu
#define PSX_LINK_END            0x00FFFFFFu     /* end of a list */

static inline uint32_t psx_link(const void *p)
{
    return (uint32_t) (uintptr_t) p & PSX_LINK_MASK;
}

static inline void *psx_pointer(uint32_t link)
{
    link &= PSX_LINK_MASK;
    return (void *) (uintptr_t) (link < PSX_RAM_SIZE ? PSX_RAM_ADDRESS | link : link);
}

#endif /* PSX_PSX_H */
