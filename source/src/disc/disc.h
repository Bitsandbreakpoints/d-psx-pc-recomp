/*
 * disc.h - the player's own disc images (.bin/.cue or .chd) and their
 * ISO9660 file system.  Nothing of the game is shipped with the port: all
 * data is read from here.
 *
 * D comes on three discs; one of them is open at a time (the one in the
 * virtual drive, src/psx/libcd.c).  Only the data track is read: the game
 * never plays the CD audio tracks.  The movies and the music are XA
 * (Mode 2 Form 2) sectors, so the image must keep the complete 2352-byte
 * sectors: a .iso (2048-byte sectors) cannot hold them.
 */
#ifndef DISC_DISC_H
#define DISC_DISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISC_SECTOR_DATA        2048    /* user data of a Mode 2 Form 1 sector */
#define DISC_RAW_SECTOR         2352    /* a complete sector */
#define DISC_COUNT              3

/* disc_check() result */
enum DiscStatus {
    DISC_NONE = 0,                  /* nothing chosen */
    DISC_UNREADABLE,                /* not a disc image we can read */
    DISC_WRONG_GAME,                /* a PS1 disc, but not D (USA) */
    DISC_OTHER_DUMP,                /* D (USA), but not the known dump */
    DISC_OK                         /* the known dump (SHA1 matches) */
};

/* Opens a .bin/.cue/.chd; FALSE if it cannot be read. */
int disc_open(const char *path);
void disc_close(void);
int disc_is_open(void);

/* Reads `count` sectors of user data (Form 1) starting at `lba`. */
int disc_read_sectors(uint32_t lba, uint32_t count, void *out);

/* Reads one complete 2352-byte sector (sync, header, subheader, data). */
int disc_read_raw(uint32_t lba, uint8_t out[DISC_RAW_SECTOR]);

/* The `count` sectors from `lba` are needed soon (a movie streams): they
 * are read ahead on a thread of its own into a cache, so the reads that
 * follow do not wait for the disk. */
void disc_prefetch(uint32_t lba, uint32_t count);

/* Finds a file ("\\MOVIE\\WARP.STR;1", case and ";1" optional). */
int disc_find_file(const char *path, uint32_t *lba, uint32_t *size);

/* Sectors of the data track of the opened image. */
uint32_t disc_sector_count(void);

/* Which disc of D the opened image is (1..3, from SYSTEM.CNF); 0 if none. */
int disc_number(void);

/* The name of the executable in SYSTEM.CNF ("SLUS_001.28"); FALSE if none. */
int disc_boot_file(char *name, int size);

/*
 * Opens the image and checks that it is a disc of D (USA); *number gets
 * the disc (1..3).  With `verify` the data track's SHA1 is compared with
 * the known dump (takes a few seconds).  `detail` gets a short explanation
 * (UTF-8).  The image stays open.
 */
int disc_check(const char *path, int verify, int *number, char *detail, int detail_size);

/* How far the running disc_check() has come with the checksum, 0..1000
 * (it may run on another thread: the launcher shows it). */
int disc_check_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* DISC_DISC_H */
