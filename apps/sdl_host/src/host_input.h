#pragma once

#include <SDL3/SDL.h>

#include "engine_api.h"

struct PresentationTransform {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 1.0f;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;

    bool Contains(float x, float y) const;
};

PresentationTransform CalculatePresentationTransform(
    uint32_t surface_width, uint32_t surface_height, float output_width,
    float output_height);

// Maps an SDL3 keycode to the TVP virtual-key codes expected by the engine
// (the same semantics as apps/godot_app/scripts/main.gd:_kirikiri_virtual_key).
// Returns 0 when the key has no TVP representation.
int SdlKeyToTvpVk(SDL_Keycode key);

// Maps SDL3 key modifiers to the TVP modifier bit mask used by the engine
// (0x01 shift, 0x02 alt, 0x04 ctrl, 0x80 repeat).
int SdlModifiersToTvp(SDL_Keymod mod, bool repeat);

// Sends one SDL event to the engine as an engine_input_event_t.
void ForwardSdlEventToEngine(engine_handle_t engine, const SDL_Event *event,
                             const PresentationTransform &transform);
