#include "sdl_render_backend.h"

#include <mutex>
#include <vector>

namespace {

SDL_Renderer *g_sdl_renderer = nullptr;

std::mutex g_released_mutex;
std::vector<SDL_Texture *> g_released_textures;

}  // namespace

void TVPSetSdlRenderer(SDL_Renderer *renderer) { g_sdl_renderer = renderer; }

SDL_Renderer *TVPGetSdlRenderer() { return g_sdl_renderer; }

void TVPQueueSdlTextureRelease(SDL_Texture *tex) {
    if (tex == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_released_mutex);
    g_released_textures.push_back(tex);
}

void TVPFlushReleasedSdlTextures() {
    std::vector<SDL_Texture *> pending;
    {
        std::lock_guard<std::mutex> lock(g_released_mutex);
        pending.swap(g_released_textures);
    }
    for (SDL_Texture *tex : pending) {
        SDL_DestroyTexture(tex);
    }
}
