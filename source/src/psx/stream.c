/*
 * stream.c - continuous reading (CdRead2()) and LIBCD's streaming
 * functions (St*): the movies (STR files) and their XA sound.
 *
 * The drive reads in real time: 150 sectors a second at double speed, 75
 * at single speed, counted from when the reading started, so a movie keeps
 * the pace it has on the console.  The sectors due are read whenever the
 * game asks for a frame (StGetNext()) and at every vertical blank
 * (stream_tick()):
 *  - XA audio sectors (in real-time mode, CdlModeRT) go to the XA decoder
 *    (xa.c), as the drive plays them itself;
 *  - video sectors (32-byte STR header + 2016 bytes) are put together into
 *    whole frames.  StGetNext() hands out the oldest complete frame: its
 *    header and its data, one after the other, in the game's ring buffer.
 * Reading goes on past the end of a movie into whatever follows on the
 * disc, as on the console; the game notices by the frame numbers.
 */
#include "psx/psx.h"
#include "psx/stream.h"
#include "psx/xa.h"
#include "psx/savestate.h"
#include "disc/disc.h"
#include "platform/threads.h"


#define SECTORS_PER_SECOND_DOUBLE   150
#define SECTORS_PER_SECOND_SINGLE   75
#define MAX_CATCH_UP_SECTORS        75      /* after a pause, do not read more at once */
#define SLOW_READ_MS                8.0     /* the log notes reads this slow */
#define READ_AHEAD_SECTORS          300     /* 2 s at double speed (disc_prefetch) */

/* the subheader (bytes 16..23 of a sector) */
#define SUBHEADER_SUBMODE           18
#define SUBMODE_VIDEO               0x02
#define SUBMODE_AUDIO               0x04
#define SUBMODE_DATA                0x08
#define SECTOR_DATA                 24

/* a video sector: the STR header, then the frame's data */
#define STR_HEADER_SIZE             32
#define STR_DATA_SIZE               (DISC_SECTOR_DATA - STR_HEADER_SIZE)
#define STR_SECTOR_ID               0x0160
#define MAX_FRAME_SECTORS           32
#define FRAME_QUEUE                 3       /* complete frames waiting for the game */

#define RING_FRAME_DATA             64      /* where a frame's data starts in the ring */

/* how long StGetNext() lets the game wait before it says "not yet" */
#define WAIT_POLL_MS                1

typedef struct {
    StHeader header;
    int sectors_seen;
    uint8_t data[MAX_FRAME_SECTORS * STR_DATA_SIZE];
} Frame;

static int s_reading;                   /* CdRead2() runs */
static int s_mode;                      /* its CdlSetmode bits */
static int s_next_lba;                  /* next sector to read */
static uint32_t s_sectors_read;         /* since the start */
static double s_start;                  /* threads_seconds() when it started */
static double s_paused_at;              /* > 0 while paused */

static uint32_t *s_ring;                /* StSetRing() */
static uint32_t s_ring_bytes;
static Frame s_assembling;
static int s_assembling_active;
static Frame s_queue[FRAME_QUEUE];
static int s_queue_read, s_queue_count;
static int s_last_width, s_last_height;     /* of the frame handed out last */
static int s_audio_sectors, s_video_frames;     /* of this reading (the log) */

void stream_reset(void)
{
    s_reading = 0;
    s_paused_at = 0;
    s_ring = NULL;
    s_queue_count = 0;
    s_assembling_active = 0;
    xa_reset();
}

static double seconds_since_start(void)
{
    return threads_seconds() - s_start;
}

static int sectors_per_second(void)
{
    return s_mode & CdlModeSpeed ? SECTORS_PER_SECOND_DOUBLE : SECTORS_PER_SECOND_SINGLE;
}

static void log_end(const char *why)
{
    if (s_reading) {
        psx_log("stream: %s after %u sectors (%d XA, %d frames)", why, s_sectors_read,
                s_audio_sectors, s_video_frames);
    }
}

void stream_start(int lba, int mode)
{
    log_end("restarted");
    psx_log("stream: reading from %d (mode %02x)", lba, mode);
    s_audio_sectors = s_video_frames = 0;
    s_reading = 1;
    s_mode = mode;
    s_next_lba = lba;
    s_sectors_read = 0;
    s_queue_count = 0;
    s_assembling_active = 0;
    xa_reset();
    s_start = threads_seconds();
}

void stream_stop(void)
{
    log_end("stopped");
    if (s_reading) {
        disc_prefetch((uint32_t) s_next_lba, 0);   /* nothing more to read ahead */
    }
    s_reading = 0;
    xa_reset();
}

/* A video sector: into the frame being put together. */
static void video_sector(const uint8_t *sector)
{
    const StHeader *h = (const StHeader *) (sector + SECTOR_DATA);
    if (h->id != STR_SECTOR_ID || h->sector_count == 0 || h->sector_count > MAX_FRAME_SECTORS
        || h->sector_index >= h->sector_count) {
        return;
    }
    if (!s_assembling_active || s_assembling.header.frame_number != h->frame_number) {
        s_assembling = (Frame) { 0 };
        s_assembling.header = *h;
        s_assembling_active = 1;
    }
    memcpy(s_assembling.data + h->sector_index * STR_DATA_SIZE,
           sector + SECTOR_DATA + STR_HEADER_SIZE, STR_DATA_SIZE);
    if (++s_assembling.sectors_seen < h->sector_count) {
        return;
    }
    s_assembling_active = 0;
    if (s_queue_count == FRAME_QUEUE) {
        return;                         /* the game is behind: the frame is lost */
    }
    s_queue[(s_queue_read + s_queue_count) % FRAME_QUEUE] = s_assembling;
    s_queue_count++;
    s_video_frames++;
}

/* Reads the sectors due by now. */
static void advance(void)
{
    uint8_t sector[DISC_RAW_SECTOR];
    if (!s_reading) {
        return;
    }
    /* the sectors after these are read ahead meanwhile */
    disc_prefetch((uint32_t) s_next_lba, READ_AHEAD_SECTORS);
    uint32_t due = (uint32_t) (seconds_since_start() * sectors_per_second());
    if (due > s_sectors_read + MAX_CATCH_UP_SECTORS) {
        /* the game was paused (settings): go on from here */
        s_sectors_read = due - MAX_CATCH_UP_SECTORS;
    }
    while (s_sectors_read < due) {
        s_sectors_read++;
        double read_start = threads_seconds();
        libetc_io_begin();
        int ok = disc_read_raw((uint32_t) s_next_lba, sector);
        libetc_io_end();
        double read_ms = (threads_seconds() - read_start) * 1000;
        if (read_ms > SLOW_READ_MS) {
            psx_log("stream: slow read %.1f ms at %d", read_ms, s_next_lba);
        }
        if (!ok) {
            s_reading = 0;              /* the end of the data track */
            return;
        }
        s_next_lba++;
        uint8_t submode = sector[SUBHEADER_SUBMODE];
        if (submode & SUBMODE_AUDIO) {
            if (s_mode & CdlModeRT) {
                xa_decode_sector(sector);
                s_audio_sectors++;
            }
        } else if ((submode & (SUBMODE_VIDEO | SUBMODE_DATA)) && (s_mode & CdlModeStream)) {
            video_sector(sector);
        }
    }
}

void stream_pause(int paused)
{
    if (paused && s_paused_at == 0) {
        s_paused_at = threads_seconds();
    } else if (!paused && s_paused_at > 0) {
        s_start += threads_seconds() - s_paused_at;     /* the pause does not count */
        s_paused_at = 0;
    }
}

void stream_tick(void)
{
    advance();
}

void stream_frame_size(int *width, int *height)
{
    *width = s_last_width;
    *height = s_last_height;
}

/* ---- LIBCD's streaming functions -------------------------------------- */

void StSetRing(uint32_t *ring_addr, uint32_t ring_size)
{
    s_ring = ring_addr;
    s_ring_bytes = ring_size * DISC_SECTOR_DATA;
}

void StSetStream(int mode, uint32_t start_frame, uint32_t end_frame,
                 void (*func1)(void), void (*func2)(void))
{
    s_queue_count = 0;
    s_assembling_active = 0;
}

uint32_t StGetNext(uint32_t **addr, uint32_t **header)
{
    advance();
    if (s_queue_count == 0) {
        if (s_reading) {
            libetc_wait(WAIT_POLL_MS);  /* the next sectors are on their way */
        }
        return 1;
    }
    Frame *f = &s_queue[s_queue_read];
    uint32_t bytes = f->header.sector_count * STR_DATA_SIZE;
    if (!s_ring || RING_FRAME_DATA + bytes > s_ring_bytes) {
        psx_log("StGetNext: no ring buffer or the frame does not fit");
        s_queue_read = (s_queue_read + 1) % FRAME_QUEUE;
        s_queue_count--;
        return 1;
    }
    uint8_t *ring = (uint8_t *) s_ring;
    memcpy(ring, &f->header, sizeof(StHeader));
    memcpy(ring + RING_FRAME_DATA, f->data, bytes);
    s_queue_read = (s_queue_read + 1) % FRAME_QUEUE;
    s_queue_count--;
    s_last_width = f->header.width;
    s_last_height = f->header.height;
    *header = (uint32_t *) ring;
    *addr = (uint32_t *) (ring + RING_FRAME_DATA);
    return 0;
}

uint32_t StFreeRing(uint32_t *base)
{
    return 0;
}

void StCdInterrupt(void)
{
}

uint32_t StCdIntrFlag;

/* (a save state: savestate.c; the drive's clock is kept as the time read
 * so far, which goes on from the moment the state is loaded) */
void stream_state(StateIO *io)
{
    double now = threads_seconds();
    double elapsed = (s_paused_at > 0 ? s_paused_at : now) - s_start;
    state_section(io, "CDST");
    STATE(io, s_reading);
    STATE(io, s_mode);
    STATE(io, s_next_lba);
    STATE(io, s_sectors_read);
    STATE(io, elapsed);
    STATE(io, s_ring);
    STATE(io, s_ring_bytes);
    STATE(io, s_assembling);
    STATE(io, s_assembling_active);
    STATE(io, s_queue);
    STATE(io, s_queue_read);
    STATE(io, s_queue_count);
    STATE(io, s_last_width);
    STATE(io, s_last_height);
    STATE(io, s_audio_sectors);
    STATE(io, s_video_frames);
    STATE(io, StCdIntrFlag);
    if (io->loading) {
        s_start = now - elapsed;
        if (s_paused_at > 0) {
            s_paused_at = now;
        }
        if (s_reading) {
            disc_prefetch((uint32_t) s_next_lba, READ_AHEAD_SECTORS);
        }
    }
}
