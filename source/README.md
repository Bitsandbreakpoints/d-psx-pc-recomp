# D: PC port (source)

A native Windows version of the PlayStation game *D* (WARP / Acclaim,
1995, three discs), built from a decompilation of the game. No emulator is
needed.

This folder holds the port's own program. The decompiled game code it is
built together with is not published, so it does not build on its own
(see the main README).

The launcher finds and checks the three discs (`.bin/.cue` or `.chd`). The
game runs natively: the movies (MDEC pictures and XA sound streamed from
the disc at the console's pace), the title screen and its music, the
castle, the inventory and the sound effects. The disc change happens by
itself, with a virtual lid. The picture can be shown sharp, smooth, as a
CRT or as a soft TV. The movies' colours can be upsampled smoothly, and
the logos can be skipped. The "Softer bass" setting (Sound, on by default)
takes the deepest bass out of the sound effects: scrolling in the
inventory is a deep thump that can make speakers rattle. On the console it
was heard through the TV's small speaker. The movies and the music are not
changed.

Keyboard or XInput controller; F1 (or Select+Start) opens the settings in
the game. Start pauses at any moment in the game, even in the middle of a
movie (except in the intro, on the title screen and in the first movie,
where Start has a use in the game); P, Pause or R3 (the right stick pressed
in) always pause. The picture stays, and the movie and its sound go on
exactly where they stopped.

The pause screen is a menu: continue, **save state**, **load state**,
settings and back to the launcher. There are eight slots, each with a
small picture, the disc, the time played and the date. "LOAD GAME" in the
launcher lists them: Enter/A starts the game straight from a state (the
right disc is put in), and the Delete button, Del or X deletes one (after
a question). Saving works while looking around, in the movies and in the
inventory, but not in the middle of a puzzle or on the title screen. The
states are kept in `saves/` next to `d.exe` (under 1.5 MB each). They keep
working when the port is rebuilt; only if the game's code (`game.dll`) or
the layout of what is saved changes can they not be loaded, and the list
then says why.

## You need your own discs
The port contains no graphics, sound, movies or other files from the
game, and no BIOS. You choose your own images of *D (USA)* in the
launcher: "CHOOSE DISCS" (select one, two or all three files with
Ctrl/Shift) or drop the files on the window. If you choose only one disc,
the other two are found by themselves if they lie next to it with the same
name and another disc number, for example `D (USA) (Disc 1).chd` →
`(Disc 2)`, `(Disc 3)`. Disc 1 is enough to start: when the game asks for
the next disc and it has not been chosen, the game window asks for the
file.

The images must have whole sectors (`.bin/.cue`, or a `.chd` made from
one), since the movies and the music are in XA sectors. An `.iso` is not
enough. The first time a disc is chosen, the SHA-1 of its data track is
compared with the known image (a few seconds per disc, in the background:
the launcher shows a spinning indicator with the disc's name and how far
it has come, and PLAY waits until it is done); after that, the file's size
and time are enough.

The settings are kept in `settings.ini` next to the program.

## Building
Requirements: Windows 10/11 and Python 3. The game code comes from the
decompilation project (`../src`, `../include`), the PS1 types from
PSn00bSDK's headers in `../tools/psn00bsdk`, and the list of the game's
uninitialised variables from `../asm/data/bss.bss.s`. None of these are
published, so the build stops without them.

```bash
build.bat
```

The first time, the compiler (llvm-mingw) and ninja are downloaded to
`tools/` (SHA-256 checked, about 190 MB). The result is `build/d.exe` and
`build/game.dll`. `build.bat --debug` makes a debug build.

## How it works
- Two files: `d.exe` (the launcher, the window, the PS1 libraries under
  `src/psx/`) and `game.dll`, the game's decompiled code and data at a
  fixed address (0x60000000), which calls the libraries in `d.exe` (the
  list is written by `tools/exe_exports.py` during the build). The save
  states hold addresses of the game's code, so they stay valid as long as
  `game.dll` is the same: the port can be rebuilt without breaking them.
- A 32-bit Windows program (`--large-address-aware`): the PS1 memory lies
  at its original addresses (0x80000000), and the game's uninitialised
  variables lie where they were on the console (absolute symbols that
  `configure.py` generates), so the game code runs as it is. The only
  change to the game code is `PC_SAFE_POINT()` at the top of its main loop
  (an empty macro on the PS1).
- Direct3D 11 for the picture, XInput for controllers, XAudio2 for sound.
  All of them are part of Windows.
- The Sony PS1 libraries that D calls (82 functions: graphics, CD and
  streaming, MDEC, sound, controllers, counters) are written anew in C
  under `src/psx/`. The base (window, GPU, SPU, launcher, disc reader) comes
  from the PC port of American Pool.
- The movies: the CD reads in real time (150 sectors a second);
  `stream.c` puts the STR frames together and sends the XA sound to
  `xa.c`. The disc reader reads the next two seconds ahead on a thread of
  its own, and the sound plays on while the image is read (a cold read
  from a CHD can take tenths of a second). `mdec.c` decodes the frames
  (VLC, IDCT, YCbCr→RGB), and the game's own movie player puts them into
  VRAM column by column.
- Save states (`src/psx/savestate.c`): at the top of the game's main loop
  the whole state of the game is in memory (nothing on the stack), so the
  state is taken there: the PS1 memory, the game's own variables and the
  state of the library replacements (VRAM, sound RAM and voices, the XA
  queue, the CD and its streaming, MDEC, counters, controllers, debug
  text), each part behind a mark, packed with miniz. Loading starts the
  game again and puts the state back at the first pass. The pause menu
  lets the game run one more frame (silent, under the pause screen) to
  that point. The state holds addresses of the game's code (callbacks),
  hence a checksum of `game.dll` in the file header, and the size of every
  part: a state from a build where a part has changed is refused, with
  the reason in the list.

## Tests
None of the tests use the global keyboard, so the computer can be used
while they run. The discs are taken from `build/settings.ini` (choose
them in the launcher once).

- `python tests/play.py`: scripted runs (intro, the castle, the compact,
  the disc change) with picture montages in `build/test/play/`;
  `python tests/play.py puzzles` starts every puzzle and system screen (on
  the right disc).
- `python tests/compare_duckstation.py [disc1.cue]`: the same screens in
  DuckStation and in the port, with pictures and differences in
  `build/test/compare/` (the dining hall: a mean of 0.9 out of 255).
- `python tests/compare_sound.py [disc1.cue]`: the sound in DuckStation
  and in the port through the same steps: the title music (XA) and the
  dining hall's background sound (SPU) are compared in level, spectrum
  and waveform. DuckStation's audio capture is decoded with
  `tests/mf_to_wav.c` (Windows Media Foundation).
- `python tests/savestate.py`: saves a state (the dining hall, a movie,
  the castle on disc 2) and plays on, loads it in a new run with the same
  buttons afterwards and compares the pictures (`build/test/savestate/`;
  the dining hall and disc 2: identical). In a puzzle, saving must be
  refused.
- `tests/test_mdec.c`: decodes an STR frame on its own with `mdec.c`.

## Debugging
Environment variables: `D_AUTOSTART=1` (starts the game at once),
`D_INPUT=<frame>[-<last>]:<button>,…` (presses buttons),
`D_VRAM_DUMP=<frame>,…` (`vram_<n>.bmp` and `shot_<n>.bmp`),
`D_WAV_DUMP=<file>` (records what is heard), `D_MUTE=1`,
`D_LEAVE=<frame>`, `D_PAUSE=<frame>:<ms>` (pauses as P does),
`D_SAVE_STATE=<frame>:<slot>` (saves a state without the pause screen),
`D_LOAD_STATE=<slot>` (with `D_AUTOSTART`: starts from a state),
`D_SAVE_DIR=<folder>` (states somewhere other than `saves/`),
`D_EVENT=<vblank>:<event>[@<location>]` (starts a puzzle or a system
screen, for example `75` = disc change), `D_NATIVE=1` (original
resolution, no enhancements) and `D_SOUND_LOG=1` (logs every `SsVoKeyOn`
with the program's tones). The log is written to `d.log` next to the exe;
functions that are not written yet log their first call there.

See `THIRD_PARTY.md` for the licences.
