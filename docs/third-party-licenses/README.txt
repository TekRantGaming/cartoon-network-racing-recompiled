Third-party software in Cartoon Network Racing Recompiled

The port itself is under the GNU General Public License 3.0 (LICENSE.txt). It contains or ships these:

Compiled into the games
  PCSX2                     GPL-3.0 or later   https://github.com/PCSX2/pcsx2 (GS, microVU, IPU and SPU2 code; see that
                                               project for the libraries it builds on)
  PS2Recomp                 GPL-3.0            https://github.com/ran-j/PS2Recomp
  KillzoneRecomp            GPL-3.0            https://github.com/drakolordx7/KillzoneRecomp
  SMAA                      MIT                https://github.com/iryoku/smaa

Shipped beside the games as DLLs, unmodified
  FFmpeg (avcodec, avutil, swresample, swscale)
                            LGPL-2.1 or later  https://ffmpeg.org  Source: https://ffmpeg.org/download.html
  SDL 3                     zlib               https://www.libsdl.org
  libpng, zlib, zstd, libwebp, libjpeg-turbo, brotli, libjxl, shaderc, DirectX Shader Compiler
                            licences below
  Microsoft Visual C++ runtime (msvcp140, vcruntime140)
                            redistributed under the Visual Studio licence terms

Compiled into the launcher
  TRG Launcher              MIT                https://github.com/TekRantGaming/trg-launcher
  SDL 2                     zlib               https://www.libsdl.org
  Dear ImGui                MIT                https://github.com/ocornut/imgui
  stb                       MIT or public domain   https://github.com/nothings/stb

The launcher downloads game artwork from SteamGridDB when it runs. None of it is in this package.

No part of Cartoon Network Racing's own files is included: the game's data comes from the player's disc.

The licence texts follow.
