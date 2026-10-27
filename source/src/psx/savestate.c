/*
 * savestate.c - save states (see savestate.h).
 *
 * A file (saves/slotN.state next to d.exe): a header (with the size of
 * every part), the picture of the moment (for the lists of states), then
 * the state itself, deflated.  The state is every part of savestate.h in
 * turn, each behind a four-letter mark; reading it back runs the same
 * functions the other way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include "psx/psx.h"
#include "psx/savestate.h"
#include "psx/gpu.h"
#include "psx/log.h"
#include "platform/files.h"
#include "game.h"

void psx_exit(void);                    /* system.c */

#define STATE_MAGIC             "D-STATE"
/* (+1 when a part changes its meaning but not its size) */
#define STATE_VERSION           2
#define STATE_PATH              "saves/slot%d.state"
#define STATE_CAPACITY          (6 * 1024 * 1024)   /* what a state takes, about */
#define DEFLATE_LEVEL           3
/* the game passed the top of its main loop at most this many vertical
 * blanks ago: it does so every frame (a movie: every frame of it) */
#define RECENT_PASS_VBLANKS     8
/* a requested save gives up when no pass comes in this time */
#define SAVE_TIMEOUT_VBLANKS    120

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t build;                     /* file_game_build_id() */
    int64_t saved_at;                   /* time() */
    uint32_t disc;
    uint32_t play_frames;
    uint32_t thumb_width, thumb_height;
    uint32_t state_size;                /* unpacked */
    uint32_t packed_size;
    uint32_t part_count;
    uint32_t reserved[5];
    StatePart parts[STATE_MAX_PARTS];   /* the layout */
} StateHeader;

#define THUMB_PIXELS            (PSX_STATE_THUMB_WIDTH * PSX_STATE_THUMB_HEIGHT)
#define THUMB_BYTES             (THUMB_PIXELS * 4)
#define OPAQUE                  0xFF000000u

static int s_passed;                    /* the game reached the top of its loop */
static int s_last_pass;                 /* VSync(-1) then */
static int s_save_slot = -1;            /* a save waits for the next pass */
static int s_save_requested_at;
static int s_save_result;
static uint8_t *s_load;                 /* a state waiting for the next start */
static size_t s_load_size;
static int s_load_disc;
static char s_error[200];

/* ---- the stream of parts ---------------------------------------------- */

void state_bytes(StateIO *io, void *p, size_t size)
{
    if (io->failed) {
        return;
    }
    if (io->counting) {
        io->pos += size;
        return;
    }
    if (io->loading) {
        if (io->pos + size > io->size) {
            io->failed = 1;
            return;
        }
        memcpy(p, io->data + io->pos, size);
    } else {
        if (io->pos + size > io->size) {
            size_t capacity = io->size ? io->size : STATE_CAPACITY;
            while (capacity < io->pos + size) {
                capacity *= 2;
            }
            uint8_t *bigger = (uint8_t *) realloc(io->data, capacity);
            if (!bigger) {
                io->failed = 1;
                return;
            }
            io->data = bigger;
            io->size = capacity;
        }
        memcpy(io->data + io->pos, p, size);
    }
    io->pos += size;
}

void state_section(StateIO *io, const char *tag)
{
    char mark[4];
    memcpy(mark, tag, sizeof(mark));
    if (!io->loading) {
        if (io->part_count >= STATE_MAX_PARTS) {
            io->failed = 1;
            return;
        }
        StatePart *part = &io->parts[io->part_count++];
        memcpy(part->tag, tag, sizeof(part->tag));
        part->size = (uint32_t) io->pos;            /* (its start, until finish_parts()) */
    }
    state_bytes(io, mark, sizeof(mark));
    if (io->loading && !io->failed && memcmp(mark, tag, sizeof(mark)) != 0) {
        psx_log("state: part \"%.4s\" missing", tag);
        io->failed = 1;
    }
}

static void all_parts(StateIO *io)
{
    memory_state(io);
    game_data_state(io);
    gpu_state(io);
    libgpu_state(io);
    spu_state(io);
    libsnd_state(io);                   /* (after the SPU: it sets its volume) */
    xa_state(io);
    stream_state(io);
    libcd_state(io);
    libpress_state(io);
    libetc_state(io);
    libapi_state(io);
    libpad_state(io);
    font_state(io);
    state_section(io, "END ");
}

/* the parts' starts into their sizes */
static void finish_parts(StateIO *io)
{
    for (int i = 0; i < io->part_count; i++) {
        uint32_t end = i + 1 < io->part_count ? io->parts[i + 1].size : (uint32_t) io->pos;
        io->parts[i].size = end - io->parts[i].size;
    }
}

/* this program's layout of a state (measured once) */
static const StateIO *state_layout(void)
{
    static StateIO s_layout;
    static int s_measured;
    if (!s_measured) {
        s_layout.counting = 1;
        all_parts(&s_layout);
        finish_parts(&s_layout);
        s_measured = 1;
    }
    return &s_layout;
}

/*
 * The builds of v1.0.0 and v1.0.1 took the id over the whole of game.dll,
 * debug information and all, so it changed with the folder it was built
 * in; their game.dll's code and data are those of the second id, and their
 * states load wherever that game is.
 */
static const struct {
    uint64_t whole_file, code;
} EARLY_BUILDS[] = {
    { 0x17A454853CC6D5B0ull, 0xA73721206F87B8C8ull },     /* v1.0.0, v1.0.1 */
};

/* TRUE if a state of that id was saved with the game that runs now */
static int same_game(uint64_t build)
{
    uint64_t current = file_game_build_id();
    if (build == current) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(EARLY_BUILDS) / sizeof(EARLY_BUILDS[0]); i++) {
        if (build == EARLY_BUILDS[i].whole_file && current == EARLY_BUILDS[i].code) {
            return 1;
        }
    }
    return 0;
}

/* NULL if this program can load a state with that header, else why not */
static const char *cannot_load(const StateHeader *header)
{
    const StateIO *layout = state_layout();
    if (header->version != STATE_VERSION || header->header_size != sizeof(StateHeader)
        || header->part_count != (uint32_t) layout->part_count
        || memcmp(header->parts, layout->parts, sizeof(StatePart) * layout->part_count) != 0) {
        return "Saved by an older version of the program";
    }
    if (!same_game(header->build)) {
        return "Saved with other game code (game.dll)";
    }
    return NULL;
}

/* ---- files ------------------------------------------------------------ */

/* (tests: D_SAVE_DIR=<folder> keeps their states apart from the player's) */
static void slot_path(int slot, char *out, int size)
{
    char name[64];
    const char *folder = getenv("D_SAVE_DIR");
    if (folder && folder[0]) {
        snprintf(out, size, "%s/slot%d.state", folder, slot + 1);
        return;
    }
    snprintf(name, sizeof(name), STATE_PATH, slot + 1);
    file_exe_relative(name, out, size);
}

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
    psx_log("state: %s", text);
}

const char *psx_state_error(void)
{
    return s_error;
}

/* the header of slot's file and its picture; FALSE if there is none (of
 * another version: only the version is filled in) */
static int read_header(int slot, StateHeader *header, uint32_t *thumbnail)
{
    static uint8_t buffer[sizeof(StateHeader) + THUMB_BYTES];
    char path[1024];
    slot_path(slot, path, sizeof(path));
    int n = file_read(path, buffer, sizeof(buffer));
    if (n < (int) sizeof(StateHeader)) {
        return 0;
    }
    memcpy(header, buffer, sizeof(*header));
    if (memcmp(header->magic, STATE_MAGIC, sizeof(header->magic)) != 0) {
        return 0;
    }
    int current = header->version == STATE_VERSION && header->header_size == sizeof(StateHeader);
    if (!current) {
        uint32_t version = header->version;
        memset(header, 0, sizeof(*header));
        header->version = version;
    }
    if (thumbnail) {
        if (current && n == (int) sizeof(buffer) && header->thumb_width == PSX_STATE_THUMB_WIDTH
            && header->thumb_height == PSX_STATE_THUMB_HEIGHT) {
            memcpy(thumbnail, buffer + sizeof(StateHeader), THUMB_BYTES);
        } else {
            for (int i = 0; i < THUMB_PIXELS; i++) {
                thumbnail[i] = OPAQUE;
            }
        }
    }
    return 1;
}

int psx_state_info(int slot, PsxStateInfo *info)
{
    StateHeader header;
    memset(info, 0, sizeof(*info) - sizeof(info->thumbnail));
    if (slot < 0 || slot >= PSX_STATE_SLOTS || !read_header(slot, &header, info->thumbnail)) {
        return 0;
    }
    info->exists = 1;
    info->problem = cannot_load(&header);
    info->usable = info->problem == NULL;
    info->disc = (int) header.disc;
    info->play_frames = header.play_frames;
    info->saved_at = header.saved_at;
    return 1;
}

int psx_state_delete(int slot)
{
    char path[1024];
    if (slot < 0 || slot >= PSX_STATE_SLOTS) {
        return 0;
    }
    slot_path(slot, path, sizeof(path));
    int ok = file_delete(path);
    psx_log("state: slot %d %s", slot + 1, ok ? "deleted" : "could not be deleted");
    return ok;
}

/* the displayed picture, shrunk (each pixel the mean of its area) */
static void make_thumbnail(uint32_t *out)
{
    int w, h;
    const uint32_t *picture = gpu_display_rgba(&w, &h);
    for (int ty = 0; ty < PSX_STATE_THUMB_HEIGHT; ty++) {
        for (int tx = 0; tx < PSX_STATE_THUMB_WIDTH; tx++) {
            if (!picture || w <= 0 || h <= 0) {
                out[ty * PSX_STATE_THUMB_WIDTH + tx] = OPAQUE;
                continue;
            }
            int x0 = tx * w / PSX_STATE_THUMB_WIDTH, x1 = (tx + 1) * w / PSX_STATE_THUMB_WIDTH;
            int y0 = ty * h / PSX_STATE_THUMB_HEIGHT, y1 = (ty + 1) * h / PSX_STATE_THUMB_HEIGHT;
            x1 = x1 > x0 ? x1 : x0 + 1;
            y1 = y1 > y0 ? y1 : y0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    uint32_t c = picture[y * w + x];
                    r += c & 0xFF;
                    g += c >> 8 & 0xFF;
                    b += c >> 16 & 0xFF;
                    n++;
                }
            }
            out[ty * PSX_STATE_THUMB_WIDTH + tx] = OPAQUE | (b / n) << 16 | (g / n) << 8 | r / n;
        }
    }
}

static int save_to(int slot)
{
    StateIO io = { 0 };
    all_parts(&io);
    if (io.failed) {
        free(io.data);
        set_error("Out of memory.");
        return 0;
    }
    mz_ulong packed_size = mz_compressBound((mz_ulong) io.pos);
    size_t file_size = sizeof(StateHeader) + THUMB_BYTES + packed_size;
    uint8_t *file = (uint8_t *) malloc(file_size);
    if (!file) {
        free(io.data);
        set_error("Out of memory.");
        return 0;
    }
    uint8_t *packed = file + sizeof(StateHeader) + THUMB_BYTES;
    int ok = mz_compress2(packed, &packed_size, io.data, (mz_ulong) io.pos, DEFLATE_LEVEL) == MZ_OK;
    finish_parts(&io);
    StateHeader header = { 0 };
    memcpy(header.magic, STATE_MAGIC, sizeof(header.magic));
    header.version = STATE_VERSION;
    header.header_size = sizeof(StateHeader);
    header.build = file_game_build_id();
    header.part_count = (uint32_t) io.part_count;
    memcpy(header.parts, io.parts, sizeof(header.parts));
    header.saved_at = (int64_t) time(NULL);
    header.disc = g_System.disc;
    header.play_frames = g_Game.play_time;
    header.thumb_width = PSX_STATE_THUMB_WIDTH;
    header.thumb_height = PSX_STATE_THUMB_HEIGHT;
    header.state_size = (uint32_t) io.pos;
    header.packed_size = (uint32_t) packed_size;
    memcpy(file, &header, sizeof(header));
    make_thumbnail((uint32_t *) (file + sizeof(StateHeader)));
    char path[1024];
    slot_path(slot, path, sizeof(path));
    ok = ok && file_write(path, file, (int) (sizeof(StateHeader) + THUMB_BYTES + packed_size));
    if (ok) {
        psx_log("state: slot %d saved (%u KB, %u KB packed)", slot + 1, (unsigned) (io.pos / 1024),
                (unsigned) (packed_size / 1024));
    } else {
        set_error("The state could not be written.");
    }
    free(file);
    free(io.data);
    return ok;
}

/* ---- saving and loading ----------------------------------------------- */

int psx_state_can_save(void)
{
    if (!s_passed || g_System.mode != MODE_RUN_PHASE) {
        return 0;
    }
    switch (g_Game.phase) {
    case PHASE_LOCATION:
    case PHASE_MOVIE:
    case PHASE_INVENTORY:
        return VSync(-1) - s_last_pass <= RECENT_PASS_VBLANKS;
    default:
        return 0;                       /* (the puzzles and system screens loop) */
    }
}

void psx_state_request_save(int slot)
{
    s_save_slot = slot;
    s_save_requested_at = VSync(-1);
    s_save_result = 0;
}

int psx_state_save_result(void)
{
    if (s_save_slot >= 0 && VSync(-1) - s_save_requested_at > SAVE_TIMEOUT_VBLANKS) {
        s_save_slot = -1;
        s_save_result = -1;
        set_error("The game cannot be saved here.");
    }
    return s_save_result;
}

void psx_state_cancel_load(void)
{
    free(s_load);
    s_load = NULL;
    s_load_disc = 0;
}

int psx_state_pending_disc(void)
{
    return s_load ? s_load_disc : 0;
}

int psx_state_load(int slot)
{
    StateHeader header;
    psx_state_cancel_load();
    if (slot < 0 || slot >= PSX_STATE_SLOTS || !read_header(slot, &header, NULL)) {
        set_error("There is no state in that slot.");
        return 0;
    }
    const char *problem = cannot_load(&header);
    if (problem) {
        char text[160];
        snprintf(text, sizeof(text), "%s: it cannot be loaded.", problem);
        set_error(text);
        return 0;
    }
    size_t file_size = sizeof(StateHeader) + THUMB_BYTES + header.packed_size;
    uint8_t *file = (uint8_t *) malloc(file_size);
    uint8_t *state = (uint8_t *) malloc(header.state_size ? header.state_size : 1);
    char path[1024];
    slot_path(slot, path, sizeof(path));
    mz_ulong state_size = header.state_size;
    int ok = file && state && file_read(path, file, (int) file_size) == (int) file_size
             && mz_uncompress(state, &state_size, file + sizeof(StateHeader) + THUMB_BYTES,
                              header.packed_size) == MZ_OK
             && state_size == header.state_size && header.disc >= 1 && header.disc <= 3;
    free(file);
    if (!ok) {
        free(state);
        set_error("The state cannot be read.");
        return 0;
    }
    s_load = state;
    s_load_size = state_size;
    s_load_disc = (int) header.disc;
    psx_log("state: slot %d read (disc %d)", slot + 1, s_load_disc);
    return 1;
}

/* the game (re)starts (psx_run_game()) */
void savestate_game_started(void)
{
    s_passed = 0;
    s_save_slot = -1;
}

/* the top of the game's main loop (../include/common.h) */
void psx_safe_point(void)
{
    int vblank = VSync(-1);
    if (s_passed && vblank - s_last_pass > RECENT_PASS_VBLANKS && g_System.mode == MODE_RUN_PHASE
        && g_Game.phase >= PHASE_LOCATION && g_Game.phase <= PHASE_INVENTORY) {
        psx_log("state: %d vertical blanks between two passes (phase %d)", vblank - s_last_pass,
                g_Game.phase);
    }
    s_passed = 1;
    s_last_pass = vblank;
    if (s_load) {
        StateIO io = { 0 };
        io.data = s_load;
        io.size = s_load_size;
        io.loading = 1;
        all_parts(&io);
        free(s_load);
        s_load = NULL;
        s_last_pass = VSync(-1);
        if (io.failed || io.pos != io.size) {
            psx_log("state: does not fit this program (%u of %u bytes)", (unsigned) io.pos,
                    (unsigned) io.size);
            psx_exit();                 /* (half of it is in place: back to the launcher) */
        }
        psx_log("state: restored (disc %u, mode %u, phase %d)", g_System.disc, g_System.mode,
                g_Game.phase);
    }
    if (s_save_slot >= 0) {
        int slot = s_save_slot;
        s_save_slot = -1;
        s_save_result = save_to(slot) ? 1 : -1;
    }
}
