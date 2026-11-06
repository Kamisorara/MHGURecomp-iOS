// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
// Display mode, size and the 3D resolution come from the settings (runtime/config.c, mhgurecomp.ini).
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "mods.h"
#include "rt.h"

volatile uint64_t g_present_count;
static SDL_Window *window;

SDL_Window *ui_window(void) { return window; }
void backend_init(void);

// The window's size when windowed: the settings' size (or the last windowed size), no larger than the desktop's free
// area with the window frame.
static int win_w, win_h;
static void fit_desktop(int *w, int *h) {
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (window) SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    if (!top) top = 32, left = right = bottom = 8;  // (not known before the window exists: typical Windows 10 frame)
    r.w -= left + right, r.h -= top + bottom;
    if (*w <= r.w && *h <= r.h) return;
    int fw = r.w, fh = r.w * *h / *w;
    if (fh > r.h) fh = r.h, fw = r.h * *w / *h;
    LOG("[ui] %dx%d does not fit the desktop: window %dx%d\n", *w, *h, fw, fh);
    *w = fw, *h = fh;
}

// Full screen as the settings say (exclusive: the monitor mode closest to Resolution; else borderless desktop), or
// windowed. The cursor only shows in a window.
static void set_fullscreen(bool on) {
#ifdef __APPLE__
    (void)on;
    // On iOS the SDL window fills the UIWindow/screen natively. Mode switching is not supported.
    return;
#else
    bool was = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    if (on && !was) SDL_GetWindowSize(window, &win_w, &win_h);
    if (on && g_config.display_mode == DISPLAY_FULLSCREEN) {
        SDL_DisplayMode m;
        SDL_DisplayID d = SDL_GetDisplayForWindow(window);
        if (SDL_GetClosestFullscreenDisplayMode(d, g_config.width, g_config.height, 0.0f, false, &m)) {
            SDL_SetWindowFullscreenMode(window, &m);
            LOG("[ui] fullscreen %dx%d @ %.0f Hz\n", m.w, m.h, m.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            LOG("[ui] no fullscreen mode near %dx%d (%s): borderless instead\n", g_config.width, g_config.height,
                SDL_GetError());
        }
    } else if (on) {
        SDL_SetWindowFullscreenMode(window, NULL);
    }
    SDL_SetWindowFullscreen(window, on);
    if (!on && was) {  // (SDL restores the frame's size as the client size after an exclusive mode: set it again)
        SDL_SyncWindow(window);
        fit_desktop(&win_w, &win_h);
        SDL_SetWindowSize(window, win_w, win_h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (on) SDL_HideCursor(); else SDL_ShowCursor();
#endif
}

void ui_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s\n", SDL_GetError());
    config_load();
    // the controller in the background (ini BackgroundInput): SDL's hint, set before its gamepads start (some of its
    // Windows input backends only read it then); keyboard and mouse events only ever reach the focused window
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_config.background_input ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) FATAL("SDL_InitSubSystem: %s\n", SDL_GetError());
    win_w = g_config.width, win_h = g_config.height;
#ifndef __APPLE__
    fit_desktop(&win_w, &win_h);
#else
    // On iOS/iPadOS, query the screen display mode directly to get full display size in points
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!dm) dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm && dm->w > 0 && dm->h > 0) {
        win_w = dm->w;
        win_h = dm->h;
    }
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
    // On iOS, force borderless fullscreen so the view fills the entire iPad screen natively
    flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN;
#endif
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", win_w, win_h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
#ifndef __APPLE__
    if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
#endif
    backend_init();
}

// Alt+Enter: windowed <-> the full-screen mode of the settings (Windowed in the settings: borderless).
void ui_toggle_fullscreen(void) {
    set_fullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// iOS: the app to the background (or gone for good). iOS forbids submitting GPU work from the background, and a
// submission there loses the device (MoltenVK: "Insufficient Permission ... BackgroundExecutionNotPermitted"): the
// game stops (runtime/hle/vi.c's pause, its guest clock held) before the app is truly backgrounded, and goes on
// where it was when it is back. (An event watch: acted on before the call returns.)
static bool ios_lifecycle(void *ud, SDL_Event *e) {
    (void)ud;
    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_TERMINATING:
        vi_set_paused(true);
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        vi_set_paused(false);
        break;
    default: break;
    }
    return true;
}
#endif

void vi_set_paused(bool on);  // (runtime/hle/vi.c: the game's pause in the background)
int ui_run(void) {
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    SDL_AddEventWatch(ios_lifecycle, NULL);
#endif
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        host_input_update();
        cheats_frame();
    }
}
