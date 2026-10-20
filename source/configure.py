#!/usr/bin/env python3
"""Writes build.ninja for the PC port of D (32-bit Windows, llvm-mingw).

    python configure.py [--debug]

The game itself is the decompiled code of the project (../src), compiled
for x86 with PC_PORT into build/game.dll at a fixed address (GAME_DLL_BASE),
apart from the rest of the port (build/d.exe): a save state holds addresses
of the game's code and data, so it fits every build of d.exe as long as
game.dll stays the same (src/psx/savestate.c).  The game calls the PS1
library replacements in d.exe through an import library made from a list
of them (tools/exe_exports.py writes build/gen/d.def, which d.exe exports);
d.exe calls the game's main() (game/game.def).  Its uninitialised variables (.bss) are not C
definitions in the project: the PS1 build reserves them in assembly
(../asm/data/bss.bss.s).  The PC port puts them at their original
addresses in the PS1 memory (0x80000000), so this script turns that list
(and the linker aliases of ../config/undefined_syms.txt) into absolute
symbols: build/gen/bss_symbols.c.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(HERE, 'tools')
BIN = os.path.join(TOOLS, 'llvm-mingw', 'bin')
CC = os.path.join(BIN, 'i686-w64-mingw32-clang.exe')
CXX = os.path.join(BIN, 'i686-w64-mingw32-clang++.exe')
WINDRES = os.path.join(BIN, 'i686-w64-mingw32-windres.exe')

OUT = 'build/d.exe'
GAME_DLL = 'build/game.dll'
# (above the exe and its DLLs, below the PS1 memory at 0x80000000; no
#  address randomisation, so it loads there unless something else has
#  taken the place - then save states are refused, savestate.c)
GAME_DLL_BASE = 0x60000000
GAME_DEF = 'game/game.def'              # what game.dll exports
GAME_IMPLIB = 'build/gen/libgame.a'
EXE_DEF = 'build/gen/d.def'             # what d.exe exports to game.dll
EXE_IMPLIB = 'build/gen/libd.a'
DLLTOOL = os.path.join(BIN, 'llvm-dlltool.exe')
SOURCE_DIRS = ['src']
# Dear ImGui (MIT): core + Win32/D3D11 backends
EXTRA_SOURCES = ['third_party/imgui/' + f for f in (
    'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui_demo.cpp',
    'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx11.cpp')]
# libchdr (BSD-3) with zstd, LZMA, miniz, FLAC: one unity file, own flags
CHDR_DIR = 'third_party/libchdr'
# (the upstream unity.c leaves out the A/V codec that libchdr_chd.c refers to)
CHDR_SOURCES = [CHDR_DIR + '/unity.c', CHDR_DIR + '/src/libchdr_codec_avhuff.c']
# (MINIZ_DEFLATE_APIS: miniz's compressor too, for the save states)
CHDR_CFLAGS = ('-O2 -w -std=gnu11 -DMINIZ_DEFLATE_APIS ' + ' '.join('-I' + CHDR_DIR + '/' + d for d in (
    'include', 'deps/lzma-26.02/include', 'deps/miniz-3.1.2', 'deps/zstd-1.5.7')))

# headers for the game code and the PS1 library replacements (src/psx):
# ours first (include/psx_compat), then the project's, then PSn00bSDK's
# (-mno-ms-bitfields: bit-fields laid out as by GCC on the PS1)
PSX_INCLUDES = ('-mno-ms-bitfields -Iinclude/psx_compat -I../include -Isrc '
                '-Wno-incompatible-library-redeclaration '
                '-idirafter ../tools/psn00bsdk/include/libpsn00b')
# (-fno-zero-initialized-in-bss: all variables in the data section that
#  src/psx/game_data.c resets, see pc_game.h)
# (-fcommon: the original's tentative definitions (int x;) of variables
#  that live in .bss resolve to the absolute symbols of bss_symbols.c)
# (-msse2 -mfpmath=sse: float/double rounded as IEEE single/double)
GAME_FLAGS = ('-w -fwrapv -fno-strict-aliasing -fno-zero-initialized-in-bss -fcommon '
              '-msse2 -mfpmath=sse -DPC_PORT ' + PSX_INCLUDES + ' -include pc_game.h')
# files that resources.rc builds into the exe
RESOURCE_FILES = ['src/resources.h']
LIBS = ['d3d11', 'dxgi', 'd3dcompiler_47', 'xinput', 'ole32', 'user32', 'gdi32', 'shell32',
        'comdlg32', 'winmm', 'dwmapi']

BSS_ASM = os.path.join(ROOT, 'asm', 'data', 'bss.bss.s')
ALIASES = os.path.join(ROOT, 'config', 'undefined_syms.txt')
GEN_BSS = 'build/gen/bss_symbols.c'


def rel(path):
    return os.path.relpath(path, HERE).replace('\\', '/')


def bss_symbols():
    """[(name, address)] of the game's .bss and of the linker aliases."""
    if not os.path.exists(BSS_ASM):
        sys.exit(f'{BSS_ASM} is missing: run "build.py split" in the project first')
    symbols = []
    label = None
    for line in open(BSS_ASM):
        m = re.match(r'\s*dlabel\s+(\w+)', line)
        if m:
            label = m.group(1)
            continue
        m = re.match(r'\s*/\*\s*([0-9A-Fa-f]{8})\s*\*/', line)
        if m and label:
            symbols.append((label, int(m.group(1), 16)))
            label = None
    addresses = dict(symbols)
    for line in open(ALIASES):
        m = re.match(r'\s*(\w+)\s*=\s*(\w+)\s*(?:\+\s*(0x[0-9A-Fa-f]+|\d+))?\s*;', line)
        if m:
            base = addresses[m.group(2)]
            symbols.append((m.group(1), base + (int(m.group(3), 0) if m.group(3) else 0)))
    return symbols


def write_bss_symbols():
    lines = ['/* GENERATED by configure.py from ../asm/data/bss.bss.s and',
             ' * ../config/undefined_syms.txt: the game\'s uninitialised variables at',
             ' * their original addresses in the PS1 memory (i686: C names get a "_"). */',
             '__asm__(']
    for name, address in bss_symbols():
        lines.append(f'    ".globl _{name}\\n\\t.set _{name}, 0x{address:08X}\\n"')
    lines.append(');')
    os.makedirs(os.path.join(HERE, 'build', 'gen'), exist_ok=True)
    path = os.path.join(HERE, GEN_BSS)
    text = '\n'.join(lines) + '\n'
    if not os.path.exists(path) or open(path).read() != text:
        open(path, 'w', newline='\n').write(text)


def main():
    debug = '--debug' in sys.argv
    # (frame pointers: the crash handler in main_pc.cpp can walk the stack)
    opt = '-O0 -g -fno-omit-frame-pointer' if debug else '-O2 -g -fno-omit-frame-pointer'
    common = (f'{opt} -Wall -Wno-unused-function -Iinclude -Isrc -Ithird_party/imgui '
              '-Ithird_party/libchdr/include -Ithird_party/libchdr/deps/miniz-3.1.2 -Ithird_party/stb -DWIN32_LEAN_AND_MEAN -DNOMINMAX '
              '-DUNICODE -D_UNICODE')
    sources = []
    for d in SOURCE_DIRS:
        for ext in ('c', 'cpp'):
            sources += glob.glob(os.path.join(HERE, d, '**', '*.' + ext), recursive=True)
    sources.sort()
    sources += [os.path.join(HERE, p) for p in EXTRA_SOURCES]
    write_bss_symbols()

    lines = [
        '# GENERATED by configure.py',
        f'cflags = {common} -std=gnu11',
        f'cxxflags = {common} -std=gnu++17 -fno-exceptions -fno-rtti',
        'ldflags = -static -municode -mwindows -Wl,--large-address-aware -Wl,--disable-dynamicbase',
        f'libs = {" ".join("-l" + l for l in LIBS)}',
        '',
        'rule cc',
        f'  command = "{CC}" $cflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CC $in',
        'rule cxx',
        f'  command = "{CXX}" $cxxflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CXX $in',
        'rule rc',
        f'  command = "{WINDRES}" -Isrc -i $in -o $out',
        '  description = RC $in',
        'rule link',
        f'  command = "{CXX}" $ldflags $in -o $out $libs',
        '  description = LINK $out',
        'rule link_dll',
        f'  command = "{CXX}" -shared -static -Wl,--image-base=0x{GAME_DLL_BASE:08X} '
        f'-Wl,--disable-dynamicbase -Wl,--no-insert-timestamp $in -o $out -Wl,--out-implib,{GAME_IMPLIB}',
        '  description = LINK $out',
        'rule exe_exports',
        f'  command = "{sys.executable}" tools/exe_exports.py $out @$out.rsp',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '  description = EXPORTS $out',
        '  restat = 1',                  # (unchanged: game.dll is not linked again)
        'rule dlltool',
        f'  command = "{DLLTOOL}" -m i386 -d $in -l $out -D d.exe',
        '  description = DLLTOOL $out',
        'rule configure',
        f'  command = "{sys.executable}" configure.py {" ".join(sys.argv[1:])}',
        '  generator = 1',
        '  description = CONFIGURE',
        '',
        f'build build.ninja: configure | configure.py {rel(BSS_ASM)} {rel(ALIASES)}',
        '',
    ]
    objs = []
    for src in sources:
        r = rel(src)
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        rule = 'cxx' if src.endswith('.cpp') else 'cc'
        lines.append(f'build {obj}: {rule} {r}')
        if r.startswith('src/psx/'):
            # the PS1 library replacements see the same headers as the game
            if rule == 'cc':
                lines.append(f'  cflags = {common} -std=gnu11 -DPC_PORT {PSX_INCLUDES}')
            else:
                lines.append(f'  cxxflags = {common} -std=gnu++17 -fno-exceptions -fno-rtti -DPC_PORT {PSX_INCLUDES}')
        objs.append(obj)
    for r in CHDR_SOURCES:
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        lines.append(f'build {obj}: cc {r}')
        lines.append(f'  cflags = {CHDR_CFLAGS}')
        objs.append(obj)
    for r in sorted(rel(p) for p in glob.glob(os.path.join(HERE, 'src', '*.rc'))):
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.res.o'
        lines.append(f'build {obj}: rc {r} | {" ".join(RESOURCE_FILES)}')
        objs.append(obj)
    # the game code (game.dll)
    game_objs = []
    for src in sorted(glob.glob(os.path.join(ROOT, 'src', '*.c'))):
        path = '../' + os.path.relpath(src, ROOT).replace('\\', '/')
        obj = 'build/obj/game/' + os.path.splitext(path[3:])[0] + '.o'
        flags = GAME_FLAGS
        if path == '../src/main.c':
            flags += ' -Dmain=psx_main'         # main() of the PS1 program
        lines.append(f'build {obj}: cc {path}')
        lines.append(f'  cflags = {opt} {flags} -std=gnu99')
        game_objs.append(obj)
    # the bounds of the game's data section, exported (game/game_dll.c)
    obj = 'build/obj/game/game_dll.o'
    lines.append(f'build {obj}: cc game/game_dll.c')
    game_objs.append(obj)
    # (both have the game's .bss at its PS1 addresses)
    bss = 'build/obj/gen/bss_symbols.o'
    lines.append(f'build {bss}: cc {GEN_BSS}')
    # what the game takes from d.exe -> d.exe's exports and their import library
    lines.append(f'build {EXE_DEF}: exe_exports {" ".join(game_objs)} | {" ".join(objs)} tools/exe_exports.py')
    lines.append(f'build {EXE_IMPLIB}: dlltool {EXE_DEF}')
    lines.append(f'build {GAME_DLL} | {GAME_IMPLIB}: link_dll {" ".join(game_objs)} {bss} {EXE_IMPLIB} {GAME_DEF}')
    lines.append(f'build {OUT}: link {" ".join(objs)} {bss} {GAME_IMPLIB} {EXE_DEF}')
    lines.append(f'default {OUT} {GAME_DLL}')
    open(os.path.join(HERE, 'build.ninja'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'wrote build.ninja ({len(sources)} sources)')


if __name__ == '__main__':
    main()
