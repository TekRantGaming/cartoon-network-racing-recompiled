#pragma once
// Player 1 input: an SDL gamepad mapped 1:1 to a DualShock 2, plus a fixed keyboard layout.
// Control remapping belongs to the future launcher; the layout here is only a sensible default.
#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>

void cnrInputInit();
void cnrInputShutdown();
// Main thread: every SDL event, then cnrInputPoll() once per pump.
void cnrInputOnEvent(const SDL_Event &e);
void cnrInputPoll();
// PS2PadProvider (runtime/ps2_pad_provider.h): fills the 32-byte DualShock 2 reply.
bool cnrPadProvider(int port, int slot, uint8_t *data, size_t size);
