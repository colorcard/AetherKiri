#include "host_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Engine input event type constants (engine_api.h).
constexpr uint32_t kPointerDown = 1;
constexpr uint32_t kPointerMove = 2;
constexpr uint32_t kPointerUp = 3;
constexpr uint32_t kPointerScroll = 4;
constexpr uint32_t kKeyDown = 5;
constexpr uint32_t kKeyUp = 6;
constexpr uint32_t kTextInput = 7;

// Pointer modifier flags used by the Godot host (main.gd).
constexpr int32_t kPointerModLeft = 0x08;
constexpr int32_t kPointerModRight = 0x10;
constexpr int32_t kPointerModMiddle = 0x20;

engine_input_event_t MakeInputEvent(uint32_t type) {
    engine_input_event_t event;
    memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    event.type = type;
    event.timestamp_micros = SDL_GetTicks() * 1000;
    return event;
}

int MapMouseButton(uint8_t sdl_button) {
    // SDL3: left=1, right=2, middle=3, x1=4, x2=5.
    // Engine expects: left=0, right=1, middle=2 (main.gd:_map_mouse_button).
    switch(sdl_button) {
        case SDL_BUTTON_RIGHT:
            return 1;
        case SDL_BUTTON_MIDDLE:
            return 2;
        default:
            return 0;
    }
}

int32_t PointerModifiersFromButtons(uint32_t sdl_button_mask) {
    int32_t modifiers = 0;
    if((sdl_button_mask & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0)
        modifiers |= kPointerModLeft;
    if((sdl_button_mask & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0)
        modifiers |= kPointerModRight;
    if((sdl_button_mask & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0)
        modifiers |= kPointerModMiddle;
    return modifiers;
}

// Decodes one UTF-8 sequence starting at `text` into `out_codepoint`.
// Returns the number of bytes consumed, or 0 on invalid input.
int DecodeUtf8Codepoint(const char *text, uint32_t *out_codepoint) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
    if(p[0] < 0x80) {
        *out_codepoint = p[0];
        return 1;
    }
    int length = 0;
    uint32_t codepoint = 0;
    if((p[0] & 0xE0) == 0xC0) {
        length = 2;
        codepoint = p[0] & 0x1F;
    } else if((p[0] & 0xF0) == 0xE0) {
        length = 3;
        codepoint = p[0] & 0x0F;
    } else if((p[0] & 0xF8) == 0xF0) {
        length = 4;
        codepoint = p[0] & 0x07;
    } else {
        return 0;
    }
    for(int i = 1; i < length; ++i) {
        if((p[i] & 0xC0) != 0x80)
            return 0;
        codepoint = (codepoint << 6) | (p[i] & 0x3F);
    }
    *out_codepoint = codepoint;
    return length;
}

}  // namespace

bool PresentationTransform::Contains(float x, float y) const {
    return width > 0.0f && height > 0.0f && x >= offset_x && y >= offset_y &&
        x < offset_x + width && y < offset_y + height;
}

PresentationTransform CalculatePresentationTransform(
    uint32_t surface_width, uint32_t surface_height, float output_width,
    float output_height) {
    PresentationTransform result;
    result.surface_width = surface_width;
    result.surface_height = surface_height;
    if(surface_width == 0 || surface_height == 0 || output_width <= 0.0f ||
       output_height <= 0.0f) {
        return result;
    }
    result.scale = std::min(output_width / static_cast<float>(surface_width),
                            output_height / static_cast<float>(surface_height));
    result.width = static_cast<float>(surface_width) * result.scale;
    result.height = static_cast<float>(surface_height) * result.scale;
    result.offset_x = (output_width - result.width) * 0.5f;
    result.offset_y = (output_height - result.height) * 0.5f;
    return result;
}

int SdlKeyToTvpVk(SDL_Keycode key) {
    switch(key) {
        case SDLK_BACKSPACE:
            return 0x08;
        case SDLK_TAB:
            return 0x09;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return 0x0D;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
            return 0x10;
        case SDLK_LCTRL:
        case SDLK_RCTRL:
            return 0x11;
        case SDLK_LALT:
        case SDLK_RALT:
            return 0x12;
        case SDLK_PAUSE:
            return 0x13;
        case SDLK_CAPSLOCK:
            return 0x14;
        case SDLK_ESCAPE:
            return 0x1B;
        case SDLK_SPACE:
            return 0x20;
        case SDLK_PAGEUP:
            return 0x21;
        case SDLK_PAGEDOWN:
            return 0x22;
        case SDLK_END:
            return 0x23;
        case SDLK_HOME:
            return 0x24;
        case SDLK_LEFT:
            return 0x25;
        case SDLK_UP:
            return 0x26;
        case SDLK_RIGHT:
            return 0x27;
        case SDLK_DOWN:
            return 0x28;
        case SDLK_PRINTSCREEN:
            return 0x2C;
        case SDLK_INSERT:
            return 0x2D;
        case SDLK_DELETE:
            return 0x2E;
        case SDLK_HELP:
            return 0x2F;
        default:
            break;
    }
    if(key >= SDLK_F1 && key <= SDLK_F24)
        return 0x70 + (key - SDLK_F1);
    // Printable keys carry their ASCII/Unicode value directly in SDL3.
    if(key >= 'a' && key <= 'z')
        return key - 0x20;  // upper-case like main.gd
    if(key >= 0 && key <= 0xFF)
        return static_cast<int>(key);
    return 0;
}

int SdlModifiersToTvp(SDL_Keymod mod, bool repeat) {
    int modifiers = 0;
    if((mod & SDL_KMOD_SHIFT) != 0)
        modifiers |= 0x01;
    if((mod & SDL_KMOD_ALT) != 0)
        modifiers |= 0x02;
    if((mod & SDL_KMOD_CTRL) != 0)
        modifiers |= 0x04;
    if(repeat)
        modifiers |= 0x80;  // echo
    return modifiers;
}

void ForwardSdlEventToEngine(engine_handle_t engine, const SDL_Event *event,
                             const PresentationTransform &transform) {
    if(engine == nullptr)
        return;

    const auto map_x = [&](float x) {
        return (x - transform.offset_x) /
            std::max(transform.scale, 0.000001f);
    };
    const auto map_y = [&](float y) {
        return (y - transform.offset_y) /
            std::max(transform.scale, 0.000001f);
    };

    switch(event->type) {
        case SDL_EVENT_MOUSE_MOTION: {
            if(!transform.Contains(event->motion.x, event->motion.y))
                break;
            auto event_out = MakeInputEvent(kPointerMove);
            event_out.x = map_x(event->motion.x);
            event_out.y = map_y(event->motion.y);
            event_out.delta_x = event->motion.xrel / transform.scale;
            event_out.delta_y = event->motion.yrel / transform.scale;
            event_out.pointer_id = 0;
            event_out.modifiers = PointerModifiersFromButtons(
                event->motion.state);
            engine_send_input(engine, &event_out);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if(!transform.Contains(event->button.x, event->button.y))
                break;
            const bool pressed =
                event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            auto event_out = MakeInputEvent(pressed ? kPointerDown
                                                    : kPointerUp);
            event_out.x = map_x(event->button.x);
            event_out.y = map_y(event->button.y);
            event_out.pointer_id = 0;
            event_out.button = MapMouseButton(event->button.button);
            int win_w = 0, win_h = 0;
            SDL_GetWindowSize(SDL_GetWindowFromID(event->button.windowID),
                              &win_w, &win_h);
            fprintf(stderr,
                    "[input] %s win=(%.1f,%.1f) winsize=%dx%d "
                    "engine=(%.1f,%.1f) button=%d viewport=(%.1f,%.1f %.1fx%.1f)\n",
                    pressed ? "down" : "up", event->button.x, event->button.y,
                    win_w, win_h, event_out.x, event_out.y, event_out.button,
                    transform.offset_x, transform.offset_y, transform.width,
                    transform.height);
            engine_send_input(engine, &event_out);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if(!transform.Contains(event->wheel.mouse_x,
                                   event->wheel.mouse_y))
                break;
            // SDL3: wheel up -> +y. Engine/Godot expects up -> -delta_y.
            float wheel_y = event->wheel.y;
            if(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                wheel_y = -wheel_y;
            auto event_out = MakeInputEvent(kPointerScroll);
            event_out.x = map_x(event->wheel.mouse_x);
            event_out.y = map_y(event->wheel.mouse_y);
            event_out.delta_y = -wheel_y;
            event_out.pointer_id = 0;
            engine_send_input(engine, &event_out);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool pressed = event->type == SDL_EVENT_KEY_DOWN;
            const auto &key = event->key;
            auto event_out =
                MakeInputEvent(pressed ? kKeyDown : kKeyUp);
            event_out.key_code =
                SdlKeyToTvpVk(key.key);
            event_out.modifiers = SdlModifiersToTvp(
                static_cast<SDL_Keymod>(key.mod), key.repeat != 0);
            // Character input arrives separately via SDL_EVENT_TEXT_INPUT
            // (which also carries IME composition); keep the key event's
            // codepoint at zero so the engine does not duplicate it.
            event_out.unicode_codepoint = 0;
            engine_send_input(engine, &event_out);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            // Forward each codepoint as a separate text-input event.
            const char *text = event->text.text;
            while(*text != '\0') {
                uint32_t codepoint = 0;
                const int n = DecodeUtf8Codepoint(text, &codepoint);
                if(n <= 0)
                    break;
                auto event_out = MakeInputEvent(kTextInput);
                event_out.unicode_codepoint = codepoint;
                engine_send_input(engine, &event_out);
                text += n;
            }
            break;
        }
        default:
            break;
    }
}
