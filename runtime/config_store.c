// Live copies of the settings the game and render threads read while the host may change them (runtime/touch.cpp,
// runtime/host_input.c). g_config keeps what this process started with; these atomics hold the values to read from
// any thread without a lock, published once at start-up here, and by the settings menu when it saves.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>

#include "config.h"

static SDL_AtomicInt live_touch_controls, live_touch_hud, live_lock_on, live_touch_vibration, live_phone_rumble,
    live_language;
static SDL_AtomicInt live_touch_show[TB_COUNT];
static SDL_AtomicInt live_touch_combo[TOUCH_COMBOS], live_touch_combo_pad[TOUCH_COMBOS];
static SDL_AtomicInt live_mouse_sens, live_touch_cam, live_touch_opacity;  // floats as their bits

static void set_float(SDL_AtomicInt *a, float f) {
    union { float f; int32_t i; } u = {.f = f};
    SDL_SetAtomicInt(a, u.i);
}
static float get_float(const SDL_AtomicInt *a) {
    union { int32_t i; float f; } u = {.i = SDL_GetAtomicInt((SDL_AtomicInt *)a)};
    return u.f;
}

// The game's buttons, as the [Touch] Combo* file writes them ("A+X"), in hle/input.h BTN_* order.
static const char *const COMBO_NAMES[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR",
                                          "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

uint32_t config_combo_mask(const char *text) {
    uint32_t mask = 0;
    while (*text) {
        const char *end = strchr(text, '+');
        size_t n = end ? (size_t)(end - text) : strlen(text);
        bool known = false;
        for (int i = 0; i < 16; i++)
            if (strlen(COMBO_NAMES[i]) == n && !strncmp(text, COMBO_NAMES[i], n)) mask |= 1u << i, known = true;
        if (!known) return 0;
        text += n + (end ? 1 : 0);
    }
    return mask;
}
void config_combo_text(uint32_t mask, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 16; i++)
        if (mask >> i & 1) n += (size_t)snprintf(out + n, size - n, "%s%s", n ? "+" : "", COMBO_NAMES[i]);
}

// After the ini, the environment and the automatic resolution are worked out (config_load): the values the rest of
// the process starts with.
void config_runtime_init(const Config *c) {
    SDL_SetAtomicInt(&live_touch_controls, c->touch_controls);
    SDL_SetAtomicInt(&live_touch_hud, c->touch_hud);
    SDL_SetAtomicInt(&live_lock_on, c->lock_on);
    SDL_SetAtomicInt(&live_touch_vibration, c->touch_vibration);
    SDL_SetAtomicInt(&live_phone_rumble, c->phone_rumble);
    SDL_SetAtomicInt(&live_language, c->language);
    set_float(&live_mouse_sens, c->mouse_sensitivity);
    set_float(&live_touch_cam, c->touch_camera);
    set_float(&live_touch_opacity, c->touch_opacity);
    for (int i = 0; i < TB_COUNT; i++) SDL_SetAtomicInt(&live_touch_show[i], c->touch_show[i]);
    for (int i = 0; i < TOUCH_COMBOS; i++) {
        SDL_SetAtomicInt(&live_touch_combo[i], (int)config_combo_mask(c->touch_combo[i]));
        SDL_SetAtomicInt(&live_touch_combo_pad[i], c->touch_combo_pad[i]);
    }
}

int config_live_touch_controls(void) { return SDL_GetAtomicInt(&live_touch_controls); }
int config_live_touch_hud(void) { return SDL_GetAtomicInt(&live_touch_hud); }
int config_live_lock_on(void) { return SDL_GetAtomicInt(&live_lock_on); }
int config_live_touch_show(int button) { return SDL_GetAtomicInt(&live_touch_show[button]); }
uint32_t config_live_touch_combo(int index) { return (uint32_t)SDL_GetAtomicInt(&live_touch_combo[index]); }
int config_live_touch_combo_pad(int index) { return SDL_GetAtomicInt(&live_touch_combo_pad[index]); }
float config_live_touch_camera(void) { return get_float(&live_touch_cam); }
float config_live_touch_opacity(void) { return get_float(&live_touch_opacity); }
int config_live_touch_vibration(void) { return SDL_GetAtomicInt(&live_touch_vibration); }
int config_live_phone_rumble(void) { return SDL_GetAtomicInt(&live_phone_rumble); }
float config_live_mouse_sensitivity(void) { return get_float(&live_mouse_sens); }
int config_live_face_buttons(void) { return g_config.face_buttons; }  // (read before the pads are: no live change yet)
int config_language(void) { return SDL_GetAtomicInt(&live_language); }
