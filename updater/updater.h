// SHA-256 (what net/ldn.c hashes the matchmaking key with; the desktop updater's release check uses it too).
// updater/updater.c - the desktop updater itself - is not part of the iOS build.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);  // 64 hex digits and a NUL
