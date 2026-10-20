#!/usr/bin/env python3
"""Save states: the game is played to a point, a state is saved there
(D_SAVE_STATE), the game is played on; then a second run starts from the
state (D_LOAD_STATE) with the same buttons after it.  The pictures of both
runs at the same moments go side by side into a montage
(build/test/savestate/<scenario>/montage.png: the run that saved above,
the one that loaded below), and the mean difference of each pair is
printed (0 = the same picture).

    python tests/savestate.py [scenario ...]      (default: all)

Scenarios:
  castle   saved while looking around the dining hall; then the inventory
           (L1), an item, back, a turn to the right
  movie    saved in the movie of walking on (a turn to the right, then up):
           it must go on where it was (the movie runs in real time, so
           the two runs can be a movie frame apart)
  disc2    the disc change (D_EVENT 75), saved in the castle on disc 2: the
           run that loads must start with disc 2 in the drive
  puzzle   a puzzle (D_EVENT 7, r11): no state can be saved there (the log
           says so, and the run that loads finds no state)

The states go to build/test/savestate/states (D_SAVE_DIR), not the
player's saves.
"""
import os
import sys

from testlib import PORT, run_port

OUT = os.path.join(PORT, 'build', 'test', 'savestate')
STATES = os.path.join(OUT, 'states')
TIMEOUT_SECONDS = 300
PRESS_EVERY = 40
TO_CASTLE = [(t, 'start') for t in range(300, 2200, PRESS_EVERY)]

# (frame of the save, buttons after it, pictures after it, frames after
# it); the frames after the save count from the save (the run that loads
# starts there)
SCENARIOS = {
    'castle': (2400, [(100, 'l1'), (400, 'right'), (600, 'circle'), (800, 'right')],
               [30, 250, 500, 700, 1000], 1100),
    'disc2': (3300, [(100, 'l1'), (400, 'right')], [30, 250, 500], 600),
    'puzzle': (2700, [], [30], 100),
    'movie': (2510, [], [4, 8, 14, 20, 40, 300], 400),
}
EVENTS = {'disc2': '2400:75', 'puzzle': '2400:7'}
MOVIE_START = [(2300, 'right'), (2500, 'up')]


def difference(a, b):
    from PIL import ImageChops, ImageStat
    if a.size != b.size:
        return float('inf')
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


def run(name):
    save_at, after, pictures, length = SCENARIOS[name]
    before = TO_CASTLE + (MOVIE_START if name == 'movie' else [])
    out = os.path.join(OUT, name)
    env = {'D_SAVE_DIR': STATES, 'D_SAVE_STATE': f'{save_at}:1'}
    # the run that saves: the frames after the save are save_at + 1 + n
    # (the state is taken at the game's next pass, a frame later)
    script = before + [(save_at + 1 + f, k) for f, k in after]
    frames = [save_at + 1 + f for f in pictures]
    text = ','.join(f'{f}:{k}' for f, k in script)
    if os.path.exists(os.path.join(STATES, 'slot1.state')):
        os.remove(os.path.join(STATES, 'slot1.state'))
    log = run_port(os.path.join(out, 'saved'), text, frames, save_at + 1 + length,
                   TIMEOUT_SECONDS, EVENTS.get(name), wav='sound.wav', env=env)
    saved = [line for line in log if 'state' in line or 'CRASH' in line or 'disc ' in line]
    print(f'{name}, saving run: ' + '; '.join(saved))
    if not any('saved (' in line for line in log):
        print(f'{name}: no state was saved' + (' (as it should be)' if name == 'puzzle' else ''))
        return
    # the run that loads
    text = ','.join(f'{f}:{k}' for f, k in after)
    env = {'D_SAVE_DIR': STATES, 'D_LOAD_STATE': '1'}
    log = run_port(os.path.join(out, 'loaded'), text, pictures, length, TIMEOUT_SECONDS,
                   wav='sound.wav', env=env)
    loaded = [line for line in log if 'state' in line or 'CRASH' in line or 'frames in' in line]
    print(f'{name}, loading run: ' + '; '.join(loaded))
    compare(out, frames, pictures)


def compare(out, frames, pictures):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print('Pillow is not installed (pip install pillow): no montage')
        return
    pairs = []
    for a, b in zip(frames, pictures):
        pa = os.path.join(out, 'saved', f'shot_{a}.bmp')
        pb = os.path.join(out, 'loaded', f'shot_{b}.bmp')
        if os.path.exists(pa) and os.path.exists(pb):
            pairs.append((b, Image.open(pa).convert('RGB'), Image.open(pb).convert('RGB')))
        else:
            print(f'  +{b}: picture missing')
    if not pairs:
        return
    w = max(p.width for _, p, _ in pairs)
    h = max(p.height for _, p, _ in pairs)
    sheet = Image.new('RGB', ((w + 8) * len(pairs), 2 * h + 24), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (frame, a, b) in enumerate(pairs):
        sheet.paste(a, (i * (w + 8), 16))
        sheet.paste(b, (i * (w + 8), 24 + h))
        d = difference(a, b)
        draw.text((i * (w + 8) + 2, 2), f'+{frame}: {d:.1f}', fill=(255, 255, 255))
        print(f'  +{frame}: mean difference {d:.2f}')
    sheet.save(os.path.join(out, 'montage.png'))


def main():
    for name in sys.argv[1:] or list(SCENARIOS):
        run(name)


if __name__ == '__main__':
    main()
