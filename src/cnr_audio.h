#pragma once
// SPU2 sound on PCSX2's SPU2 (ext/kzspu2), driven by the IOP emulator, played through SDL3.

// Before PS2Runtime::initialize. outputDevice=false emulates the SPU2 without opening a device (automated runs).
// CNR_AUDIO=0 disables sound entirely.
bool cnrAudioInstall(bool outputDevice);
