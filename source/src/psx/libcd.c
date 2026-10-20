/*
 * libcd.c - LIBCD: the CD-ROM drive, read from the player's disc images
 * (src/disc/), and the virtual lid for the disc change.
 *
 * Ordinary reads finish at once: CdRead() copies the sectors and
 * CdReadSync() / CdSync() report completion.  Streaming (CdRead2() and
 * the St* functions: the movies and the XA music) is in stream.c.
 *
 * The disc change: when a disc is finished, the game shows "insert the
 * next disc" and polls the drive's status (CdControl(CdlNop)) until the
 * lid has been opened and closed again.  The virtual lid opens after a
 * short moment, the drive gets the next disc's image, and the lid closes.
 * If that disc was not chosen in the launcher, the lid stays open until
 * psx_insert_disc() gets it.
 */
#include <stdio.h>
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/stream.h"
#include "psx/savestate.h"
#include "disc/disc.h"
#include "platform/threads.h"
#include "game.h"

/* sectors before LBA 0: the 2 second pregap (00:02:00) */
#define PREGAP_SECTORS          150
#define SECTORS_PER_SECOND      75

#define SLOW_READ_MS            8.0     /* the log notes reads this slow */

/* how many status polls (about one per frame) the lid takes */
#define LID_OPENS_AFTER         60      /* the "insert the next disc" screen stays a second */
#define LID_CLOSES_AFTER        30

enum LidState {
    LID_CLOSED,
    LID_OPENING,                    /* counting polls before it opens */
    LID_OPEN,                       /* counting polls before it closes */
    LID_WAITING                     /* open: the next disc was not chosen */
};

static char s_disc_paths[DISC_COUNT][1024];
static int s_lid = LID_CLOSED;
static int s_lid_polls;
static int s_wanted_disc;               /* the disc the lid waits for */

static int s_position;                  /* LBA of the next read (CdlSetloc) */
static int s_mode;                      /* CdlSetmode */
static CdlCB s_ready_callback;
static void (*s_data_callback)(void);

static int bcd_to_int(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0xF);
}

static uint8_t int_to_bcd(int value)
{
    return (uint8_t) ((value / 10) << 4 | value % 10);
}

/* ---- the discs -------------------------------------------------------- */

void psx_set_discs(const char *const paths[3], int first)
{
    for (int i = 0; i < DISC_COUNT; i++) {
        snprintf(s_disc_paths[i], sizeof(s_disc_paths[i]), "%s", paths[i] ? paths[i] : "");
    }
    if (first >= 1 && first <= DISC_COUNT && s_disc_paths[first - 1][0]) {
        disc_open(s_disc_paths[first - 1]);
    }
}

int psx_missing_disc(void)
{
    return s_lid == LID_WAITING ? s_wanted_disc : 0;
}

void psx_insert_disc(const char *path)
{
    if (s_wanted_disc >= 1 && s_wanted_disc <= DISC_COUNT) {
        snprintf(s_disc_paths[s_wanted_disc - 1], sizeof(s_disc_paths[0]), "%s", path);
    }
}

/* One status poll while the game waits for the next disc. */
static void lid_poll(void)
{
    switch (s_lid) {
    case LID_CLOSED:
        s_lid = LID_OPENING;
        s_lid_polls = 0;
        /* the game only takes the disc after the current one */
        s_wanted_disc = g_System.disc < DISC_COUNT ? (int) g_System.disc + 1 : DISC_COUNT;
        break;
    case LID_OPENING:
        if (++s_lid_polls >= LID_OPENS_AFTER) {
            stream_stop();
            disc_close();
            s_lid = LID_WAITING;
            s_lid_polls = 0;
            psx_log("lid open: waiting for disc %d", s_wanted_disc);
        }
        break;
    case LID_WAITING:
        if (s_disc_paths[s_wanted_disc - 1][0] && disc_open(s_disc_paths[s_wanted_disc - 1])) {
            psx_log("disc %d in the drive", disc_number());
            s_lid = LID_OPEN;
        }
        break;
    case LID_OPEN:
        if (++s_lid_polls >= LID_CLOSES_AFTER) {
            s_lid = LID_CLOSED;
        }
        break;
    }
}

static int lid_is_open(void)
{
    return s_lid == LID_OPEN || s_lid == LID_WAITING;
}

/* ---- commands --------------------------------------------------------- */

void libcd_reset(void)
{
    s_position = 0;
    s_mode = 0;
    s_ready_callback = NULL;
    s_data_callback = NULL;
    s_lid = LID_CLOSED;
    stream_reset();
}

int CdInit(void)
{
    s_position = 0;
    return 1;
}

void CdSetDebug(int level)
{
}

/* The status byte (CdlStat*): the motor runs, the lid as above. */
static uint8_t drive_status(void)
{
    return (uint8_t) (CdlStatStandby | (lid_is_open() ? CdlStatShellOpen : 0));
}

int CdControl(CdlCommand cmd, const void *param, uint8_t *result)
{
    switch (cmd) {
    case CdlNop:
        lid_poll();
        break;
    case CdlSetloc:
    case CdlSeekL:
    case CdlSeekP:
        if (param) {
            s_position = CdPosToInt((const CdlLOC *) param);
        }
        break;
    case CdlSetmode:
        if (param) {
            s_mode = *(const uint8_t *) param;
        }
        break;
    case CdlPause:
    case CdlStop:
        stream_stop();
        break;
    default:
        break;
    }
    if (result) {
        result[0] = drive_status();
    }
    return 1;
}

int CdControlB(CdlCommand cmd, const void *param, uint8_t *result)
{
    return CdControl(cmd, param, result);
}

CdlIntrResult CdSync(int mode, uint8_t *result)
{
    if (result) {
        result[0] = drive_status();
    }
    return CdlComplete;
}

CdlCB CdReadyCallback(CdlCB func)
{
    CdlCB old = s_ready_callback;
    s_ready_callback = func;
    return old;
}

void (*CdDataCallback(void (*func)(void)))(void)
{
    void (*old)(void) = s_data_callback;
    s_data_callback = func;
    return old;
}

CdlLOC *CdIntToPos(int i, CdlLOC *p)
{
    i += PREGAP_SECTORS;
    p->minute = int_to_bcd(i / (SECTORS_PER_SECOND * 60));
    p->second = int_to_bcd(i / SECTORS_PER_SECOND % 60);
    p->sector = int_to_bcd(i % SECTORS_PER_SECOND);
    return p;
}

int CdPosToInt(const CdlLOC *p)
{
    return (bcd_to_int(p->minute) * 60 + bcd_to_int(p->second)) * SECTORS_PER_SECOND
           + bcd_to_int(p->sector) - PREGAP_SECTORS;
}

/* ---- reading ---------------------------------------------------------- */

int CdRead(int sectors, uint32_t *buf, int mode)
{
    stream_stop();
    double read_start = threads_seconds();
    libetc_io_begin();
    int ok = disc_read_sectors((uint32_t) s_position, (uint32_t) sectors, buf);
    libetc_io_end();
    double read_ms = (threads_seconds() - read_start) * 1000;
    if (read_ms > SLOW_READ_MS) {
        psx_log("CdRead: %d sectors at %d took %.1f ms", sectors, s_position, read_ms);
    }
    if (!ok) {
        psx_log("CdRead: cannot read %d sectors at %d", sectors, s_position);
        return 0;
    }
    s_position += sectors;
    return 1;
}

int CdReadSync(int mode, uint8_t *result)
{
    if (result) {
        result[0] = drive_status();
    }
    return 0;               /* no sectors left: done */
}

/* Starts reading continuously (streaming, stream.c). */
int CdRead2(int mode)
{
    stream_start(s_position, mode);
    return 1;
}

CdlFILE *CdSearchFile(CdlFILE *loc, const char *filename)
{
    uint32_t lba, size;
    const char *name;

    if (lid_is_open()) {
        return NULL;
    }
    libetc_io_begin();
    int found = disc_find_file(filename, &lba, &size);
    libetc_io_end();
    if (!found) {
        return NULL;
    }
    CdIntToPos((int) lba, &loc->pos);
    loc->size = (int) size;
    /* the name without the directories */
    name = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    snprintf(loc->name, sizeof(loc->name), "%s", name);
    return loc;
}

/* (a save state: savestate.c; the disc in the drive must be the one it
 * had, which the PC side opens before the game starts) */
void libcd_state(StateIO *io)
{
    int disc = disc_is_open() ? disc_number() : 0;
    state_section(io, "CD  ");
    STATE(io, disc);
    STATE(io, s_lid);
    STATE(io, s_lid_polls);
    STATE(io, s_wanted_disc);
    STATE(io, s_position);
    STATE(io, s_mode);
    STATE(io, s_ready_callback);
    STATE(io, s_data_callback);
    if (io->loading && disc >= 1 && disc <= DISC_COUNT
        && (!disc_is_open() || disc_number() != disc) && s_disc_paths[disc - 1][0]) {
        disc_open(s_disc_paths[disc - 1]);
    }
}
