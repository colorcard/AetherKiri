#pragma once

#include <SDL3/SDL.h>

#include "engine_api.h"

// Maps an SDL3 keycode to the TVP virtual-key codes expected by the engine
// (the same semantics as apps/godot_app/scripts/main.gd:_kirikiri_virtual_key).
// Returns 0 when the key has no TVP representation.
int SdlKeyToTvpVk(SDL_Keycode key);

// Maps SDL3 key modifiers to the TVP modifier bit mask used by the engine
// (0x01 shift, 0x02 alt, 0x04 ctrl, 0x80 repeat).
int SdlModifiersToTvp(SDL_Keymod mod, bool repeat);

// Sends one SDL event to the engine as an engine_input_event_t.
void ForwardSdlEventToEngine(engine_handle_t engine, const SDL_Event *event,
                             float surface_scale_x, float surface_scale_y);
