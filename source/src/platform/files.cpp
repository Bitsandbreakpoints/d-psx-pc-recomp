/*
 * files.cpp - whole-file reading and writing with UTF-8 paths.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "platform/files.h"

static void to_wide(const char *in, wchar_t *out, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size);
}

int file_read(const char *path, void *buffer, int size)
{
    wchar_t wpath[MAX_PATH];
    to_wide(path, wpath, MAX_PATH);
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) {
        return -1;
    }
    int n = (int) fread(buffer, 1, (size_t) size, f);
    fclose(f);
    return n;
}

/* creates the folders above `wpath` */
static void make_folders(wchar_t *wpath)
{
    for (wchar_t *p = wpath; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p > wpath && p[-1] != L':') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(wpath, NULL);
            *p = c;
        }
    }
}

int file_write(const char *path, const void *data, int size)
{
    wchar_t wpath[MAX_PATH], wtemp[MAX_PATH + 8];
    to_wide(path, wpath, MAX_PATH);
    make_folders(wpath);
    swprintf(wtemp, MAX_PATH + 8, L"%ls.tmp", wpath);
    FILE *f = _wfopen(wtemp, L"wb");
    if (!f) {
        return 0;
    }
    int ok = fwrite(data, 1, (size_t) size, f) == (size_t) size;
    ok &= fclose(f) == 0;
    if (!ok) {
        DeleteFileW(wtemp);
        return 0;
    }
    return MoveFileExW(wtemp, wpath, MOVEFILE_REPLACE_EXISTING) != 0;
}

int file_delete(const char *path)
{
    wchar_t wpath[MAX_PATH];
    to_wide(path, wpath, MAX_PATH);
    return DeleteFileW(wpath) != 0;
}

void file_exe_relative(const char *path, char *out, int size)
{
    int absolute = (path[0] && path[1] == ':') || path[0] == '\\' || path[0] == '/';
    if (absolute) {
        snprintf(out, (size_t) size, "%s", path);
        return;
    }
    wchar_t exe[MAX_PATH];
    char dir[MAX_PATH * 3];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (n > 0 && exe[n - 1] != L'\\' && exe[n - 1] != L'/') {
        n--;
    }
    exe[n] = 0;
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, dir, sizeof(dir), NULL, NULL);
    snprintf(out, (size_t) size, "%s%s", dir, path);
}

/* FNV-1a over the file (read once), with the address it was loaded at */
uint64_t file_game_build_id(void)
{
    static uint64_t s_id;
    if (s_id) {
        return s_id;
    }
    HMODULE game = GetModuleHandleW(L"game.dll");
    wchar_t path[MAX_PATH];
    if (!game || !GetModuleFileNameW(game, path, MAX_PATH)) {
        return 0;
    }
    FILE *f = _wfopen(path, L"rb");
    uint64_t hash = 0xCBF29CE484222325ull ^ (uint64_t) (uintptr_t) game;
    if (f) {
        static uint8_t buffer[64 * 1024];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            for (size_t i = 0; i < n; i++) {
                hash = (hash ^ buffer[i]) * 0x100000001B3ull;
            }
        }
        fclose(f);
    }
    s_id = hash;
    return s_id;
}
