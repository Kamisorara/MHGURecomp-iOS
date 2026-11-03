// Player settings (config.h): mhgurecomp.ini in the game folder (next to the executable, or MHGU_HOME; with a game
// folder argument, the current folder). A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1,
//                        MHGU_PAD_POSITIONAL=0|1 (test runs; override the file)
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

// The values a fresh settings file gets. Kept in one place so config_defaults() and g_config can't drift apart.
#define CONFIG_DEFAULT_VALUES                                                                                          \
    DISPLAY_BORDERLESS, 1920, 1080, 0, 0, 60, 1, 1, 1, FACE_POSITION, 39500, "", 1, 1, 1, {{1, 1, 1, 1}, 2}, 1.0f, 0.0f, \
        1.0f, UI_CENTER, 1, 0, SCALE_AUTO, 0, COMBO_LENIENT, TOUCH_AUTO, 1, 1.0f, 0.8f, 1, 1, 1, TOUCH_SHOW_DEFAULT,     \
        {"", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0}, 0

Config g_config = {CONFIG_DEFAULT_VALUES};
Config config_defaults(void) { Config c = {CONFIG_DEFAULT_VALUES}; return c; }

// A hunting party's screen layout: what the settings menu edits before saving (runtime/config_store.c takes the
// snapshot the live getters read). g_config itself keeps what this process started with where a change needs a restart.
Config config_editable(void) { return g_config; }
static volatile int g_window_focused = 1;

int config_live_combo_window(void) {
    return g_config.combo_window;
}

void config_window_focus(int focused) {
    g_window_focused = focused;
}

int config_is_window_focused(void) {
    return g_window_focused;
}
uint32_t g_render_size[2] = {1920, 1080};
float g_render_aspect = 16.0f / 9.0f;
float g_fov_scale = 1.0f;
uint32_t g_ui_size[2] = {1280, 720};
float g_ui_anchor_aspect = 16.0f / 9.0f;

static const char DEFAULT_INI[] =
    "; MHGU Recomp settings. Delete this file to get the defaults back: it is written again at the next start.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed or Borderless (a borderless window covering the whole screen).\n"
    "; Alt+Enter or F11 switches between Windowed and the full-screen mode chosen here.\n"
    "Mode = Borderless\n"
    "\n"
    "; Screen size: WxH (1280x720, 1600x900, 1920x1080, 2560x1440...) or Desktop (the monitor's own resolution).\n"
    "; Fullscreen switches the monitor to this mode, Windowed opens a window this size (it can be resized), and\n"
    "; Borderless always covers the screen, scaling the game's image to it.\n"
    "Resolution = 1920x1080\n"
    "\n"
    "; The game's 3D resolution: Auto (as large as fits Resolution, in the picture's shape below) or WxH. Lower is\n"
    "; faster, higher is sharper; the Switch renders 1920x1080 docked. Above Resolution it supersamples (smoothest at\n"
    "; exactly double, e.g. 3840x2160 for 1920x1080).\n"
    "RenderResolution = Auto\n"
    "\n"
    "; The picture's shape: Auto (the screen's own; the game renders edge to edge at it — wider than 16:9 shows\n"
    "; more to the sides, narrower (an iPad's 4:3) more above and below) or a ratio such as 16:9 (letterboxed on\n"
    "; a screen of another shape) or 21:9.\n"
    "AspectRatio = Auto\n"
    "\n"
    "; Field of view: 1.0 is the game's, above 1.0 (1.1, 1.2) shows more, below 1.0 less. 0.5 to 2.\n"
    "FOV = 1.0\n"
    "\n"
    "; Where the HUD goes on a picture wider than 16:9: Center (in the middle 16:9, as the game lays it out) or Edges\n"
    "; (out to the sides of the screen). 21:9 or 32:9 spread it that wide, at most to the sides.\n"
    "UIAnchor = Center\n"
    "\n"
    "; Stretch: On or Off. Stretches the image to fill the whole screen (useful on 4:3 iPad displays without black bars).\n"
    "Stretch = Off\n"
    "\n"
    "; ScaleMode: Fit, Fill or Stretch (Stretch = On above is the same as Stretch here) — how the picture meets a\n"
    "; screen of another shape, i.e. when AspectRatio above is not Auto. Fit keeps the whole picture centred with\n"
    "; bars around it; Fill zooms it until the screen is full (a little falls outside at the sides); Stretch\n"
    "; distorts it. Auto: Fill on screens narrower than 16:10 (iPads), Fit elsewhere.\n"
    "ScaleMode = Auto\n"
    "\n"
    "[Anti-aliasing]\n"
    "; MSAA: Off, 2x, 4x or 8x. Smooths the edges of 3D geometry and of some cut-out leaves and fences; each step\n"
    "; costs GPU time and video memory, a lot on older cards (an R9 270X at 1080p: 56 fps off, 35 at 2x, 28 at 4x).\n"
    "; F7 cycles through them while playing, to compare. (The Switch version has none.)\n"
    "MSAA = Off\n"
    "\n"
    "; FXAA: On or Off. The game's own edge smoothing: a filter over the finished image, slightly blurry.\n"
    "FXAA = On\n"
    "\n"
    "[Performance]\n"
    "; Frame rate cap: 30 (the Switch version's rate) to 240. Game speed stays the same at any rate.\n"
    "FPS = 60\n"
    "\n"
    "; VSync: On or Off. On: no tearing, but no more frames than the monitor shows (a 60 Hz monitor stays at 60 fps).\n"
    "; Off: frames are shown as soon as they are done, up to FPS (tearing possible).\n"
    "VSync = On\n"
    "\n"
    "[Controls]\n"
    "; Controller face buttons: Position or Label.\n"
    "; Position: the Switch layout the game is made for - the right button is A (confirm), the bottom one B, the left\n"
    ";   one Y, the top one X, whatever is printed on them (on an Xbox controller the in-game icons then don't match).\n"
    "; Label: by the letter printed on the button (an Xbox controller's A is A; the icons match, the layout is awkward).\n"
    "FaceButtons = Position\n"
    "\n"
    "; The controller while the game's window is in the background (a wiki in front of it, or on another screen):\n"
    "; On (it keeps working) or Off. The keyboard and mouse only work while the window has the focus.\n"
    "BackgroundInput = On\n"
    "\n"
    "[Mouse]\n"
    "; Mouse look: click in the game window to capture the mouse (it then turns the camera, as the right stick), Esc\n"
    "; lets it go. Sensitivity: 1 = full stick tilt at a brisk mouse movement; 0.5 slower, 2 faster (0.1 to 10). The\n"
    "; game's own camera options (speed, inverted axes) apply on top.\n"
    "MouseSensitivity = 1.0\n"
    "\n"
    "[Multiplayer]\n"
    "; Local Play (the Switch's wireless play with people nearby) over the network. Create a hub under Local Play to\n"
    "; host: the others reach your computer on this port. Over the internet the host forwards it in their router,\n"
    "; TCP and UDP (TCP alone works too, but copes worse with a bad connection); nobody else needs to.\n"
    "Port = 39500\n"
    "\n"
    "; Search Hubs finds hubs on your LAN by itself. To reach a host elsewhere, put its address here: an IP or name,\n"
    "; with :port if it isn't the Port above. Several: separate them with commas. Example: Join = 203.0.113.7:39500\n"
    "Join =\n"
    "\n"
    "; Small monsters (Jaggi, Aptonoth...) synced between the players in a zone, like large monsters are: On or Off.\n"
    "; Off is the original game: each player sees them in different places. Everyone in the hunt should use the same.\n"
    "SyncSmallMonsters = On\n"
    "\n"
    "[Monster HP]\n"
    "; Large monster HP in Hub quests by the number of hunters on the quest. Each quest sets its monsters' HP as a\n"
    "; multiplier (most Hub quests are made for about two hunters); these factors multiply it: 0.6 = 60% of the\n"
    "; quest's HP, 1 = the original game. Only HP changes: attack, defense, size and the rest stay as they are.\n"
    "; Applies to every quest but the Village ones (played alone anyway): Hub and G Rank, Arena, Special Permit,\n"
    "; Event and Hub Prowler quests. In a hub, everyone uses the host's settings (each game computes the HP itself).\n"
    "1Player = 1.0\n"
    "2Players = 1.0\n"
    "3Players = 1.0\n"
    "4Players = 1.0\n"
    "\n"
    "; One quest set apart: Quest<number> = factors for 1, 2, 3 and 4 hunters (or one factor for all). The log\n"
    "; (log.txt) names the quest number whenever a monster's HP is set, e.g. \"[hpscale] quest 10107, ...\".\n"
    "; Example: Quest10107 = 1.0, 1.0, 1.2, 1.4\n"
    "\n"
    "[Village Quests]\n"
    "; Village quests at the Hub's and the Hunters' Pub's quest counter, to play them together: On or Off. In the\n"
    "; counter's rank menu or star menu, L or R switches between the Hub's quests and the Village quests (the same\n"
    "; list as at a village: what your village progress has unlocked).\n"
    "VillageQuestsInHub = On\n"
    "\n"
    "; Village quests are made for one hunter: posted in the Hub or Pub, their large monsters' HP is multiplied by this\n"
    "; first, then by the [Monster HP] factor for the number of hunters. 1 = as in the village.\n"
    "HubHP = 2.0\n"
    "\n"
    "[Bonus Content]\n"
    "; The free downloads (event and challenge quests, item packs, Palicos) can't be fetched from Capcom's server any\n"
    "; more. On: they are copied from a save that has them (a \"donor\" save file named system, in a folder called bonus\n"
    "; next to mhgurecomp.exe) into a save that has never had any downloads, such as a new one. Only the download data\n"
    "; is written, once; saves that already have downloads are left alone. Without a donor save nothing happens.\n"
    "; Off: saves are never changed.\n"
    "BonusContent = On\n"
    "\n"
    "[Touch]\n"
    "; The controls on the screen (runtime/touch.cpp). Auto: shown while no controller is connected, hidden when one is.\n"
    "; On: always. Off: never.\n"
    "Controls = Auto\n"
    "\n"
    "; MoveHUD: On or Off. While the controls show, the game's own HUD moves out of the thumbs' way: the status to the\n"
    "; left edge, the item bar to the bottom middle, the Hunter Arts and the target camera to the top right.\n"
    "MoveHUD = On\n"
    "\n"
    "; CameraSpeed: the camera's turn for a drag across the screen's height. 1 is about 90 degrees. 0.2 to 5.\n"
    "CameraSpeed = 1.0\n"
    "\n"
    "; Opacity of the controls: 0.2 (faint) to 1 (solid).\n"
    "Opacity = 0.8\n"
    "\n"
    "; Vibration: a short tick when a touch control is pressed. PhoneRumble: the game's rumble on the phone too.\n"
    "Vibration = On\n"
    "PhoneRumble = On\n"
    "\n"
    "; LockOn: the camera stays on the monster, and with a bow or a bowgun the shots turn to it (the target camera's\n"
    "; icon on the screen, or a controller's right stick click, turns it on and off).\n"
    "LockOn = On\n"
    "\n"
    "; Show*: which of the game's buttons are on the screen in a hunt. The attacks, dodge, guard and sheathe are what a\n"
    "; hunt needs; the rest can be turned off to clear the screen.\n"
    "ShowAttacks = On\n"
    "ShowDodge = On\n"
    "ShowGuard = On\n"
    "ShowSheathe = On\n"
    "ShowPlus = On\n"
    "ShowMinus = Off\n"
    "ShowDpad = Off\n"
    "ShowL = Off\n"
    "ShowZL = Off\n"
    "ShowZR = Off\n"
    "ShowL3 = Off\n"
    "ShowR3 = Off\n"
    "ShowStick = On\n"
    "ShowRanged = On\n"
    "ShowFire = On\n"
    "ShowSecond = On\n"
    "ShowAim = On\n"
    "ShowRangedSpecial = On\n"
    "ShowAmmo = On\n"
    "ShowItemArrows = Off\n"
    "\n"
    "; Combo1..Combo6: the game's buttons one screen button presses together (\"A+X\", \"L+R\"); its label is the keys.\n"
    "; Combo1Pad..: a controller's button that does the same (L3, R3, Minus, Guide, Misc, Paddle1..4, Touchpad).\n"
    "; Empty: no such button on the screen.\n"
    "Combo1 =\n"
    "Combo2 =\n"
    "Combo3 =\n"
    "Combo4 =\n"
    "Combo5 =\n"
    "Combo6 =\n"
    "\n"
    "[Application]\n"
    "; Language of the port's own menus: English or Spanish.\n"
    "Language = English\n";

static void ini_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    snprintf(buf, size, "mhgurecomp.ini");  // (started on its own the game runs in its folder, or MHGU_HOME: main.c)
}

// lower case, spaces and '_' / '-' removed: "Borderless Window" == "borderlesswindow"
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') *d++ = (char)tolower((unsigned char)*s);
    *d = 0;
}

static bool parse_size(const char *v, int *w, int *h) {
    int a, b;
    char x;
    if (sscanf(v, "%d %c %d", &a, &x, &b) != 3 || (x != 'x' && x != 'X' && x != '*')) return false;
    if (a < 320 || b < 180 || a > 16384 || b > 16384) return false;
    *w = a, *h = b;
    return true;
}

// [Monster HP] factors: "0.6", "0.6x", or a comma separated list of up to four (squashed: no spaces). The count read,
// 0 when one is not a number in HP_SCALE_MIN..HP_SCALE_MAX.
static int parse_factors(const char *v, float f[4]) {
    int n = 0;
    while (*v && n < 4) {
        char *end;
        float x = strtof(v, &end);
        if (end == v || !(x >= HP_SCALE_MIN && x <= HP_SCALE_MAX)) return 0;
        f[n++] = x;
        v = end + (*end == 'x');
        if (*v == ',') v++;
        else if (*v) return 0;
    }
    return *v ? 0 : n;
}

// The vertical field of view (radians) a camera of this aspect gets (tools/gen.py PATCHES: the three callers of the
// perspective matrix that load a camera's). One made for the screen (its 16:9 aspect replaced with g_render_aspect)
// gets the FOV factor and, on a picture narrower than 16:9 (16:10), the height that keeps 16:9's width (Vert+): more
// above and below instead of less to the sides. Other cameras keep their own.
float camera_fovy(float fovy, float aspect) {
    if (aspect != g_render_aspect) return fovy;
    fovy *= g_fov_scale;
    if (aspect < 16.0f / 9.0f) fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}

// A picture shape: "Auto" (0), "21:9" / "21/9", or a number (0.5 .. 10).
static bool parse_aspect(const char *v, float *out) {
    float n, d, a;
    char sep, *end;
    if (!strcasecmp(v, "auto")) { *out = 0; return true; }
    if (sscanf(v, "%f %c %f", &n, &sep, &d) == 3 && (sep == ':' || sep == '/') && n > 0 && d > 0) {
        a = n / d;
    } else {
        a = strtof(v, &end);
        if (end == v || *end) return false;
    }
    if (a < 0.5f || a > 10.0f) return false;
    *out = a;
    return true;
}

// UIAnchor: Center (also 1, 16:9), 21:9 (2), 32:9 (3) or Edges, as Config.ui_anchor's UI_*
static bool parse_anchor(const char *v, int *out) {
    if (!strcasecmp(v, "center") || !strcmp(v, "1") || !strcmp(v, "16:9")) *out = UI_CENTER;
    else if (!strcmp(v, "2") || !strcmp(v, "21:9")) *out = UI_21_9;
    else if (!strcmp(v, "3") || !strcmp(v, "32:9")) *out = UI_32_9;
    else if (!strcasecmp(v, "edges")) *out = UI_EDGES;
    else return false;
    return true;
}

static int desktop_w, desktop_h;
static bool render_auto = true;

static bool parse_bool(const char *v, int *out) {
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) { *out = 1; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) { *out = 0; return true; }
    return false;
}

// [Touch] Show<button> (the key already squashed, lower case: "showattacks" -> "attacks"). -1: not one of them.
static int touch_show_key(const char *rest, const char *v) {
    static const struct { const char *key; int tb; } keys[] = {
        {"attacks", TB_ATTACKS}, {"dodge", TB_DODGE}, {"guard", TB_GUARD}, {"sheathe", TB_SHEATHE}, {"plus", TB_PLUS},
        {"minus", TB_MINUS}, {"dpad", TB_DPAD}, {"l", TB_L}, {"zl", TB_ZL}, {"zr", TB_ZR}, {"l3", TB_L3}, {"r3", TB_R3},
        {"stick", TB_STICK}, {"ranged", TB_RANGED}, {"fire", TB_FIRE}, {"second", TB_SECOND}, {"aim", TB_AIM},
        {"rangedspecial", TB_RSPECIAL}, {"ammo", TB_AMMO}, {"itemarrows", TB_ITEM_ARROWS},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (!strcmp(rest, keys[i].key)) {
            int on;
            if (!parse_bool(v, &on)) return -1;
            g_config.touch_show[keys[i].tb] = on;
            return keys[i].tb;
        }
    return -1;
}

// [Touch] Combo<n>Pad: the name of a controller's button a combined screen button presses too.
static int pad_button_of(const char *v) {
    static const char *const names[PADB_COUNT] = {"", "l3", "r3", "minus", "guide", "misc", "paddle1", "paddle2",
                                                 "paddle3", "paddle4", "touchpad"};
    for (int i = 1; i < PADB_COUNT; i++)
        if (!strcmp(v, names[i])) return i;
    return PADB_NONE;
}

static void set_key(const char *key, char *val, int line) {
    char k[64], v[256];
    snprintf(k, sizeof k, "%s", key);
    snprintf(v, sizeof v, "%s", val);
    squash(k);
    squash(v);
    bool ok = true;
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "fullscreen") || !strcmp(v, "exclusive") || !strcmp(v, "exclusivefullscreen"))
            g_config.display_mode = DISPLAY_FULLSCREEN;
        else if (!strcmp(v, "windowed") || !strcmp(v, "window")) g_config.display_mode = DISPLAY_WINDOWED;
        else if (!strncmp(v, "borderless", 10)) g_config.display_mode = DISPLAY_BORDERLESS;
        else ok = false;
    } else if (!strcmp(k, "resolution")) {
        if (!strcmp(v, "desktop") || !strcmp(v, "auto")) g_config.width = g_config.height = 0;
        else ok = parse_size(v, &g_config.width, &g_config.height);
    } else if (!strcmp(k, "renderresolution")) {
        if (!strcmp(v, "auto")) render_auto = true;
        else if ((ok = parse_size(v, &g_config.render_w, &g_config.render_h))) render_auto = false;
    } else if (!strcmp(k, "aspectratio")) {
        ok = parse_aspect(v, &g_config.aspect_ratio);
    } else if (!strcmp(k, "fov")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.5f && f <= 2.0f)) g_config.fov = f;
    } else if (!strcmp(k, "uianchor")) {
        ok = parse_anchor(v, &g_config.ui_anchor);
    } else if (!strcmp(k, "msaa")) {
        if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "0") || !strcmp(v, "1") || !strcmp(v, "1x"))
            g_config.msaa = 1;
        else if (!strcmp(v, "2x") || !strcmp(v, "2")) g_config.msaa = 2;
        else if (!strcmp(v, "4x") || !strcmp(v, "4")) g_config.msaa = 4;
        else if (!strcmp(v, "8x") || !strcmp(v, "8")) g_config.msaa = 8;
        else ok = false;
    } else if (!strcmp(k, "backgroundinput")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.background_input = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.background_input = 0;
        else ok = false;
    } else if (!strcmp(k, "muteinbackground")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.mute_in_background = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.mute_in_background = 0;
        else ok = false;
    } else if (!strcmp(k, "comboinputwindow") || !strcmp(k, "combowindow")) {
        if (!strcmp(v, "lenient") || !strcmp(v, "default")) g_config.combo_window = COMBO_LENIENT;
        else if (!strcmp(v, "strict")) g_config.combo_window = COMBO_STRICT;
        else ok = false;
    } else if (!strcmp(k, "stretch")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.stretch = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.stretch = 0;
        else ok = false;
    } else if (!strcmp(k, "scalemode")) {
        if (!strcmp(v, "fit") || !strcmp(v, "aspect")) g_config.scale_mode = SCALE_FIT;
        else if (!strcmp(v, "fill") || !strcmp(v, "crop") || !strcmp(v, "zoom")) g_config.scale_mode = SCALE_FILL;
        else if (!strcmp(v, "stretch")) g_config.scale_mode = SCALE_STRETCH;
        else if (!strcmp(v, "auto")) g_config.scale_mode = SCALE_AUTO;
        else ok = false;
    } else if (!strcmp(k, "fxaa")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fxaa = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fxaa = 0;
        else ok = false;
    } else if (!strcmp(k, "vsync")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.vsync = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.vsync = 0;
        else ok = false;
    } else if (!strcmp(k, "facebuttons")) {
        if (!strcmp(v, "position") || !strcmp(v, "positional") || !strcmp(v, "nintendo"))
            g_config.face_buttons = FACE_POSITION;
        else if (!strcmp(v, "label") || !strcmp(v, "labels") || !strcmp(v, "xbox"))
            g_config.face_buttons = FACE_LABEL;
        else ok = false;
    } else if (!strcmp(k, "mousesensitivity")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.1f && f <= 10.0f)) g_config.mouse_sensitivity = f;
    } else if (!strcmp(k, "port")) {
        int p = atoi(v);
        if (p > 0 && p < 65536) g_config.ldn_port = p;
        else ok = false;
    } else if (!strcmp(k, "join")) {
        snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", val);  // (as written: names keep their dashes)
    } else if (!strcmp(k, "bonuscontent")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.bonus_content = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.bonus_content = 0;
        else ok = false;
    } else if (!strcmp(k, "villagequestsinhub")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.village_in_hub = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.village_in_hub = 0;
        else ok = false;
    } else if (!strcmp(k, "hubhp")) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.village = f[0];
    } else if (!strcmp(k, "syncsmallmonsters")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.sync_small_monsters = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.sync_small_monsters = 0;
        else ok = false;
    } else if (k[0] >= '1' && k[0] <= '4' && (!strcmp(k + 1, "player") || !strcmp(k + 1, "players"))) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.scale[k[0] - '1'] = f[0];
    } else if (!strncmp(k, "quest", 5) && isdigit((unsigned char)k[5])) {
        HpScale *h = &g_config.hp;
        char *end;
        unsigned long q = strtoul(k + 5, &end, 10);
        float f[4];
        int n = parse_factors(v, f);
        if ((ok = !*end && q && (n == 1 || n == 4))) {
            int i = 0;
            while (i < h->quests && h->quest[i].quest != q) i++;
            if (i == HP_QUESTS_MAX) { LOG("[config] line %d: more than %d quests set apart\n", line, HP_QUESTS_MAX); return; }
            h->quest[i].quest = (uint32_t)q;
            for (int j = 0; j < 4; j++) h->quest[i].scale[j] = f[n == 4 ? j : 0];
            if (i == h->quests) h->quests++;
        }
    } else if (!strcmp(k, "fps") || !strcmp(k, "framerate")) {
        int f = atoi(v);
        if (f >= 30 && f <= 240) g_config.fps = f;
        else ok = false;
    } else if (!strcmp(k, "controls")) {  // ([Touch] Controls: no other section has a key of this name)
        if (!strcmp(v, "auto")) g_config.touch_controls = TOUCH_AUTO;
        else if (!strcmp(v, "on") || !strcmp(v, "always")) g_config.touch_controls = TOUCH_ON;
        else if (!strcmp(v, "off") || !strcmp(v, "never")) g_config.touch_controls = TOUCH_OFF;
        else ok = false;
    } else if (!strcmp(k, "movehud") || !strcmp(k, "touchhud")) {
        ok = parse_bool(v, &g_config.touch_hud);
    } else if (!strcmp(k, "cameraspeed")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.2f && f <= 5.0f)) g_config.touch_camera = f;
    } else if (!strcmp(k, "opacity")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.2f && f <= 1.0f)) g_config.touch_opacity = f;
    } else if (!strcmp(k, "vibration")) {
        ok = parse_bool(v, &g_config.touch_vibration);
    } else if (!strcmp(k, "phonerumble")) {
        ok = parse_bool(v, &g_config.phone_rumble);
    } else if (!strcmp(k, "lockon")) {
        ok = parse_bool(v, &g_config.lock_on);
    } else if (!strncmp(k, "show", 4) && (ok = touch_show_key(k + 4, v) >= 0)) {
        (void)ok;
    } else if (!strncmp(k, "combo", 5) && isdigit((unsigned char)k[5])) {
        // Combo<n> = "A+X" ("" none); Combo<n>Pad = a controller's button doing the same
        int n = atoi(k + 5), pad = 0;
        const char *rest = k + 5;
        while (isdigit((unsigned char)*rest)) rest++;
        if (!strcmp(rest, "pad")) pad = 1;
        else ok = !*rest;
        if (ok && (n < 1 || n > TOUCH_COMBOS)) ok = false;
        else if (ok && pad) g_config.touch_combo_pad[n - 1] = pad_button_of(val);
        else if (ok) snprintf(g_config.touch_combo[n - 1], sizeof g_config.touch_combo[0], "%s", val);  // (as written)
    } else if (!strcmp(k, "language")) {
        if (!strcmp(v, "english") || !strcmp(v, "en") || !strcmp(v, "0")) g_config.language = 0;
        else if (!strcmp(v, "spanish") || !strcmp(v, "es") || !strcmp(v, "1")) g_config.language = 1;
        else ok = false;
    } else {
        LOG("[config] line %d: unknown setting \"%s\" (ignored)\n", line, key);
        return;
    }
    if (!ok) LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// The largest size inside w x h matching the given aspect ratio, even numbers.
static void fit_aspect(int w, int h, float aspect, int *rw, int *rh) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if ((float)w / (float)h > aspect)
        w = (int)((float)h * aspect + 0.5f);
    else
        h = (int)((float)w / aspect + 0.5f);
    *rw = w & ~1, *rh = h & ~1;
}

// Sections of DEFAULT_INI the file lacks (written by an older build) are added with their defaults at its end; the
// player's own settings stay as they are.
static void append_missing_sections(const char *path, const char *seen) {
    FILE *o = NULL;
    for (const char *p = strstr(DEFAULT_INI, "\n["); p;) {
        const char *start = p + 1, *end = strstr(start, "\n["), *close = strchr(start, ']');
        p = end;
        char name[64], key[70];
        snprintf(name, sizeof name, "%.*s", (int)(close - start - 1), start + 1);
        for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
        snprintf(key, sizeof key, "|%s|", name);
        if (strstr(seen, key)) continue;
        if (!o && !(o = fopen(path, "a"))) return;
        fprintf(o, "\n%.*s", (int)(end ? end - start : (long)strlen(start)), start);
        LOG("[config] added the [%s] section to %s\n", name, path);
    }
    if (o) fclose(o);
}

void config_load(void) {
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *o = fopen(path, "w");
        if (o) { fputs(DEFAULT_INI, o); fclose(o); LOG("[config] wrote the default settings to %s\n", path); }
        else LOG("[config] %s is missing and could not be written; using the defaults\n", path);
    } else {
        char buf[512], seen[512] = "|";
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (*s == '[' && strchr(s, ']') && strlen(seen) + strlen(s) + 2 < sizeof seen) {
                for (char *q = s + 1; *q != ']'; q++) {
                    size_t n = strlen(seen);
                    seen[n] = (char)tolower((unsigned char)*q), seen[n + 1] = 0;
                }
                strcat(seen, "|");
            }
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
        append_missing_sections(path, seen);
    }

    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    desktop_w = dm ? dm->w : 1920, desktop_h = dm ? dm->h : 1080;
    float screen_w = dm ? (float)(dm->w > dm->h ? dm->w : dm->h) : 1920.0f;  // the screen's landscape shape,
    float screen_h = dm ? (float)(dm->w > dm->h ? dm->h : dm->w) : 1080.0f;  // before the 1080p floor below
#if defined(__APPLE__)
    // On iOS/iPadOS, SDL display modes are returned in UIKit points (e.g. 480x300 or 960x640).
    // Ensure native 1080p dimensions for sharp Retina display rendering!
    if (desktop_w < 1920) {
        desktop_w = 1920;
        desktop_h = 1080;
    }
#endif
    if (!g_config.width) g_config.width = desktop_w, g_config.height = desktop_h;

    const char *e = getenv("MHGU_WINDOW");
    int w, h;
    if (e && parse_size(e, &w, &h)) g_config.display_mode = DISPLAY_WINDOWED, g_config.width = w, g_config.height = h;
    if (getenv("MHGU_HIDDEN")) g_config.display_mode = DISPLAY_WINDOWED;
    if ((e = getenv("MHGU_MSAA"))) g_config.msaa = atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : atoi(e) >= 2 ? 2 : 1;
    if ((e = getenv("MHGU_FXAA"))) g_config.fxaa = atoi(e) != 0;
    if ((e = getenv("MHGU_BACKGROUND_INPUT"))) g_config.background_input = atoi(e) != 0;
    if ((e = getenv("MHGU_STRETCH"))) g_config.stretch = atoi(e) != 0;
    if ((e = getenv("MHGU_SCALEMODE"))) {
        if (!strcasecmp(e, "fit")) g_config.scale_mode = SCALE_FIT;
        else if (!strcasecmp(e, "fill")) g_config.scale_mode = SCALE_FILL;
        else if (!strcasecmp(e, "stretch")) g_config.scale_mode = SCALE_STRETCH;
        else if (!strcasecmp(e, "auto")) g_config.scale_mode = SCALE_AUTO;
    }
    if ((e = getenv("MHGU_PAD_POSITIONAL"))) g_config.face_buttons = atoi(e) ? FACE_POSITION : FACE_LABEL;
    if ((e = getenv("MHGU_LDN_PORT")) && atoi(e) > 0) g_config.ldn_port = atoi(e);
    if ((e = getenv("MHGU_SYNC_ZAKO"))) g_config.sync_small_monsters = atoi(e) != 0;
    if ((e = getenv("MHGU_LDN_JOIN"))) snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", e);
    if ((e = getenv("MHGU_ASPECT"))) parse_aspect(e, &g_config.aspect_ratio);
    if ((e = getenv("MHGU_FOV")) && strtof(e, NULL) >= 0.5f && strtof(e, NULL) <= 2.0f) g_config.fov = strtof(e, NULL);
    if ((e = getenv("MHGU_UI_ANCHOR"))) parse_anchor(e, &g_config.ui_anchor);
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;

    int tw = g_config.width, th = g_config.height;  // (Borderless shows the image at the desktop size)
#if defined(__APPLE__)
    if (tw < 1920) tw = 1920;
    if (th < 1080) th = 1080;
#else
    if (g_config.display_mode == DISPLAY_BORDERLESS) tw = tw < desktop_w ? tw : desktop_w, th = th < desktop_h ? th : desktop_h;
#endif
    // The picture's scale mode: Fit / Fill / Stretch, Auto decided by the screen's shape. Fill zooms until the
    // screen is full (backend_present: the oversized viewport is cropped by the full-screen scissor, no bars on
    // iPads); Fit keeps the whole picture with bars. Stretch = On (the older setting) wins when ScaleMode is Auto.
    if (g_config.stretch && g_config.scale_mode == SCALE_AUTO) g_config.scale_mode = SCALE_STRETCH;
    if (g_config.scale_mode == SCALE_AUTO) {
        float wa = screen_w / screen_h;  // (tw/th is floored to 16:9 on Apple: not the screen's real shape)
        g_config.scale_mode = wa < 1.55f ? SCALE_FILL : SCALE_FIT;
        LOG("[config] ScaleMode Auto: the screen is %.3f wide, using %s\n", wa,
            g_config.scale_mode == SCALE_FILL ? "Fill" : "Fit");
    }
    // The picture's shape: the window's or AspectRatio's. On iOS/iPadOS the window is floored to 16:9 above, so
    // Auto takes the physical screen's shape (screen_w / screen_h, recorded before that floor): the game renders
    // edge to edge — wider than 16:9 shows more to the sides, narrower (an iPad's 4:3) more above and below
    // (Vert+ camera, camera_fovy), the HUD keeps its 16:9 shape flattened onto it (nvn/vulkan/exec.c: the 1280x720
    // canvas floor below is what makes that flattening exact).
#if defined(__APPLE__)
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : screen_w / screen_h;
#else
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : (float)tw / (float)th;
#endif
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;  // (1366x768 and the like)
    if (render_auto) {
        fit_aspect(tw, th, aspect, &w, &h);
    } else {
        fit_aspect(g_config.render_w, g_config.render_h, aspect, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not the picture's shape (%.3f): using %dx%d\n", g_config.render_w,
                g_config.render_h, aspect, w, h);
    }
    g_config.render_w = w, g_config.render_h = h;
    g_render_size[0] = (uint32_t)w;
    g_render_size[1] = (uint32_t)h;
    g_render_aspect = aspect;
    g_fov_scale = g_config.fov;

    // The HUD's width: 16:9 (Center), 21:9, 32:9 or the picture's (Edges), at most the picture's; the game's 2D canvas
    // gets that width (gen.py PATCHES), exec.c places the HUD's parts in it.
    float ui = g_config.ui_anchor == UI_EDGES  ? aspect
             : g_config.ui_anchor == UI_32_9 ? 32.0f / 9.0f
             : g_config.ui_anchor == UI_21_9 ? 21.0f / 9.0f
                                             : 16.0f / 9.0f;
    g_ui_anchor_aspect = ui < aspect ? ui : aspect;
    g_ui_size[1] = 720;
    g_ui_size[0] = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
    if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;

    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d (shape %.3f, FOV x%.2f, HUD %.3f wide), %d fps, vsync %s, MSAA %dx, FXAA %s, "
        "face buttons by %s, controller %s in the background\n", path, modes[g_config.display_mode], g_config.width,
        g_config.height, g_config.render_w, g_config.render_h, g_render_aspect, g_fov_scale, g_ui_anchor_aspect,
        g_config.fps, g_config.vsync ? "on" : "off",
        g_config.msaa, g_config.fxaa ? "on" : "off", g_config.face_buttons == FACE_LABEL ? "label" : "position",
        g_config.background_input ? "on" : "off");
    const HpScale *hp = &g_config.hp;
    LOG("[config] monster HP in Hub quests: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, %d quest(s) set apart\n",
        hp->scale[0], hp->scale[1], hp->scale[2], hp->scale[3], hp->quests);
    LOG("[config] Village quests at the Hub counter: %s (their HP x%.2f first)\n", g_config.village_in_hub ? "on" : "off",
        hp->village);
    config_runtime_init(&g_config);  // (the live copies the game and render threads read: runtime/config_store.c)
}

void config_set_join(const char *join_addr) {
    if (!join_addr) return;
    snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", join_addr);
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    char *content = (char *)malloc((size_t)sz + 1024);
    if (!content) { fclose(f); return; }
    size_t rd = fread(content, 1, (size_t)sz, f);
    content[rd] = 0;
    fclose(f);

    char *line = content;
    bool replaced = false;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char temp[64];
        if (len < sizeof temp) {
            memcpy(temp, line, len);
            temp[len] = 0;
            char *p = temp;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncasecmp(p, "join", 4) && (*(p + 4) == ' ' || *(p + 4) == '\t' || *(p + 4) == '=')) {
                FILE *out = fopen(path, "w");
                if (out) {
                    fwrite(content, 1, (size_t)(line - content), out);
                    fprintf(out, "Join = %s\n", join_addr);
                    if (eol) fwrite(eol + 1, 1, strlen(eol + 1), out);
                    fclose(out);
                    replaced = true;
                }
                break;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    if (!replaced) {
        FILE *out = fopen(path, "a");
        if (out) {
            fprintf(out, "\n[Multiplayer]\nJoin = %s\n", join_addr);
            fclose(out);
        }
    }
    free(content);
    LOG("[config] updated Join = %s in %s\n", join_addr, path);
}
