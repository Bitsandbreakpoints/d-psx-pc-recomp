#!/usr/bin/env python3
"""Packs the PC port for players: build/package/d-pc.zip.

    python tools/package.py [--scan-only]

The zip holds d.exe, game.dll (the game's code), the player's README
(README.txt) and the licences of the third-party code built into them.  It
never holds anything from the game: before packing, both are searched for
data from the three discs
(the disc images in ../disc, discN.bin, if they are there): every 64-byte
block of the discs' data files - the executable, the pictures, the sound
bank, everything but the movies and audio tracks (STR, DA, WAV), which are
only ever streamed - that is not mostly one repeated value is looked for
at every offset of the exe.  Blocks that the PS1 build's link map
(../build/SLUS_001.28.map) places in the data of a decompiled source file
(../src) are expected - the decompilation's own tables and texts, compiled
into the port as into the PS1 build - and are listed per file; any other
block stops the packing.  Settings and logs from test runs never go in
(only the files listed here are packed).
"""
import collections
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import psxiso  # noqa: E402  (the project's ISO9660 reader)

EXE = os.path.join(HERE, 'build', 'd.exe')
GAME_DLL = os.path.join(HERE, 'build', 'game.dll')
OUT_DIR = os.path.join(HERE, 'build', 'package')
ZIP_NAME = 'd-pc.zip'
FOLDER = 'D PC'

BLOCK = 64
MIN_DISTINCT_BYTES = 12        # skip blocks that are mostly one value (zeros, padding)
DISC_IMAGES = ['disc/disc1.bin', 'disc/disc2.bin', 'disc/disc3.bin']
STREAMED = ('.STR', '.DA', '.WAV')
# the game's executable on the disc -> (PS1 build link map, PS1 address of
# the file's first byte: the image follows a 2 KB header)
EXE_FILES = ('SLUS_001.28', 'SLUS_001.73', 'SLUS_001.74')
LINK_MAP = 'build/SLUS_001.28.map'
EXE_BASE = 0x80010000 - 0x800

# licence files: (name in the zip, source, None or the text a licence
# block at the end of a single-file library starts with)
LICENSES = [
    ('imgui.txt', 'third_party/imgui/LICENSE.txt', None),
    ('libchdr.txt', 'third_party/libchdr/LICENSE.txt', None),
    ('zstd.txt', 'third_party/libchdr/deps/zstd-1.5.7/LICENSE', None),
    ('lzma.txt', 'third_party/libchdr/deps/lzma-26.02/LICENSE', None),
    ('miniz.txt', 'third_party/libchdr/deps/miniz-3.1.2/miniz.c', 'Copyright 2013-2014 RAD'),
    ('dr_flac.txt', 'third_party/libchdr/include/dr_libs/dr_flac.h', 'This software is available as a choice'),
    ('stb_image.txt', 'third_party/stb/stb_image.h', 'This software is available under 2 licenses'),
]

README = """D - PC port
===========

A native Windows version of the PlayStation game D (1995, WARP /
Acclaim), built from a decompilation of the game's code. It contains no
graphics, movies, sound or other files from the game: you need your own
copy of the three discs (D (USA), SLUS-00128 / 00173 / 00174) as disc
images.

How to play
-----------
1. Make images of your own discs with complete sectors: .bin + .cue, or a
   .chd made from them (a .iso cannot hold the movies), e.g.
   "D (USA) (Disc 1).bin" / ".cue".
2. Start d.exe and click CHOOSE DISCS: select all three images at once
   (Ctrl/Shift-click), or drop them on the window. If you load only one,
   the others are found next to it if they have the same name with
   another disc number. Each disc is checked against the known release
   the first time (a few seconds each).
3. Choose PLAY. When the game asks for the next disc, it is put in by
   itself (or the game window asks for the file).

In the game: Start (or P) pauses, with a menu: continue, save a state,
load one (8 slots), the settings, back to the launcher. LOAD GAME in the
launcher lists the states: start from one, or delete one. Esc goes back
to the launcher, F1 (or Select+Start on the controller) shows the
settings. The states are kept in the "saves" folder next to the program;
keep d.exe and game.dll together.

Controls, graphics (picture filters such as CRT, full screen, smoother
movies) and sound are set under SETTINGS. The settings are kept in
settings.ini next to the program.

Requires Windows 10 or later.

The licences of the third-party code are in the "licenses" folder.
D is a trademark of its owner; this project is not affiliated with WARP,
Acclaim or Sony.
"""


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def disc_files():
    """{name: data} of the discs' data files that must not be in the exe."""
    files = {}
    for image in DISC_IMAGES:
        path = os.path.join(ROOT, image)
        if not os.path.exists(path):
            continue
        disc = psxiso.Disc(path)
        _, root_lba, root_size, _ = psxiso.root(disc)
        for name, lba, size in psxiso.walk(disc, root_lba, root_size):
            plain = name.split(';')[0]
            if plain.upper().endswith(STREAMED):
                continue
            sectors = (size + psxiso.USER_DATA_FORM1 - 1) // psxiso.USER_DATA_FORM1
            data = b''.join(disc.data(lba + i) for i in range(sectors))[:size]
            if files.get(plain, data) == data:
                files[plain] = data         # (the same file on several discs: once)
            else:
                files[f'{os.path.basename(image)}:{plain}'] = data
    return files


def scan(exe):
    """[(file, offset, exe offset)] of blocks of disc files found in the exe."""
    files = disc_files()
    if not files:
        print('scan: no disc images (../disc/discN.bin) - skipped')
        return []
    blocks = {}                 # first 8 bytes -> {block: (file, offset)}
    count = 0
    for name, data in files.items():
        for offset in range(0, len(data) - BLOCK + 1, BLOCK):
            block = data[offset:offset + BLOCK]
            if len(set(block)) >= MIN_DISTINCT_BYTES:
                blocks.setdefault(block[:8], {}).setdefault(block, (name, offset))
                count += 1
    hits = []
    for i in range(len(exe) - BLOCK + 1):
        candidates = blocks.get(exe[i:i + 8])
        if candidates:
            where = candidates.get(exe[i:i + BLOCK])
            if where:
                hits.append((where[0], where[1], i))
    print(f'scan: {count} blocks from {len(files)} disc files, {len(hits)} found in the exe')
    return hits


def map_sections(path):
    """[(start, size, section, object)] of a GNU ld map."""
    result, pending = [], None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\s+(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', line)
            if m:
                result.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4)))
                pending = None
                continue
            m = re.match(r'\s+(\.\S+)\s*$', line)
            if m:
                pending = m.group(1)
                continue
            m = re.match(r'\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', line)
            if m and pending:
                result.append((int(m.group(1), 16), int(m.group(2), 16), pending, m.group(3)))
                pending = None
    return result


def classify(hits):
    """Splits the hits into data of decompiled source files ({object: bytes})
    and the rest."""
    sections = None
    decompiled, other = collections.Counter(), []
    for name, offset, at in hits:
        where = None
        if name.split(':')[-1] in EXE_FILES and os.path.exists(os.path.join(ROOT, LINK_MAP)):
            if sections is None:
                sections = map_sections(os.path.join(ROOT, LINK_MAP))
            address = EXE_BASE + offset
            for start, size, section, obj in sections:
                if start <= address < start + size:
                    where = (section, obj)
                    break
        if (where and where[1].replace(chr(92), '/').startswith('build/src/')
                and where[0] in ('.data', '.rodata', '.sdata')):
            decompiled[where[1][len('build/'):-len('.o')]] += BLOCK
        else:
            other.append((name, offset, at))
    return decompiled, other


def license_text(source, start):
    text = read(os.path.join(HERE, source)).decode('utf-8', 'replace')
    if start:
        text = text[text.rindex(start):]
        end = text.find('*/')
        text = text[:end] if end >= 0 else text
        # without the comment's leading " * " and its closing row of stars
        lines = [re.sub(r'^ ?\* ?', '', line) for line in text.split('\n')]
        text = '\n'.join(line for line in lines if line.strip('*') or not line)
    return text.replace('\r\n', '\n').strip() + '\n'


def main():
    if not os.path.exists(EXE) or not os.path.exists(GAME_DLL):
        sys.exit('build/d.exe or build/game.dll is missing: build first (build.bat)')
    exe, dll = read(EXE), read(GAME_DLL)
    for name, data in (('d.exe', exe), ('game.dll', dll)):
        print(f'{name}:')
        decompiled, other = classify(scan(data))
        for obj, size in sorted(decompiled.items(), key=lambda x: -x[1]):
            print(f'  decompiled data: {size:6} bytes from {obj}')
        if other:
            for disc_file, offset, at in other[:30]:
                print(f'  NOT from the decompiled source: {disc_file} +0x{offset:X} at 0x{at:X}')
            sys.exit(f'stopped: {name} contains data from the discs (see above)')
    if '--scan-only' in sys.argv:
        return
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, ZIP_NAME)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(f'{FOLDER}/d.exe', exe)
        z.writestr(f'{FOLDER}/game.dll', dll)
        z.writestr(f'{FOLDER}/README.txt', README.replace('\n', '\r\n').encode('utf-8-sig'))
        z.writestr(f'{FOLDER}/licenses/THIRD_PARTY.md', read(os.path.join(HERE, 'THIRD_PARTY.md')))
        for name, source, start in LICENSES:
            z.writestr(f'{FOLDER}/licenses/{name}',
                       license_text(source, start).replace('\n', '\r\n').encode('utf-8'))
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            print(f'  {info.file_size:>9}  {info.filename}')
    print(f'{os.path.relpath(path, HERE)}: {os.path.getsize(path)} bytes, '
          f'sha1 {hashlib.sha1(read(path)).hexdigest()}')


if __name__ == '__main__':
    main()
