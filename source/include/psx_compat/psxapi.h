/*
 * psxapi.h - stands in for PSn00bSDK's BIOS API header in the PC port.
 *
 * PSn00bSDK's header declares BIOS file functions with the names of C
 * library functions (open, read, getc ...) that clash with the Windows C
 * library.  They are renamed while the original is included; the game only
 * uses the events, root counters and critical sections (src/psx/libapi.c).
 */
#ifndef PC_PSXAPI_H
#define PC_PSXAPI_H

#include <stdint.h>
#include <stddef.h>

/* (the C library's stdio.h has these as macros) */
#pragma push_macro("SEEK_SET")
#pragma push_macro("SEEK_CUR")
#pragma push_macro("SEEK_END")
#undef SEEK_SET
#undef SEEK_CUR
#undef SEEK_END
#define open    psx_bios_open
#define close   psx_bios_close
#define lseek   psx_bios_lseek
#define read    psx_bios_read
#define write   psx_bios_write
#define getc    psx_bios_getc
#define putc    psx_bios_putc
#define ioctl   psx_bios_ioctl
#define isatty  psx_bios_isatty
#define erase   psx_bios_erase
#define cd      psx_bios_cd
#define _get_errno psx_bios_get_errno
#define JumpBuffer void   /* (only for HookEntryInt, unused) */
#include_next <psxapi.h>
#undef open
#undef close
#undef lseek
#undef read
#undef write
#undef getc
#undef putc
#undef ioctl
#undef isatty
#undef erase
#undef cd
#undef _get_errno
#undef JumpBuffer
#pragma pop_macro("SEEK_SET")
#pragma pop_macro("SEEK_CUR")
#pragma pop_macro("SEEK_END")

#endif /* PC_PSXAPI_H */
