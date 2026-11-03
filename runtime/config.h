// Player settings: mhgurecomp.ini in the game folder (written with the defaults when it is missing).
#pragma once
#include <stdint.h>
#include <stddef.h>

// A phone (iOS, Android): no mouse to capture, no window modes, no keyboard. The touch controls draw over the game and
// move the HUD out of the thumbs' way (runtime/touch.cpp). Files shared with the desktop builds ask this, not
// __ANDROID__, which iOS never defines.
#if defined(__ANDROID__)
#define MHGU_PHONE 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#define MHGU_PHONE TARGET_OS_IPHONE  // 1 on iOS and iPadOS, 0 on the Mac
#else
#define MHGU_PHONE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { DISPLAY_WINDOWED, DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS };
enum { FACE_POSITION, FACE_LABEL };
enum { COMBO_LENIENT, COMBO_STRICT };
enum { SCALE_FIT, SCALE_FILL, SCALE_STRETCH, SCALE_AUTO };  // how the picture meets a screen of another shape
enum { TOUCH_AUTO, TOUCH_ON, TOUCH_OFF };  // [Touch] Controls (runtime/touch.cpp)

// The game's buttons, for showing each on the screen or not ([Touch] Show*: runtime/touch.cpp)
enum { TB_ATTACKS, TB_DODGE, TB_GUARD, TB_SHEATHE, TB_PLUS, TB_MINUS, TB_DPAD, TB_L, TB_ZL, TB_ZR, TB_L3, TB_R3, TB_STICK,
       TB_RANGED, TB_FIRE, TB_SECOND, TB_AIM, TB_RSPECIAL, TB_AMMO, TB_ITEM_ARROWS, TB_COUNT };
#define TOUCH_SHOW_DEFAULT {1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0}
#define TOUCH_COMBOS 6
// A controller's buttons a combined button can be on (the less used ones)
enum { PADB_NONE, PADB_L3, PADB_R3, PADB_MINUS, PADB_GUIDE, PADB_MISC, PADB_PADDLE1, PADB_PADDLE2, PADB_PADDLE3, PADB_PADDLE4,
       PADB_TOUCHPAD, PADB_COUNT };

// [Monster HP]: factors on the HP multiplier of Hub-type quests' large monsters
#define HP_QUESTS_MAX 256
#define HP_SCALE_MIN 0.05f
#define HP_SCALE_MAX 20.0f
typedef struct {
    float scale[4];  // by the number of hunters (index: hunters - 1)
    float village;   // Village quests posted in the Hub / Pub: this first, then scale ([Village Quests] HubHP)
    int quests;
    struct { uint32_t quest; float scale[4]; } quest[HP_QUESTS_MAX];  // per quest, in place of scale
} HpScale;

typedef struct {
    int display_mode;         // DISPLAY_*
    int width, height;        // screen / window size (Fullscreen: the monitor mode)
    int render_w, render_h;   // the game's 3D resolution (16:9)
    int fps;                  // frame rate cap: 30..240
    int vsync;                // 1: present without tearing (mailbox / FIFO, at most the refresh rate), 0: immediate
    int msaa;                 // samples of the 3D scene: 1 (off), 2, 4, 8
    int fxaa;                 // the game's own FXAA pass: 1 on, 0 off
    int face_buttons;         // FACE_*: controller face buttons by position (the Switch layout) or by their labels
    int ldn_port;             // local play: the port a hosted hub listens on (TCP; UDP for LAN search)
    char ldn_join[256];       // local play: hosts to search besides the LAN (host[:port], comma separated)
    int sync_small_monsters;  // multiplayer: small monsters synced like large ones (runtime/net/emsync.c)
    int bonus_content;        // free downloads copied from a donor save into saves without any (runtime/services/bonus.c)
    int village_in_hub;       // L / R at the Hub's / Pub's quest counter switch to Village quests (runtime/hle/villagehub.c)
    HpScale hp;               // large monster HP in Hub-type quests (runtime/hle/hpscale.c)
    float mouse_sensitivity;  // mouse look: the speed for full right stick tilt is divided by it (runtime/host_input.c)
    float aspect_ratio;       // the picture's shape (width / height), 0: the window's; never below 16:10 (config.c)
    float fov;                // 3D camera field of view factor (1: the game's)
    int ui_anchor;            // where the HUD goes on a picture wider than 16:9: UI_*
    int background_input;     // the controller keeps working while the window is in the background (runtime/ui.c)
    int stretch;              // 1: stretch to fill entire screen (no letterboxing), 0: maintain aspect ratio
    int scale_mode;           // SCALE_*: Fit (bars), Fill (zoom, cropped by the scissor) or Stretch; Auto picks per screen
    int mute_in_background;   // [Audio] MuteInBackground: no sound while the window is not focused
    int combo_window;         // [Controls] ComboInputWindow: COMBO_LENIENT / COMBO_STRICT
    // [Touch]: the controls on a phone's screen (runtime/touch.cpp)
    int touch_controls;       // TOUCH_*: Auto (no controller connected), On, Off
    int touch_hud;            // MoveHUD: the HUD moves out of the thumbs' way while they show
    float touch_camera;       // CameraSpeed: the camera's turn for a drag (1: about 90 degrees a screen height)
    float touch_opacity;      // Opacity of the controls, 0.2 .. 1
    int touch_vibration;      // Vibration: a short tick when a control is pressed (runtime/haptics.c)
    int phone_rumble;         // PhoneRumble: the game's rumble on the phone's vibrator too
    int lock_on;              // LockOn: the camera stays on the monster, and with it a bow's or gun's shots
    int touch_show[TB_COUNT]; // Show*: which of the game's buttons are on the screen in a hunt
    char touch_combo[TOUCH_COMBOS][32];  // Combo1..: the buttons one screen button presses together ("A+X"; "" none)
    int touch_combo_pad[TOUCH_COMBOS];   // Combo1Pad..: PADB_*, a controller's button that does the same
    int language;             // 0: English, 1: Spanish (the port's own menus only)
} Config;
uint32_t config_combo_mask(const char *text);  // "A+X" -> the BTN_* bits, 0 if there is nothing or anything else in it
void config_combo_text(uint32_t mask, char *out, size_t size);  // and back
// The phone's low-end switch sets these two (runtime/touch.cpp, the settings menu)
#define LOW_END_SCALE 40  // RenderScale: the 3D picture in percent of the screen
#define LOW_END_FPS 30
enum { UI_CENTER = 1, UI_21_9, UI_32_9, UI_EDGES };  // the middle 16:9, 21:9, 32:9, the picture's sides

extern Config g_config;
extern uint32_t g_render_size[2];  // read by the game code (tools/gen.py PATCHES: its docked render size)
extern float g_render_aspect;      // the picture's shape, 16:10 or wider: the 3D cameras' aspect (tools/gen.py PATCHES)
extern float g_fov_scale;          // 3D camera field of view factor (tools/gen.py PATCHES)
extern uint32_t g_ui_size[2];      // the game's 2D canvas size (tools/gen.py PATCHES: 0xdbcd48, 0xdbcd54)
extern float g_ui_anchor_aspect;   // the HUD's width / height, 16:9 .. g_render_aspect (nvn/vulkan/exec.c)
float camera_fovy(float fovy, float aspect);  // a 3D camera's vertical field of view for its aspect (gen.py PATCHES)

void config_load(void);  // before the window and the guest start
void config_set_join(const char *join_addr);
int config_live_combo_window(void);
void config_window_focus(int focused);
int config_is_window_focused(void);
// What the settings menu edits (runtime/settings_panel.cpp): the file's values before the environment overrides and the
// automatic resolution are worked out, and a copy of g_config to change and save.
Config config_editable(void);
Config config_defaults(void);
void config_runtime_init(const Config *effective);  // publish the live copies (runtime/config_store.c)

// Live settings: read through an atomic from any thread (the render and game threads included), so a change in the
// menu takes effect at once, without a lock (runtime/config_store.c).
int config_live_touch_controls(void);  // [Touch]
int config_live_touch_hud(void);
int config_live_lock_on(void);
int config_live_touch_show(int button);       // TB_*
uint32_t config_live_touch_combo(int index);  // its buttons (config_combo_mask)
int config_live_touch_combo_pad(int index);   // PADB_*
float config_live_touch_camera(void);
float config_live_touch_opacity(void);
int config_live_touch_vibration(void);
int config_live_phone_rumble(void);
float config_live_mouse_sensitivity(void);
int config_live_face_buttons(void);
int config_language(void);  // the port's own menus' language, atomic across host threads

#ifdef __cplusplus
}
#endif
