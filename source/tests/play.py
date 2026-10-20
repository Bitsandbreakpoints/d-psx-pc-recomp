#!/usr/bin/env python3
"""Scripted play-through of the port: each scenario starts the game with
D_INPUT, saves pictures at fixed frames and makes a montage
(build/test/play/<scenario>/montage.png).  Look at the montages; the
script itself reports crashes, the frame count and what the log says
about the movies (stream) and the disc change.

    python tests/play.py [scenario ...]      (default: all)

Scenarios:
  intro        the logos, the opening movie (skipped with Start), the title
  castle       Start on the title, the first movies skipped, the dining
               hall: turn right, the inventory (L1) and its items
  disc_change  as castle, then the disc change (D_EVENT 75, EVENT_DISKGAME):
               "insert disc 2", the virtual lid, disc 2 starts
  compact      the inventory (L1), the compact used (Circle): its hint
  puzzle_<name> a puzzle or system screen started with D_EVENT in the
               castle (after one or two disc changes for those of discs 2
               and 3): r11, r13 (disc 1); r04door, r04loop,
               r05fight, r06, r09star, r09lever (disc 2); r14, ending (disc 3).
               Not part of "all" (python tests/play.py puzzles runs them).

The frames count vertical blanks: 60 a second, but only about 15 a second
while a movie plays (the game waits for each movie frame), so the times
depend on the movies' lengths.  The settings in build/settings.ini apply,
but the logos always play (D_KEEP_LOGOS, testlib.py): the scripts expect them.
"""
import os
import sys

from testlib import PORT, montage, run_port

OUT = os.path.join(PORT, 'build', 'test', 'play')

# Start skips the opening (the logos cannot be skipped), then the title
# takes Start; the first movies of the game are skipped with Start too.
# Presses every PRESS_EVERY frames, since the movies' frame counts vary a
# little from run to run (Start does nothing in the castle's view).
PRESS_EVERY = 40
TO_TITLE = [(t, 'start') for t in range(300, 500, PRESS_EVERY)]
TO_CASTLE = [(t, 'start') for t in range(300, 2200, PRESS_EVERY)]
IN_CASTLE = [(2300, 'right'), (2500, 'start'), (2700, 'l1'), (3100, 'right'), (3400, 'l1')]


def intro():
    return TO_TITLE, [60, 200, 320, 600, 900], 1000, None


def castle():
    return TO_CASTLE + IN_CASTLE, [2150, 2280, 2450, 2900, 3200, 3600], 3700, None


def compact():
    keys = [(2400, 'l1'), (2700, 'circle'), (3100, 'cross'), (3300, 'cross')]
    return TO_CASTLE + keys, [2600, 2800, 2950, 3100, 3400], 3600, None


def disc_change():
    return TO_CASTLE, [2300, 2450, 2600, 2900, 3300], 3600, '2400:75'


# event ids (game.h; @location for puzzles that look at where the player
# stands) and on which disc the screen's files are
PUZZLES = {
    'r11': (7, 1), 'r13': (17, 1),
    'r04door': (44, 2), 'r04loop': (33, 2), 'r05fight': (47, 2), 'r06': ('57@118', 2),
    'r09star': (67, 2), 'r09lever': (68, 2),
    'r14': (70, 3), 'ending': (76, 3),
}
EVENT_DISC_CHANGE = 75
DISC_CHANGE_FRAMES = 600            # from the event to the next disc's castle view
FIRST_EVENT = 2400


def puzzle(event, disc):
    """Castle, disc changes up to `disc`, then the event; Cross and the
    D-pad a few times."""
    events, at = [], FIRST_EVENT
    for _ in range(disc - 1):
        events.append(f'{at}:{EVENT_DISC_CHANGE}')
        at += DISC_CHANGE_FRAMES
    events.append(f'{at}:{event}')
    keys = [(at + 200, 'cross'), (at + 260, 'right'), (at + 320, 'up'), (at + 380, 'cross'),
            (at + 440, 'left'), (at + 500, 'circle')]
    frames = [at + 60, at + 180, at + 300, at + 450, at + 600]
    return TO_CASTLE + keys, frames, at + 700, ','.join(events)


SCENARIOS = {'intro': intro, 'castle': castle, 'compact': compact, 'disc_change': disc_change}
for _name, (_event, _disc) in PUZZLES.items():
    SCENARIOS[f'puzzle_{_name}'] = (lambda e=_event, d=_disc: puzzle(e, d))
TIMEOUT_SECONDS = 400


def main():
    names = sys.argv[1:] or ['intro', 'castle', 'compact', 'disc_change']
    if names == ['puzzles']:
        names = [f'puzzle_{n}' for n in PUZZLES]
    for name in names:
        script, frames, last, events = SCENARIOS[name]()
        out = os.path.join(OUT, name)
        text = ','.join(f'{frame}:{key}' for frame, key in script)
        log = run_port(out, text, frames, last, TIMEOUT_SECONDS, events)
        montage(out, frames, os.path.join(out, 'montage.png'))
        summary = [line for line in log
                   if 'CRASH' in line or 'frames in' in line or 'disc' in line or 'lid' in line]
        movies = sum(1 for line in log if line.startswith('stream: reading'))
        print(f'{name}: {movies} streams; ' + ('; '.join(summary) or 'no summary in the log'))


if __name__ == '__main__':
    main()
