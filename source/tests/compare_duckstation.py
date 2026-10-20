#!/usr/bin/env python3
"""Compares the port with DuckStation: the same buttons in both, pictures
of the same screens, a difference image and numbers per screen
(build/test/compare/montage.png).

    python tests/compare_duckstation.py [disc1.cue]

The disc defaults to ../disc/disc1.cue.  DuckStation comes from
../tools/duckstation; it is copied once to build/test/duckstation with its
own settings (software renderer, 1x, cropped to the display area,
keyboard pad, muted, screenshots of the display area as it is in VRAM), so
the project's emulator settings are not touched.  Keys go to the emulator's
window with PostMessage (the keyboard and the focus stay yours).  The port
runs with D_NATIVE=1 (original resolution).

The screens are ones where the timing does not matter (the emulator is
driven by time, the port by frame numbers): the title, the dining hall
(the first place of the castle) and the view after turning right.  The
emulator's title is found by the pictures' sizes: the logos are 320x240,
the opening movie 288x144 - Enter is pressed once that shows (it skips the
movie), and the title is taken a few seconds later.
"""
import glob
import os
import shutil
import subprocess
import sys
import time

from testlib import PORT, ROOT, close, find_window, post_keys, run_port

OUT = os.path.join(PORT, 'build', 'test', 'compare')
EMU_SOURCE = os.path.join(ROOT, 'tools', 'duckstation')
EMU_DIR = os.path.join(PORT, 'build', 'test', 'duckstation')
EMU_EXE = 'duckstation-qt-x64-ReleaseLTCG.exe'
DEFAULT_DISC = os.path.join(ROOT, 'disc', 'disc1.cue')
EMU_COPY_SKIP = ('cache', 'covers', 'gamesettings', 'memcards', 'savestates', 'screenshots',
                 'videos', 'settings.ini')

EMU_SETTINGS = """[Main]
SettingsVersion = 3
ConfirmPowerOff = false
PauseOnFocusLoss = false
SaveStateOnExit = false
SetupWizardIncomplete = false
StartFullscreen = false

[UI]
MainWindowWidth = 800
MainWindowHeight = 600

[AutoUpdater]
CheckAtStartup = false

[Cheevos]
Enabled = false

[BIOS]
PatchFastBoot = true
PathNTSCJ = scph5502.bin
PathNTSCU = scph5502.bin
PathPAL = scph5502.bin
SearchDirectory = bios

[GPU]
Renderer = Software
ResolutionScale = 1
PGXPEnable = false

[Display]
CropMode = BordersUncorrected
ShowOSDMessages = false
ScreenshotMode = UncorrectedInternalResolution
ScreenshotFormat = PNG

[CDROM]
RegionCheck = false

[Audio]
Backend = Null
OutputMuted = true

[InternalPostProcessing]
Enabled = false

[PostProcessing]
Enabled = false

[Pad1]
Type = DigitalController
Up = Keyboard/Up
Down = Keyboard/Down
Left = Keyboard/Left
Right = Keyboard/Right
Cross = Keyboard/X
Circle = Keyboard/C
Square = Keyboard/Z
Triangle = Keyboard/S
Start = Keyboard/Return
Select = Keyboard/Backspace
L1 = Keyboard/Q
R1 = Keyboard/E

[Hotkeys]
Screenshot = Keyboard/F10
"""

# the emulator after the title: (screen, keys before it, seconds to wait after them)
EMU_STEPS = [
    # Start on the title, the game's first movies skipped with Start
    ('castle', 'enter ' + ' '.join(['wait2500 enter'] * 10), 4.0),
    ('turned', 'right', 4.0),
]
TITLE_TIMEOUT = 120             # seconds
TITLE_POLL = 1.0
# the opening movie: 288x144 (DuckStation's crop may take a column off);
# the logos are 320x240
OPENING_WIDTHS = range(286, 291)
OPENING_HEIGHT = 144
TITLE_WAIT = 7.0                # after skipping the opening: the title fades in
# the port: the same screens at these frames, in two runs (the title in the
# first; the second goes on into the castle)
PORT_TITLE = ('title', ','.join(f'{t}:start' for t in range(300, 500, 40)), 900)
CASTLE_INPUT = ','.join([f'{t}:start' for t in range(300, 2200, 40)] + ['2300:right'])
PORT_CASTLE = [('castle', 2280), ('turned', 2450)]

DIFF_THRESHOLD = 24             # a pixel "differs" above this (0..255, any channel)
DIFF_GAIN = 4                   # the difference picture, brighter
CROP_SLACK = 2                  # pixels the two pictures' sizes may differ without scaling


def setup_emulator():
    if not os.path.isdir(EMU_DIR):
        shutil.copytree(EMU_SOURCE, EMU_DIR, ignore=shutil.ignore_patterns(*EMU_COPY_SKIP))
    with open(os.path.join(EMU_DIR, 'settings.ini'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(EMU_SETTINGS)


def screenshot(proc, shots, out):
    """The emulator's screenshot (F10) moved to `out`; FALSE if none came."""
    before = set(glob.glob(os.path.join(shots, '**', '*.png'), recursive=True))
    post_keys(find_window(proc.pid, child=True), 'f10')
    time.sleep(1.5)
    new = set(glob.glob(os.path.join(shots, '**', '*.png'), recursive=True)) - before
    if not new:
        return False
    shutil.move(new.pop(), out)
    return True


def picture_size(path):
    from PIL import Image
    with Image.open(path) as picture:
        return picture.size


def run_emulator(cue):
    shots = os.path.join(EMU_DIR, 'screenshots')
    os.makedirs(shots, exist_ok=True)
    proc = subprocess.Popen([os.path.join(EMU_DIR, EMU_EXE), '-batch', '-nofullscreen', cue],
                            cwd=EMU_DIR)
    title = os.path.join(OUT, 'emu_title.png')
    try:
        start = time.time()
        while time.time() - start < TITLE_TIMEOUT:
            time.sleep(TITLE_POLL)
            if screenshot(proc, shots, title) and picture_size(title)[0] in OPENING_WIDTHS                     and picture_size(title)[1] == OPENING_HEIGHT:
                post_keys(find_window(proc.pid, child=True), 'enter')   # skips the opening
                break
        else:
            print('title: the opening movie was not seen in the emulator')
        time.sleep(TITLE_WAIT)
        screenshot(proc, shots, title)
        for name, keys, wait in EMU_STEPS:
            post_keys(find_window(proc.pid, child=True), keys)
            time.sleep(wait)
            if not screenshot(proc, shots, os.path.join(OUT, f'emu_{name}.png')):
                print(f'{name}: no emulator screenshot')
    finally:
        close(proc)


def compare(pairs, out):
    """Prints the mean difference per screen and writes a montage:
    emulator | port | difference (brighter)."""
    from PIL import Image, ImageChops, ImageDraw
    rows = []
    for name, emu_path, port_path in pairs:
        if not (os.path.exists(emu_path) and os.path.exists(port_path)):
            print(f'{name:10} missing picture')
            continue
        port = Image.open(port_path).convert('RGB')
        emu = Image.open(emu_path).convert('RGB')
        if abs(emu.width - port.width) <= CROP_SLACK and abs(emu.height - port.height) <= CROP_SLACK:
            # (DuckStation's crop can take a column off: compare the common part)
            size = (min(emu.width, port.width), min(emu.height, port.height))
            emu, port = emu.crop((0, 0) + size), port.crop((0, 0) + size)
        else:
            emu = emu.resize(port.size, Image.BILINEAR)
        diff = ImageChops.difference(emu, port)
        pixels = list(diff.get_flattened_data()) if hasattr(diff, 'get_flattened_data') \
            else list(diff.getdata())
        mean = sum(sum(p) for p in pixels) / (3.0 * len(pixels))
        differing = sum(1 for p in pixels if max(p) > DIFF_THRESHOLD) * 100.0 / len(pixels)
        print(f'{name:10} {port.size[0]}x{port.size[1]}  mean {mean:.1f}, differing {differing:.1f} %')
        rows.append((name, emu, port, diff.point(lambda v: min(255, v * DIFF_GAIN))))
    if not rows:
        return
    w = max(r[2].width for r in rows)
    h = max(r[2].height for r in rows)
    sheet = Image.new('RGB', (w * 3, h * len(rows)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (name, emu, port, diff) in enumerate(rows):
        for j, picture in enumerate((emu, port, diff)):
            sheet.paste(picture, (j * w, i * h))
        draw.text((3, i * h + 3), f'{name}: DuckStation | port | difference', fill=(255, 255, 0))
    sheet.save(out)


def main():
    cue = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DISC
    if not os.path.exists(cue):
        sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    setup_emulator()
    run_emulator(cue)
    native = {'D_NATIVE': '1'}
    name, script, frame = PORT_TITLE
    run_port(os.path.join(OUT, 'port_title'), script, [frame], frame + 10, 300, env=native)
    frames = [f for _, f in PORT_CASTLE]
    run_port(os.path.join(OUT, 'port_castle'), CASTLE_INPUT, frames, max(frames) + 10, 400,
             env=native)
    pairs = [(name, os.path.join(OUT, f'emu_{name}.png'),
              os.path.join(OUT, 'port_title', f'shot_{frame}.bmp'))]
    pairs += [(n, os.path.join(OUT, f'emu_{n}.png'), os.path.join(OUT, 'port_castle', f'shot_{f}.bmp'))
              for n, f in PORT_CASTLE]
    compare(pairs, os.path.join(OUT, 'montage.png'))
    print(os.path.join(OUT, 'montage.png'))


if __name__ == '__main__':
    main()
