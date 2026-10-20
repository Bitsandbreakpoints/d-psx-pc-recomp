/*
 * savestate.h - save states: the whole state of the running game in a
 * file, and back.
 *
 * A state is taken at the top of the game's main loop (psx_safe_point(),
 * ../include/common.h), where everything the game knows is in memory: the
 * PS1 memory, the game's own variables, and the state of the library
 * replacements (VRAM, sound RAM and voices, the CD and its streaming, ...).
 * Looking around, the movies and the inventory pass there every frame;
 * the puzzles and the system screens (title, disc change, ending) run
 * loops of their own, so no state can be saved while they run.
 *
 * Loading starts the game afresh and puts the state back at the first
 * pass, before the game has done anything.  The state holds addresses of
 * the game's code and data (its callbacks, pointers to its tables), so it
 * only fits the game.dll that saved it - which stays the same through
 * rebuilds of the rest of the port (configure.py).  The file also lists
 * the size of every part: a state whose parts are laid out otherwise (the
 * port's own state changed) is refused too.  psx_state_info() tells.
 */
#ifndef PSX_SAVESTATE_H
#define PSX_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_STATE_SLOTS         8
#define PSX_STATE_THUMB_WIDTH   160     /* the picture of a state (4:3) */
#define PSX_STATE_THUMB_HEIGHT  120

typedef struct {
    int exists;                         /* there is a file */
    int usable;                         /* ... that this program can load */
    const char *problem;                /* else why not */
    int disc;                           /* 1..3 */
    uint32_t play_frames;               /* the game's clock (60 a second) */
    int64_t saved_at;                   /* time() */
    uint32_t thumbnail[PSX_STATE_THUMB_WIDTH * PSX_STATE_THUMB_HEIGHT];    /* 0xAABBGGRR */
} PsxStateInfo;

/* What is in slot `slot` (0 .. PSX_STATE_SLOTS - 1); FALSE if nothing. */
int psx_state_info(int slot, PsxStateInfo *info);

/* TRUE when the game passes the top of its main loop often enough (it
 * did so in the last few frames) for a state to be saved now. */
int psx_state_can_save(void);

/* The state goes to `slot` at the next pass (the game must run on until
 * then: a frame or two). */
void psx_state_request_save(int slot);

/* After psx_state_request_save(): 0 still waiting, 1 saved, -1 failed
 * (then psx_state_error() says why). */
int psx_state_save_result(void);

/* Reads the state of `slot`; it is put back when the game is started
 * next (psx_run_game()).  FALSE if it cannot be used (psx_state_error()). */
int psx_state_load(int slot);

/* The disc the loaded state needs in the drive (1..3), 0 if none waits. */
int psx_state_pending_disc(void);
void psx_state_cancel_load(void);

/* Deletes the state of `slot` (the player chose to); FALSE if it could not
 * be deleted. */
int psx_state_delete(int slot);

/* Why the last save or load failed. */
const char *psx_state_error(void);

/* ---- for the library replacements ------------------------------------ */

/* One stream through which each part of the state is written, or read
 * back in the same order. */
#define STATE_MAX_PARTS         24

/* a part of a state: its mark and its size */
typedef struct {
    char tag[4];
    uint32_t size;
} StatePart;

typedef struct {
    uint8_t *data;
    size_t size, pos;
    int loading;
    int counting;                       /* only the sizes (state_layout()) */
    int failed;
    StatePart parts[STATE_MAX_PARTS];   /* (writing, counting) */
    int part_count;
} StateIO;

/* The bytes of `p` into the stream, or from it when loading. */
void state_bytes(StateIO *io, void *p, size_t size);
#define STATE(io, var)          state_bytes((io), &(var), sizeof(var))

/* A four-letter mark before each part (loading checks it). */
void state_section(StateIO *io, const char *tag);

/* the parts, in the order of the file */
void memory_state(StateIO *io);
void game_data_state(StateIO *io);
void gpu_state(StateIO *io);
void libgpu_state(StateIO *io);
void spu_state(StateIO *io);
void libsnd_state(StateIO *io);
void xa_state(StateIO *io);
void stream_state(StateIO *io);
void libcd_state(StateIO *io);
void libpress_state(StateIO *io);
void libetc_state(StateIO *io);
void libapi_state(StateIO *io);
void libpad_state(StateIO *io);
void font_state(StateIO *io);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_H */
