// Mod folders (mods.h).
#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"
#include "zip_util.h"
#include "sz_util.h"

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

const char *mods_dir(void) {
    static char s_dir[1024];
    if (s_dir[0]) return s_dir;

    const char *e = getenv("MHGU_MODS_DIR");
    if (e && *e && is_dir(e)) {
        snprintf(s_dir, sizeof s_dir, "%s", e);
        return s_dir;
    }
    extern const char *g_game_dir;  // (main.c)
    if (g_game_dir) {
        static char in_game[1100];
        snprintf(in_game, sizeof in_game, "%s/mods", g_game_dir);
        if (is_dir(in_game)) { snprintf(s_dir, sizeof s_dir, "%s", in_game); return s_dir; }
    }
    const char *candidates[] = {
        "mods", "Mods", "MODS",
        "data/mods", "data/Mods", "data/MODS"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (is_dir(candidates[i])) {
            snprintf(s_dir, sizeof s_dir, "%s", candidates[i]);
            return s_dir;
        }
    }
    snprintf(s_dir, sizeof s_dir, "%s", e && *e ? e : "mods");
    return s_dir;
}

void mods_make_dir(void) {
    const char *d = mods_dir();
    if (!is_dir(d)) mkdir(d, 0755);
}

static int cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

#define MAX_MODS 256
static char *g_names[MAX_MODS];
static int g_nmods = -1;

static void add_mod_name(const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < g_nmods; i++) {
        if (!strcasecmp(g_names[i], name)) return;
    }
    if (g_nmods < MAX_MODS) {
        g_names[g_nmods++] = strdup(name);
    }
}

// Find a folder named `want` (case-insensitive) either as `dir` itself or up to `depth` levels below it.
static bool find_dir(const char *dir, const char *want, int depth, char *out, size_t n) {
    if (!dir || !want || depth < 0) return false;
    // Check if dir itself matches want
    const char *slash = strrchr(dir, '/');
    const char *last = slash ? slash + 1 : dir;
    if (!strcasecmp(last, want)) {
        snprintf(out, n, "%s", dir);
        return true;
    }
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    // 1. Check direct children first
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strcasecmp(e->d_name, want)) {
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (is_dir(p)) {
                closedir(d);
                snprintf(out, n, "%s", p);
                return true;
            }
        }
    }
    if (depth == 0) {
        closedir(d);
        return false;
    }
    rewinddir(d);
    // 2. Recurse into subdirectories
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "__MACOSX")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) {
            if (find_dir(p, want, depth - 1, out, n)) {
                closedir(d);
                return true;
            }
        }
    }
    closedir(d);
    return false;
}

static void dump_dir_diag(const char *dir, int depth) {
    if (depth > 2) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                LOG("[mods-diag] %*s[DIR]  %s\n", depth * 2, "", e->d_name);
                if (depth < 2 && strcasecmp(e->d_name, "cache") && strcasecmp(e->d_name, "logs")) {
                    dump_dir_diag(p, depth + 1);
                }
            } else {
                LOG("[mods-diag] %*s[FILE] %s (%lld bytes)\n", depth * 2, "", e->d_name, (long long)st.st_size);
            }
        }
    }
    closedir(d);
}

static bool has_child_ci(const char *parent, const char *want) {
    DIR *d = opendir(parent);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strcasecmp(e->d_name, want)) {
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", parent, e->d_name);
            if (is_dir(p)) { found = true; break; }
        }
    }
    closedir(d);
    return found;
}

static bool has_loose_files(const char *parent) {
    DIR *d = opendir(parent);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".nro") || !strcasecmp(dot, ".ips") || !strcasecmp(dot, ".pchtxt") || !strcasecmp(dot, ".arc"))) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static bool is_reserved_app_dir(const char *name) {
    static const char *const reserved[] = {
        "data", "save", "cheats", "mods", "cache", "logs",
        "Frameworks", "PlugIns", "SharedFrameworks", "System"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (!strcasecmp(name, reserved[i])) return true;
    }
    return false;
}

static bool is_romfs_subfolder(const char *name) {
    static const char *const names[] = {
        "romfs", "nativeNX", "arc", "sound", "loc", "movie",
        "system", "sa", "eng", "fre", "ger", "ita", "spa",
        "chS", "chT", "table", "plugins"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcasecmp(name, names[i])) return true;
    }
    return false;
}

static bool has_direct_romfs_content(const char *dir) {
    static const char *const direct_names[] = {
        "romfs", "nativeNX", "arc", "sound", "loc", "movie",
        "system", "sa", "eng", "fre", "ger", "ita", "spa",
        "chS", "chT", "table", "plugins"
    };
    for (size_t i = 0; i < sizeof(direct_names) / sizeof(direct_names[0]); i++) {
        if (has_child_ci(dir, direct_names[i])) return true;
    }
    return has_loose_files(dir);
}

static void check_and_add_mod(const char *base_dir, const char *entry_name) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", base_dir, entry_name);
    if (!is_dir(p)) return;

    char dummy[1100];
    if (find_dir(p, "romfs", 8, dummy, sizeof dummy) ||
        find_dir(p, "nativeNX", 8, dummy, sizeof dummy) ||
        find_dir(p, "plugins", 8, dummy, sizeof dummy) ||
        find_dir(p, "arc", 8, dummy, sizeof dummy) ||
        find_dir(p, "exefs", 8, dummy, sizeof dummy) ||
        find_dir(p, "0100770008dd8000", 8, dummy, sizeof dummy) ||
        find_dir(p, "0100770008DD8000", 8, dummy, sizeof dummy) ||
        has_loose_files(p)) {
        if (!strcmp(base_dir, ".")) {
            add_mod_name(entry_name);
        } else {
            char rel[1100];
            snprintf(rel, sizeof rel, "%s/%s", base_dir, entry_name);
            add_mod_name(rel);
        }
    }
}

static void scan_dir_for_mods(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while (g_nmods < MAX_MODS && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) add_mod_name(e->d_name);
    }
    closedir(d);
}

static void list_mods(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    const char *mdir = mods_dir();

    // 1. Diagnostic filesystem dump to log.txt for complete visibility
    LOG("[mods-diag] === Mods Filesystem Diagnostic ===\n");
    LOG("[mods-diag] Active mods dir: '%s'\n", mdir);
    dump_dir_diag(mdir, 1);
    LOG("[mods-diag] Documents root ('.'):\n");
    dump_dir_diag(".", 1);
    if (is_dir("data")) {
        LOG("[mods-diag] Data folder ('data'):\n");
        dump_dir_diag("data", 1);
    }

    // 2. Automatically unpack any .zip and .7z archives placed in mods/, Documents, or data/
    zip_extract_all_in_dir(mdir, mdir);
    zip_extract_all_in_dir(".", mdir);
    if (is_dir("data")) zip_extract_all_in_dir("data", mdir);

    sz_extract_all_in_dir(mdir, mdir);
    sz_extract_all_in_dir(".", mdir);
    if (is_dir("data")) sz_extract_all_in_dir("data", mdir);

    // 3. Load order override file
    char path[1100];
    snprintf(path, sizeof path, "%s/load_order.txt", mdir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (g_nmods < MAX_MODS && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            add_mod_name(s);
        }
        fclose(f);
        LOG("[mods] %d mod(s) listed in %s\n", g_nmods, path);
        return;
    }

    // 4. Check if mdir itself directly contains RomFS data (loose arc, sound, loc, nativeNX, romfs)
    bool mdir_has_direct_romfs = has_direct_romfs_content(mdir);
    if (mdir_has_direct_romfs) {
        add_mod_name(".");
    }

    // 5. Scan the active mods directory
    DIR *d = opendir(mdir);
    if (d) {
        struct dirent *e;
        while (g_nmods < MAX_MODS && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            if (e->d_name[0] == '_') {
                LOG("[mods] Note: folder '%s' is disabled (starts with '_'). Remove '_' to enable.\n", e->d_name);
                continue;
            }
            if (mdir_has_direct_romfs && is_romfs_subfolder(e->d_name)) continue;
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", mdir, e->d_name);
            if (is_dir(p)) add_mod_name(e->d_name);
        }
        closedir(d);
    }

    // 6. If data/mods exists and is distinct from mdir, scan it too
    if (strcasecmp(mdir, "data/mods") && is_dir("data/mods")) {
        scan_dir_for_mods("data/mods");
    }

    // 7. Support Atmosphere Switch CFW layout: atmosphere/contents/<title_id>
    const char *atmos_paths[] = { "atmosphere/contents", "mods/atmosphere/contents", "data/atmosphere/contents" };
    for (size_t i = 0; i < sizeof(atmos_paths) / sizeof(atmos_paths[0]); i++) {
        if (is_dir(atmos_paths[i])) {
            DIR *ad = opendir(atmos_paths[i]);
            if (ad) {
                struct dirent *e;
                while (g_nmods < MAX_MODS && (e = readdir(ad))) {
                    if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
                    char p[1100];
                    snprintf(p, sizeof p, "%s/%s", atmos_paths[i], e->d_name);
                    if (is_dir(p)) add_mod_name(p);
                }
                closedir(ad);
            }
        }
    }

    // 8. Support contents/<title_id> directly
    const char *contents_paths[] = { "contents", "mods/contents", "data/contents" };
    for (size_t i = 0; i < sizeof(contents_paths) / sizeof(contents_paths[0]); i++) {
        if (is_dir(contents_paths[i])) {
            DIR *cd = opendir(contents_paths[i]);
            if (cd) {
                struct dirent *e;
                while (g_nmods < MAX_MODS && (e = readdir(cd))) {
                    if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
                    char p[1100];
                    snprintf(p, sizeof p, "%s/%s", contents_paths[i], e->d_name);
                    if (is_dir(p)) add_mod_name(p);
                }
                closedir(cd);
            }
        }
    }

    // 9. Scan current directory (Documents) for any user mod folders placed at top-level
    DIR *cur = opendir(".");
    if (cur) {
        struct dirent *e;
        while (g_nmods < MAX_MODS && (e = readdir(cur))) {
            if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
            if (is_reserved_app_dir(e->d_name)) continue;
            check_and_add_mod(".", e->d_name);
        }
        closedir(cur);
    }

    // 10. Scan data/ for any user mod folders placed inside data/
    if (is_dir("data")) {
        DIR *dd = opendir("data");
        if (dd) {
            struct dirent *e;
            while (g_nmods < MAX_MODS && (e = readdir(dd))) {
                if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
                if (!strcasecmp(e->d_name, "romfs") || !strcasecmp(e->d_name, "exefs") || !strcasecmp(e->d_name, "mods")) continue;
                check_and_add_mod("data", e->d_name);
            }
            closedir(dd);
        }
    }

    qsort(g_names, g_nmods, sizeof *g_names, cmp);
    LOG("[mods] discovered %d mod(s) across '%s'\n", g_nmods, mdir);
    for (int i = 0; i < g_nmods; i++) {
        LOG("[mods]   mod[%d]: %s\n", i + 1, g_names[i]);
    }
}

int mods_romfs_layers(const char **out, int max) {
    list_mods();
    int k = 0;
    const char *mdir = mods_dir();
    for (int i = 0; i < g_nmods && k < max; i++) {
        char mod[1100], path[1100];
        if (!strcmp(g_names[i], ".")) {
            snprintf(mod, sizeof mod, "%s", mdir);
        } else if (g_names[i][0] == '/' || !strncmp(g_names[i], "atmosphere/", 11) || !strncmp(g_names[i], "contents/", 9) || !strncmp(g_names[i], "data/", 5) || strchr(g_names[i], '/')) {
            snprintf(mod, sizeof mod, "%s", g_names[i]);
        } else {
            snprintf(mod, sizeof mod, "%s/%s", mdir, g_names[i]);
            if (!is_dir(mod)) {
                snprintf(mod, sizeof mod, "data/mods/%s", g_names[i]);
                if (!is_dir(mod)) {
                    snprintf(mod, sizeof mod, "data/%s", g_names[i]);
                    if (!is_dir(mod)) {
                        snprintf(mod, sizeof mod, "%s", g_names[i]);
                        if (!is_dir(mod)) {
                            LOG("[mods] %s: no such directory\n", g_names[i]);
                            continue;
                        }
                    }
                }
            }
        }
        if (!is_dir(mod)) continue;

        bool found = false;

        // 1. Search for a 'romfs' directory (depth up to 8 for Atmosphere / Ryujinx / standard mod packages)
        if (find_dir(mod, "romfs", 8, path, sizeof path)) {
            found = true;
        }
        // 2. Search for a 'nativeNX' directory (depth up to 8)
        else if (find_dir(mod, "nativeNX", 8, path, sizeof path)) {
            found = true;
        }
        // 3. Mod directory itself (loose folders/files like arc, loc, sa, system, or flat files)
        else {
            snprintf(path, sizeof path, "%s", mod);
            found = true;
        }

        if (found) {
            out[k++] = strdup(path);
            LOG("[mods] [%d] %s: active romfs layer -> %s\n", k, g_names[i], path);
        }
    }
    return k;
}

// ------------------------------------------------------------------ exefs patches
typedef struct {
    Module *m;
    const char *mod, *file;
    int data, code, bad;
    uint32_t first_code[4];
} Apply;

// One patch: `len` bytes at offset `off` of the module's memory image.
static void apply(Apply *a, uint32_t off, const uint8_t *bytes, uint32_t len) {
    Module *m = a->m;
    if (!len) return;
    if ((uint64_t)off + len > m->size) { a->bad++; return; }
    if (off < m->text_off + m->text_size && off + len > m->text_off) {  // code: compiled ahead, see mods.h
        if (a->code < 4) a->first_code[a->code] = m->base + off;
        a->code++;
        return;
    }
    memcpy(G2H(m->base + off), bytes, len);
    a->data++;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

// Leading hex digits of `s` as a build id prefix: does it match `id`? (Patches name or tag a shortened id.)
static bool id_matches(const char *s, const uint8_t id[32]) {
    int n = 0;
    for (; hexval(s[n]) >= 0 && n < 64; n++)
        if (hexval(s[n]) != ((id[n / 2] >> (n & 1 ? 0 : 4)) & 15)) return false;
    return n >= 16;  // (at least 8 bytes of the id)
}

// IPSwitch text patches: "@nsobid-<id>", "@flag offset_shift <n>", "@little-endian" / "@big-endian",
// "@enabled" / "@disabled" blocks of "<hex offset> <hex bytes | "string">" lines, "@stop", // comments.
// Offsets + offset_shift are IPS offsets (the NSO file's 0x100-byte header included).
static void pchtxt(Apply *a, const char *path, const uint8_t id[32]) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    bool ours = false, on = false, le = false;
    long shift = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *cm = strstr(s, "//");
        if (cm) *cm = 0;
        s[strcspn(s, "\r\n")] = 0;
        if (!*s) continue;
        if (!strncasecmp(s, "@nsobid-", 8)) { ours = id_matches(s + 8, id); continue; }
        if (!strncasecmp(s, "@flag", 5)) {
            char key[64];
            long v;
            if (sscanf(s + 5, "%63s %li", key, &v) == 2 && !strcasecmp(key, "offset_shift")) shift = v;
            continue;
        }
        if (!strncasecmp(s, "@little-endian", 14)) { le = true; continue; }
        if (!strncasecmp(s, "@big-endian", 11)) { le = false; continue; }
        if (!strncasecmp(s, "@enabled", 8)) { on = true; continue; }
        if (!strncasecmp(s, "@disabled", 9)) { on = false; continue; }
        if (!strncasecmp(s, "@stop", 5)) break;
        if (*s == '@' || !ours || !on) continue;
        char *end;
        unsigned long off = strtoul(s, &end, 16);
        if (end == s || !isspace((unsigned char)*end)) continue;
        while (isspace((unsigned char)*end)) end++;
        uint8_t buf[2048];
        uint32_t n = 0;
        if (*end == '"') {  // a string, written without its terminator
            for (char *q = end + 1; *q && *q != '"' && n < sizeof buf; q++) {
                if (*q == '\\' && q[1]) {
                    q++;
                    buf[n++] = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q == '0' ? 0 : (uint8_t)*q;
                } else {
                    buf[n++] = (uint8_t)*q;
                }
            }
        } else {
            for (char *q = end; hexval(q[0]) >= 0 && hexval(q[1]) >= 0 && n < sizeof buf; q += 2)
                buf[n++] = (uint8_t)(hexval(q[0]) << 4 | hexval(q[1]));
            if (le)
                for (uint32_t i = 0; i < n / 2; i++) { uint8_t t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
        }
        long ips = (long)off + shift;
        if (ips < 0x100) { a->bad++; continue; }
        apply(a, (uint32_t)(ips - 0x100), buf, n);
    }
    fclose(f);
}

// IPS ("PATCH", 24-bit offsets, "EOF") and IPS32 ("IPS32", 32-bit offsets, "EEOF"); offsets include the NSO's
// 0x100-byte header. A size of 0 is a run of one byte.
static void ips(Apply *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? len : 1);
    long got = (long)fread(b, 1, len, f);
    fclose(f);
    bool i32 = got >= 5 && !memcmp(b, "IPS32", 5);
    if (!i32 && (got < 5 || memcmp(b, "PATCH", 5))) { a->bad++; free(b); return; }
    int ow = i32 ? 4 : 3;
    long p = 5;
    while (p + ow <= got) {
        if (!memcmp(b + p, i32 ? "EEOF" : "EOF", ow)) break;
        uint32_t off = 0;
        for (int i = 0; i < ow; i++) off = off << 8 | b[p + i];
        p += ow;
        if (p + 2 > got) break;
        uint32_t size = (uint32_t)b[p] << 8 | b[p + 1];
        p += 2;
        uint8_t *data = b + p, run[0x10000];
        if (!size) {
            if (p + 3 > got) break;
            size = (uint32_t)b[p] << 8 | b[p + 1];
            memset(run, b[p + 2], size);
            data = run;
            p += 3;
        } else {
            if (p + size > got) break;
            p += size;
        }
        if (off < 0x100) { a->bad++; continue; }
        apply(a, off - 0x100, data, size);
    }
    free(b);
}

void mods_patch_module(Module *m, const uint8_t build_id[32]) {
    list_mods();
    const char *mdir = mods_dir();
    for (int i = 0; i < g_nmods; i++) {
        char mod[1100], ex[1100];
        if (!strcmp(g_names[i], ".")) {
            snprintf(mod, sizeof mod, "%s", mdir);
        } else if (g_names[i][0] == '/' || !strncmp(g_names[i], "atmosphere/", 11) || !strncmp(g_names[i], "contents/", 9) || !strncmp(g_names[i], "data/", 5) || strchr(g_names[i], '/')) {
            snprintf(mod, sizeof mod, "%s", g_names[i]);
        } else {
            snprintf(mod, sizeof mod, "%s/%s", mdir, g_names[i]);
            if (!is_dir(mod)) {
                snprintf(mod, sizeof mod, "data/mods/%s", g_names[i]);
                if (!is_dir(mod)) {
                    snprintf(mod, sizeof mod, "data/%s", g_names[i]);
                    if (!is_dir(mod)) {
                        snprintf(mod, sizeof mod, "%s", g_names[i]);
                        if (!is_dir(mod)) continue;
                    }
                }
            }
        }
        if (!find_dir(mod, "exefs", 8, ex, sizeof ex)) {
            // Also check if .pchtxt or .ips are placed directly inside the mod directory
            snprintf(ex, sizeof ex, "%s", mod);
        }
        DIR *d = opendir(ex);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", ex, e->d_name);
            Apply a = {m, g_names[i], e->d_name, 0, 0, 0, {0}};
            if (!strcasecmp(dot, ".pchtxt")) pchtxt(&a, path, build_id);
            else if (!strcasecmp(dot, ".ips") && id_matches(e->d_name, build_id)) ips(&a, path);
            else continue;
            if (!a.data && !a.code && !a.bad) continue;  // (for another module or version)
            LOG("[mods] %s: %s for %s: %d data patch(es) applied", a.mod, a.file, m->name, a.data);
            if (a.code) {
                LOG(", %d code patch(es) NOT applied (at", a.code);
                for (int k = 0; k < a.code && k < 4; k++) LOG(" %08x", a.first_code[k]);
                LOG("%s): the port runs the game's code compiled ahead of time, patched instructions change nothing",
                    a.code > 4 ? " ..." : "");
            }
            if (a.bad) LOG(", %d outside the module (ignored)", a.bad);
            LOG("\n");
        }
        closedir(d);
    }
}
