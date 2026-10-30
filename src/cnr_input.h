#pragma once
// Player 1 input: an SDL gamepad mapped to a DualShock 2, plus a keyboard. Keys, the gamepad's buttons, the stick
// deadzone and vibration strength come from the launcher's settings file (cnrInputInit reads them).
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
// The game's motors (scePadSetActDirect): small is on or off, large 0..255. Any thread.
void cnrInputSetRumble(bool small, uint8_t large);
// Started by the launcher: holding Back and Start for a second, or Esc for a second and a half, asks to close the game.
void cnrInputEnableReturnHold();
bool cnrInputReturnRequested();
