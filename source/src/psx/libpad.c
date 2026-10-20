/*
 * libpad.c - the controllers, through LIBETC's original controller API
 * (PadInit / PadRead / PadStop), which D uses.
 *
 * PadRead() returns the buttons of both ports at once, active high: port 1
 * in the low 16 bits, port 2 in the high 16 bits.  Each half is the
 * controller's reply with its two bytes swapped (PADLup = 1 << 12 ...,
 * see ../include/psyq/libetc_ext.h).  As on the PS1, the controllers are
 * read at every vertical blank; the buttons come from the PC side
 * (psx_set_pad_reader(): keyboard / XInput after the settings).
 */
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/savestate.h"

#define PAD_PORTS               2
#define BUTTONS_LOW_BYTE        0x00FFu

static int s_running;
static uint32_t s_buttons;              /* as PadRead() returns them */
static PsxPadReader s_reader;

void psx_set_pad_reader(PsxPadReader reader)
{
    s_reader = reader;
}

void libpad_reset(void)
{
    s_running = 0;
    s_buttons = 0;
}

/* The reply's two bytes swapped: Select..Left to bits 8..15, L2..Square
 * to bits 0..7. */
static uint32_t padread_order(int buttons)
{
    return (uint32_t) ((buttons & BUTTONS_LOW_BYTE) << 8 | (buttons >> 8 & BUTTONS_LOW_BYTE));
}

/* at every vertical blank */
void libpad_update(void)
{
    uint32_t state = 0;
    if (s_running) {
        for (int port = 0; port < PAD_PORTS; port++) {
            int buttons = s_reader ? s_reader(port) : -1;
            if (buttons > 0) {
                state |= padread_order(buttons) << (16 * port);
            }
        }
    }
    s_buttons = state;
}

void PadInit(int mode)
{
    s_running = 1;
    s_buttons = 0;
}

uint32_t PadRead(int id)
{
    return s_buttons;
}

void PadStop(void)
{
    s_running = 0;
    s_buttons = 0;
}

/* (a save state: savestate.c) */
void libpad_state(StateIO *io)
{
    state_section(io, "PAD ");
    STATE(io, s_running);
    STATE(io, s_buttons);
}
