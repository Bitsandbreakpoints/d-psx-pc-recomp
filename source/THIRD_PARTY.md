# Third-party code and tools

| Component | Licence | Used for | In the source |
|---|---|---|---|
| llvm-mingw (clang, mingw-w64) | Apache 2.0 with LLVM exception / ZPL and others | compiler | no – downloaded by `tools/setup.py` |
| ninja | Apache 2.0 | build tool | no – downloaded by `tools/setup.py` |
| Dear ImGui 1.92.9b | MIT | launcher and settings | yes, `third_party/imgui/` (licence in `LICENSE.txt`) |
| libchdr (commit 47d7941) | BSD-3-Clause | reading `.chd` disc images | yes, `third_party/libchdr/` (licence in `LICENSE.txt`) |
| – zstd 1.5.7 | BSD-3-Clause (chosen from GPLv2/BSD) | CHD decoding | yes, `third_party/libchdr/deps/zstd-1.5.7/` |
| – LZMA SDK 26.02 (LzmaDec only) | public domain | CHD decoding | yes, `third_party/libchdr/deps/lzma-26.02/` |
| – miniz 3.1.2 | MIT | CHD decoding (zlib) | yes, `third_party/libchdr/deps/miniz-3.1.2/` |
| – dr_flac | public domain / MIT-0 | CHD decoding (FLAC) | yes, inside libchdr |
| stb_image 2.30 | public domain / MIT | decoding the launcher picture (JPEG) | yes, `third_party/stb/stb_image.h` |

## Pictures

None: the launcher's background is drawn in code.  (stb_image is still
part of the source for `ui_image_from_resource()`, which nothing uses at
the moment.)

The game's own files (graphics, sound, data) are not included; they are
read from the player's own discs.
