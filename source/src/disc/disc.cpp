/*
 * disc.cpp - reading the player's disc images: raw 2352-byte sectors
 * (.bin, also through a .cue) and compressed MAME CHD images (libchdr);
 * ISO9660 directories; the check that it is a disc of D (USA).
 *
 * Only the data track (track 1) is used.  In a .bin with all tracks (one
 * FILE in the .cue) it ends where the .cue puts track 2; in a CHD it is
 * the first track's frames.
 *
 * Reading ahead: a movie is read sector by sector in real time, and the
 * first read of a part of the image can be slow (a CHD hunk to unpack, a
 * file Windows has not cached yet).  disc_prefetch() names the sectors
 * coming next; a thread of its own reads them into a small cache, so the
 * game rarely waits for the disk.  One lock guards the image and the cache.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disc/disc.h"
#include "disc/sha1.h"
#include "libchdr/chd.h"
#include "libchdr/cdrom.h"

#define PVD_LBA                 16
#define USER_DATA_OFFSET        24      /* Mode 2: sync 12 + header 4 + subheader 8 */
#define MODE_BYTE               15
#define MODE_2                  2
#define FRAMES_PER_SECOND       75      /* CD sectors per second (MSF) */

/* the three discs: the executable in SYSTEM.CNF and the SHA1 of the data
 * track's 2352-byte sectors (Redump dump; the same for a CHD made from it) */
static const struct {
    const char *exe;
    uint32_t sectors;
    const char *sha1;
} KNOWN_DISCS[DISC_COUNT] = {
    { "SLUS_001.28", 284874, "cd3a0bed796ebed8206aa430f61b42baf56b76ca" },
    { "SLUS_001.73", 303341, "f31fee0a8ef628904c531007cb1b39abfbcd46b4" },
    { "SLUS_001.74", 183025, "b4dbd1ae384fe844ca7d2452c08ea7645d0647cb" },
};

enum ImageKind {
    KIND_NONE,
    KIND_RAW,                       /* .bin: 2352-byte sectors */
    KIND_CHD                        /* CHD: frames of 2352 + 96 bytes of subcode */
};

/* the lock around the image and the cache (two threads read) */
static CRITICAL_SECTION s_lock;
static struct LockInit {
    LockInit() { InitializeCriticalSection(&s_lock); }
} s_lock_init;

/* the read-ahead cache: whole sectors, at lba % CACHE_SECTORS */
#define CACHE_SECTORS           512     /* 1.2 MB, over 3 s of a movie */
typedef struct {
    uint32_t lba;
    int valid;
    uint8_t data[DISC_RAW_SECTOR];
} CachedSector;
static CachedSector s_cache[CACHE_SECTORS];
static HANDLE s_prefetch_thread, s_prefetch_wake;
static volatile LONG s_prefetch_next, s_prefetch_end;  /* the sectors still to read ahead */

static int s_kind = KIND_NONE;
static uint32_t s_sectors;              /* of the data track */
static int s_number;                    /* 1..3, 0 = unknown */

/* .bin */
static HANDLE s_file = INVALID_HANDLE_VALUE;

/* .chd */
static FILE *s_chd_stdio;
static chd_file *s_chd;
static uint8_t *s_hunk;                 /* the last hunk read */
static uint32_t s_hunk_number;
static uint32_t s_frames_per_hunk;

static void utf8_to_wide(const char *in, wchar_t *out, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size);
}

static HANDLE open_utf8(const char *path)
{
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    return CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static int read_at(uint64_t offset, void *out, uint32_t size)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG) offset;
    return SetFilePointerEx(s_file, pos, NULL, FILE_BEGIN)
           && ReadFile(s_file, out, size, &got, NULL) && got == size;
}

static int has_extension(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && _stricmp(path + n - e, ext) == 0;
}

/*
 * A .cue: the image is the first FILE "name" BINARY next to it.  If track 2
 * is in the same file, the data track ends where track 2 starts (its
 * INDEX 00, or INDEX 01 without a pregap): *data_sectors, 0 = whole file.
 */
static int parse_cue(const char *cue, char *bin, int size, uint32_t *data_sectors)
{
    HANDLE f = open_utf8(cue);
    char text[8192];
    DWORD got = 0;
    *data_sectors = 0;
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ReadFile(f, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(f);
    text[got] = '\0';
    const char *p = strstr(text, "FILE \"");
    if (!p) {
        return 0;
    }
    p += 6;
    const char *end = strchr(p, '"');
    if (!end) {
        return 0;
    }
    const char *slash = strrchr(cue, '\\');
    const char *slash2 = strrchr(cue, '/');
    if (!slash || (slash2 && slash2 > slash)) {
        slash = slash2;
    }
    int dir_len = slash ? (int) (slash - cue + 1) : 0;
    int name_len = (int) (end - p);
    if (dir_len + name_len + 1 > size) {
        return 0;
    }
    memcpy(bin, cue, dir_len);
    memcpy(bin + dir_len, p, name_len);
    bin[dir_len + name_len] = '\0';

    const char *next_file = strstr(end, "FILE \"");
    const char *track2 = strstr(end, "TRACK 02");
    if (track2 && (!next_file || track2 < next_file)) {
        int m, s, fr;
        const char *index = strstr(track2, "INDEX 00");
        if (!index || (next_file && index > next_file)) {
            index = strstr(track2, "INDEX 01");
        }
        if (index && sscanf(index + 8, "%d:%d:%d", &m, &s, &fr) == 3) {
            *data_sectors = (uint32_t) ((m * 60 + s) * FRAMES_PER_SECOND + fr);
        }
    }
    return 1;
}

static int open_raw(const char *path, uint32_t data_sectors)
{
    LARGE_INTEGER size;
    uint8_t sector[DISC_RAW_SECTOR];
    static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

    s_file = open_utf8(path);
    if (s_file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    GetFileSizeEx(s_file, &size);
    if (size.QuadPart % DISC_RAW_SECTOR != 0 || size.QuadPart < (PVD_LBA + 1) * DISC_RAW_SECTOR
        || !read_at((uint64_t) PVD_LBA * DISC_RAW_SECTOR, sector, DISC_RAW_SECTOR)
        || memcmp(sector, SYNC, sizeof(SYNC)) != 0 || sector[MODE_BYTE] != MODE_2) {
        return 0;
    }
    s_kind = KIND_RAW;
    s_sectors = (uint32_t) (size.QuadPart / DISC_RAW_SECTOR);
    if (data_sectors && data_sectors < s_sectors) {
        s_sectors = data_sectors;
    }
    return 1;
}

/* First track of a CD CHD: it must keep the complete sectors (MODE2_RAW). */
static int open_chd(const char *path)
{
    wchar_t wpath[MAX_PATH];
    char meta[256];
    uint32_t len;
    int track = 0, frames = 0;
    char type[32] = "";

    utf8_to_wide(path, wpath, MAX_PATH);
    s_chd_stdio = _wfopen(wpath, L"rb");
    if (!s_chd_stdio || chd_open_file(s_chd_stdio, CHD_OPEN_READ, NULL, &s_chd) != CHDERR_NONE) {
        return 0;
    }
    const chd_header *header = chd_get_header(s_chd);
    if (header->hunkbytes % CD_FRAME_SIZE != 0) {
        return 0;                       /* not a CD image */
    }
    if (chd_get_metadata(s_chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, NULL,
                         NULL) == CHDERR_NONE
        || chd_get_metadata(s_chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &len,
                            NULL, NULL) == CHDERR_NONE) {
        meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = '\0';
        sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%*s FRAMES:%d", &track, type, &frames);
    }
    if (strcmp(type, "MODE2_RAW") != 0) {
        return 0;                       /* without the raw sectors there are no XA sectors */
    }
    s_frames_per_hunk = header->hunkbytes / CD_FRAME_SIZE;
    s_sectors = frames > 0 ? (uint32_t) frames : (uint32_t) (header->logicalbytes / CD_FRAME_SIZE);
    s_hunk = (uint8_t *) malloc(header->hunkbytes);
    s_hunk_number = 0xFFFFFFFF;
    s_kind = KIND_CHD;
    return s_hunk != NULL;
}

static int read_number(void);

static int open_locked(const char *path);
static void close_locked(void);

int disc_open(const char *path)
{
    EnterCriticalSection(&s_lock);
    int ok = open_locked(path);
    LeaveCriticalSection(&s_lock);
    return ok;
}

void disc_close(void)
{
    EnterCriticalSection(&s_lock);
    close_locked();
    LeaveCriticalSection(&s_lock);
}

static int open_locked(const char *path)
{
    char bin[MAX_PATH * 3];
    uint32_t data_sectors = 0;
    close_locked();
    if (has_extension(path, ".cue")) {
        if (!parse_cue(path, bin, sizeof(bin), &data_sectors)) {
            return 0;
        }
        path = bin;
    }
    if (has_extension(path, ".chd") ? open_chd(path) : open_raw(path, data_sectors)) {
        s_number = read_number();
        return 1;
    }
    close_locked();
    return 0;
}

static void close_locked(void)
{
    InterlockedExchange(&s_prefetch_end, s_prefetch_next);     /* nothing to read ahead */
    for (int i = 0; i < CACHE_SECTORS; i++) {
        s_cache[i].valid = 0;
    }
    if (s_file != INVALID_HANDLE_VALUE) {
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
    }
    if (s_chd) {
        chd_close(s_chd);
        s_chd = NULL;
    }
    if (s_chd_stdio) {
        fclose(s_chd_stdio);
        s_chd_stdio = NULL;
    }
    free(s_hunk);
    s_hunk = NULL;
    s_kind = KIND_NONE;
    s_sectors = 0;
    s_number = 0;
}

int disc_is_open(void)
{
    return s_kind != KIND_NONE;
}

uint32_t disc_sector_count(void)
{
    return s_sectors;
}

int disc_number(void)
{
    return s_number;
}

/* The CHD frame (2352 bytes + subcode) of a sector. */
static const uint8_t *chd_frame(uint32_t lba)
{
    uint32_t hunk = lba / s_frames_per_hunk;
    if (hunk != s_hunk_number) {
        if (chd_read(s_chd, hunk, s_hunk) != CHDERR_NONE) {
            s_hunk_number = 0xFFFFFFFF;
            return NULL;
        }
        s_hunk_number = hunk;
    }
    return s_hunk + (lba % s_frames_per_hunk) * CD_FRAME_SIZE;
}

static int read_raw_locked(uint32_t lba, uint8_t out[DISC_RAW_SECTOR]);

int disc_read_raw(uint32_t lba, uint8_t out[DISC_RAW_SECTOR])
{
    EnterCriticalSection(&s_lock);
    CachedSector *c = &s_cache[lba % CACHE_SECTORS];
    int ok;
    if (c->valid && c->lba == lba) {
        memcpy(out, c->data, DISC_RAW_SECTOR);
        ok = 1;
    } else {
        ok = read_raw_locked(lba, out);
    }
    LeaveCriticalSection(&s_lock);
    return ok;
}

/* Reads ahead whatever disc_prefetch() asked for, one sector at a time
 * (so the game's own reads get the lock in between). */
static DWORD WINAPI prefetch_thread(void *)
{
    for (;;) {
        WaitForSingleObject(s_prefetch_wake, INFINITE);
        for (;;) {
            LONG lba = s_prefetch_next;
            if (lba >= s_prefetch_end) {
                break;
            }
            EnterCriticalSection(&s_lock);
            CachedSector *c = &s_cache[(uint32_t) lba % CACHE_SECTORS];
            if (s_kind != KIND_NONE && !(c->valid && c->lba == (uint32_t) lba)) {
                c->valid = read_raw_locked((uint32_t) lba, c->data);
                c->lba = (uint32_t) lba;
            }
            LeaveCriticalSection(&s_lock);
            InterlockedCompareExchange(&s_prefetch_next, lba + 1, lba);
        }
    }
    return 0;
}

void disc_prefetch(uint32_t lba, uint32_t count)
{
    if (count > CACHE_SECTORS - 1) {
        count = CACHE_SECTORS - 1;
    }
    if (!s_prefetch_thread) {
        s_prefetch_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_prefetch_thread = CreateThread(NULL, 0, prefetch_thread, NULL, 0, NULL);
        if (!s_prefetch_thread) {
            return;
        }
        SetThreadPriority(s_prefetch_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    InterlockedExchange(&s_prefetch_end, (LONG) (lba + count));
    InterlockedExchange(&s_prefetch_next, (LONG) lba);
    SetEvent(s_prefetch_wake);
}

static int read_raw_locked(uint32_t lba, uint8_t out[DISC_RAW_SECTOR])
{
    if (s_kind == KIND_NONE || lba >= s_sectors) {
        return 0;
    }
    if (s_kind == KIND_CHD) {
        const uint8_t *frame = chd_frame(lba);
        if (!frame) {
            return 0;
        }
        memcpy(out, frame, DISC_RAW_SECTOR);
        return 1;
    }
    return read_at((uint64_t) lba * DISC_RAW_SECTOR, out, DISC_RAW_SECTOR);
}

int disc_read_sectors(uint32_t lba, uint32_t count, void *out)
{
    uint8_t *dst = (uint8_t *) out;
    uint8_t raw[DISC_RAW_SECTOR];

    if (s_kind == KIND_NONE || lba + count > s_sectors) {
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!disc_read_raw(lba + i, raw)) {
            return 0;
        }
        memcpy(dst + i * DISC_SECTOR_DATA, raw + USER_DATA_OFFSET, DISC_SECTOR_DATA);
    }
    return 1;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* Compares a path component with an ISO name ("NAME.EXT;1"). */
static int name_matches(const char *want, int want_len, const uint8_t *name, int name_len)
{
    int i;
    for (i = 0; i < want_len; i++) {
        if (i >= name_len || toupper((unsigned char) want[i]) != toupper(name[i])) {
            return 0;
        }
    }
    return i == name_len || name[i] == ';';
}

/* Looks for `want` in the directory at lba/size. */
static int find_in_dir(uint32_t lba, uint32_t size, const char *want, int want_len,
                       uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
    uint8_t sector[DISC_SECTOR_DATA];
    for (uint32_t done = 0; done < size; done += DISC_SECTOR_DATA, lba++) {
        if (!disc_read_sectors(lba, 1, sector)) {
            return 0;
        }
        for (uint32_t pos = 0; pos < DISC_SECTOR_DATA;) {
            const uint8_t *rec = sector + pos;
            if (rec[0] == 0) {
                break;                  /* rest of the sector is unused */
            }
            if (name_matches(want, want_len, rec + 33, rec[32])) {
                *out_lba = le32(rec + 2);
                *out_size = le32(rec + 10);
                *is_dir = (rec[25] & 2) != 0;
                return 1;
            }
            pos += rec[0];
        }
    }
    return 0;
}

int disc_find_file(const char *path, uint32_t *lba, uint32_t *size)
{
    uint8_t pvd[DISC_SECTOR_DATA];
    uint32_t dir_lba, dir_size;
    int is_dir = 1;

    if (!disc_read_sectors(PVD_LBA, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
        return 0;
    }
    dir_lba = le32(pvd + 156 + 2);
    dir_size = le32(pvd + 156 + 10);
    while (*path == '\\' || *path == '/') {
        path++;
    }
    while (*path) {
        const char *end = path;
        while (*end && *end != '\\' && *end != '/' && *end != ';') {
            end++;
        }
        if (!is_dir || !find_in_dir(dir_lba, dir_size, path, (int) (end - path), &dir_lba,
                                    &dir_size, &is_dir)) {
            return 0;
        }
        if (*end == ';') {
            while (*end && *end != '\\' && *end != '/') {
                end++;
            }
        }
        path = *end ? end + 1 : end;
    }
    *lba = dir_lba;
    *size = dir_size;
    return !is_dir;
}

/* SYSTEM.CNF of the open image, as a string; FALSE if there is none. */
static int read_system_cnf(char cnf[DISC_SECTOR_DATA + 1])
{
    uint32_t lba, size;
    if (!disc_find_file("SYSTEM.CNF;1", &lba, &size) || !disc_read_sectors(lba, 1, cnf)) {
        return 0;
    }
    cnf[DISC_SECTOR_DATA] = '\0';
    return 1;
}

int disc_boot_file(char *name, int size)
{
    char cnf[DISC_SECTOR_DATA + 1];
    if (!read_system_cnf(cnf)) {
        return 0;
    }
    const char *p = strstr(cnf, "cdrom:");
    if (!p) {
        return 0;
    }
    p += 6;
    while (*p == '\\' || *p == '/') {
        p++;
    }
    int n = 0;
    while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && n < size - 1) {
        name[n] = p[n];
        n++;
    }
    name[n] = '\0';
    return n > 0;
}

static int read_number(void)
{
    char cnf[DISC_SECTOR_DATA + 1];
    if (!read_system_cnf(cnf)) {
        return 0;
    }
    for (int i = 0; i < DISC_COUNT; i++) {
        if (strstr(cnf, KNOWN_DISCS[i].exe)) {
            return i + 1;
        }
    }
    return 0;
}

static volatile LONG s_progress;        /* disc_check_progress() */
#define PROGRESS_FULL           1000
#define PROGRESS_EVERY          1024    /* sectors */

int disc_check_progress(void)
{
    return (int) s_progress;
}

/* SHA1 of the data track's 2352-byte sectors. */
static int track_sha1(char hex[41])
{
    Sha1 ctx;
    uint8_t digest[20];
    uint8_t raw[DISC_RAW_SECTOR];

    sha1_init(&ctx);
    for (uint32_t lba = 0; lba < s_sectors; lba++) {
        if (!disc_read_raw(lba, raw)) {
            return 0;
        }
        sha1_update(&ctx, raw, DISC_RAW_SECTOR);
        if (lba % PROGRESS_EVERY == 0) {
            InterlockedExchange(&s_progress, (LONG) ((uint64_t) lba * PROGRESS_FULL / s_sectors));
        }
    }
    sha1_final(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    return 1;
}

int disc_check(const char *path, int verify, int *number, char *detail, int detail_size)
{
    char hex[41];

    *number = 0;
    InterlockedExchange(&s_progress, 0);
    if (!path || !*path) {
        snprintf(detail, detail_size, "No disc chosen.");
        return DISC_NONE;
    }
    if (!disc_open(path)) {
        snprintf(detail, detail_size,
                 "The file cannot be read as a PS1 disc with complete sectors "
                 "(.bin/.cue or .chd; a .iso cannot hold the movies).");
        return DISC_UNREADABLE;
    }
    if (s_number == 0) {
        snprintf(detail, detail_size, "This is not a disc of D (USA).");
        return DISC_WRONG_GAME;
    }
    *number = s_number;
    if (!verify) {
        snprintf(detail, detail_size, "D (USA) disc %d.", s_number);
        return DISC_OTHER_DUMP;
    }
    if (s_sectors == KNOWN_DISCS[s_number - 1].sectors && track_sha1(hex)
        && strcmp(hex, KNOWN_DISCS[s_number - 1].sha1) == 0) {
        snprintf(detail, detail_size, "D (USA) disc %d – verified%s.", s_number,
                 s_kind == KIND_CHD ? " (CHD)" : "");
        return DISC_OK;
    }
    snprintf(detail, detail_size,
             "D (USA) disc %d, but a different copy than the known one (the SHA1 "
             "differs). It should work.", s_number);
    return DISC_OTHER_DUMP;
}
