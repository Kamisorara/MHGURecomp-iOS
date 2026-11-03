// The phone's haptics ([Touch] Vibration, [Controls] PhoneRumble). The Android port plays them through its
// com.mhgu.recomp.Haptics class by JNI; here, CoreHaptics: a transient for the touch buttons' click, a continuous
// player renewed before it runs out for the game's rumble. Elsewhere these do nothing (a controller rumbles itself:
// host_input.c). The timings and the strength curve are the Android port's.
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "config.h"
#include "haptics.h"
#include "platform.h"
#include "rt.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>

#define TAP_NS        25000000ull  // clicks closer together than this are one
#define RUMBLE_MS     120          // each one-shot's length
#define RENEW_NS      90000000ull  // renewed this often while the rumble lasts (before it runs out: no gap)
#define CHANGE_NS     20000000ull  // a new strength at most this often (the game sends one every 5 ms)
#define CHANGE_AMP    20           // and only when it moved more than this (amplitude 1..255)
#define TAP_RESUME_NS 20000000ull  // a click replaces the rumble on the motor: the rumble again this long after
#define FLOOR         1311         // the stronger motor under 2 %: off (a fade's tail, that the phone would buzz with)

// The engine, looked for once on first use. 0: not yet, 1: there, -1: not (no haptics: never tried again).
static int ready;
static CHHapticEngine *engine;
static id<CHHapticPatternPlayer> rumble_player;  // the one-shot playing (nil: none)

static uint64_t tap_ns;                 // the last click
static int rumble_amp;                  // the amplitude playing (1..255; 0: none)
static uint64_t rumble_ns, rumble_due;  // when it was sent, when it must be renewed

// The engine started and ready (creating it is cheap, starting it takes a moment; the first click may fall in that).
static CHHapticEngine *eng(void) {
    if (ready < 0) return NULL;
    if (engine) return engine;
    NSError *err = nil;
    CHHapticEngine *e = [CHHapticEngine engineAndReturnError:&err];
    if (!e) {
        LOG("[haptics] no CoreHaptics engine: %s\n", err.localizedDescription.UTF8String ?: "?");
        ready = -1;
        return NULL;
    }
    __weak CHHapticEngine *we = e;
    e.resetHandler = ^{  // (the audio session changed: start again)
        [we startWithCompletionHandler:^(NSError *_) {}];
    };
    engine = e;
    [e startWithCompletionHandler:^(NSError *se) {
        if (se) LOG("[haptics] start failed: %s\n", se.localizedDescription.UTF8String ?: "?");
    }];
    LOG("[haptics] CoreHaptics ready\n");
    ready = 1;
    return e;
}

// A one-off player for one haptic event, started now.
static void play(CHHapticEvent *ev) {
    CHHapticEngine *e = eng();
    if (!e) return;
    NSError *err = nil;
    CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[]];
    id<CHHapticPatternPlayer> p = [e createPlayerWithPattern:pat error:&err];
    if (!p) return;
    [p startAtTime:0 error:&err];
}

void haptics_tap(void) {
    if (!config_live_touch_vibration()) return;
    uint64_t now = plat_now_ns();
    if (tap_ns && now - tap_ns < TAP_NS) return;
    if (!eng()) return;
    tap_ns = now;
    play([[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:@[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDIntensity value:1.0f]
    ] relativeTime:0]);
    if (rumble_amp && rumble_due > now + TAP_RESUME_NS) rumble_due = now + TAP_RESUME_NS;
}

void haptics_rumble(uint32_t v) {
    uint32_t lo = v >> 16, hi = v & 0xFFFF, s = lo > hi ? lo : hi;
    bool on = s >= FLOOR && config_live_phone_rumble();
    if (!on && !rumble_amp) return;  // (nothing playing, nothing wanted: almost always)
    if (!on) {
        [rumble_player stopAtTime:0];
        rumble_player = nil;
        rumble_amp = 0;
        return;
    }
    // (a phone's small motor is hardly felt under ~60: a floor, and a curve that lifts the weak values)
    int amp = 50 + (int)(205.0f * powf(s / 65535.0f, 0.7f) + 0.5f);
    if (amp > 255) amp = 255;
    uint64_t now = plat_now_ns();
    bool change = !rumble_amp || (abs(amp - rumble_amp) > CHANGE_AMP && now - rumble_ns >= CHANGE_NS);
    if (!change && now < rumble_due) return;
    float f = amp / 255.0f;
    CHHapticEvent *ev = [[CHHapticEvent alloc]
        initWithEventType:CHHapticEventTypeHapticContinuous
               parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDIntensity value:f] ]
              relativeTime:0
                  duration:RUMBLE_MS / 1000.0];
    NSError *err = nil;
    CHHapticEngine *e = eng();
    if (!e) return;
    CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[]];
    id<CHHapticPatternPlayer> p = [e createPlayerWithPattern:pat error:&err];
    if (!p) return;
    [rumble_player stopAtTime:0];  // (the previous one-shot, if its renewal came early)
    [p startAtTime:0 error:&err];
    rumble_player = p;
    rumble_amp = amp, rumble_ns = now, rumble_due = now + RENEW_NS;
}

void haptics_stop(void) {
    if (ready <= 0) return;  // (nothing was ever sent)
    [rumble_player stopAtTime:0];
    rumble_player = nil;
    rumble_amp = 0;
}

#else
void haptics_tap(void) {}
void haptics_rumble(uint32_t v) { (void)v; }
void haptics_stop(void) {}
#endif
