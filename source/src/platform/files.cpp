/*
 * files.cpp - whole-file reading and writing with UTF-8 paths.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
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

#define FNV_OFFSET              0xCBF29CE484222325ull
#define FNV_PRIME               0x100000001B3ull

static uint64_t fnv1a(uint64_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * FNV_PRIME;
    }
    return hash;
}

/*
 * FNV-1a over what game.dll loads - its code and data sections, with their
 * addresses - and the address it was loaded at.  Not over the whole file:
 * its debug information names the folder it was built in, and its build id
 * changes with every link, while the game is the same.
 */
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
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 0 ? (uint8_t *) malloc((size_t) size) : NULL;
    int ok = file && fread(file, 1, (size_t) size, f) == (size_t) size;
    fclose(f);
    uint64_t hash = FNV_OFFSET ^ (uint64_t) (uintptr_t) game;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *) file;
    if (ok && size > (long) sizeof(*dos) && dos->e_magic == IMAGE_DOS_SIGNATURE
        && dos->e_lfanew > 0 && dos->e_lfanew + (long) sizeof(IMAGE_NT_HEADERS32) <= size) {
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *) (file + dos->e_lfanew);
        const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            if ((const uint8_t *) (section + 1) > file + size
                || (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
                || strncmp((const char *) section->Name, ".buildid", IMAGE_SIZEOF_SHORT_NAME) == 0) {
                continue;
            }
            DWORD length = section->Misc.VirtualSize < section->SizeOfRawData
                               ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (section->PointerToRawData + (uint64_t) length > (uint64_t) size) {
                continue;
            }
            DWORD address = section->VirtualAddress;
            hash = fnv1a(hash, (const uint8_t *) &address, sizeof(address));
            hash = fnv1a(hash, file + section->PointerToRawData, length);
        }
    }
    free(file);
    s_id = hash;
    return s_id;
}
