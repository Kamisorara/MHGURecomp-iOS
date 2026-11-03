// Darwin / macOS / iOS host layer (runtime/platform.h).
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "../platform.h"

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#endif

// ---- guest address space
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull
#define MAX_CHUNKS ((0x100000000ull + COMMIT_CHUNK - 1) / COMMIT_CHUNK)

enum { CHUNK_FREE = 0, CHUNK_BUSY, CHUNK_READY };
static _Atomic unsigned char g_chunk[MAX_CHUNKS];

static bool chunk_back(uint64_t base) {
    void *p = mmap((void *)(uintptr_t)base, COMMIT_CHUNK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == (void *)(uintptr_t)base) return true;
    return mprotect((void *)(uintptr_t)base, COMMIT_CHUNK, PROT_READ | PROT_WRITE) == 0;
}

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    uint64_t idx = (a - g_lo) / COMMIT_CHUNK;
    if (idx >= MAX_CHUNKS) return false;
    unsigned char expect = CHUNK_FREE;
    if (atomic_compare_exchange_strong(&g_chunk[idx], &expect, CHUNK_BUSY)) {
        bool ok = chunk_back(g_lo + idx * COMMIT_CHUNK);
        atomic_store(&g_chunk[idx], ok ? CHUNK_READY : CHUNK_FREE);
        return ok;
    }
    while (atomic_load(&g_chunk[idx]) == CHUNK_BUSY) atomic_thread_fence(memory_order_acquire);
    return atomic_load(&g_chunk[idx]) == CHUNK_READY;
}

static void fault_handler(int sig, siginfo_t *si, void *uc);

uintptr_t g_guest_base = 0;

bool plat_reserve_guest(uint64_t base, uint64_t size) {
    void *p = MAP_FAILED;

    // 1. If a fixed base was specified, try it first
    if (base != 0) {
        p = mmap((void *)(uintptr_t)base, size, PROT_NONE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (p == (void *)(uintptr_t)base) {
            fprintf(stderr, "[plat] Successfully reserved 4 GB guest space at fixed address %p\n", p);
        } else {
            p = MAP_FAILED;
        }
    }

    // 2. If fixed base was not specified or failed (e.g. unprivileged iOS virtual address limits),
    // let the kernel allocate 4 GB anywhere in the process!
    if (p == MAP_FAILED) {
        p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            fprintf(stderr, "[plat] Successfully dynamically reserved 4 GB guest space at %p\n", p);
        }
    }

    if (p == MAP_FAILED) {
        fprintf(stderr, "[plat] FATAL: failed to reserve 4 GB virtual guest space: %s (errno %d)\n", strerror(errno), errno);
        return false;
    }

    g_guest_base = (uintptr_t)p;
    g_lo = g_guest_base;
    g_hi = g_guest_base + size;
    fprintf(stderr, "[plat] Guest memory active: 0x%llx - 0x%llx\n", (unsigned long long)g_lo, (unsigned long long)g_hi);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~(COMMIT_CHUNK - 1), e = (addr + size + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
    for (uint64_t c = a; c < e && c < g_hi; c += COMMIT_CHUNK) lazy_commit(c);
    if (e > g_hi)
        fprintf(stderr, "[plat] commit %llx+%llx past the guest window\n", (unsigned long long)addr,
                (unsigned long long)size);
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

bool plat_committed(uint64_t addr, uint64_t *end) {
    if (!g_lo || addr < g_lo || addr >= g_hi) { *end = addr + PLAT_PAGE; return false; }
    uint64_t idx = (addr - g_lo) / COMMIT_CHUNK;
    if (atomic_load(&g_chunk[idx]) != CHUNK_READY) {
        *end = g_lo + (idx + 1) * COMMIT_CHUNK;
        return false;
    }
    uint64_t n = idx;
    while (n + 1 < MAX_CHUNKS && n - idx < 4096 && atomic_load(&g_chunk[n + 1]) == CHUNK_READY) n++;
    *end = g_lo + (n + 1) * COMMIT_CHUNK;
    return true;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return mprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { sched_yield(); continue; }
        struct timespec due = {(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
        nanosleep(&due, NULL);
    }
}

// ---- process
void plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t sz = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        char real[PATH_MAX];
        if (realpath(buf, real)) {
            snprintf(buf, size, "%s", real);
        }
        return true;
    }
    return false;
}

static bool g_own_log;

static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        struct stat st;
        if (stat(buf, &st) != 0) {
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
        }
        *s = '/';
    }
    struct stat st;
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(buf, 0777) == 0 || errno == EEXIST;
}

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

void plat_setup_ios_documents(void) {
    @autoreleasepool {
        static bool s_log_opened = false;
        if (s_log_opened) return;
        s_log_opened = true;

        NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *docPath = [paths firstObject];
        if (!docPath || [docPath length] == 0) return;

        NSFileManager *fm = [NSFileManager defaultManager];

        // 1. Ensure Documents root directory exists
        [fm createDirectoryAtPath:docPath withIntermediateDirectories:YES attributes:nil error:nil];

        // 2. Rotate previous log to log.prev.txt so crash diagnostics are NEVER lost on restart!
        NSString *logPath = [docPath stringByAppendingPathComponent:@"log.txt"];
        NSString *prevPath = [docPath stringByAppendingPathComponent:@"log.prev.txt"];
        [fm removeItemAtPath:prevPath error:nil];
        [fm moveItemAtPath:logPath toPath:prevPath error:nil];

        // 3. Open log.txt with synchronous POSIX descriptor and unbuffered stdio
        int fd = open([logPath UTF8String], O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0666);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        freopen([logPath UTF8String], "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
        freopen([logPath UTF8String], "a", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);
        g_own_log = true;

        // 4. CRUCIAL FOR iOS FILES APP: Write README directly into Documents/
        // Apple's fileproviderd hides app containers under "On My iPad" if Documents is empty!
        NSString *readmePath = [docPath stringByAppendingPathComponent:@"README.txt"];
        NSString *readmeContent =
            @"======================================================\n"
            @" Monster Hunter Generations Ultimate (MHGU Recomp)\n"
            @" Native Switch ARM64 iOS / iPadOS Port\n"
            @"======================================================\n\n"
            @"HOW TO PLAY:\n\n"
            @"1. Copy your Nintendo Switch v1.4.0 game dump into 'data/':\n"
            @"     • data/exefs/main       (decompiled game executable)\n"
            @"     • data/exefs/main.npdm  (game metadata)\n"
            @"     • data/exefs/sdk        (if present in your dump)\n"
            @"     • data/romfs/           (all game romfs asset folders)\n\n"
            @"2. MODS & HD TEXTURES:\n"
            @"   Place mod folders in 'mods/' (e.g. mods/MHGU-HD-Project/romfs/nativeNX/...).\n"
            @"   Both RomFS mods and Atmosphere mod folders are supported!\n\n"
            @"3. Your save files will be stored in 'save/' automatically.\n\n"
            @"4. Game execution logs are written to 'log.txt' (and 'log.prev.txt').\n\n"
            @"Once you place your files, return to the MHGU Recomp app!\n";

        if (![fm fileExistsAtPath:readmePath]) {
            [readmeContent writeToFile:readmePath atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        // 5. Create subdirectories: data/exefs, data/romfs, save, mods, cheats
        NSString *dataPath = [docPath stringByAppendingPathComponent:@"data"];
        NSString *exefsPath = [dataPath stringByAppendingPathComponent:@"exefs"];
        NSString *romfsPath = [dataPath stringByAppendingPathComponent:@"romfs"];
        NSString *savePath = [docPath stringByAppendingPathComponent:@"save"];
        NSString *modsPath = [docPath stringByAppendingPathComponent:@"mods"];
        NSString *cheatsPath = [docPath stringByAppendingPathComponent:@"cheats"];

        [fm createDirectoryAtPath:exefsPath withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createDirectoryAtPath:romfsPath withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createDirectoryAtPath:savePath withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createDirectoryAtPath:modsPath withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createDirectoryAtPath:cheatsPath withIntermediateDirectories:YES attributes:nil error:nil];

        // 6. Exclude game data and mods from device backups (Documents is backed up by default)
        for (NSString *d in @[@"data", @"save", @"mods", @"cheats"]) {
            NSURL *u = [NSURL fileURLWithPath:[docPath stringByAppendingPathComponent:d]];
            [u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        }

        // 7. Placeholder instruction files inside exefs, romfs, and mods
        NSString *exefsHelp = [exefsPath stringByAppendingPathComponent:@"put_main_and_npdm_here.txt"];
        if (![fm fileExistsAtPath:exefsHelp]) {
            [@"Place 'main', 'main.npdm', and 'sdk' from your Switch v1.4.0 exefs here.\n"
                writeToFile:exefsHelp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        NSString *romfsHelp = [romfsPath stringByAppendingPathComponent:@"put_romfs_folders_here.txt"];
        if (![fm fileExistsAtPath:romfsHelp]) {
            [@"Place all folders and files from your Switch v1.4.0 romfs here.\n"
                writeToFile:romfsHelp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        NSString *modsHelp = [modsPath stringByAppendingPathComponent:@"README_MODS.txt"];
        if (![fm fileExistsAtPath:modsHelp]) {
            [@"======================================================\n"
             @" Monster Hunter Generations Ultimate - Mod Directory\n"
             @"======================================================\n\n"
             @"Just drop your mod folders here in 'mods/'!\n\n"
             @"Examples:\n"
             @"  mods/FF_Damage_Numbers/\n"
             @"  mods/Key_Quest_Markers/\n"
             @"  mods/MHGU-HD-Project/\n\n"
             @"Everything is auto-detected:\n"
             @"• Zip & 7-Zip Archives (.zip / .7z) - automatically unpacked & installed on startup!\n"
             @"• Forge Plugins (.nro) - floating damage numbers, quest markers, HUD mods\n"
             @"• RomFS Mods - HD textures, models, UI packs, sound mods\n"
             @"• ExeFS Patches (.ips / .pchtxt) - 60fps patches, aspect ratio, FOV\n\n"
             @"To temporarily disable any mod, prefix its folder name with '_' (e.g. _Key_Quest_Markers).\n"
                writeToFile:modsHelp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        // Clean out any stale pre-Metal-shader-fix caches so shaders retranslate with mhgu_ helpers
        NSString *cachePath = [docPath stringByAppendingPathComponent:@"cache"];
        NSString *cacheTag = [cachePath stringByAppendingPathComponent:@"mhgu_msl_v1.tag"];
        if (![fm fileExistsAtPath:cacheTag]) {
            [fm removeItemAtPath:cachePath error:nil];
            [fm createDirectoryAtPath:cachePath withIntermediateDirectories:YES attributes:nil error:nil];
            [@"v1" writeToFile:cacheTag atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        // 8. Set working directory to Documents and export environment variables
        chdir([docPath UTF8String]);
        setenv("MHGU_LOG_FILES", "1", 0);
        setenv("MHGU_MOVIE_TRACE", "1", 0);
        setenv("MHGU_MODS_DIR", [modsPath UTF8String], 1);
        setenv("MHGU_CHEATS_DIR", [cheatsPath UTF8String], 1);
        fprintf(stderr, "=== MHGU Recomp iOS Startup ===\n");
        fprintf(stderr, "[plat] Documents initialized at: %s\n", [docPath UTF8String]);
        fflush(stderr);
    }
}

// Early constructor so logging and filesystem setup run before UIApplicationMain
__attribute__((constructor(101))) static void darwin_early_log_init(void) {
    plat_setup_ios_documents();
}

@interface MHGUMissingFilesVC : UIViewController
@property (nonatomic, copy) NSString *mainPath;
@property (nonatomic, copy) NSString *docPath;
@property (nonatomic, assign) BOOL filesFound;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIView *container;
@end

@implementation MHGUMissingFilesVC

- (BOOL)shouldAutorotate {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.07 green:0.09 blue:0.13 alpha:1.0];

    self.scrollView = [[UIScrollView alloc] initWithFrame:self.view.bounds];
    self.scrollView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.scrollView];

    CGFloat width = self.view.bounds.size.width > 100 ? self.view.bounds.size.width : 1024;
    self.container = [[UIView alloc] initWithFrame:CGRectMake(30, 20, width - 60, 540)];
    self.container.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.scrollView addSubview:self.container];

    // 1. App Title
    UILabel *titleLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.container.bounds.size.width, 36)];
    titleLabel.text = @"Monster Hunter Generations Ultimate";
    titleLabel.font = [UIFont boldSystemFontOfSize:26];
    titleLabel.textColor = [UIColor whiteColor];
    titleLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.container addSubview:titleLabel];

    // Subtitle
    UILabel *subLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 38, self.container.bounds.size.width, 24)];
    subLabel.text = @"Native ARM64 iOS / iPadOS Port (Switch v1.4.0)";
    subLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
    subLabel.textColor = [UIColor colorWithRed:0.6 green:0.65 blue:0.75 alpha:1.0];
    subLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.container addSubview:subLabel];

    // 2. Alert Badge Box
    UIView *badgeBox = [[UIView alloc] initWithFrame:CGRectMake(0, 72, self.container.bounds.size.width, 48)];
    badgeBox.backgroundColor = [UIColor colorWithRed:0.25 green:0.18 blue:0.05 alpha:1.0];
    badgeBox.layer.cornerRadius = 8;
    badgeBox.layer.borderWidth = 1;
    badgeBox.layer.borderColor = [UIColor colorWithRed:0.85 green:0.65 blue:0.15 alpha:1.0].CGColor;
    badgeBox.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.container addSubview:badgeBox];

    UILabel *badgeText = [[UILabel alloc] initWithFrame:CGRectMake(16, 0, badgeBox.bounds.size.width - 32, 48)];
    badgeText.text = @"⚠️  Action Required: Game dump files not found";
    badgeText.font = [UIFont boldSystemFontOfSize:16];
    badgeText.textColor = [UIColor colorWithRed:1.0 green:0.85 blue:0.4 alpha:1.0];
    badgeText.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [badgeBox addSubview:badgeText];

    // 3. Instruction Card
    UIView *card = [[UIView alloc] initWithFrame:CGRectMake(0, 130, self.container.bounds.size.width, 240)];
    card.backgroundColor = [UIColor colorWithRed:0.11 green:0.14 blue:0.20 alpha:1.0];
    card.layer.cornerRadius = 10;
    card.layer.borderWidth = 1;
    card.layer.borderColor = [UIColor colorWithRed:0.2 green:0.25 blue:0.35 alpha:1.0].CGColor;
    card.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.container addSubview:card];

    UILabel *cardTitle = [[UILabel alloc] initWithFrame:CGRectMake(20, 14, card.bounds.size.width - 40, 24)];
    cardTitle.text = @"The MHGU Recomp folder is ready in your Files app:";
    cardTitle.font = [UIFont boldSystemFontOfSize:17];
    cardTitle.textColor = [UIColor whiteColor];
    [card addSubview:cardTitle];

    UITextView *infoText = [[UITextView alloc] initWithFrame:CGRectMake(16, 42, card.bounds.size.width - 32, 185)];
    infoText.backgroundColor = [UIColor clearColor];
    infoText.editable = NO;
    infoText.selectable = NO;
    infoText.scrollEnabled = NO;
    infoText.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    infoText.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    infoText.textColor = [UIColor colorWithRed:0.85 green:0.90 blue:0.95 alpha:1.0];
    infoText.text = [NSString stringWithFormat:
        @"1. Open the iPad 'Files' app (Split View or Slide Over supported).\n"
        @"2. Tap: 'On My iPad' -> 'MHGU Recomp' -> 'data'\n"
        @"3. Copy your Nintendo Switch v1.4.0 dump files into 'data':\n"
        @"     • data/exefs/main       (decompiled binary)\n"
        @"     • data/exefs/main.npdm  (program metadata)\n"
        @"     • data/exefs/sdk        (if present)\n"
        @"     • data/romfs/           (all game romfs asset folders)\n\n"
        @"Looking for: %@\n", self.mainPath ? self.mainPath : @"data/exefs/main"];
    [card addSubview:infoText];

    // 4. Action Buttons
    UIButton *checkBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    checkBtn.frame = CGRectMake(0, 385, 240, 48);
    checkBtn.backgroundColor = [UIColor colorWithRed:0.15 green:0.55 blue:0.35 alpha:1.0];
    [checkBtn setTitle:@"🔄  Check Files Again" forState:UIControlStateNormal];
    [checkBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    checkBtn.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    checkBtn.layer.cornerRadius = 8;
    [checkBtn addTarget:self action:@selector(onCheckFiles:) forControlEvents:UIControlEventTouchUpInside];
    [self.container addSubview:checkBtn];

    UIButton *exitBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    exitBtn.frame = CGRectMake(250, 385, 100, 48);
    exitBtn.backgroundColor = [UIColor colorWithRed:0.30 green:0.15 blue:0.15 alpha:1.0];
    [exitBtn setTitle:@"Exit" forState:UIControlStateNormal];
    [exitBtn setTitleColor:[UIColor colorWithRed:1.0 green:0.6 blue:0.6 alpha:1.0] forState:UIControlStateNormal];
    exitBtn.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
    exitBtn.layer.cornerRadius = 8;
    [exitBtn addTarget:self action:@selector(onExitApp:) forControlEvents:UIControlEventTouchUpInside];
    [self.container addSubview:exitBtn];

    self.statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(365, 385, self.container.bounds.size.width - 365, 48)];
    self.statusLabel.font = [UIFont systemFontOfSize:14];
    self.statusLabel.textColor = [UIColor colorWithRed:0.7 green:0.75 blue:0.85 alpha:1.0];
    self.statusLabel.numberOfLines = 2;
    self.statusLabel.text = @"Waiting for game files... (auto-checks every 2 seconds)";
    self.statusLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.container addSubview:self.statusLabel];

    self.scrollView.contentSize = CGSizeMake(self.view.bounds.size.width, 560);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    self.scrollView.frame = self.view.bounds;
    CGFloat pad = 30.0f;
    CGFloat cWidth = self.view.bounds.size.width - (pad * 2.0f);
    if (cWidth > 100) {
        self.container.frame = CGRectMake(pad, 20.0f, cWidth, 540.0f);
    }
    self.scrollView.contentSize = CGSizeMake(self.view.bounds.size.width, 560.0f);
}

- (void)onCheckFiles:(id)sender {
    if (!self.mainPath) return;
    FILE *f = fopen([self.mainPath UTF8String], "rb");
    if (f) {
        fclose(f);
        self.statusLabel.textColor = [UIColor colorWithRed:0.3 green:0.9 blue:0.4 alpha:1.0];
        self.statusLabel.text = @"Game files verified! Starting game...";
        self.filesFound = YES;
    } else {
        self.statusLabel.textColor = [UIColor colorWithRed:1.0 green:0.4 blue:0.4 alpha:1.0];
        self.statusLabel.text = [NSString stringWithFormat:@"'main' not found yet at:\n%@", self.mainPath];
    }
}

- (void)onExitApp:(id)sender {
    exit(0);
}

@end

void plat_show_missing_files_screen(const char *main_path, const char *doc_path) {
    @autoreleasepool {
        SDL_Init(SDL_INIT_VIDEO);

        SDL_Window *win = SDL_CreateWindow("MHGU Recomp", 1920, 1080, SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!win) win = SDL_CreateWindow("MHGU Recomp", 1280, 720, 0);

        UIWindow *targetWindow = nil;
        if (win) {
            SDL_PropertiesID props = SDL_GetWindowProperties(win);
            targetWindow = (__bridge UIWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
        }

        MHGUMissingFilesVC *vc = [[MHGUMissingFilesVC alloc] init];
        vc.mainPath = [NSString stringWithUTF8String:main_path];
        vc.docPath = doc_path ? [NSString stringWithUTF8String:doc_path] : nil;

        UIWindow *standaloneWindow = nil;
        if (targetWindow) {
            vc.view.frame = targetWindow.bounds;
            vc.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            [targetWindow addSubview:vc.view];
        } else {
            UIWindowScene *activeScene = nil;
            if (@available(iOS 13.0, *)) {
                for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
                    if ([scene isKindOfClass:[UIWindowScene class]]) {
                        activeScene = (UIWindowScene *)scene;
                        break;
                    }
                }
            }
            if (activeScene) {
                standaloneWindow = [[UIWindow alloc] initWithWindowScene:activeScene];
            } else {
                standaloneWindow = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
            }
            standaloneWindow.windowLevel = UIWindowLevelAlert + 1.0;
            standaloneWindow.rootViewController = vc;
            [standaloneWindow makeKeyAndVisible];
        }

        while (!vc.filesFound) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) exit(0);
            }

            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];

            FILE *f = fopen(main_path, "rb");
            if (f) {
                fclose(f);
                vc.filesFound = YES;
                break;
            }
        }

        if (targetWindow) {
            [vc.view removeFromSuperview];
        }
        if (standaloneWindow) {
            standaloneWindow.hidden = YES;
            [standaloneWindow resignKeyWindow];
        }
        if (win) {
            SDL_DestroyWindow(win);
        }
    }
}
#else
void plat_show_missing_files_screen(const char *main_path, const char *doc_path) {
    (void)main_path; (void)doc_path;
}
#endif

void plat_standalone(void) {
#if defined(__APPLE__)
    plat_setup_ios_documents();
#else
    char doc_dir[PATH_MAX] = "";
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(doc_dir, sizeof doc_dir, "%s/Documents", home);
    } else {
        char exe[PATH_MAX];
        if (plat_exe_path(exe, sizeof exe)) {
            char *s = strrchr(exe, '/');
            if (s) { *s = 0; snprintf(doc_dir, sizeof doc_dir, "%s", exe); }
        }
    }

    if (doc_dir[0]) {
        mkdir_p(doc_dir);
        chdir(doc_dir);
    }

    mkdir_p("data/exefs");
    mkdir_p("data/romfs");
    mkdir_p("save");
#endif
}

bool plat_user_dir(char *buf, size_t size) {
#if defined(__APPLE__)
    @autoreleasepool {
        NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *docPath = [paths firstObject];
        if (docPath && [docPath length] > 0) {
            snprintf(buf, size, "%s", [docPath UTF8String]);
            return true;
        }
    }
#endif
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Documents", home);
    mkdir_p(dir);
    snprintf(buf, size, "%s", dir);
    return true;
}

SDL_Window *ui_window(void);

// The files in a folder and its subfolders (main.c: whether the player's romfs is complete).
static int count_files(const char *dir, int depth) {
    DIR *dd = depth > 32 ? NULL : opendir(dir);
    if (!dd) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >= (int)sizeof sub) continue;
        int type = e->d_type;
        if (type == DT_UNKNOWN || type == DT_LNK) {  // (file systems without types; links: what they point at)
            struct stat st;
            type = stat(sub, &st) != 0 ? DT_UNKNOWN : S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR) n += count_files(sub, depth + 1);
        else if (type == DT_REG) n++;
    }
    closedir(dd);
    return n;
}
int plat_count_files(const char *dir) { return count_files(dir, 0); }

// An iPad (the touch controls' sizes: runtime/touch.cpp). macOS: no.
int plat_is_tablet(void) {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return [[UIDevice currentDevice] userInterfaceIdiom] == UIUserInterfaceIdiomPad;
#else
    return 0;
#endif
}

void plat_error_box(const char *msg) {
    fprintf(stderr, "\n*** %s\n", msg);
    fflush(stderr);
#if defined(__APPLE__)
    if (ui_window()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp", msg, ui_window());
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp", msg, NULL);
#endif
}

void plat_fatal_notice(void) {
    if (g_own_log) {
        plat_error_box("The game stopped because of an error. Check log.txt in Files > MHGURecomp.");
    }
}

#if defined(__APPLE__)
#include "../textinput.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#if TARGET_OS_IPHONE
@interface AlertReturnHandler : NSObject <UITextFieldDelegate>
@property (nonatomic, copy) void (^onReturn)(void);
@end

@implementation AlertReturnHandler
- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    if (self.onReturn) {
        self.onReturn();
    }
    return YES;
}
- (void)dealloc {
    [_onReturn release];
    [super dealloc];
}
@end
#endif

int plat_ios_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)max_chars;
    __block int r = 0;
    __block NSString *enteredText = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);

    NSString *t = (title && *title) ? [NSString stringWithUTF8String:title] : @"Character Name";
    NSString *m = (sub && *sub) ? [NSString stringWithUTF8String:sub] : @"Enter character name:";
    NSString *init = (initial && *initial) ? [NSString stringWithUTF8String:initial] : @"Hunter";
    NSString *ph = (guide && *guide) ? [NSString stringWithUTF8String:guide] : @"Hunter";

    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *ac = [UIAlertController alertControllerWithTitle:t
                                                                    message:m
                                                             preferredStyle:UIAlertControllerStyleAlert];

        __block UIAlertController *blockAc = ac;
        AlertReturnHandler *handler = [[AlertReturnHandler alloc] init];
        objc_setAssociatedObject(ac, "_retHandler", handler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [handler release];

        __block BOOL finished = NO;
        void (^finish)(BOOL isOk) = ^(BOOL isOk) {
            if (finished) return;
            finished = YES;
            r = isOk ? 1 : 0;
            if (isOk) {
                UITextField *tf = blockAc.textFields.firstObject;
                if (tf && tf.text && tf.text.length > 0) {
                    enteredText = [tf.text copy];
                }
            }
            [blockAc dismissViewControllerAnimated:YES completion:nil];
            dispatch_semaphore_signal(done);
        };

        handler.onReturn = ^{
            finish(YES);
        };

        [ac addTextFieldWithConfigurationHandler:^(UITextField * _Nonnull f) {
            f.text = init;
            f.placeholder = ph;
            f.autocapitalizationType = UITextAutocapitalizationTypeWords;
            f.clearButtonMode = UITextFieldViewModeWhileEditing;
            f.returnKeyType = UIReturnKeyDone;
            f.delegate = handler;
        }];

        UIAlertAction *okAction = [UIAlertAction actionWithTitle:@"OK"
                                                           style:UIAlertActionStyleDefault
                                                         handler:^(UIAlertAction * _Nonnull action) {
            (void)action;
            finish(YES);
        }];

        UIAlertAction *cancelAction = [UIAlertAction actionWithTitle:@"Cancel"
                                                               style:UIAlertActionStyleCancel
                                                             handler:^(UIAlertAction * _Nonnull action) {
            (void)action;
            finish(NO);
        }];

        [ac addAction:okAction];
        [ac addAction:cancelAction];

        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
            if ([s isKindOfClass:[UIWindowScene class]]) {
                for (UIWindow *w in ((UIWindowScene *)s).windows) {
                    if (w.isKeyWindow) { vc = w.rootViewController; break; }
                }
                if (vc) break;
            }
        }
        if (!vc) {
            for (UIWindow *w in UIApplication.sharedApplication.windows) {
                if (w.isKeyWindow) { vc = w.rootViewController; break; }
            }
        }
        if (!vc) vc = UIApplication.sharedApplication.windows.firstObject.rootViewController;

        while (vc.presentedViewController && !vc.presentedViewController.isBeingDismissed) {
            vc = vc.presentedViewController;
        }

        if (!vc) {
            finish(NO);
            return;
        }

        [vc presentViewController:ac animated:YES completion:^{
            UITextField *tf = ac.textFields.firstObject;
            [tf becomeFirstResponder];
        }];
    });

    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    if (!r || !enteredText || enteredText.length == 0) {
        if (enteredText) [enteredText release];
        return 0;
    }

    snprintf(out, out_cap, "%s", [enteredText UTF8String] ? [enteredText UTF8String] : "");
    [enteredText release];
    return 1;
}

void plat_ios_show_text_input(const char *title, const char *initial, int max_chars) {
    (void)title; (void)initial; (void)max_chars;
}

void plat_ios_dismiss_text_input(void) {}

#pragma clang diagnostic pop
#else
int plat_ios_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
void plat_ios_show_text_input(const char *title, const char *initial, int max_chars) {
    (void)title; (void)initial; (void)max_chars;
}
void plat_ios_dismiss_text_input(void) {}
#endif

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[PATH_MAX], exe[PATH_MAX];
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return NULL;
    if (!exe[0] && !plat_exe_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "-");
    if (!strcmp(di.dli_fname, exe)) return NULL;
    const char *slash = strrchr(di.dli_fname, '/');
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : di.dli_fname);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_fbase;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self;
    uint64_t exe = dladdr((void *)(uintptr_t)&plat_print_backtrace, &self) ? (uint64_t)(uintptr_t)self.dli_fbase : 0;
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx mhgurecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(exe ? pc - exe : 0));
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = (ucontext_t *)uc;
    uint64_t fa = si && si->si_addr ? (uint64_t)(uintptr_t)si->si_addr : 0;
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) return;

#if defined(__arm64__) || defined(__aarch64__)
    uint64_t pc = (ctx && ctx->uc_mcontext) ? (uint64_t)ctx->uc_mcontext->__ss.__pc : 0;
    uint64_t sp = (ctx && ctx->uc_mcontext) ? (uint64_t)ctx->uc_mcontext->__ss.__sp : 0;
    uint64_t lr = (ctx && ctx->uc_mcontext) ? (uint64_t)ctx->uc_mcontext->__ss.__lr : 0;
#elif defined(__x86_64__)
    uint64_t pc = (ctx && ctx->uc_mcontext) ? (uint64_t)ctx->uc_mcontext->__ss.__rip : 0;
    uint64_t sp = (ctx && ctx->uc_mcontext) ? (uint64_t)ctx->uc_mcontext->__ss.__rsp : 0;
    uint64_t lr = 0;
    uint64_t end = 0;
    if (plat_committed(sp, &end) && end - sp >= 8) lr = *(const uint64_t *)(uintptr_t)sp;
#else
    uint64_t pc = 0, sp = 0, lr = 0;
#endif

    int crash_fd = open("CRASH.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (crash_fd >= 0) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf), "FATAL SIGNAL: %s (%d) at address 0x%llx\npc=0x%llx, lr=0x%llx, sp=0x%llx\n",
                           sig_name(sig), sig, (unsigned long long)fa,
                           (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)sp);
        write(crash_fd, buf, len);
        close(crash_fd);
    }

    char what[80];
    snprintf(what, sizeof what, "%s (code %d, addr %llx)", sig_name(sig), si ? si->si_code : 0,
             (unsigned long long)fa);
    rt_fault_report(what, fa, pc, lr);
    _exit(139);
}

void (*plat_touch_hook)(uint64_t guest_addr, int write) = NULL;

void plat_crash_handler_install(void) {
    static char altstack[256 * 1024] __attribute__((aligned(16)));
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack}, old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_ONSTACK)) sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
