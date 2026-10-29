#pragma once
// IPU (MPEG decoder hardware) through ext/kzipu (PCSX2's IPU). The movie player's libmpeg calls (sceMpeg*) are bound to
// the runtime's HLE, which decodes with FFmpeg; the game then colour-converts the RAW8 macroblocks itself with IPU CSC
// commands over registers and IPU DMA (ch3/ch4), which kzipu serves. CNR_IPU=off disables it.

class PS2Runtime;

// Before PS2Runtime::initialize. Returns true when the IPU is active.
bool cnrIpuInstall();
void cnrIpuBindRuntime(PS2Runtime &runtime);
