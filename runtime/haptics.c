// The phone's haptics ([Touch] Vibration, [Controls] PhoneRumble). The Android port plays them through its
// com.mhgu.recomp.Haptics class by JNI (its runtime/haptics.c); here, no-op for now: wiring CoreHaptics
// (CHHapticEngine, already linked in the Makefile) is a follow-up, and this keeps the interface in place.
#include <stdbool.h>

#include "haptics.h"

#include "config.h"

void haptics_tap(void) {}

void haptics_rumble(uint32_t v) {
    (void)v;
}

void haptics_stop(void) {}
