# Third-party code and pictures

The release (`d.exe` and `game.dll`) is built with the following open-source
code. The full licence texts are in the `licenses` folder of the release.

| Component | Licence | Used for |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | MIT | launcher, settings, pause menu |
| [libchdr](https://github.com/rtissera/libchdr) (commit 47d7941) | BSD-3-Clause | reading `.chd` disc images |
| – zstd 1.5.7 | BSD-3-Clause (chosen from GPLv2/BSD) | CHD decoding |
| – LZMA SDK 26.02 (LzmaDec only) | public domain | CHD decoding |
| – miniz 3.1.2 | MIT | CHD decoding (zlib), packing the save states |
| – dr_flac | public domain / MIT-0 | CHD decoding (FLAC) |
| [stb_image](https://github.com/nothings/stb) 2.30 | public domain / MIT | (built in, not used at the moment) |

It is compiled with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(clang and mingw-w64) and built with [ninja](https://ninja-build.org/);
neither is part of the program.

## Pictures

| Picture | Author | Licence | Source |
|---|---|---|---|
| `images/launcher.png`, `images/load-game.png` | the port's launcher (its background is drawn in code) | – | – |
| `images/save-states.png`, `images/pause-menu.png` | the port's pause menu over the game | the game's pictures are © their respective owners; shown for identification | – |
| `images/title.png`, `images/dining-hall.png`, `images/inventory.png`, `images/puzzle.png`, `images/disc-2.png` | pictures of the game running in this port | the game's pictures are © their respective owners; shown for identification | – |
| `images/cover.jpg` | front cover of *D (USA)*, SLUS-00128 | © its respective owners; shown for identification | [PSX Data Center](https://psxdatacenter.com/games/U/D/SLUS-00128.html) |

The game's own files (movies, pictures, sound, data) are not included
anywhere in this repository or the release; they are read from the player's
own discs.
