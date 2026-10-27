/*
 * files.h - whole-file reading and writing with UTF-8 paths (for the PS1
 * library replacements, which cannot include windows.h).
 */
#ifndef PLATFORM_FILES_H
#define PLATFORM_FILES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads up to `size` bytes; returns the number read, -1 if the file
 * cannot be opened. */
int file_read(const char *path, void *buffer, int size);

/* Writes the whole file (through a temporary file, creating missing
 * folders); FALSE on failure. */
int file_write(const char *path, const void *data, int size);

/* A checksum of game.dll's code and data and where it was loaded: the
 * same as long as the game's code is (save states hold its addresses),
 * wherever it was built. */
uint64_t file_game_build_id(void);

/* Deletes the file; FALSE if it could not be deleted. */
int file_delete(const char *path);

/* A path relative to the folder of the exe (absolute paths unchanged). */
void file_exe_relative(const char *path, char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_FILES_H */
