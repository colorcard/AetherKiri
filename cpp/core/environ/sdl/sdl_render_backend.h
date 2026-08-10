#pragma once

#include <SDL3/SDL.h>

// ---------------------------------------------------------------------------
// SDL renderer injection for the engine's built-in render backend.
//
// The host creates the SDL_Renderer and injects it here; the engine's render
// manager (core/visual) then creates/paints its own SDL textures directly on
// it (krkrz-style in-engine rendering) instead of going through a host
// callback table. Presentation stays with the host.
// ---------------------------------------------------------------------------

void TVPSetSdlRenderer(SDL_Renderer *renderer);
SDL_Renderer *TVPGetSdlRenderer();

// Textures released by the engine are deferred until the host calls
// TVPFlushReleasedSdlTextures() after its present, so a frame still being
// presented is never destroyed mid-frame.
void TVPQueueSdlTextureRelease(SDL_Texture *tex);
void TVPFlushReleasedSdlTextures();
