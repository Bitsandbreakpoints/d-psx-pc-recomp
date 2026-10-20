/*
 * libgte.c - the two LIBGTE functions D uses: rsin() and rcos().
 *
 * As in the library, the sine of 0..90 degrees comes from the library's
 * table (in the game's executable, which psx_run_game() loads from the
 * disc), and the rest of the circle from its symmetry.  Angles: 4096 = 360
 * degrees, results: 4096 = 1.0.
 */
#include "psx/psx.h"

/* the library's table in the executable's data: int16 sin(a), a = 0..0x400 */
#define SIN_TABLE               0x80052BF4u

#define ANGLE_MASK              0xFFF   /* one turn */
#define QUARTER                 0x400
#define HALF                    0x800
#define THREE_QUARTERS          0xC00
#define FULL                    0x1000

static int table_sin(int a)                     /* a = 0..QUARTER */
{
    return ((const int16_t *) (uintptr_t) SIN_TABLE)[a];
}

/* sin_1 of the library: sine of 0..FULL-1 */
static int sine(int a)
{
    if (a <= HALF) {
        return a <= QUARTER ? table_sin(a) : table_sin(HALF - a);
    }
    if (a <= THREE_QUARTERS) {
        return -table_sin(a - HALF);
    }
    return -table_sin(FULL - a);
}

int rsin(int a)
{
    return a >= 0 ? sine(a & ANGLE_MASK) : -sine(-a & ANGLE_MASK);
}

int rcos(int a)
{
    return sine((a + QUARTER) & ANGLE_MASK);
}
